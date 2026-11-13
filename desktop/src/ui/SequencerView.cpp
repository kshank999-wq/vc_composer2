#include "ui/SequencerView.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <set>

#include "ui/AudioTrackStyle.h"
#include "ui/AutomationLaneView.h"
#include "ui/Theme.h"
#include "ui/TrackPanel.h"
#include "ui/WaveformCache.h"
#include "vcc/project/Automation.h"
#include "vcc/project/Performance.h"
#include "vcc/project/Render.h"
#include "vcc/project/RenderCache.h"
#include "vcc/session/Notate.h"

namespace vcc::desktop {

namespace {

constexpr int kHeader = 150;      ///< Lane header / keyboard column width.
constexpr int kScroll = 10;       ///< Vertical scroll bar width (right edge of every band).
constexpr int kToolbar = 32;
constexpr int kRuler = 34;
constexpr int kLaneHeight = 56;
constexpr int kDivider = 6;
constexpr int kHScroll = 12;
constexpr int kRollHeader = 30;
constexpr int kVelocity = 64;
constexpr int kKeyHeight = 11;
constexpr int kTakeHeight = 40;   ///< One take lane under an audio track.
constexpr int kEdge = 6;          ///< Grab width of a clip's trim edges and handles.
constexpr double kMinClip = 0.01; ///< Shortest clip a trim leaves, seconds.
constexpr int kBusHeight = 34;    ///< A bus row (automation only) at the bottom.

const juce::String kScoreHint =
    "Written in the score - drag to rewrite it (the score follows); Alt-drag to change only how it is played";

// --- Cached view of the document ----------------------------------------------------------------

struct RegionView {
    std::string id;
    std::string source;
    Ticks start = 0;
    Ticks end = 0;
    int lane = 0;
    std::vector<NoteEvent> notes;
};

struct LaneView {
    std::string id;
    juce::String name;
    juce::String version;
    juce::String family;
    int gm_program = 0;
    juce::Colour colour;
    bool reference = false;
    int channel = 0;
    /// Score notes as performed (overrides applied), with the id of the
    /// score note each came from, as written, and their overrides.
    std::vector<NoteEvent> score_notes;
    std::vector<notation::EventId> score_ids;
    std::map<notation::EventId, NoteEvent> score_written;
    /// Written start and end of each score note's tie chain, in the score.
    std::map<notation::EventId, std::pair<Ticks, Ticks>> score_span;
    std::map<notation::EventId, project::NoteOverride> performance;
    std::vector<RegionView> regions;
    int lo = 48, hi = 72;  ///< Pitch span for the lane's mini roll.
    int range_lo = 21, range_hi = 108;
    // Audio (Main Spec §11).
    bool audio = false;       ///< An audio track: clips, no MIDI.
    bool stereo = false;
    bool armed = false;
    std::vector<project::AudioClip> clips;  ///< Every lane, sorted by lane then start.
    std::vector<std::string> clip_paths;    ///< Resolved file of each clip.
    std::vector<int> takes;                 ///< Distinct take lanes (> 0) in use, ascending.
    // Automation (Main Spec §12).
    bool bus = false;                       ///< A bus row (master, fx, group): automation only.
    juce::String bus_kind;
    std::string mode = "read";
    std::vector<project::AutomationLane> automation;
};

/// One automation row under a track or bus: a shown lane, or (lane null) the
/// "choose a parameter" row of an owner whose automation is open but empty.
struct AutomationRow {
    const project::AutomationLane* lane = nullptr;
    int height = 0;
};

/// A region note named so it survives the document being copied by an edit.
struct NoteKey {
    std::string region;
    Ticks start = 0;
    int pitch = 0;
    bool operator==(const NoteKey& o) const { return region == o.region && start == o.start && pitch == o.pitch; }
};

juce::String short_source(const std::string& source)
{
    juce::String s(source);
    if (s.isEmpty()) return "MIDI";
    juce::String kind = s.upToFirstOccurrenceOf(":", false, false);
    juce::String rest = s.fromFirstOccurrenceOf(":", false, false);
    juce::String prefix;
    if (kind == "chord_explorer") prefix = "Chords";
    else if (kind == "ostinato") prefix = "Ostinato";
    else if (kind == "piano_roll") prefix = "Piano roll";
    else if (kind == "manual") prefix = "Manual";
    else prefix = kind.replaceCharacter('_', ' ');
    rest = rest.replaceCharacter('_', ' ');
    return rest.isEmpty() ? prefix : prefix + juce::String::fromUTF8(" \xc2\xb7 ") + rest;
}

juce::String note_name(int pitch)
{
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return juce::String(names[pitch % 12]) + juce::String(pitch / 12 - 1);
}

bool is_black(int pitch)
{
    const int pc = pitch % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

void style_scrollbar(juce::ScrollBar& bar)
{
    bar.setColour(juce::ScrollBar::thumbColourId, colours::outline.brighter(0.3f));
    bar.setColour(juce::ScrollBar::trackColourId, colours::background);
    bar.setAutoHide(false);
}

project::MidiRegion* find_region(project::TrackVersion& v, const std::string& id)
{
    for (auto& r : v.regions)
        if (r.id == id) return &r;
    return nullptr;
}

int find_note(const project::MidiRegion& r, Ticks start, int pitch)
{
    for (std::size_t i = 0; i < r.notes.size(); ++i)
        if (r.notes[i].start == start && r.notes[i].pitch == pitch) return static_cast<int>(i);
    return -1;
}

/// Keeps a region valid after an edit (validate_notes rules): positive
/// durations, no overlapping same-pitch notes (later entries win: an edited
/// note is appended, so it shortens or replaces what it lands on), and a
/// span covering every note, rounded out to whole bars. Returns true when
/// notes had to be shortened or replaced.
bool normalise_region(project::MidiRegion& r, const MeterMap& meter)
{
    bool clipped = false;
    std::vector<std::size_t> order(r.notes.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const auto& x = r.notes[a];
        const auto& y = r.notes[b];
        if (x.pitch != y.pitch) return x.pitch < y.pitch;
        if (x.start != y.start) return x.start < y.start;
        return a < b;
    });
    std::vector<bool> drop(r.notes.size(), false);
    for (std::size_t k = 0; k + 1 < order.size(); ++k) {
        auto& a = r.notes[order[k]];
        const auto& b = r.notes[order[k + 1]];
        if (a.pitch != b.pitch) continue;
        if (b.start == a.start) {
            drop[order[k]] = true;
            clipped = true;
        } else if (b.start < a.end()) {
            a.duration = b.start - a.start;
            clipped = true;
        }
    }
    std::vector<NoteEvent> kept;
    for (std::size_t i = 0; i < r.notes.size(); ++i)
        if (!drop[i] && r.notes[i].duration > 0) kept.push_back(r.notes[i]);
    std::sort(kept.begin(), kept.end());
    r.notes = std::move(kept);
    for (const auto& n : r.notes) {
        if (n.start < r.start) r.start = meter.bar_start(meter.bar_at(n.start));
        if (n.end() > r.end) r.end = meter.bar_start(meter.bar_at(n.end() - 1) + 1);
    }
    return clipped;
}

// --- Shared timeline model ------------------------------------------------------------------------

struct Model final : AutomationTimeline {
    explicit Model(AppContext& c) : ctx(c) {}

    AppContext& ctx;
    project::RenderCache render_cache;
    double px_per_quarter = 24.0;
    double view_start = 0.0;  ///< Tick at the left edge of the timeline.
    int timeline_width = 800;
    Ticks playhead = 0;
    bool playing = false;

    MeterMap meter;
    double bpm = 120.0;
    std::vector<project::Cue> cues;
    TempoMap tempo;
    std::vector<project::Marker> markers;               ///< hits, sync points (picture time)
    std::vector<project::FlaggedRegion> changed_regions; ///< from comparing cuts
    Ticks end = kTicksPerWhole * 32;
    std::vector<LaneView> lanes;

    std::function<void()> view_changed;   ///< zoom / scroll moved
    std::function<void()> open_piano_roll;
    std::function<void()> range_changed;  ///< the ruler selection changed

    // Audio clips and recording.
    WaveformCache waveforms;
    std::set<std::string> show_takes;     ///< tracks whose take lanes are open
    std::string clip_track, clip_id;      ///< the selected audio clip
    Ticks range_start = 0, range_end = 0; ///< ruler selection (punch range); empty when equal
    bool punch = false;
    bool recording = false;
    Ticks record_start = 0;

    // Automation (view state: which owners show their lanes, row heights, pencil).
    std::set<std::string> show_automation;
    std::map<std::string, int> lane_heights;  ///< "owner/lane id" -> row height
    bool pencil = false;
    std::function<void()> automation_layout_changed;
    std::function<void()> pencil_changed;

    std::vector<AutomationRow> automation_rows(const LaneView& l) const
    {
        std::vector<AutomationRow> rows;
        if (!show_automation.count(l.id)) return rows;
        for (const auto& a : l.automation) {
            if (!a.shown) continue;
            auto it = lane_heights.find(l.id + "/" + a.id);
            rows.push_back({&a, it == lane_heights.end() ? AutomationLaneView::kDefaultHeight : it->second});
        }
        if (rows.empty()) rows.push_back({nullptr, AutomationLaneView::kPlaceholderHeight});
        return rows;
    }
    int base_height(const LaneView& l) const { return l.bus ? kBusHeight : kLaneHeight; }
    int takes_height(const LaneView& l) const
    {
        const bool open = !l.takes.empty() && show_takes.count(l.id) > 0;
        return open ? static_cast<int>(l.takes.size()) * kTakeHeight : 0;
    }
    int automation_height(const LaneView& l) const
    {
        int h = 0;
        for (const auto& r : automation_rows(l)) h += r.height;
        return h;
    }
    int track_count() const
    {
        int n = 0;
        for (const auto& l : lanes) n += l.bus ? 0 : 1;
        return n;
    }

    // AutomationTimeline
    AppContext& context() const override { return ctx; }
    int header_width() const override { return kHeader; }
    double x_of_tick(double tick) const override { return x_of(tick); }
    double tick_at_x(double x) const override { return tick_of(x); }
    Ticks snap_tick(Ticks t) const override { return snap(t, clip_grid(), false); }
    Ticks playhead_tick() const override { return playhead; }
    std::pair<Ticks, Ticks> selected_range() const override { return {range_start, range_end}; }
    bool range_is_punch() const override { return punch; }
    void paint_timeline_grid(juce::Graphics& g, juce::Rectangle<int> area) const override;
    bool pencil_mode() const override { return pencil; }
    void set_pencil_mode(bool on) override
    {
        if (pencil == on) return;
        pencil = on;
        ctx.status(on ? "Pencil: drag in an automation lane to draw (P to switch off)" : "Pencil off");
        if (pencil_changed) pencil_changed();
    }
    void set_lane_height(const std::string& owner_id, const std::string& lane_id, int height) override
    {
        lane_heights[owner_id + "/" + lane_id] = height;
        if (automation_layout_changed) automation_layout_changed();
    }
    void set_automation_shown(const std::string& owner_id, bool on)
    {
        if (on == (show_automation.count(owner_id) > 0)) return;
        if (on) show_automation.insert(owner_id);
        else show_automation.erase(owner_id);
        if (automation_layout_changed) automation_layout_changed();
    }

    bool has_range() const { return range_end > range_start; }
    double x_of_seconds(double s) const { return x_of(static_cast<double>(tempo.tick_at_seconds(std::max(0.0, s)))); }
    double seconds_of_x(double x) const { return tempo.seconds_at(std::max<Ticks>(0, static_cast<Ticks>(tick_of(x)))); }
    /// Grid for snapping clips: follows the zoom (sixteenths close up, bars far out).
    Ticks clip_grid() const
    {
        if (px_of(kTicksPerQuarter / 4) >= 10.0) return kTicksPerQuarter / 4;
        if (px_of(kTicksPerQuarter) >= 10.0) return kTicksPerQuarter;
        return meter.at_bar(1).ticks_per_bar();
    }
    int row_height(const LaneView& l) const { return base_height(l) + takes_height(l) + automation_height(l); }
    int lane_top(std::size_t index) const
    {
        int y = 0;
        for (std::size_t i = 0; i < index && i < lanes.size(); ++i) y += row_height(lanes[i]);
        return y;
    }
    int content_height() const { return lane_top(lanes.size()); }

    double x_of(double tick) const { return kHeader + (tick - view_start) * px_per_quarter / kTicksPerQuarter; }
    double tick_of(double x) const { return view_start + (x - kHeader) * kTicksPerQuarter / px_per_quarter; }
    double visible_ticks() const { return timeline_width * kTicksPerQuarter / px_per_quarter; }
    double px_of(Ticks length) const { return static_cast<double>(length) * px_per_quarter / kTicksPerQuarter; }

    const LaneView* lane(const std::string& id) const
    {
        for (const auto& l : lanes)
            if (l.id == id) return &l;
        return nullptr;
    }
    const LaneView* selected_lane() const { return lane(ctx.session.selected_track()); }

    Ticks snap(Ticks t, Ticks grid, bool floor_mode) const
    {
        if (t < 0) t = 0;
        const Ticks bs = meter.bar_start(meter.bar_at(t));
        const Ticks off = t - bs;
        const Ticks q = floor_mode ? off / grid : (off + grid / 2) / grid;
        return bs + q * grid;
    }

    void set_view_start(double tick)
    {
        view_start = std::max(0.0, tick);
        if (view_changed) view_changed();
    }

    void zoom_about(double factor, double anchor_x)
    {
        const double anchor_tick = tick_of(anchor_x);
        px_per_quarter = juce::jlimit(3.0, 480.0, px_per_quarter * factor);
        view_start = std::max(0.0, anchor_tick - (anchor_x - kHeader) * kTicksPerQuarter / px_per_quarter);
        if (view_changed) view_changed();
    }

    void rebuild()
    {
        const auto& session = ctx.session;
        const auto& d = session.data();
        meter = d.meter;
        bpm = d.tempo.bpm_at(0);
        cues = d.cues;
        tempo = d.tempo;
        markers = d.markers;
        changed_regions = d.active_picture() ? d.active_picture()->changed_regions : std::vector<project::FlaggedRegion>{};
        end = session.content_end();
        double audio_end = 0.0;
        lanes.clear();
        for (const auto* t : session.music_tracks()) {
            LaneView l;
            l.id = t->id;
            l.name = juce::String(t->name);
            l.colour = colour_from_hex(t->color);
            l.reference = t->kind == project::TrackKind::Reference;
            l.channel = t->midi_channel;
            const auto range = session.range_for(*t);
            l.range_lo = range.first;
            l.range_hi = range.second;
            if (const auto* def = session.instrument_for(*t)) {
                l.family = juce::String(def->family);
                l.gm_program = def->unpitched ? -1 : def->gm_program;
            }
            const auto* v = t->active_version();
            if (v) l.version = juce::String(v->name);
            l.audio = t->kind == project::TrackKind::Audio;
            l.stereo = t->input_stereo;
            l.armed = t->record_armed;
            l.mode = t->automation_mode;
            l.automation = t->automation;
            if (l.audio) l.colour = audio_track_colour(*t);
            if (v) {
                l.clips = v->audio_clips;
                std::stable_sort(l.clips.begin(), l.clips.end(), [](const project::AudioClip& a, const project::AudioClip& b) {
                    return a.lane != b.lane ? a.lane < b.lane : a.start_seconds < b.start_seconds;
                });
                for (const auto& c : l.clips) {
                    l.clip_paths.push_back(session.resolve_audio_path(c.file));
                    if (c.lane > 0 && std::find(l.takes.begin(), l.takes.end(), c.lane) == l.takes.end())
                        l.takes.push_back(c.lane);
                    audio_end = std::max(audio_end, c.end_seconds());
                }
                std::sort(l.takes.begin(), l.takes.end());
            }
            if (v && !l.reference && !l.audio) {
                try {
                    // Score notes only (region notes have no score id), from
                    // the render cache: only what an edit reaches is rendered.
                    const auto& played = render_cache.render(*t);
                    for (std::size_t i = 0; i < played.notes.size(); ++i)
                        if (played.note_ids[i] != notation::kInvalidEventId) {
                            l.score_notes.push_back(played.notes[i]);
                            l.score_ids.push_back(played.note_ids[i]);
                        }
                    const auto& written = render_cache.render(*t, false);
                    for (std::size_t i = 0; i < written.notes.size(); ++i)
                        if (written.note_ids[i] != notation::kInvalidEventId)
                            l.score_written[written.note_ids[i]] = written.notes[i];
                    l.performance = v->performance;
                    l.score_span = session::written_spans(v->score);
                } catch (...) {
                    l.score_notes.clear();
                    l.score_ids.clear();
                    l.score_written.clear();
                }
                for (const auto& r : v->regions)
                    l.regions.push_back(RegionView{r.id, r.source, r.start, r.end, r.lane, r.notes});
            }
            int lo = 128, hi = -1;
            auto take = [&](const NoteEvent& n) {
                lo = std::min(lo, n.pitch);
                hi = std::max(hi, n.pitch);
            };
            for (const auto& n : l.score_notes) take(n);
            for (const auto& r : l.regions)
                for (const auto& n : r.notes) take(n);
            if (hi < 0) {
                lo = 54;
                hi = 72;
            }
            const int mid = (lo + hi) / 2;
            if (hi - lo < 14) {
                lo = mid - 7;
                hi = mid + 7;
            }
            l.lo = lo - 2;
            l.hi = hi + 2;
            lanes.push_back(std::move(l));
        }
        {
            std::vector<std::string> live;
            for (const auto& lane : lanes) live.push_back(lane.id);
            render_cache.retain(live);
        }
        // Buses at the bottom (automation only), the master last.
        std::vector<const project::Bus*> buses;
        for (const auto& b : d.buses)
            if (b.kind != "master") buses.push_back(&b);
        for (const auto& b : d.buses)
            if (b.kind == "master") buses.push_back(&b);
        for (const auto* b : buses) {
            LaneView l;
            l.bus = true;
            l.id = b->id;
            l.name = b->kind == "master" ? juce::String("Master") : juce::String(b->name);
            l.bus_kind = juce::String(b->kind);
            const juce::Colour kind_colour = b->kind == "master" ? juce::Colour(0xffc4cad3)
                                             : b->kind == "fx"   ? juce::Colour(0xff9a8cf0)
                                                                 : juce::Colour(0xff5fbfb2);
            l.colour = colour_from_hex(b->color, kind_colour);
            l.mode = b->automation_mode;
            l.automation = b->automation;
            lanes.push_back(std::move(l));
        }
        if (audio_end > 0.0) {
            const Ticks t = tempo.tick_at_seconds(audio_end);
            end = std::max(end, meter.bar_start(meter.bar_at(t) + 5));
        }
        // Forget a selected clip that went away (undo, delete).
        bool found = false;
        for (const auto& l : lanes)
            if (l.id == clip_track)
                for (const auto& c : l.clips) found = found || c.id == clip_id;
        if (!found) {
            clip_track.clear();
            clip_id.clear();
        }
    }

    const project::AudioClip* clip(const std::string& track, const std::string& id, const LaneView** lane_out = nullptr,
                                   std::size_t* index_out = nullptr) const
    {
        for (const auto& l : lanes)
            if (l.id == track)
                for (std::size_t i = 0; i < l.clips.size(); ++i)
                    if (l.clips[i].id == id) {
                        if (lane_out) *lane_out = &l;
                        if (index_out) *index_out = i;
                        return &l.clips[i];
                    }
        return nullptr;
    }
};

/// Alternating bar shading, bar lines, beat lines and an optional finer grid.
void paint_grid(juce::Graphics& g, const Model& m, juce::Rectangle<int> area, Ticks fine, bool shade)
{
    juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(area);
    const float top = static_cast<float>(area.getY());
    const float bottom = static_cast<float>(area.getBottom());
    const Ticks t0 = std::max<Ticks>(0, static_cast<Ticks>(m.tick_of(area.getX())));
    for (int bar = m.meter.bar_at(t0); bar < 100000; ++bar) {
        const Ticks bs = m.meter.bar_start(bar);
        const Ticks be = m.meter.bar_start(bar + 1);
        const float x0 = static_cast<float>(m.x_of(static_cast<double>(bs)));
        const float x1 = static_cast<float>(m.x_of(static_cast<double>(be)));
        if (x0 > area.getRight()) break;
        if (shade && bar % 2 == 0) {
            g.setColour(juce::Colours::white.withAlpha(0.022f));
            g.fillRect(juce::Rectangle<float>(x0, top, x1 - x0, bottom - top));
        }
        const Ticks beat = m.meter.at_bar(bar).beat_ticks();
        if (fine > 0 && m.px_of(fine) >= 7.0) {
            g.setColour(juce::Colours::white.withAlpha(0.035f));
            for (Ticks t = bs + fine; t < be; t += fine)
                if ((t - bs) % beat != 0) g.drawVerticalLine(juce::roundToInt(m.x_of(static_cast<double>(t))), top, bottom);
        }
        if (m.px_of(beat) >= 6.0) {
            g.setColour(juce::Colours::white.withAlpha(0.07f));
            for (Ticks t = bs + beat; t < be; t += beat)
                g.drawVerticalLine(juce::roundToInt(m.x_of(static_cast<double>(t))), top, bottom);
        }
        const double bar_px = x1 - x0;
        const int every = bar_px >= 6 ? 1 : (bar_px >= 2 ? 4 : 16);
        if ((bar - 1) % every == 0) {
            g.setColour(colours::outline.brighter(0.15f));
            g.drawVerticalLine(juce::roundToInt(x0), top, bottom);
        }
    }
}

void paint_playhead(juce::Graphics& g, const Model& m, juce::Rectangle<int> area)
{
    const double x = m.x_of(static_cast<double>(m.playhead));
    if (x < area.getX() || x > area.getRight()) return;
    g.setColour(colours::playhead);
    g.fillRect(juce::Rectangle<float>(static_cast<float>(x) - 0.5f, static_cast<float>(area.getY()), 1.5f,
                                      static_cast<float>(area.getHeight())));
}

void Model::paint_timeline_grid(juce::Graphics& g, juce::Rectangle<int> area) const { paint_grid(g, *this, area, 0, true); }

/// The "A" box in a track or bus header that shows its automation lanes.
juce::Rectangle<int> automation_button(const LaneView& l, juce::Rectangle<int> row)
{
    return l.bus ? juce::Rectangle<int>(kHeader - 30, row.getY() + 10, 22, 15)
                 : juce::Rectangle<int>(kHeader - 30, row.getY() + 24, 22, 15);
}

void paint_automation_button(juce::Graphics& g, const LaneView& l, juce::Rectangle<int> row, bool open)
{
    const auto r = automation_button(l, row).toFloat();
    const bool has = std::any_of(l.automation.begin(), l.automation.end(), [](const auto& a) { return a.points.size() > 1; });
    const juce::Colour mc = automation_ui::mode_colour(l.mode);
    const juce::Colour c = has ? mc : colours::text_dim;
    g.setColour(open ? c.withAlpha(0.28f) : colours::panel_raised);
    g.fillRoundedRectangle(r, 3.0f);
    g.setColour(open || has ? c.withAlpha(0.85f) : colours::outline.brighter(0.2f));
    g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, 1.0f);
    g.setColour(open ? colours::text : (has ? c : colours::text_dim));
    g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    g.drawText("A", r, juce::Justification::centred, false);
}

// --- Toolbar ---------------------------------------------------------------------------------------

class Toolbar final : public juce::Component {
public:
    explicit Toolbar(Model& m) : m_(m)
    {
        for (auto* b : {&zoom_out_, &zoom_in_, &fit_, &roll_, &pencil_}) addAndMakeVisible(*b);
        pencil_.setTooltip("Pencil: draw automation freehand in a lane (P)");
        pencil_.setClickingTogglesState(true);
        pencil_.setComponentID("sequencer_pencil");
        pencil_.setColour(juce::TextButton::buttonOnColourId, colours::accent.withAlpha(0.55f));
        pencil_.onClick = [this] { m_.set_pencil_mode(pencil_.getToggleState()); };
        zoom_out_.setTooltip("Zoom out (Ctrl/Cmd + mouse wheel)");
        zoom_in_.setTooltip("Zoom in (Ctrl/Cmd + mouse wheel)");
        fit_.setTooltip("Fit the whole project in the window");
        roll_.setTooltip("Show the piano roll for the selected track (or double-click a lane)");
        roll_.setClickingTogglesState(true);
        roll_.setColour(juce::TextButton::buttonOnColourId, colours::accent.withAlpha(0.55f));
        zoom_out_.onClick = [this] { m_.zoom_about(1.0 / 1.4, kHeader); };
        zoom_in_.onClick = [this] { m_.zoom_about(1.4, kHeader); };
        fit_.onClick = [this] {
            m_.px_per_quarter = juce::jlimit(3.0, 480.0, m_.timeline_width * static_cast<double>(kTicksPerQuarter) /
                                                             static_cast<double>(std::max<Ticks>(m_.end, kTicksPerWhole)));
            m_.set_view_start(0);
        };
    }

    juce::TextButton& roll_button() noexcept { return roll_; }
    juce::TextButton& pencil_button() noexcept { return pencil_; }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(colours::panel);
        g.setColour(colours::outline);
        g.drawHorizontalLine(getHeight() - 1, 0.0f, static_cast<float>(getWidth()));
        g.setColour(colours::text);
        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText("SEQUENCER", juce::Rectangle<int>(12, 0, kHeader - 12, getHeight()), juce::Justification::centredLeft);
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(12.0f));
        int lanes = m_.track_count();
        juce::String info = juce::String(lanes) + (lanes == 1 ? " track" : " tracks") + juce::String::fromUTF8("  \xc2\xb7  ") +
                            juce::String(m_.meter.bar_at(std::max<Ticks>(0, m_.end - 1))) + " bars";
        g.drawText(info, getLocalBounds().withTrimmedLeft(pencil_.getRight() + 16).withTrimmedRight(12),
                   juce::Justification::centredRight);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(0, 4).withTrimmedLeft(kHeader);
        zoom_out_.setBounds(r.removeFromLeft(28));
        r.removeFromLeft(4);
        zoom_in_.setBounds(r.removeFromLeft(28));
        r.removeFromLeft(4);
        fit_.setBounds(r.removeFromLeft(44));
        r.removeFromLeft(12);
        roll_.setBounds(r.removeFromLeft(92));
        r.removeFromLeft(6);
        pencil_.setBounds(r.removeFromLeft(64));
    }

private:
    Model& m_;
    juce::TextButton zoom_out_{"-"}, zoom_in_{"+"}, fit_{"Fit"}, roll_{"Piano Roll"}, pencil_{"Pencil"};
};

// --- Ruler -----------------------------------------------------------------------------------------

class Ruler final : public juce::Component {
public:
    explicit Ruler(Model& m) : m_(m) { setMouseCursor(juce::MouseCursor::IBeamCursor); }

    void paint(juce::Graphics& g) override
    {
        const int w = getWidth(), h = getHeight();
        g.fillAll(colours::panel_raised);
        auto header = juce::Rectangle<int>(0, 0, kHeader, h);
        g.setColour(colours::panel);
        g.fillRect(header);

        // Position readout in the header.
        const int bar = m_.meter.bar_at(m_.playhead);
        const auto sig = m_.meter.at_bar(bar);
        const Ticks in_bar = m_.playhead - m_.meter.bar_start(bar);
        const int beat = static_cast<int>(in_bar / sig.beat_ticks()) + 1;
        g.setColour(colours::text);
        g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
        g.drawText(juce::String(bar) + "." + juce::String(beat), header.reduced(10, 0).removeFromTop(h - 12),
                   juce::Justification::bottomLeft);
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(10.5f));
        g.drawText(juce::String(sig.to_string()) + juce::String::fromUTF8("  \xe2\x99\xa9=") + juce::String(m_.bpm, 0),
                   header.reduced(10, 0).removeFromTop(h - 12), juce::Justification::bottomRight);

        auto area = juce::Rectangle<int>(kHeader, 0, w - kHeader - kScroll, h);
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(area);
        const float fh = static_cast<float>(h);

        // Bar numbers and beat ticks.
        double bar_px_min = 1e9;
        {
            const Ticks t0 = std::max<Ticks>(0, static_cast<Ticks>(m_.tick_of(area.getX())));
            const int first = m_.meter.bar_at(t0);
            bar_px_min = m_.px_of(m_.meter.at_bar(first).ticks_per_bar());
        }
        int every = 1;
        while (bar_px_min * every < 34 && every < 1024) every *= 2;
        const Ticks t0 = std::max<Ticks>(0, static_cast<Ticks>(m_.tick_of(area.getX())));
        for (int b = m_.meter.bar_at(t0); b < 100000; ++b) {
            const Ticks bs = m_.meter.bar_start(b);
            const float x = static_cast<float>(m_.x_of(static_cast<double>(bs)));
            if (x > area.getRight()) break;
            const auto s = m_.meter.at_bar(b);
            const bool labelled = (b - 1) % every == 0;
            g.setColour(labelled ? colours::text_dim : colours::outline.brighter(0.2f));
            g.drawVerticalLine(juce::roundToInt(x), labelled ? fh - 20.0f : fh - 9.0f, fh);
            if (labelled) {
                g.setColour(colours::text);
                g.setFont(juce::FontOptions(11.0f));
                g.drawText(juce::String(b), juce::Rectangle<float>(x + 4.0f, fh - 20.0f, 40.0f, 13.0f),
                           juce::Justification::centredLeft);
            }
            if (m_.px_of(s.beat_ticks()) >= 8.0) {
                g.setColour(colours::outline.brighter(0.25f));
                for (int k = 1; k < s.numerator; ++k)
                    g.drawVerticalLine(juce::roundToInt(m_.x_of(static_cast<double>(bs + k * s.beat_ticks()))), fh - 5.0f, fh);
            }
        }

        // Meter changes (top band, warm).
        g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
        for (const auto& c : m_.meter.changes()) {
            const float x = static_cast<float>(m_.x_of(static_cast<double>(m_.meter.bar_start(c.bar))));
            if (x < area.getX() - 40 || x > area.getRight()) continue;
            g.setColour(colours::accent_warm);
            g.drawText(juce::String(c.signature.to_string()), juce::Rectangle<float>(x + 3.0f, 1.0f, 40.0f, 12.0f),
                       juce::Justification::centredLeft);
        }

        // Cue markers.
        for (const auto& cue : m_.cues) {
            const float x = static_cast<float>(m_.x_of(static_cast<double>(cue.start)));
            const float x2 = static_cast<float>(m_.x_of(static_cast<double>(cue.end)));
            if (x2 < area.getX() || x > area.getRight()) continue;
            g.setColour(colours::accent.withAlpha(0.25f));
            if (cue.end > cue.start) g.fillRect(juce::Rectangle<float>(x, 1.0f, x2 - x, 12.0f));
            g.setColour(colours::accent);
            g.fillRect(juce::Rectangle<float>(x, 1.0f, 2.0f, 12.0f));
            g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
            juce::String label(cue.code);
            if (!cue.title.empty()) label << " " << cue.title;
            g.setColour(colours::text);
            g.drawText(label, juce::Rectangle<float>(x + 5.0f, 1.0f, std::max(60.0f, x2 - x - 6.0f), 12.0f),
                       juce::Justification::centredLeft, true);
        }

        // Picture changes flagged by comparing cuts (Main Spec 13).
        for (const auto& r : m_.changed_regions) {
            const float x = static_cast<float>(m_.x_of(static_cast<double>(m_.tempo.tick_at_seconds(std::max(0.0, r.start_seconds)))));
            const float x2 = static_cast<float>(m_.x_of(static_cast<double>(m_.tempo.tick_at_seconds(std::max(0.0, r.end_seconds)))));
            if (x2 < area.getX() || x > area.getRight()) continue;
            g.setColour(colours::playhead.withAlpha(0.35f));
            g.fillRect(juce::Rectangle<float>(x, fh - 5.0f, std::max(2.0f, x2 - x), 4.0f));
        }

        // Hits and sync points: picture time, drawn where the music now meets them.
        for (const auto& mk : m_.markers) {
            const float x = static_cast<float>(m_.x_of(static_cast<double>(m_.tempo.tick_at_seconds(std::max(0.0, mk.seconds)))));
            if (x < area.getX() - 6 || x > area.getRight() + 6) continue;
            const juce::Colour c = mk.kind == "hit"    ? colours::playhead
                                   : mk.kind == "sync" ? colours::accent
                                                       : colours::accent_warm;
            juce::Path diamond;
            diamond.addQuadrilateral(x, fh - 16.0f, x + 5.0f, fh - 11.0f, x, fh - 6.0f, x - 5.0f, fh - 11.0f);
            g.setColour(c);
            g.fillPath(diamond);
            if (!mk.label.empty()) {
                g.setFont(juce::FontOptions(10.0f));
                g.drawText(juce::String(mk.label), juce::Rectangle<float>(x + 6.0f, fh - 17.0f, 120.0f, 11.0f),
                           juce::Justification::centredLeft, true);
            }
        }

        // Selected range (punch in/out when punch is on).
        if (m_.has_range()) {
            const float x = static_cast<float>(m_.x_of(static_cast<double>(m_.range_start)));
            const float x2 = static_cast<float>(m_.x_of(static_cast<double>(m_.range_end)));
            const juce::Colour c = m_.punch ? colours::playhead : colours::accent;
            g.setColour(c.withAlpha(0.22f));
            g.fillRect(juce::Rectangle<float>(x, 14.0f, x2 - x, fh - 14.0f));
            g.setColour(c);
            g.fillRect(juce::Rectangle<float>(x, 14.0f, 2.0f, fh - 14.0f));
            g.fillRect(juce::Rectangle<float>(x2 - 2.0f, 14.0f, 2.0f, fh - 14.0f));
            if (m_.punch && x2 - x > 60.0f) {
                g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
                g.drawText("PUNCH", juce::Rectangle<float>(x2 - 50.0f, 14.0f, 45.0f, 11.0f),
                           juce::Justification::centredRight, false);
            }
        }

        g.setColour(colours::outline);
        g.drawHorizontalLine(h - 1, static_cast<float>(area.getX()), static_cast<float>(area.getRight()));

        // Playhead marker.
        const float px = static_cast<float>(m_.x_of(static_cast<double>(m_.playhead)));
        juce::Path tri;
        tri.addTriangle(px - 6.0f, fh - 12.0f, px + 6.0f, fh - 12.0f, px, fh - 2.0f);
        g.setColour(colours::playhead);
        g.fillPath(tri);
        g.fillRect(juce::Rectangle<float>(px - 0.5f, fh - 4.0f, 1.5f, 4.0f));
    }

    // Click locates; click-drag selects a range (the punch range); right-click
    // offers the range commands.
    void mouseDown(const juce::MouseEvent& e) override
    {
        selecting_ = false;
        if (e.mods.isPopupMenu()) {
            show_menu();
            return;
        }
        locate(e);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || e.mouseDownPosition.x < kHeader) return;
        if (!selecting_ && e.getDistanceFromDragStartX() * e.getDistanceFromDragStartX() < 16) return;
        selecting_ = true;
        const bool free = e.mods.isCommandDown();
        auto at = [&](double x) {
            const Ticks t = std::max<Ticks>(0, static_cast<Ticks>(m_.tick_of(x)));
            return free ? t : m_.snap(t, m_.clip_grid(), false);
        };
        const Ticks a = at(e.mouseDownPosition.x), b = at(std::max(e.x, kHeader));
        m_.range_start = std::min(a, b);
        m_.range_end = std::max(a, b);
        if (auto* p = getParentComponent()) p->repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (selecting_ && m_.range_changed) m_.range_changed();
        selecting_ = false;
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCommandDown() || e.mods.isCtrlDown()) {
            m_.zoom_about(w.deltaY > 0 ? 1.2 : 1.0 / 1.2, e.x);
            return;
        }
        const float d = std::abs(w.deltaX) > std::abs(w.deltaY) ? w.deltaX : w.deltaY;
        m_.set_view_start(m_.view_start - d * m_.visible_ticks() * 0.25);
    }

private:
    void show_menu()
    {
        juce::PopupMenu menu;
        menu.addItem("Punch in/out on this range", m_.has_range(), m_.punch,
                     [this] { m_.ctx.commands.invokeDirectly(cmd::togglePunch, true); });
        menu.addItem("Locate to range start", m_.has_range(), false, [this] {
            m_.ctx.engine.locate(m_.range_start);
            m_.playhead = m_.range_start;
            if (auto* p = getParentComponent()) p->repaint();
        });
        menu.addSeparator();
        menu.addItem("Clear range", m_.has_range(), false, [this] {
            m_.range_start = m_.range_end = 0;
            if (m_.range_changed) m_.range_changed();
            if (auto* p = getParentComponent()) p->repaint();
        });
        menu.showMenuAsync({});
    }

    void locate(const juce::MouseEvent& e)
    {
        if (e.x < kHeader) return;
        const Ticks tick = m_.snap(static_cast<Ticks>(m_.tick_of(e.x)), kTicksPerQuarter / 4, false);
        m_.ctx.engine.locate(tick);
        m_.playhead = tick;
        if (auto* p = getParentComponent()) p->repaint();
    }

    Model& m_;
    bool selecting_ = false;
};

// --- Track lanes -------------------------------------------------------------------------------------

class LaneArea final : public juce::Component, private juce::ScrollBar::Listener {
public:
    explicit LaneArea(Model& m) : m_(m)
    {
        addAndMakeVisible(vscroll_);
        style_scrollbar(vscroll_);
        vscroll_.addListener(this);
        setWantsKeyboardFocus(true);
        m_.waveforms.on_changed = [this] { repaint(); };
    }

    ~LaneArea() override
    {
        m_.waveforms.on_changed = nullptr;
        views_.clear();
    }

    void update_scroll()
    {
        const int content = m_.content_height();
        const int visible = getHeight();
        scroll_y_ = juce::jlimit(0, std::max(0, content - visible), scroll_y_);
        vscroll_.setRangeLimits(0.0, std::max(content, visible));
        vscroll_.setCurrentRange(scroll_y_, visible, juce::dontSendNotification);
        layout_automation(false);
    }

    /// Places an AutomationLaneView on every shown automation row (reusing
    /// them, so selections survive edits); `refresh` hands them the document.
    void layout_automation(bool refresh)
    {
        std::map<std::string, std::unique_ptr<AutomationLaneView>> next;
        const int w = getWidth() - kScroll;
        int top = -scroll_y_;
        for (const auto& l : m_.lanes) {
            int y = top + m_.base_height(l) + m_.takes_height(l);
            for (const auto& row : m_.automation_rows(l)) {
                const std::string key = l.id + "/" + (row.lane ? row.lane->id : std::string("+"));
                auto it = views_.find(key);
                std::unique_ptr<AutomationLaneView> view;
                bool fresh = false;
                if (it != views_.end()) {
                    view = std::move(it->second);
                    views_.erase(it);
                } else {
                    view = std::make_unique<AutomationLaneView>(m_);
                    view->setComponentID("automation:" + key);
                    addChildComponent(*view);
                    fresh = true;
                }
                if (refresh || fresh) {
                    AutomationLaneView::Data d;
                    d.owner_id = l.id;
                    d.owner_name = l.name;
                    d.colour = l.colour;
                    d.mode = l.mode;
                    if (row.lane) d.lane = *row.lane;
                    view->set_data(std::move(d));
                }
                view->setBounds(0, y, w, row.height);
                view->setVisible(y + row.height > 0 && y < getHeight());
                y += row.height;
                next[key] = std::move(view);
            }
            top += m_.row_height(l);
        }
        // Views whose lane went away: removed now, deleted once any callback
        // that led here (a click on their own header) has returned.
        for (auto& [key, view] : views_) {
            removeChildComponent(view.get());
            graveyard_.push_back(std::move(view));
        }
        views_ = std::move(next);
        if (!graveyard_.empty()) {
            juce::Component::SafePointer<LaneArea> self(this);
            juce::MessageManager::callAsync([self] {
                if (self) self->graveyard_.clear();
            });
        }
    }

    /// The header readouts follow the playhead (only what changed repaints).
    void automation_playhead()
    {
        for (auto& [key, view] : views_)
            if (view->isVisible()) view->playhead_moved();
    }

    void ensure_visible(const std::string& id)
    {
        for (std::size_t i = 0; i < m_.lanes.size(); ++i) {
            if (m_.lanes[i].id != id) continue;
            const int y = m_.lane_top(i);
            if (y < scroll_y_) scroll_y_ = y;
            else if (y + m_.base_height(m_.lanes[i]) > scroll_y_ + getHeight())
                scroll_y_ = y + m_.base_height(m_.lanes[i]) - getHeight();
            update_scroll();
            repaint();
        }
    }

    void resized() override
    {
        vscroll_.setBounds(getWidth() - kScroll, 0, kScroll, getHeight());
        update_scroll();
    }

    void paint(juce::Graphics& g) override
    {
        const int w = getWidth(), h = getHeight();
        const auto timeline = juce::Rectangle<int>(kHeader, 0, w - kHeader - kScroll, h);
        g.fillAll(colours::background);
        g.setColour(colours::panel);
        g.fillRect(0, 0, kHeader, h);

        paint_grid(g, m_, timeline, 0, true);

        const std::string& selected = m_.ctx.session.selected_track();
        int y = -scroll_y_;
        for (const auto& l : m_.lanes) {
            const int rh = m_.row_height(l);
            const int own = rh - m_.automation_height(l);  // automation rows paint themselves
            if (y + own >= 0 && y <= h) {
                if (l.bus) paint_bus(g, l, juce::Rectangle<int>(0, y, w - kScroll, own), timeline);
                else paint_lane(g, l, juce::Rectangle<int>(0, y, w - kScroll, own), timeline, l.id == selected);
            }
            y += rh;
        }
        if (m_.track_count() == 0) {
            g.setColour(colours::text_dim);
            g.setFont(juce::FontOptions(14.0f));
            g.drawText("No tracks yet - add a track or a template", timeline, juce::Justification::centred);
        }
        // The ruler selection carried down through the lanes.
        if (m_.has_range()) {
            const float x0 = static_cast<float>(m_.x_of(static_cast<double>(m_.range_start)));
            const float x1 = static_cast<float>(m_.x_of(static_cast<double>(m_.range_end)));
            const juce::Colour c = m_.punch ? colours::playhead : colours::accent;
            juce::Graphics::ScopedSaveState state(g);
            g.reduceClipRegion(timeline);
            g.setColour(c.withAlpha(0.06f));
            g.fillRect(juce::Rectangle<float>(x0, 0.0f, x1 - x0, static_cast<float>(h)));
            g.setColour(c.withAlpha(0.45f));
            g.drawVerticalLine(juce::roundToInt(x0), 0.0f, static_cast<float>(h));
            g.drawVerticalLine(juce::roundToInt(x1), 0.0f, static_cast<float>(h));
        }
        g.setColour(colours::outline);
        g.drawVerticalLine(kHeader - 1, 0.0f, static_cast<float>(h));
        paint_playhead(g, m_, timeline);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        drag_ = Drag::None;
        const Hit hit = hit_at(e.y);
        if (!hit.lane) {
            select_clip({}, {});
            return;
        }
        const std::string track = hit.lane->id;
        const LaneView lane = *hit.lane;  // the model is rebuilt by any edit below
        if (e.x < kHeader && hit.take == 0 && automation_button(lane, hit.row).contains(e.getPosition()) &&
            !e.mods.isPopupMenu()) {
            m_.set_automation_shown(track, !m_.show_automation.count(track));
            return;
        }
        if (lane.bus) {
            select_clip({}, {});
            if (e.mods.isPopupMenu()) show_bus_menu(lane);
            return;
        }
        m_.ctx.session.select_track(track);
        if (e.x < kHeader) {
            if (e.mods.isPopupMenu()) TrackPanel::show_track_menu(m_.ctx, track);
            else if (!lane.takes.empty() && disclosure_rect(hit.row).contains(e.getPosition())) toggle_takes(track);
            return;
        }
        const int index = clip_at(lane, hit, e.position);
        if (index < 0) {
            select_clip({}, {});
            return;
        }
        const auto& c = lane.clips[static_cast<std::size_t>(index)];
        select_clip(track, c.id);
        grabKeyboardFocus();
        if (e.mods.isPopupMenu()) {
            show_clip_menu(lane, c);
            return;
        }
        drag_ = zone_for(c, clip_rect(c, hit.row), e.position);
        drag_track_ = track;
        drag_clip_ = c;
        drag_label_ = unique_label(drag_ == Drag::Move       ? "Move clip"
                                   : drag_ == Drag::TrimLeft  ? "Trim clip start"
                                   : drag_ == Drag::TrimRight ? "Trim clip end"
                                   : drag_ == Drag::FadeIn    ? "Fade in"
                                   : drag_ == Drag::FadeOut   ? "Fade out"
                                                              : "Clip gain");
        file_length_ = 0.0;
        if (auto* thumb = m_.waveforms.get(lane.clip_paths[static_cast<std::size_t>(index)]))
            file_length_ = thumb->getTotalLength();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (drag_ == Drag::None) return;
        const project::AudioClip o = drag_clip_;
        const bool free = e.mods.isCommandDown();  // Cmd/Ctrl: no snapping
        auto snapped = [&](double tick) {
            const Ticks t = std::max<Ticks>(0, static_cast<Ticks>(std::llround(tick)));
            return free ? t : m_.snap(t, m_.clip_grid(), false);
        };
        const double dticks = m_.tick_of(e.x) - m_.tick_of(e.mouseDownPosition.x);
        project::AudioClip c = o;
        switch (drag_) {
            case Drag::Move: {
                const Ticks t0 = m_.tempo.tick_at_seconds(o.start_seconds);
                c.start_seconds = std::max(0.0, m_.tempo.seconds_at(snapped(static_cast<double>(t0) + dticks)));
                break;
            }
            case Drag::TrimLeft: {
                const Ticks t0 = m_.tempo.tick_at_seconds(o.start_seconds);
                double d = m_.tempo.seconds_at(snapped(static_cast<double>(t0) + dticks)) - o.start_seconds;
                d = juce::jlimit(-o.source_offset, o.length - kMinClip, d);
                c.start_seconds = o.start_seconds + d;
                c.source_offset = o.source_offset + d;
                c.length = o.length - d;
                break;
            }
            case Drag::TrimRight: {
                const Ticks t1 = m_.tempo.tick_at_seconds(o.end_seconds());
                double len = m_.tempo.seconds_at(snapped(static_cast<double>(t1) + dticks)) - o.start_seconds;
                if (file_length_ > 0.0) len = std::min(len, file_length_ - o.source_offset);
                c.length = std::max(kMinClip, len);
                break;
            }
            case Drag::FadeIn:
                c.fade_in = juce::jlimit(0.0, o.length - o.fade_out, m_.seconds_of_x(e.x) - o.start_seconds);
                break;
            case Drag::FadeOut:
                c.fade_out = juce::jlimit(0.0, o.length - o.fade_in, o.end_seconds() - m_.seconds_of_x(e.x));
                break;
            case Drag::Gain: {
                const double per_px = e.mods.isShiftDown() ? 0.02 : 0.2;
                c.gain_db = juce::jlimit(-48.0, 12.0, o.gain_db - e.getDistanceFromDragStartY() * per_px);
                c.gain_db = std::round(c.gain_db * 10.0) / 10.0;
                break;
            }
            case Drag::None: return;
        }
        const project::AudioClip* now = m_.clip(drag_track_, o.id);
        if (!now) return;
        const auto same = [](double a, double b) { return std::abs(a - b) < 1e-9; };
        if (same(now->start_seconds, c.start_seconds) && same(now->source_offset, c.source_offset) &&
            same(now->length, c.length) && same(now->fade_in, c.fade_in) && same(now->fade_out, c.fade_out) &&
            same(now->gain_db, c.gain_db))
            return;
        m_.ctx.session.update_audio_clip(drag_track_, o.id, drag_label_, [c](project::AudioClip& x) {
            x.start_seconds = c.start_seconds;
            x.source_offset = c.source_offset;
            x.length = c.length;
            x.fade_in = c.fade_in;
            x.fade_out = c.fade_out;
            x.gain_db = c.gain_db;
        });
        if (drag_ == Drag::Gain) m_.ctx.status("Clip gain " + db_text(c.gain_db));
    }

    void mouseUp(const juce::MouseEvent&) override { drag_ = Drag::None; }

    void mouseMove(const juce::MouseEvent& e) override
    {
        auto shape = juce::MouseCursor::NormalCursor;
        const Hit hit = hit_at(e.y);
        if (hit.lane && e.x >= kHeader) {
            const int index = clip_at(*hit.lane, hit, e.position);
            if (index >= 0) {
                const auto& c = hit.lane->clips[static_cast<std::size_t>(index)];
                switch (zone_for(c, clip_rect(c, hit.row), e.position)) {
                    case Drag::TrimLeft:
                    case Drag::TrimRight: shape = juce::MouseCursor::LeftRightResizeCursor; break;
                    case Drag::FadeIn:
                    case Drag::FadeOut: shape = juce::MouseCursor::PointingHandCursor; break;
                    case Drag::Gain: shape = juce::MouseCursor::UpDownResizeCursor; break;
                    case Drag::Move:
                    case Drag::None: shape = juce::MouseCursor::NormalCursor; break;
                }
            }
        } else if (hit.lane && !hit.lane->takes.empty() && disclosure_rect(hit.row).contains(e.getPosition())) {
            shape = juce::MouseCursor::PointingHandCursor;
        } else if (hit.lane && hit.take == 0 && automation_button(*hit.lane, hit.row).contains(e.getPosition())) {
            shape = juce::MouseCursor::PointingHandCursor;
        }
        setMouseCursor(shape);
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        const Hit hit = hit_at(e.y);
        if (hit.lane && !hit.lane->audio && !hit.lane->bus && hit.take == 0) {
            m_.ctx.session.select_track(hit.lane->id);
            if (m_.open_piano_roll) m_.open_piano_roll();
        }
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (!key.getModifiers().isAnyModifierKeyDown() && (key.getKeyCode() == 'P' || key.getKeyCode() == 'p')) {
            m_.set_pencil_mode(!m_.pencil);
            return true;
        }
        if (m_.clip_id.empty()) return false;
        if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) {
            const std::string track = m_.clip_track, id = m_.clip_id;
            select_clip({}, {});
            m_.ctx.session.remove_audio_clip(track, id);
            return true;
        }
        if (key == juce::KeyPress::escapeKey) {
            select_clip({}, {});
            return true;
        }
        return false;
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCommandDown() || e.mods.isCtrlDown()) {
            m_.zoom_about(w.deltaY > 0 ? 1.2 : 1.0 / 1.2, std::max(e.x, kHeader));
            return;
        }
        const int content = m_.content_height();
        const bool horizontal = e.mods.isShiftDown() || std::abs(w.deltaX) > std::abs(w.deltaY) || content <= getHeight();
        if (horizontal) {
            const float d = std::abs(w.deltaX) > std::abs(w.deltaY) ? w.deltaX : w.deltaY;
            m_.set_view_start(m_.view_start - d * m_.visible_ticks() * 0.25);
        } else {
            scroll_y_ -= juce::roundToInt(w.deltaY * 160.0f);
            update_scroll();
            repaint();
        }
    }



    // --- Clip actions (also reachable from tests through the session) ----------------------

    /// Swaps a take onto the main lane as one undo step: whatever it overlaps
    /// on lane 0 moves to the take's lane, so nothing is lost.
    static bool promote_take(session::Session& session, const std::string& track_id, const std::string& clip_id)
    {
        return session.edit("Promote take", [&](project::ProjectData& d) {
            project::Track* t = d.find_track(track_id);
            project::TrackVersion* v = t ? t->active_version() : nullptr;
            if (!v) return false;
            project::AudioClip* take = nullptr;
            for (auto& c : v->audio_clips)
                if (c.id == clip_id) take = &c;
            if (!take || take->lane == 0) return false;
            const int lane = take->lane;
            for (auto& c : v->audio_clips)
                if (c.lane == 0 && c.start_seconds < take->end_seconds() && take->start_seconds < c.end_seconds())
                    c.lane = lane;
            take->lane = 0;
            std::stable_sort(v->audio_clips.begin(), v->audio_clips.end(),
                             [](const project::AudioClip& a, const project::AudioClip& b) {
                                 return a.lane != b.lane ? a.lane < b.lane : a.start_seconds < b.start_seconds;
                             });
            return true;
        });
    }

private:
    enum class Drag { None, Move, TrimLeft, TrimRight, FadeIn, FadeOut, Gain };

    struct Hit {
        const LaneView* lane = nullptr;
        int take = 0;  ///< 0: the track's main row; otherwise the take lane shown
        juce::Rectangle<int> row;
    };

    static juce::String db_text(double db)
    {
        return (db > 0.0 ? "+" : "") + juce::String(db, 1) + " dB";
    }

    /// An edit label that does not merge with the previous undo step (two
    /// drags in a row are two steps; every move within one drag is one).
    std::string unique_label(const std::string& base) const
    {
        return m_.ctx.session.undo_label() == base ? base + " " : base;
    }

    void select_clip(const std::string& track, const std::string& id)
    {
        if (m_.clip_track == track && m_.clip_id == id) return;
        m_.clip_track = track;
        m_.clip_id = id;
        repaint();
    }

    void toggle_takes(const std::string& track)
    {
        if (!m_.show_takes.erase(track)) m_.show_takes.insert(track);
        update_scroll();
        repaint();
    }

    void scrollBarMoved(juce::ScrollBar*, double start) override
    {
        scroll_y_ = juce::roundToInt(start);
        layout_automation(false);
        repaint();
    }

    void show_bus_menu(const LaneView& lane)
    {
        const std::string id = lane.id;
        const bool open = m_.show_automation.count(id) > 0;
        juce::PopupMenu menu;
        menu.addSectionHeader(lane.name + (lane.bus_kind == "master" ? juce::String() : " (" + lane.bus_kind + " bus)"));
        menu.addItem(open ? "Hide Automation" : "Show Automation", [this, id, open] { m_.set_automation_shown(id, !open); });
        menu.showMenuAsync({});
    }

    void paint_bus(juce::Graphics& g, const LaneView& l, juce::Rectangle<int> row, juce::Rectangle<int> timeline)
    {
        const bool master = l.bus_kind == "master";
        auto header = row.withWidth(kHeader).withTrimmedRight(1);
        g.setColour(colours::panel.darker(0.06f));
        g.fillRect(header);
        g.setColour(l.colour);
        g.fillRect(header.withWidth(5));
        auto text = header.reduced(14, 0).withTrimmedRight(26);
        g.setColour(colours::text.withAlpha(0.9f));
        g.setFont(juce::FontOptions(12.5f, master ? juce::Font::bold : juce::Font::plain));
        g.drawText(l.name, text.removeFromTop(row.getHeight() / 2 + 3).withTrimmedTop(3), juce::Justification::bottomLeft,
                   true);
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(10.0f));
        g.drawText(master ? juce::String("Master bus") : juce::String(l.bus_kind == "fx" ? "FX bus" : "Group bus"), text,
                   juce::Justification::topLeft, true);
        paint_automation_button(g, l, row, m_.show_automation.count(l.id) > 0);
        auto body = row.withLeft(kHeader).withRight(timeline.getRight());
        g.setColour(colours::background.darker(0.2f).withAlpha(0.6f));
        g.fillRect(body);
        g.setColour(colours::outline.withAlpha(0.8f));
        g.drawHorizontalLine(row.getBottom() - 1, 0.0f, static_cast<float>(row.getRight()));
        if (master) {
            g.setColour(colours::outline.brighter(0.1f));
            g.drawHorizontalLine(row.getY(), 0.0f, static_cast<float>(row.getRight()));
        }
    }

    Hit hit_at(int y) const
    {
        Hit hit;
        int top = -scroll_y_;
        for (const auto& l : m_.lanes) {
            const int rh = m_.row_height(l);
            if (y >= top && y < top + rh) {
                hit.lane = &l;
                const int base = m_.base_height(l);
                if (y < top + base) {
                    hit.row = {0, top, getWidth() - kScroll, base};
                } else if (y < top + base + m_.takes_height(l)) {
                    const int k = (y - top - base) / kTakeHeight;
                    hit.take = l.takes[static_cast<std::size_t>(std::min<int>(k, static_cast<int>(l.takes.size()) - 1))];
                    hit.row = {0, top + base + k * kTakeHeight, getWidth() - kScroll, kTakeHeight};
                } else {
                    hit.take = -1;  // an automation row (its own component normally takes the mouse)
                    hit.row = {0, y, getWidth() - kScroll, 1};
                }
                return hit;
            }
            top += rh;
        }
        return hit;
    }

    juce::Rectangle<float> clip_rect(const project::AudioClip& c, juce::Rectangle<int> row) const
    {
        const float x0 = static_cast<float>(m_.x_of_seconds(c.start_seconds));
        const float x1 = static_cast<float>(m_.x_of_seconds(c.end_seconds()));
        return {x0, static_cast<float>(row.getY()) + 3.0f, std::max(3.0f, x1 - x0),
                static_cast<float>(row.getHeight()) - 6.0f};
    }

    static juce::Rectangle<int> disclosure_rect(juce::Rectangle<int> row)
    {
        return {14, row.getY() + kLaneHeight - 17, kHeader - 20, 14};
    }

    static juce::Rectangle<float> gain_rect(juce::Rectangle<float> r)
    {
        if (r.getWidth() < 110.0f) return {};
        return {r.getRight() - 54.0f, r.getY() + 1.0f, 46.0f, 12.0f};
    }

    /// The clip under the mouse on the hit row (topmost first), or -1.
    int clip_at(const LaneView& l, const Hit& hit, juce::Point<float> p) const
    {
        if (hit.take == 0 && l.clips.empty()) return -1;
        for (int i = static_cast<int>(l.clips.size()); --i >= 0;) {
            const auto& c = l.clips[static_cast<std::size_t>(i)];
            if (c.lane != hit.take) continue;
            if (clip_rect(c, hit.row).expanded(2.0f, 0.0f).contains(p)) return i;
        }
        return -1;
    }

    Drag zone_for(const project::AudioClip& c, juce::Rectangle<float> r, juce::Point<float> p) const
    {
        const bool wide = r.getWidth() >= 4.0f * kEdge;
        if (wide && p.y < r.getY() + 12.0f) {
            const float fi = static_cast<float>(m_.x_of_seconds(c.start_seconds + c.fade_in));
            const float fo = static_cast<float>(m_.x_of_seconds(c.end_seconds() - c.fade_out));
            if (std::abs(p.x - std::max(fi, r.getX() + 4.0f)) <= kEdge) return Drag::FadeIn;
            if (std::abs(p.x - std::min(fo, r.getRight() - 4.0f)) <= kEdge) return Drag::FadeOut;
        }
        if (gain_rect(r).expanded(2.0f).contains(p)) return Drag::Gain;
        if (p.x - r.getX() <= kEdge) return Drag::TrimLeft;
        if (r.getRight() - p.x <= kEdge) return Drag::TrimRight;
        return Drag::Move;
    }

    void show_clip_menu(const LaneView& lane, const project::AudioClip& c)
    {
        auto& session = m_.ctx.session;
        const std::string track = lane.id, id = c.id;
        juce::PopupMenu menu;
        menu.addSectionHeader(juce::String(c.name.empty() ? std::string("Clip") : c.name));
        if (c.lane > 0)
            menu.addItem("Promote take", [&session, track, id] { promote_take(session, track, id); });
        menu.addItem(c.muted ? "Unmute clip" : "Mute clip", [this, track, id, muted = c.muted] {
            m_.ctx.session.update_audio_clip(track, id, unique_label(muted ? "Unmute clip" : "Mute clip"),
                                             [muted](project::AudioClip& x) { x.muted = !muted; });
        });
        juce::PopupMenu gain;
        for (double db : {6.0, 3.0, 0.0, -3.0, -6.0, -12.0})
            gain.addItem(db_text(db), true, std::abs(c.gain_db - db) < 0.05, [this, track, id, db] {
                m_.ctx.session.update_audio_clip(track, id, unique_label("Clip gain"),
                                                 [db](project::AudioClip& x) { x.gain_db = db; });
            });
        gain.addSeparator();
        gain.addItem("Drag the dB box on the clip to fine-tune", false, false, [] {});
        menu.addSubMenu("Clip gain (" + db_text(c.gain_db) + ")", gain);
        menu.addItem("Remove fades", c.fade_in > 0.0 || c.fade_out > 0.0, false, [this, track, id] {
            m_.ctx.session.update_audio_clip(track, id, unique_label("Remove fades"), [](project::AudioClip& x) {
                x.fade_in = 0.0;
                x.fade_out = 0.0;
            });
        });
        if (!lane.takes.empty()) {
            const bool open = m_.show_takes.count(track) > 0;
            menu.addItem(open ? "Hide take lanes" : "Show take lanes", [this, track] { toggle_takes(track); });
        }
        menu.addSeparator();
        menu.addItem("Delete clip", [this, track, id] {
            select_clip({}, {});
            m_.ctx.session.remove_audio_clip(track, id);
        });
        menu.showMenuAsync({});
    }

    void paint_header(juce::Graphics& g, const LaneView& l, juce::Rectangle<int> row, bool selected)
    {
        auto header = row.withWidth(kHeader).withHeight(kLaneHeight);
        g.setColour(selected ? colours::panel_raised : colours::panel);
        g.fillRect(row.withWidth(kHeader).withTrimmedRight(1));
        g.setColour(l.colour);
        g.fillRect(header.withWidth(5));
        auto text = header.reduced(14, 7).withTrimmedLeft(-2);
        g.setColour(l.reference ? colours::accent_warm : (selected ? colours::text : colours::text.withAlpha(0.85f)));
        g.setFont(juce::FontOptions(13.0f, selected ? juce::Font::bold : juce::Font::plain));
        auto name_row = text.removeFromTop(18);
        if (l.armed) {
            g.setColour(colours::playhead);
            g.fillEllipse(name_row.removeFromRight(10).withSizeKeepingCentre(9, 9).toFloat());
            g.setColour(l.reference ? colours::accent_warm : (selected ? colours::text : colours::text.withAlpha(0.85f)));
        }
        g.drawText(l.name, name_row, juce::Justification::centredLeft, true);
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(11.0f));
        juce::String sub = l.reference ? juce::String("Temp / reference")
                           : l.audio   ? juce::String(l.stereo ? "Audio - stereo" : "Audio - mono")
                                       : l.version;
        if (l.armed) sub << "  - armed";
        g.drawText(sub, text.removeFromTop(15).withTrimmedRight(26), juce::Justification::centredLeft, true);
        paint_automation_button(g, l, row, m_.show_automation.count(l.id) > 0);
        if (!l.takes.empty()) {
            const bool open = m_.show_takes.count(l.id) > 0;
            auto d = disclosure_rect(row);
            juce::Path tri;
            const float cx = static_cast<float>(d.getX()) + 4.0f, cy = static_cast<float>(d.getCentreY());
            if (open) tri.addTriangle(cx - 4.0f, cy - 2.5f, cx + 4.0f, cy - 2.5f, cx, cy + 3.0f);
            else tri.addTriangle(cx - 2.5f, cy - 4.0f, cx - 2.5f, cy + 4.0f, cx + 3.0f, cy);
            g.setColour(colours::text_dim);
            g.fillPath(tri);
            g.setFont(juce::FontOptions(10.5f));
            const int n = static_cast<int>(l.takes.size());
            g.drawText(juce::String(n) + (n == 1 ? " take lane" : " take lanes"), d.withTrimmedLeft(12),
                       juce::Justification::centredLeft, true);
        } else if (!l.reference && !l.audio) {
            int count = static_cast<int>(l.score_notes.size());
            for (const auto& r : l.regions) count += static_cast<int>(r.notes.size());
            if (count > 0) {
                g.setFont(juce::FontOptions(10.0f));
                g.drawText(juce::String(count) + (count == 1 ? " note" : " notes"), text, juce::Justification::centredLeft);
            }
        } else if (l.audio) {
            int n = 0;
            for (const auto& c : l.clips) n += c.lane == 0 ? 1 : 0;
            if (n > 0) {
                g.setFont(juce::FontOptions(10.0f));
                g.drawText(juce::String(n) + (n == 1 ? " clip" : " clips"), text, juce::Justification::centredLeft);
            }
        }
    }

    void paint_clip(juce::Graphics& g, const LaneView& l, std::size_t index, juce::Rectangle<int> row,
                    juce::Rectangle<int> body, bool take_row)
    {
        const auto& c = l.clips[index];
        const auto r = clip_rect(c, row);
        if (r.getRight() < body.getX() || r.getX() > body.getRight()) return;
        const bool selected = m_.clip_track == l.id && m_.clip_id == c.id;
        const juce::String name(c.name.empty() ? juce::File(juce::String(c.file)).getFileNameWithoutExtension() : juce::String(c.name));
        const std::string& path = l.clip_paths[index];

        if (!m_.waveforms.exists(path)) {
            // Missing file: hatched, named, still movable and deletable.
            g.setColour(colours::missing.withAlpha(0.12f));
            g.fillRoundedRectangle(r, 3.0f);
            {
                juce::Graphics::ScopedSaveState s(g);
                g.reduceClipRegion(r.toNearestInt());
                g.setColour(colours::missing.withAlpha(0.35f));
                for (float x = r.getX() - r.getHeight(); x < r.getRight(); x += 9.0f)
                    g.drawLine(x, r.getBottom(), x + r.getHeight(), r.getY(), 1.0f);
                g.setColour(colours::text);
                g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
                g.drawText("Missing: " + name, r.reduced(6.0f, 0.0f), juce::Justification::centredLeft, true);
            }
            g.setColour(selected ? colours::text : colours::missing);
            g.drawRoundedRectangle(r, 3.0f, selected ? 2.0f : 1.0f);
            return;
        }

        const float alpha = c.muted ? 0.45f : (take_row ? 0.75f : 1.0f);
        const juce::Colour colour = l.colour;
        g.setColour(colour.withAlpha(0.2f * alpha));
        g.fillRoundedRectangle(r, 3.0f);
        const auto band = r.withHeight(13.0f);
        g.setColour(colour.withAlpha(0.45f * alpha));
        g.fillRoundedRectangle(band, 3.0f);

        // Waveform: only the visible part is drawn, so close zoom stays cheap.
        auto wave = r.withTrimmedTop(14.0f).reduced(0.0f, 1.0f);
        const auto visible = wave.getIntersection(body.toFloat());
        if (auto* thumb = m_.waveforms.get(path); thumb && !visible.isEmpty() && wave.getWidth() > 0.0f) {
            const double t0 = c.source_offset + c.length * (visible.getX() - wave.getX()) / wave.getWidth();
            const double t1 = c.source_offset + c.length * (visible.getRight() - wave.getX()) / wave.getWidth();
            const float gain = juce::Decibels::decibelsToGain(static_cast<float>(c.gain_db));
            g.setColour((c.muted ? colours::text_dim : colour.brighter(0.75f)).withAlpha(0.9f * alpha));
            thumb->drawChannels(g, visible.toNearestInt(), t0, t1, gain);
        }

        // Fades as curves; what they take away is shaded.
        const float top = wave.getY(), bottom = wave.getBottom();
        auto curve = [&](float xa, float xb, bool in) {
            juce::Path shade, line;
            constexpr int kSteps = 24;
            shade.startNewSubPath(xa, top);
            for (int i = 0; i <= kSteps; ++i) {
                const float t = static_cast<float>(i) / kSteps;
                const float level = in ? std::sin(t * juce::MathConstants<float>::halfPi)
                                       : std::cos(t * juce::MathConstants<float>::halfPi);
                const float x = xa + (xb - xa) * t, y = bottom - (bottom - top) * level;
                shade.lineTo(x, y);
                if (i == 0) line.startNewSubPath(x, y);
                else line.lineTo(x, y);
            }
            shade.lineTo(xb, top);
            shade.closeSubPath();
            g.setColour(juce::Colours::black.withAlpha(0.35f));
            g.fillPath(shade);
            g.setColour(colours::text.withAlpha(0.8f));
            g.strokePath(line, juce::PathStrokeType(1.2f));
        };
        const float fi = static_cast<float>(m_.x_of_seconds(c.start_seconds + c.fade_in));
        const float fo = static_cast<float>(m_.x_of_seconds(c.end_seconds() - c.fade_out));
        if (c.fade_in > 0.0) curve(r.getX(), fi, true);
        if (c.fade_out > 0.0) curve(fo, r.getRight(), false);

        g.setColour(colours::text.withAlpha(alpha));
        g.setFont(juce::FontOptions(10.0f));
        juce::String label = name;
        if (c.muted) label << " (muted)";
        g.drawText(label, band.reduced(5.0f, 0.0f).withTrimmedRight(gain_rect(r).isEmpty() ? 0.0f : 56.0f),
                   juce::Justification::centredLeft, true);

        // Clip gain box (drag up/down) and fade handles.
        if (const auto gr = gain_rect(r); !gr.isEmpty() && (selected || std::abs(c.gain_db) > 0.05)) {
            g.setColour(colours::background.withAlpha(0.7f));
            g.fillRoundedRectangle(gr, 2.0f);
            g.setColour(std::abs(c.gain_db) > 0.05 ? colours::accent_warm : colours::text_dim);
            g.setFont(juce::FontOptions(9.5f, juce::Font::bold));
            g.drawText(db_text(c.gain_db), gr, juce::Justification::centred, false);
        }
        if (selected && r.getWidth() >= 4.0f * kEdge) {
            g.setColour(colours::text);
            for (float x : {std::max(fi, r.getX() + 4.0f), std::min(fo, r.getRight() - 4.0f)})
                g.fillRect(juce::Rectangle<float>(x - 3.5f, top - 3.5f, 7.0f, 7.0f));
        }
        g.setColour(selected ? colours::text : colour.withAlpha(0.8f * alpha));
        g.drawRoundedRectangle(r, 3.0f, selected ? 2.0f : 1.0f);
    }

    void paint_lane(juce::Graphics& g, const LaneView& l, juce::Rectangle<int> rows, juce::Rectangle<int> timeline,
                    bool selected)
    {
        const auto row = rows.withHeight(kLaneHeight);
        paint_header(g, l, rows, selected);

        // Take lanes under the track.
        if (m_.row_height(l) > kLaneHeight) {
            for (std::size_t k = 0; k < l.takes.size(); ++k) {
                const auto take_row = juce::Rectangle<int>(0, row.getBottom() + static_cast<int>(k) * kTakeHeight,
                                                           rows.getWidth(), kTakeHeight);
                g.setColour(colours::background.darker(0.25f));
                g.fillRect(take_row.withLeft(kHeader).withRight(timeline.getRight()));
                g.setColour(colours::text_dim);
                g.setFont(juce::FontOptions(11.0f));
                g.drawText("Take lane " + juce::String(l.takes[k]), take_row.withWidth(kHeader).reduced(22, 0),
                           juce::Justification::centredLeft, true);
                g.setColour(colours::outline.withAlpha(0.5f));
                g.drawHorizontalLine(take_row.getBottom() - 1, static_cast<float>(kHeader), static_cast<float>(take_row.getRight()));
                const auto body = take_row.withLeft(kHeader).withRight(timeline.getRight());
                juce::Graphics::ScopedSaveState state(g);
                g.reduceClipRegion(body);
                for (std::size_t i = 0; i < l.clips.size(); ++i)
                    if (l.clips[i].lane == l.takes[k]) paint_clip(g, l, i, take_row, body, true);
            }
        }

        // Body.
        auto body = row.withLeft(kHeader).withRight(timeline.getRight());
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(body);
        if (selected) {
            g.setColour(colours::selection.withAlpha(0.12f));
            g.fillRect(body);
        }
        g.setColour(colours::outline.withAlpha(0.8f));
        g.drawHorizontalLine(rows.getBottom() - 1, 0.0f, static_cast<float>(rows.getRight()));

        bool has_main_clips = false;
        for (const auto& c : l.clips) has_main_clips = has_main_clips || c.lane == 0;

        if (l.reference) {
            g.setColour(colours::accent_warm.withAlpha(0.06f));
            for (int x = body.getX() - body.getHeight(); x < body.getRight(); x += 14)
                g.drawLine(static_cast<float>(x), static_cast<float>(body.getBottom()),
                           static_cast<float>(x + body.getHeight()), static_cast<float>(body.getY()), 1.0f);
            if (!has_main_clips) {
                g.setColour(colours::accent_warm.withAlpha(0.6f));
                g.setFont(juce::FontOptions(11.0f, juce::Font::italic));
                g.drawText("Temp / reference - no MIDI", body.reduced(10, 0), juce::Justification::centredLeft);
            }
        } else if (l.audio) {
            if (!has_main_clips && !(l.armed && m_.recording)) {
                g.setColour(colours::text_dim.withAlpha(0.5f));
                g.setFont(juce::FontOptions(11.0f, juce::Font::italic));
                g.drawText(l.armed ? "Armed - press Record and Play to record here"
                                   : "Empty audio track - record, or File > Import Audio",
                           body.reduced(10, 0), juce::Justification::centredLeft);
            }
        } else {
            paint_notes(g, l, body);
        }

        for (std::size_t i = 0; i < l.clips.size(); ++i)
            if (l.clips[i].lane == 0) paint_clip(g, l, i, row, body, false);

        // A pass being recorded grows from where it started to the playhead.
        if (l.armed && m_.recording && m_.playhead > m_.record_start) {
            const float x0 = static_cast<float>(m_.x_of(static_cast<double>(m_.record_start)));
            const float x1 = static_cast<float>(m_.x_of(static_cast<double>(m_.playhead)));
            auto rr = juce::Rectangle<float>(x0, static_cast<float>(row.getY()) + 3.0f, x1 - x0,
                                             static_cast<float>(row.getHeight()) - 6.0f);
            g.setColour(colours::playhead.withAlpha(0.28f));
            g.fillRoundedRectangle(rr, 3.0f);
            g.setColour(colours::playhead);
            g.drawRoundedRectangle(rr, 3.0f, 1.2f);
            g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
            g.drawText("Recording", rr.reduced(5.0f, 2.0f).removeFromBottom(12.0f), juce::Justification::centredRight, false);
        }
    }

    void paint_notes(juce::Graphics& g, const LaneView& l, juce::Rectangle<int> body)
    {
        const auto notes_area = body.reduced(0, 5).toFloat();
        const int span = std::max(1, l.hi - l.lo + 1);
        const float row_h = notes_area.getHeight() / static_cast<float>(span);
        const float note_h = std::max(2.0f, std::min(row_h, 5.0f));
        auto note_rect = [&](const NoteEvent& n) {
            const float x0 = static_cast<float>(m_.x_of(static_cast<double>(n.start)));
            const float x1 = static_cast<float>(m_.x_of(static_cast<double>(n.end())));
            const float y = notes_area.getBottom() - (static_cast<float>(n.pitch - l.lo) + 0.5f) * row_h - note_h / 2.0f;
            return juce::Rectangle<float>(x0, y, std::max(1.5f, x1 - x0 - 0.5f), note_h);
        };

        // Regions (behind their notes).
        for (const auto& r : l.regions) {
            const float x0 = static_cast<float>(m_.x_of(static_cast<double>(r.start)));
            const float x1 = static_cast<float>(m_.x_of(static_cast<double>(r.end)));
            if (x1 < body.getX() || x0 > body.getRight()) continue;
            auto rr = juce::Rectangle<float>(x0 + 0.5f, static_cast<float>(body.getY()) + 2.0f, x1 - x0 - 1.0f,
                                             static_cast<float>(body.getHeight()) - 4.0f);
            g.setColour(l.colour.withAlpha(0.13f));
            g.fillRoundedRectangle(rr, 3.0f);
            g.setColour(l.colour.withAlpha(0.65f));
            g.drawRoundedRectangle(rr, 3.0f, 1.0f);
            auto label = rr.withHeight(13.0f);
            g.setColour(l.colour.withAlpha(0.32f));
            g.fillRoundedRectangle(label, 3.0f);
            g.setColour(colours::text);
            g.setFont(juce::FontOptions(10.0f));
            g.drawText(short_source(r.source), label.reduced(4.0f, 0.0f), juce::Justification::centredLeft, true);
        }

        // Score-linked notes: solid track colour.
        g.setColour(l.colour.withAlpha(0.9f));
        for (const auto& n : l.score_notes) {
            auto r = note_rect(n);
            if (r.getRight() < body.getX() || r.getX() > body.getRight()) continue;
            g.fillRoundedRectangle(r, 1.0f);
        }
        // MIDI-region notes: brighter, outlined.
        const auto bright = l.colour.brighter(0.7f);
        for (const auto& reg : l.regions)
            for (const auto& n : reg.notes) {
                auto r = note_rect(n);
                if (r.getRight() < body.getX() || r.getX() > body.getRight()) continue;
                g.setColour(bright);
                g.fillRoundedRectangle(r, 1.0f);
                g.setColour(juce::Colours::black.withAlpha(0.45f));
                g.drawRoundedRectangle(r, 1.0f, 0.6f);
            }
    }

    Model& m_;
    juce::ScrollBar vscroll_{true};
    int scroll_y_ = 0;
    Drag drag_ = Drag::None;
    std::string drag_track_, drag_label_;
    project::AudioClip drag_clip_;
    double file_length_ = 0.0;  ///< Of the dragged clip's file, when known.
    std::map<std::string, std::unique_ptr<AutomationLaneView>> views_;  ///< "owner/lane id" ("owner/+": empty row)
    std::vector<std::unique_ptr<AutomationLaneView>> graveyard_;
};

// --- Divider between the lanes and the piano roll ----------------------------------------------

class Divider final : public juce::Component {
public:
    std::function<void(int)> on_drag;  ///< y delta since the drag began
    std::function<void()> on_begin;

    Divider() { setMouseCursor(juce::MouseCursor::UpDownResizeCursor); }
    void paint(juce::Graphics& g) override
    {
        g.fillAll(colours::panel);
        g.setColour(colours::outline);
        g.drawHorizontalLine(0, 0.0f, static_cast<float>(getWidth()));
        g.drawHorizontalLine(getHeight() - 1, 0.0f, static_cast<float>(getWidth()));
        g.setColour(colours::text_dim.withAlpha(0.5f));
        const float cx = getWidth() / 2.0f;
        for (int i = -1; i <= 1; ++i) g.fillEllipse(cx + i * 7.0f - 1.5f, getHeight() / 2.0f - 1.5f, 3.0f, 3.0f);
    }
    void mouseDown(const juce::MouseEvent&) override
    {
        if (on_begin) on_begin();
    }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (on_drag) on_drag(e.getDistanceFromDragStartY());
    }
};

// --- Piano roll ------------------------------------------------------------------------------------

/// The Performance... panel: exact offsets for the selected score notes
/// (how they are played, not what is written). Each slider sets that offset
/// on every selected note; a slider drag is one undo step.
class PerformancePanel final : public juce::Component {
public:
    PerformancePanel(AppContext& ctx, std::string track, std::vector<notation::EventId> ids, double bpm)
        : ctx_(ctx), track_(std::move(track)), ids_(std::move(ids)), bpm_(bpm)
    {
        project::NoteOverride start;
        if (const auto* perf = ctx_.session.performance_of(track_))
            if (auto it = perf->find(ids_.front()); it != perf->end()) start = it->second;
        title_.setText(ids_.size() == 1 ? juce::String("How this note is played")
                                        : "How these " + juce::String(static_cast<int>(ids_.size())) + " notes are played",
                       juce::dontSendNotification);
        title_.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        title_.setColour(juce::Label::textColourId, colours::text);
        addAndMakeVisible(title_);
        const double q = static_cast<double>(kTicksPerQuarter);
        add_row(0, "Timing", -q / 2, q / 2, static_cast<double>(start.onset), " ticks");
        add_row(1, "Length", -q, 2 * q, static_cast<double>(start.length), " ticks");
        add_row(2, "Velocity", -64, 64, start.velocity, "");
        add_row(3, "CC", -64, 64, start.cc, "");
        reset_.setButtonText("As written");
        reset_.onClick = [this] {
            ctx_.session.clear_performance(track_, ids_);
            for (auto& r : rows_) r.slider.setValue(0.0, juce::dontSendNotification);
            refresh_labels();
        };
        addAndMakeVisible(reset_);
        refresh_labels();
        setSize(360, 34 + 4 * 30 + 40);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(12, 8);
        title_.setBounds(r.removeFromTop(24));
        for (auto& row : rows_) {
            auto line = r.removeFromTop(30);
            row.name.setBounds(line.removeFromLeft(62));
            row.value.setBounds(line.removeFromRight(86));
            row.slider.setBounds(line.reduced(4, 4));
        }
        reset_.setBounds(r.removeFromBottom(28).removeFromRight(110));
    }

    void paint(juce::Graphics& g) override { g.fillAll(colours::panel); }

private:
    struct Row {
        juce::Label name, value;
        juce::Slider slider{juce::Slider::LinearHorizontal, juce::Slider::NoTextBox};
    };

    void add_row(int index, const juce::String& name, double lo, double hi, double value, const juce::String&)
    {
        auto& row = rows_[static_cast<std::size_t>(index)];
        row.name.setText(name, juce::dontSendNotification);
        row.name.setColour(juce::Label::textColourId, colours::text_dim);
        row.value.setColour(juce::Label::textColourId, colours::text);
        row.value.setJustificationType(juce::Justification::centredRight);
        row.slider.setRange(lo, hi, 1.0);
        row.slider.setValue(value, juce::dontSendNotification);
        row.slider.setDoubleClickReturnValue(true, 0.0);
        row.slider.onDragStart = [this] { flip_ = !flip_; };
        row.slider.onValueChange = [this, index] { apply(index); };
        addAndMakeVisible(row.name);
        addAndMakeVisible(row.slider);
        addAndMakeVisible(row.value);
    }

    void apply(int index)
    {
        static const char* labels[] = {"Performance timing", "Performance length", "Performance velocity", "Performance CC"};
        const auto v = static_cast<int>(rows_[static_cast<std::size_t>(index)].slider.getValue());
        ctx_.session.edit_performance(track_, ids_, std::string(labels[index]) + (flip_ ? " " : ""),
                                      [index, v](notation::EventId, project::NoteOverride& o) {
                                          switch (index) {
                                              case 0: o.onset = v; break;
                                              case 1: o.length = v; break;
                                              case 2: o.velocity = v; break;
                                              default: o.cc = v; break;
                                          }
                                      });
        refresh_labels();
    }

    void refresh_labels()
    {
        auto ms = [&](double ticks) {
            return juce::roundToInt(1000.0 * 60.0 / std::max(1.0, bpm_) * ticks / static_cast<double>(kTicksPerQuarter));
        };
        auto sign = [](int v) { return (v > 0 ? "+" : "") + juce::String(v); };
        rows_[0].value.setText(sign(ms(rows_[0].slider.getValue())) + " ms", juce::dontSendNotification);
        rows_[1].value.setText(sign(ms(rows_[1].slider.getValue())) + " ms", juce::dontSendNotification);
        rows_[2].value.setText(sign(static_cast<int>(rows_[2].slider.getValue())), juce::dontSendNotification);
        rows_[3].value.setText(sign(static_cast<int>(rows_[3].slider.getValue())), juce::dontSendNotification);
    }

    AppContext& ctx_;
    std::string track_;
    std::vector<notation::EventId> ids_;
    double bpm_ = 120.0;
    juce::Label title_;
    std::array<Row, 4> rows_;
    juce::TextButton reset_;
    bool flip_ = false;
};

/// Notate Region (Main Spec §8, MIDI -> notation): a MIDI region written
/// into the track's score. The settings re-plan as they change, and the
/// buttons are the plan's readings - where the region lands on written
/// notes, or where the quantizer heard triplets, more than one.
class RegionNotatePanel final : public juce::Component {
public:
    RegionNotatePanel(AppContext& ctx, std::string track, std::string region)
        : ctx_(ctx), track_(std::move(track)), region_(std::move(region))
    {
        grid_.addItem("Eighth notes", 1);
        grid_.addItem("Sixteenth notes", 2);
        grid_.addItem("32nd notes", 3);
        grid_.setSelectedId(2, juce::dontSendNotification);
        grid_.onChange = [this] { replan(); };
        triplets_.setButtonText("Allow triplets");
        triplets_.setToggleState(true, juce::dontSendNotification);
        triplets_.onClick = [this] { replan(); };
        feel_.setButtonText("Keep the played feel (timing and velocity as a performance)");
        feel_.setToggleState(true, juce::dontSendNotification);
        feel_.onClick = [this] { replan(); };
        for (auto* l : {&grid_label_, &summary_}) {
            l->setColour(juce::Label::textColourId, colours::text);
            addAndMakeVisible(*l);
        }
        grid_label_.setText("Quantize to", juce::dontSendNotification);
        summary_.setJustificationType(juce::Justification::topLeft);
        summary_.setFont(juce::FontOptions(12.5f));
        for (auto* t : {&triplets_, &feel_}) t->setColour(juce::ToggleButton::textColourId, colours::text);
        addAndMakeVisible(grid_);
        addAndMakeVisible(triplets_);
        addAndMakeVisible(feel_);
        cancel_.setButtonText("Cancel");
        cancel_.onClick = [this] { close(); };
        addAndMakeVisible(cancel_);
        setSize(520, 300);
        replan();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(16, 12);
        auto row = r.removeFromTop(26);
        grid_label_.setBounds(row.removeFromLeft(90));
        grid_.setBounds(row.removeFromLeft(170));
        row.removeFromLeft(16);
        triplets_.setBounds(row);
        r.removeFromTop(6);
        feel_.setBounds(r.removeFromTop(24));
        r.removeFromTop(8);
        auto buttons = r.removeFromBottom(30);
        cancel_.setBounds(buttons.removeFromRight(90));
        for (auto it = choices_.rbegin(); it != choices_.rend(); ++it) {
            buttons.removeFromRight(8);
            (*it)->setBounds(buttons.removeFromRight(juce::jmin(200, (*it)->getBestWidthForHeight(30) + 24)));
        }
        summary_.setBounds(r);
    }

    void paint(juce::Graphics& g) override { g.fillAll(colours::panel); }

private:
    void replan()
    {
        session::RegionNotateSettings s;
        s.grid = grid_.getSelectedId() == 1   ? notation::NoteValue::Eighth
                 : grid_.getSelectedId() == 3 ? notation::NoteValue::ThirtySecond
                                              : notation::NoteValue::Sixteenth;
        s.allow_triplets = triplets_.getToggleState();
        s.keep_feel = feel_.getToggleState();
        plan_ = ctx_.session.plan_notate_region(track_, region_, s);
        for (auto& b : choices_) removeChildComponent(b.get());
        choices_.clear();
        juce::String text;
        if (!plan_.ok()) {
            text = juce::String(plan_.error);
        } else {
            if (!plan_.question.empty()) text << juce::String(plan_.question) << "\n\n";
            for (std::size_t k = 0; k < plan_.options.size(); ++k) {
                const auto& o = plan_.options[k];
                text << juce::String(o.label) << ": " << juce::String(o.detail) << "\n";
                auto b = std::make_unique<juce::TextButton>(juce::String(o.label));
                b->onClick = [this, k] { apply(k); };
                if (k == 0) b->setColour(juce::TextButton::buttonColourId, colours::accent.withAlpha(0.6f));
                addAndMakeVisible(*b);
                choices_.push_back(std::move(b));
            }
        }
        summary_.setText(text.trimEnd(), juce::dontSendNotification);
        resized();
    }

    void apply(std::size_t k)
    {
        if (ctx_.session.apply_notate(track_, plan_, k, "Notate region")) {
            if (ctx_.status) ctx_.status(juce::String(session::describe(plan_.options[k])));
        } else if (ctx_.status) {
            ctx_.status("The track changed meanwhile - nothing was notated");
        }
        close();
    }

    void close()
    {
        // A launched dialog is deleted when its modal state ends.
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState(0);
    }

    AppContext& ctx_;
    std::string track_, region_;
    session::NotatePlan plan_;
    juce::Label grid_label_, summary_;
    juce::ComboBox grid_;
    juce::ToggleButton triplets_, feel_;
    std::vector<std::unique_ptr<juce::TextButton>> choices_;
    juce::TextButton cancel_;
};

class PianoRoll final : public juce::Component, private juce::ScrollBar::Listener {
public:
    explicit PianoRoll(Model& m) : m_(m)
    {
        setName("vcc-piano-roll");
        setWantsKeyboardFocus(true);
        addAndMakeVisible(vscroll_);
        style_scrollbar(vscroll_);
        vscroll_.addListener(this);
        addAndMakeVisible(snap_);
        snap_.addItem("Grid 1/4", 1);
        snap_.addItem("Grid 1/8", 2);
        snap_.addItem("Grid 1/16", 3);
        snap_.addItem("Grid 1/8T", 4);
        snap_.setSelectedId(3, juce::dontSendNotification);
        snap_.setTooltip("Grid and snap for creating, moving and resizing notes");
        snap_.onChange = [this] { repaint(); };
    }

    Ticks grid() const
    {
        switch (snap_.getSelectedId()) {
            case 1: return kTicksPerQuarter;
            case 2: return kTicksPerQuarter / 2;
            case 4: return kTicksPerQuarter / 3;
            default: return kTicksPerQuarter / 4;
        }
    }

    /// Called after the document or the selection changed.
    void document_changed()
    {
        const auto* l = m_.selected_lane();
        const std::string id = l ? l->id : std::string{};
        if (id != track_) {
            track_ = id;
            selection_.clear();
            perf_sel_.clear();
            drag_ = Drag::None;
            centre_on_notes();
        }
        // Drop selected notes that no longer exist.
        if (l) {
            std::vector<NoteKey> kept;
            for (const auto& k : selection_)
                if (region_note(*l, k)) kept.push_back(k);
            selection_ = std::move(kept);
            std::vector<notation::EventId> kept_ids;
            for (auto sid : perf_sel_)
                if (score_index(*l, sid) >= 0) kept_ids.push_back(sid);
            perf_sel_ = std::move(kept_ids);
        }
        repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto header = r.removeFromTop(kRollHeader);
        snap_.setBounds(header.withTrimmedLeft(kHeader).removeFromLeft(110).reduced(0, 4));
        vscroll_.setBounds(getWidth() - kScroll, kRollHeader, kScroll, grid_area().getHeight());
        update_scroll();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(colours::background);
        const auto* l = m_.selected_lane();
        paint_header(g, l);
        const auto grid = grid_area();
        const auto keys = juce::Rectangle<int>(0, grid.getY(), kHeader, grid.getHeight());
        const auto vel = velocity_area();

        if (!l || l->reference || l->audio) {
            g.setColour(colours::panel);
            g.fillRect(getLocalBounds().withTrimmedTop(kRollHeader));
            g.setColour(colours::text_dim);
            g.setFont(juce::FontOptions(14.0f));
            g.drawText(!l           ? "Select an instrument track to edit its MIDI"
                       : l->audio   ? "Audio tracks hold no MIDI - edit clips in the lanes above (drag, trim, fade; right-click for more)"
                                    : "Temp / reference tracks hold no MIDI - select an instrument track",
                       getLocalBounds().withTrimmedTop(kRollHeader), juce::Justification::centred);
            return;
        }

        paint_rows(g, grid);
        paint_grid(g, m_, grid, grid_step(), false);
        paint_regions(g, *l, grid);
        paint_notes(g, *l, grid);
        paint_playhead(g, m_, grid);
        paint_keyboard(g, keys);
        paint_velocity(g, *l, vel);
        g.setColour(colours::outline);
        g.drawVerticalLine(kHeader - 1, static_cast<float>(grid.getY()), static_cast<float>(vel.getBottom()));
    }

    // --- Mouse ------------------------------------------------------------------------------------------

    void mouseMove(const juce::MouseEvent& e) override
    {
        const auto* l = m_.selected_lane();
        juce::String hint;
        auto shape = juce::MouseCursor::NormalCursor;
        if (l && !l->reference && !l->audio && grid_area().contains(e.getPosition())) {
            bool edge = false;
            if (hit_region_note(*l, e.position, edge)) {
                shape = edge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::DraggingHandCursor;
            } else if (auto sid = hit_score_note(*l, e.position, edge)) {
                shape = edge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::DraggingHandCursor;
                hint = performance_hint(*l, *sid);
            }
        }
        setMouseCursor(shape);
        if (hint != hint_) {
            hint_ = hint;
            repaint(0, 0, getWidth(), kRollHeader);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        const auto* l = m_.selected_lane();
        drag_ = Drag::None;
        if (!l || l->reference || l->audio) return;
        const auto pos = e.position;

        if (keyboard_area().contains(e.getPosition())) {
            audition(*l, pitch_at(pos.y));
            return;
        }

        if (velocity_area().contains(e.getPosition())) {
            if (auto sid = score_velocity_hit(*l, pos.x); sid && !velocity_hit(*l, pos.x)) {
                if (!is_perf_selected(*sid)) perf_sel_ = {*sid};
                selection_.clear();
                perf_anchor_ = *sid;
                vel_value_ = velocity_at(pos.y);
                drag_ = Drag::PerfVelocity;
                repaint();
                return;
            }
            if (auto key = velocity_hit(*l, pos.x)) {
                perf_sel_.clear();
                const bool sel = is_selected(*key);
                vel_targets_ = sel ? selection_ : std::vector<NoteKey>{*key};
                if (!sel) selection_ = {*key};
                vel_value_ = velocity_at(pos.y);
                drag_ = Drag::Velocity;
                repaint();
            }
            return;
        }

        if (!grid_area().contains(e.getPosition())) return;
        bool edge = false;
        if (e.mods.isPopupMenu()) {
            if (auto hit = hit_region_note(*l, pos, edge)) {
                const NoteKey key{hit->first, hit->second.start, hit->second.pitch};
                perf_sel_.clear();
                if (!is_selected(key)) selection_ = {key};
                repaint();
                show_region_menu(hit->first);
            } else if (auto sid = hit_score_note(*l, pos, edge)) {
                if (!is_perf_selected(*sid)) perf_sel_ = {*sid};
                selection_.clear();
                repaint();
                show_performance_menu();
            }
            return;
        }
        if (auto hit = hit_region_note(*l, pos, edge)) {
            perf_sel_.clear();
            const NoteKey key{hit->first, hit->second.start, hit->second.pitch};
            if (e.mods.isShiftDown()) {
                if (is_selected(key)) {
                    selection_.erase(std::find(selection_.begin(), selection_.end(), key));
                    repaint();
                    return;
                }
                selection_.push_back(key);
            } else if (!is_selected(key)) {
                selection_ = {key};
            }
            anchor_ = hit->second;
            down_tick_ = m_.tick_of(pos.x);
            down_pitch_ = pitch_at(pos.y);
            dt_ = 0;
            dp_ = 0;
            drag_ = edge ? Drag::Resize : Drag::Move;
            audition(*l, anchor_.pitch);
            repaint();
            return;
        }
        if (auto sid = hit_score_note(*l, pos, edge)) {
            selection_.clear();
            if (e.mods.isShiftDown()) {
                if (is_perf_selected(*sid)) {
                    perf_sel_.erase(std::find(perf_sel_.begin(), perf_sel_.end(), *sid));
                    repaint();
                    return;
                }
                perf_sel_.push_back(*sid);
            } else if (!is_perf_selected(*sid)) {
                perf_sel_ = {*sid};
            }
            perf_anchor_ = *sid;
            down_tick_ = m_.tick_of(pos.x);
            down_pitch_ = pitch_at(pos.y);
            dt_ = 0;
            dp_ = 0;
            free_ = false;
            drag_ = edge ? Drag::PerfLength : Drag::PerfMove;
            hint_ = performance_hint(*l, *sid);
            status(hint_);
            audition(*l, l->score_notes[static_cast<std::size_t>(score_index(*l, *sid))].pitch);
            repaint();
            return;
        }
        if (!e.mods.isShiftDown()) {
            selection_.clear();
            perf_sel_.clear();
        }
        create_pitch_ = pitch_at(pos.y);
        create_start_ = m_.snap(static_cast<Ticks>(m_.tick_of(pos.x)), grid(), true);
        create_end_ = create_start_ + grid();
        drag_ = Drag::Create;
        audition(*l, create_pitch_);
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        const auto pos = e.position;
        switch (drag_) {
            case Drag::Create: {
                const Ticks t = m_.snap(static_cast<Ticks>(m_.tick_of(pos.x)), grid(), false);
                create_end_ = std::max(create_start_ + grid(), t);
                break;
            }
            case Drag::Move: {
                const double raw = m_.tick_of(pos.x) - down_tick_;
                const Ticks target = m_.snap(std::max<Ticks>(0, anchor_.start + static_cast<Ticks>(raw)), grid(), false);
                dt_ = target - anchor_.start;
                dp_ = pitch_at(pos.y) - down_pitch_;
                clamp_move();
                break;
            }
            case Drag::Resize: {
                const double raw = m_.tick_of(pos.x) - down_tick_;
                const Ticks end = m_.snap(std::max<Ticks>(0, anchor_.end() + static_cast<Ticks>(raw)), grid(), false);
                dt_ = std::max<Ticks>(anchor_.start + grid(), end) - anchor_.end();
                break;
            }
            case Drag::Velocity:
            case Drag::PerfVelocity: vel_value_ = velocity_at(pos.y); break;
            case Drag::PerfMove:
            case Drag::PerfLength: {
                // Snapped, the written note moves on the grid and the score
                // follows. Alt: free, the small departures a grid cannot
                // express, kept as a performance.
                const auto* l = m_.selected_lane();
                const Ticks raw = static_cast<Ticks>(std::llround(m_.tick_of(pos.x) - down_tick_));
                free_ = e.mods.isAltDown();
                Ticks d = raw;
                if (!free_ && l) {
                    const auto span = l->score_span.count(perf_anchor_) ? l->score_span.at(perf_anchor_)
                                                                         : std::pair<Ticks, Ticks>{0, 0};
                    if (drag_ == Drag::PerfMove) {
                        d = m_.snap(std::max<Ticks>(0, span.first + raw), grid(), false) - span.first;
                    } else {
                        const Ticks end = m_.snap(std::max<Ticks>(0, span.second + raw), grid(), false);
                        d = std::max(span.first + grid(), end) - span.second;
                    }
                }
                dt_ = d;
                dp_ = drag_ == Drag::PerfMove ? juce::jlimit(-24, 24, pitch_at(pos.y) - down_pitch_) : 0;
                if (l) hint_ = drag_hint(*l);
                break;
            }
            case Drag::None: return;
        }
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        const Drag d = drag_;
        drag_ = Drag::None;
        switch (d) {
            case Drag::Create: commit_create(); break;
            case Drag::Move:
                if (dt_ != 0 || dp_ != 0) commit_move();
                break;
            case Drag::Resize:
                if (dt_ != 0) commit_resize();
                break;
            case Drag::Velocity: commit_velocity(); break;
            case Drag::PerfMove:
                if (dt_ != 0 || dp_ != 0) commit_score_edit(Drag::PerfMove);
                break;
            case Drag::PerfLength:
                if (dt_ != 0) commit_score_edit(Drag::PerfLength);
                break;
            case Drag::PerfVelocity: commit_performance(Drag::PerfVelocity); break;
            case Drag::None: break;
        }
        repaint();
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCommandDown() || e.mods.isCtrlDown()) {
            m_.zoom_about(w.deltaY > 0 ? 1.2 : 1.0 / 1.2, std::max(e.x, kHeader));
            return;
        }
        if (e.mods.isShiftDown() || std::abs(w.deltaX) > std::abs(w.deltaY)) {
            const float d = std::abs(w.deltaX) > std::abs(w.deltaY) ? w.deltaX : w.deltaY;
            m_.set_view_start(m_.view_start - d * m_.visible_ticks() * 0.25);
            return;
        }
        scroll_y_ -= juce::roundToInt(w.deltaY * 200.0f);
        update_scroll();
        repaint();
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) {
            if (selection_.empty() && !perf_sel_.empty()) {
                status("Score notes are deleted in the Composer view - right-click to reset how they are played");
                return true;
            }
            if (selection_.empty()) return false;
            commit_delete();
            return true;
        }
        if (key == juce::KeyPress::escapeKey) {
            selection_.clear();
            perf_sel_.clear();
            repaint();
            return true;
        }
        if (key.getModifiers().isCommandDown() && (key.getKeyCode() == 'A' || key.getKeyCode() == 'a')) {
            selection_.clear();
            if (const auto* l = m_.selected_lane())
                for (const auto& r : l->regions)
                    for (const auto& n : r.notes) selection_.push_back(NoteKey{r.id, n.start, n.pitch});
            repaint();
            return true;
        }
        return false;
    }

private:
    enum class Drag { None, Create, Move, Resize, Velocity, PerfMove, PerfLength, PerfVelocity };

    juce::Rectangle<int> grid_area() const
    {
        return {kHeader, kRollHeader, std::max(0, getWidth() - kHeader - kScroll),
                std::max(0, getHeight() - kRollHeader - kVelocity)};
    }
    juce::Rectangle<int> keyboard_area() const
    {
        const auto g = grid_area();
        return {0, g.getY(), kHeader, g.getHeight()};
    }
    juce::Rectangle<int> velocity_area() const
    {
        return {kHeader, getHeight() - kVelocity, std::max(0, getWidth() - kHeader - kScroll), kVelocity};
    }

    Ticks grid_step() const { return grid(); }

    float y_of(int pitch) const
    {
        return static_cast<float>(kRollHeader + (127 - pitch) * kKeyHeight - scroll_y_);
    }
    int pitch_at(float y) const
    {
        const int row = static_cast<int>(std::floor((y - kRollHeader + scroll_y_) / kKeyHeight));
        return juce::jlimit(0, 127, 127 - row);
    }
    int velocity_at(float y) const
    {
        const auto v = velocity_area();
        const float frac = (static_cast<float>(v.getBottom()) - 4.0f - y) / static_cast<float>(v.getHeight() - 10);
        return juce::jlimit(1, 127, juce::roundToInt(frac * 127.0f));
    }

    void update_scroll()
    {
        const int content = 128 * kKeyHeight;
        const int visible = grid_area().getHeight();
        scroll_y_ = juce::jlimit(0, std::max(0, content - visible), scroll_y_);
        vscroll_.setRangeLimits(0.0, content);
        vscroll_.setCurrentRange(scroll_y_, visible, juce::dontSendNotification);
    }

    void scrollBarMoved(juce::ScrollBar*, double start) override
    {
        scroll_y_ = juce::roundToInt(start);
        repaint();
    }

    void centre_on_notes()
    {
        const auto* l = m_.selected_lane();
        int centre = 60;
        if (l) {
            long sum = 0, count = 0;
            for (const auto& n : l->score_notes) sum += n.pitch, ++count;
            for (const auto& r : l->regions)
                for (const auto& n : r.notes) sum += n.pitch, ++count;
            centre = count > 0 ? static_cast<int>(sum / count) : (l->range_lo + l->range_hi) / 2;
        }
        scroll_y_ = (127 - centre) * kKeyHeight - std::max(0, grid_area().getHeight()) / 2;
        update_scroll();
    }

    juce::Rectangle<float> note_rect(const NoteEvent& n) const
    {
        const float x0 = static_cast<float>(m_.x_of(static_cast<double>(n.start)));
        const float x1 = static_cast<float>(m_.x_of(static_cast<double>(n.end())));
        return {x0, y_of(n.pitch) + 1.0f, std::max(3.0f, x1 - x0 - 1.0f), static_cast<float>(kKeyHeight) - 1.0f};
    }

    bool is_selected(const NoteKey& k) const
    {
        return std::find(selection_.begin(), selection_.end(), k) != selection_.end();
    }

    static const NoteEvent* region_note(const LaneView& l, const NoteKey& k)
    {
        for (const auto& r : l.regions) {
            if (r.id != k.region) continue;
            for (const auto& n : r.notes)
                if (n.start == k.start && n.pitch == k.pitch) return &n;
        }
        return nullptr;
    }

    std::optional<std::pair<std::string, NoteEvent>> hit_region_note(const LaneView& l, juce::Point<float> p, bool& edge) const
    {
        for (auto r = l.regions.rbegin(); r != l.regions.rend(); ++r)
            for (auto n = r->notes.rbegin(); n != r->notes.rend(); ++n) {
                const auto rect = note_rect(*n);
                if (!rect.expanded(0.0f, 0.5f).contains(p)) continue;
                edge = p.x > rect.getRight() - std::min(7.0f, rect.getWidth() / 3.0f);
                return std::make_pair(r->id, *n);
            }
        return std::nullopt;
    }

    /// The score note under `p` (as performed), and whether `p` is on its end.
    std::optional<notation::EventId> hit_score_note(const LaneView& l, juce::Point<float> p, bool& edge) const
    {
        for (std::size_t i = l.score_notes.size(); i-- > 0;) {
            if (i >= l.score_ids.size() || l.score_ids[i] == notation::kInvalidEventId) continue;
            const auto rect = note_rect(l.score_notes[i]);
            if (!rect.expanded(0.0f, 0.5f).contains(p)) continue;
            edge = p.x > rect.getRight() - std::min(7.0f, rect.getWidth() / 3.0f);
            return l.score_ids[i];
        }
        return std::nullopt;
    }

    static int score_index(const LaneView& l, notation::EventId id)
    {
        for (std::size_t i = 0; i < l.score_ids.size(); ++i)
            if (l.score_ids[i] == id) return static_cast<int>(i);
        return -1;
    }

    bool is_perf_selected(notation::EventId id) const
    {
        return std::find(perf_sel_.begin(), perf_sel_.end(), id) != perf_sel_.end();
    }

    std::optional<notation::EventId> score_velocity_hit(const LaneView& l, float x) const
    {
        std::optional<notation::EventId> best;
        float best_d = 6.0f;
        for (std::size_t i = 0; i < l.score_notes.size() && i < l.score_ids.size(); ++i) {
            if (l.score_ids[i] == notation::kInvalidEventId) continue;
            float d = std::abs(static_cast<float>(m_.x_of(static_cast<double>(l.score_notes[i].start))) - x);
            if (is_perf_selected(l.score_ids[i])) d -= 0.5f;
            if (d < best_d) {
                best_d = d;
                best = l.score_ids[i];
            }
        }
        return best;
    }

    /// "+12 ms late, held 40 ms longer, velocity +6" for one score note.
    juce::String describe_override(const project::NoteOverride& o) const
    {
        auto ms = [&](Ticks t) {
            return juce::roundToInt(1000.0 * 60.0 / std::max(1.0, m_.bpm) * static_cast<double>(std::llabs(t)) /
                                    static_cast<double>(kTicksPerQuarter));
        };
        juce::StringArray parts;
        if (o.onset != 0) parts.add(juce::String(ms(o.onset)) + " ms " + (o.onset < 0 ? "early" : "late"));
        if (o.length != 0) parts.add(juce::String(ms(o.length)) + " ms " + (o.length < 0 ? "shorter" : "longer"));
        if (o.velocity != 0) parts.add("velocity " + juce::String(o.velocity > 0 ? "+" : "") + juce::String(o.velocity));
        if (o.cc != 0) parts.add("CC " + juce::String(o.cc > 0 ? "+" : "") + juce::String(o.cc));
        return parts.isEmpty() ? juce::String("as written") : parts.joinIntoString(", ");
    }

    juce::String performance_hint(const LaneView& l, notation::EventId id) const
    {
        const auto it = l.performance.find(id);
        if (it == l.performance.end() || it->second.empty()) return kScoreHint;
        return "Played " + describe_override(it->second) + "  -  right-click to reset or humanize";
    }

    juce::String drag_hint(const LaneView& l) const
    {
        project::NoteOverride o;
        if (auto it = l.performance.find(perf_anchor_); it != l.performance.end()) o = it->second;
        if (!free_) {
            juce::String what = drag_ == Drag::PerfLength ? "Rewrites its length" : "Rewrites the note";
            if (dp_ != 0) what << " (" << (dp_ > 0 ? "+" : "") << dp_ << " semitones)";
            return what + " in the score  -  Alt-drag to change only how it is played";
        }
        if (drag_ == Drag::PerfMove) o.onset += dt_;
        if (drag_ == Drag::PerfLength) o.length += dt_;
        return "Played " + describe_override(project::clamp_override(o)) + "  (performance only)";
    }

    std::optional<NoteKey> velocity_hit(const LaneView& l, float x) const
    {
        std::optional<NoteKey> best;
        float best_d = 6.0f;
        for (const auto& r : l.regions)
            for (const auto& n : r.notes) {
                const float nx = static_cast<float>(m_.x_of(static_cast<double>(n.start)));
                const NoteKey k{r.id, n.start, n.pitch};
                float d = std::abs(nx - x);
                if (is_selected(k)) d -= 0.5f;  // prefer selected notes on a shared start
                if (d < best_d) {
                    best_d = d;
                    best = k;
                }
            }
        return best;
    }

    void clamp_move()
    {
        const auto* l = m_.selected_lane();
        if (!l) return;
        Ticks min_start = anchor_.start;
        int lo = 127, hi = 0;
        for (const auto& k : selection_)
            if (const auto* n = region_note(*l, k)) {
                min_start = std::min(min_start, n->start);
                lo = std::min(lo, n->pitch);
                hi = std::max(hi, n->pitch);
            }
        if (min_start + dt_ < 0) dt_ = -min_start;
        dp_ = juce::jlimit(-lo, 127 - hi, dp_);
    }

    void audition(const LaneView& l, int pitch)
    {
        if (m_.ctx.engine.playing()) return;
        NoteEvent n;
        n.start = 0;
        n.duration = kTicksPerQuarter / 2;
        n.pitch = pitch;
        n.velocity = 90;
        n.channel = l.channel;
        m_.ctx.engine.preview({n}, l.family.toStdString(), l.gm_program, m_.bpm, false);
    }

    void status(const juce::String& text) const
    {
        if (m_.ctx.status) m_.ctx.status(text);
    }

    // --- Edits: one session.edit per gesture -------------------------------------------------------

    void commit_create()
    {
        const auto* l = m_.selected_lane();
        if (!l) return;
        const std::string track_id = l->id;
        NoteEvent note;
        note.start = create_start_;
        note.duration = create_end_ - create_start_;
        note.pitch = create_pitch_;
        note.velocity = last_velocity_;
        note.channel = l->channel;
        std::string region_id;
        bool clipped = false;
        const bool ok = m_.ctx.session.edit("Add note", [&](project::ProjectData& d) {
            auto* t = d.find_track(track_id);
            if (!t) return false;
            auto* v = t->active_version();
            if (!v) return false;
            note.channel = t->midi_channel;
            project::MidiRegion* target = nullptr;
            for (auto& r : v->regions)
                if (r.start <= note.start && note.start < r.end && (!target || r.lane < target->lane)) target = &r;
            if (!target) {
                std::set<std::string> used;
                for (const auto& tr : d.tracks)
                    for (const auto& ver : tr.versions)
                        for (const auto& r : ver.regions) used.insert(r.id);
                std::string id;
                do {
                    id = "rgn-pr-" + juce::Uuid().toString().substring(0, 12).toStdString();
                } while (used.count(id) != 0);
                project::MidiRegion r;
                r.id = id;
                r.start = d.meter.bar_start(d.meter.bar_at(note.start));
                r.end = d.meter.bar_start(d.meter.bar_at(note.end() - 1) + 1);
                r.source = "piano_roll";
                v->regions.push_back(std::move(r));
                target = &v->regions.back();
            }
            target->notes.push_back(note);
            clipped = normalise_region(*target, d.meter);
            region_id = target->id;
            return true;
        });
        if (ok) {
            selection_ = {NoteKey{region_id, note.start, note.pitch}};
            status(clipped ? juce::String("Note added - an overlapping note of the same pitch was shortened")
                           : "Added " + note_name(note.pitch));
        }
    }

    void commit_move()
    {
        const auto* l = m_.selected_lane();
        if (!l) return;
        const std::string track_id = l->id;
        const Ticks dt = dt_;
        const int dp = dp_;
        auto keys = selection_;
        bool clipped = false;
        const bool ok = m_.ctx.session.edit(keys.size() == 1 ? "Move note" : "Move notes", [&](project::ProjectData& d) {
            auto* t = d.find_track(track_id);
            auto* v = t ? t->active_version() : nullptr;
            if (!v) return false;
            // Remove every moved note first, then add them back moved, so notes in
            // the selection never collide with their own old positions.
            std::map<std::string, std::vector<NoteEvent>> moved;
            for (const auto& k : keys) {
                auto* r = find_region(*v, k.region);
                if (!r) continue;
                const int i = find_note(*r, k.start, k.pitch);
                if (i < 0) continue;
                NoteEvent n = r->notes[static_cast<std::size_t>(i)];
                r->notes.erase(r->notes.begin() + i);
                n.start = std::max<Ticks>(0, n.start + dt);
                n.pitch = juce::jlimit(0, 127, n.pitch + dp);
                moved[k.region].push_back(n);
            }
            if (moved.empty()) return false;
            for (auto& [id, notes] : moved) {
                auto* r = find_region(*v, id);
                for (auto& n : notes) r->notes.push_back(n);
                clipped = normalise_region(*r, d.meter) || clipped;
            }
            return true;
        });
        if (ok) {
            for (auto& k : selection_) {
                k.start = std::max<Ticks>(0, k.start + dt);
                k.pitch = juce::jlimit(0, 127, k.pitch + dp);
            }
            if (clipped) status("Moved - overlapping notes of the same pitch were shortened");
        }
    }

    void commit_resize()
    {
        const auto* l = m_.selected_lane();
        if (!l) return;
        const std::string track_id = l->id;
        const Ticks dt = dt_;
        const Ticks min_len = grid() / 2;
        auto keys = selection_;
        bool clipped = false;
        m_.ctx.session.edit(keys.size() == 1 ? "Resize note" : "Resize notes", [&](project::ProjectData& d) {
            auto* t = d.find_track(track_id);
            auto* v = t ? t->active_version() : nullptr;
            if (!v) return false;
            std::set<std::string> touched;
            for (const auto& k : keys) {
                auto* r = find_region(*v, k.region);
                if (!r) continue;
                const int i = find_note(*r, k.start, k.pitch);
                if (i < 0) continue;
                auto& n = r->notes[static_cast<std::size_t>(i)];
                n.duration = std::max(min_len, n.duration + dt);
                // Move it to the back so it wins any overlap it now makes.
                NoteEvent copy = n;
                r->notes.erase(r->notes.begin() + i);
                r->notes.push_back(copy);
                touched.insert(k.region);
            }
            if (touched.empty()) return false;
            for (const auto& id : touched) clipped = normalise_region(*find_region(*v, id), d.meter) || clipped;
            return true;
        });
        if (clipped) status("Resized - overlapping notes of the same pitch were shortened");
    }

    void commit_velocity()
    {
        const auto* l = m_.selected_lane();
        if (!l || vel_targets_.empty()) return;
        const std::string track_id = l->id;
        const int value = vel_value_;
        auto keys = vel_targets_;
        vel_targets_.clear();
        const bool ok = m_.ctx.session.edit("Set velocity", [&](project::ProjectData& d) {
            auto* t = d.find_track(track_id);
            auto* v = t ? t->active_version() : nullptr;
            if (!v) return false;
            bool any = false;
            for (const auto& k : keys)
                if (auto* r = find_region(*v, k.region)) {
                    const int i = find_note(*r, k.start, k.pitch);
                    if (i >= 0 && r->notes[static_cast<std::size_t>(i)].velocity != value) {
                        r->notes[static_cast<std::size_t>(i)].velocity = value;
                        any = true;
                    }
                }
            return any;
        });
        if (ok) {
            last_velocity_ = value;
            status("Velocity " + juce::String(value));
        }
    }

    void commit_delete()
    {
        const auto* l = m_.selected_lane();
        if (!l) return;
        const std::string track_id = l->id;
        auto keys = selection_;
        const bool ok = m_.ctx.session.edit(keys.size() == 1 ? "Delete note" : "Delete notes", [&](project::ProjectData& d) {
            auto* t = d.find_track(track_id);
            auto* v = t ? t->active_version() : nullptr;
            if (!v) return false;
            bool any = false;
            for (const auto& k : keys)
                if (auto* r = find_region(*v, k.region)) {
                    const int i = find_note(*r, k.start, k.pitch);
                    if (i >= 0) {
                        r->notes.erase(r->notes.begin() + i);
                        any = true;
                    }
                }
            // A region the piano roll made and that is now empty goes away.
            v->regions.erase(std::remove_if(v->regions.begin(), v->regions.end(),
                                            [](const project::MidiRegion& r) {
                                                return r.notes.empty() && r.source == "piano_roll";
                                            }),
                             v->regions.end());
            return any;
        });
        if (ok) {
            status(keys.size() == 1 ? juce::String("Deleted 1 note") : "Deleted " + juce::String(keys.size()) + " notes");
            selection_.clear();
        }
    }

    // --- Performance overrides (Main Spec §8): how score notes are played ---------------------------

    /// Each gesture is its own undo step: same-label edits coalesce, so the
    /// label alternates between two spellings from one gesture to the next.
    std::string gesture_label(const char* base)
    {
        flip_ = !flip_;
        return std::string(base) + (flip_ ? "" : " ");
    }

    void commit_performance(Drag kind)
    {
        const auto* l = m_.selected_lane();
        if (!l || perf_sel_.empty()) return;
        const auto ids = perf_sel_;
        const Ticks dt = dt_;
        bool ok = false;
        if (kind == Drag::PerfVelocity) {
            const int i = score_index(*l, perf_anchor_);
            if (i < 0) return;
            // The whole selection moves by the anchor's change, so the
            // written dynamic shape is kept.
            const int delta = vel_value_ - l->score_notes[static_cast<std::size_t>(i)].velocity;
            if (delta == 0) return;
            ok = m_.ctx.session.edit_performance(l->id, ids, gesture_label("Performance velocity"),
                                                 [delta](notation::EventId, project::NoteOverride& o) { o.velocity += delta; });
        } else {
            const bool onset = kind == Drag::PerfMove;
            ok = m_.ctx.session.edit_performance(l->id, ids, gesture_label(onset ? "Performance timing" : "Performance length"),
                                                 [dt, onset](notation::EventId, project::NoteOverride& o) {
                                                     (onset ? o.onset : o.length) += dt;
                                                 });
        }
        hint_.clear();
        if (ok)
            if (const auto* now = m_.selected_lane()) status(performance_hint(*now, perf_anchor_));
    }

    /// A drag of score notes, carried into the score where it can be written
    /// (Main Spec §8, MIDI -> notation) and kept as a performance where it
    /// cannot. One undo step for the gesture. A single note whose edit can
    /// be read more than one way asks which reading was meant.
    void commit_score_edit(Drag kind)
    {
        const auto* l = m_.selected_lane();
        if (!l || perf_sel_.empty()) return;
        const std::string track = l->id;
        const bool move = kind == Drag::PerfMove;
        const Ticks dt = dt_;
        const int dp = dp_;
        const bool free = free_;
        // Targets from what the roll showed when the drag began.
        std::vector<std::pair<notation::EventId, session::NoteTarget>> targets;
        for (auto sid : perf_sel_) {
            const int i = score_index(*l, sid);
            if (i < 0) continue;
            const auto& n = l->score_notes[static_cast<std::size_t>(i)];
            session::NoteTarget t{n.start, n.duration, juce::jlimit(0, 127, n.pitch + dp)};
            if (move)
                t.start = std::max<Ticks>(0, t.start + dt);
            else
                t.duration = std::max<Ticks>(1, t.duration + dt);
            targets.emplace_back(sid, t);
        }
        // Moving later, the last note first, so a note never lands on one
        // that is about to move out of its way.
        std::sort(targets.begin(), targets.end(), [&](const auto& a, const auto& b) {
            return (move && dt > 0) ? a.second.start > b.second.start : a.second.start < b.second.start;
        });
        const std::string label = gesture_label(move ? "Edit score note" : "Lengthen score note");
        auto& session = m_.ctx.session;
        int written = 0, performed = 0;
        juce::String last;
        for (const auto& [sid, target] : targets) {
            const auto plan = session.plan_note_edit(track, sid, target);
            if (!plan.ok()) continue;
            if (targets.size() == 1 && !free && !plan.ambiguity.empty()) {
                ask_reading(track, plan, label);
                return;
            }
            std::size_t choice = 0;
            if (free)
                for (std::size_t k = 0; k < plan.options.size(); ++k)
                    if (plan.options[k].id == "override") choice = k;
            if (session.apply_notate(track, plan, choice, label, true)) {
                const auto& o = plan.options[choice];
                (o.id == "override" ? performed : written)++;
                last = juce::String(session::describe(o));
            }
        }
        hint_.clear();
        if (written + performed == 1)
            status(last);
        else if (written + performed > 1)
            status(juce::String(written) + " written into the score, " + juce::String(performed) + " kept as a performance");
    }

    /// The reconciliation prompt: an edit that can be read more than one way
    /// (a triplet, an overlap, a spelling) offers its readings.
    void ask_reading(const std::string& track, const session::NotatePlan& plan, const std::string& label)
    {
        auto* w = new juce::AlertWindow("Notate the edit", juce::String(plan.question), juce::MessageBoxIconType::QuestionIcon);
        juce::String details;
        for (std::size_t k = 0; k < plan.options.size(); ++k) {
            const auto& o = plan.options[k];
            details << juce::String(o.label) << (o.detail.empty() ? juce::String() : ": " + juce::String(o.detail)) << "\n";
            w->addButton(juce::String(o.label), static_cast<int>(k) + 1);
        }
        w->addTextBlock(details.trimEnd());
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<PianoRoll> self(this);
        w->enterModalState(true, juce::ModalCallbackFunction::create([self, track, plan, label](int r) {
                               if (!self || r <= 0) return;
                               const auto k = static_cast<std::size_t>(r - 1);
                               if (self->m_.ctx.session.apply_notate(track, plan, k, label))
                                   self->status(juce::String(session::describe(plan.options[k])));
                               else
                                   self->status("The score changed meanwhile - nothing was written");
                           }),
                           true);
    }

    void show_region_menu(const std::string& region)
    {
        juce::PopupMenu menu;
        menu.addSectionHeader("MIDI region");
        juce::Component::SafePointer<PianoRoll> self(this);
        menu.addItem("Notate this region...", [self, region] {
            if (self) self->open_region_notation(region);
        });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
    }

public:
    /// Opens the Notate Region dialog for a region of the selected track.
    void open_region_notation(const std::string& region)
    {
        const auto* l = m_.selected_lane();
        if (!l) return;
        juce::DialogWindow::LaunchOptions o;
        o.dialogTitle = "Notate Region";
        o.content.setOwned(new RegionNotatePanel(m_.ctx, l->id, region));
        o.componentToCentreAround = this;
        o.dialogBackgroundColour = colours::panel;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = false;
        o.resizable = false;
        o.launchAsync();
    }

private:
    void show_performance_menu()
    {
        const auto* l = m_.selected_lane();
        if (!l || perf_sel_.empty()) return;
        bool any = false;
        for (auto id : perf_sel_) any = any || l->performance.count(id) > 0;
        juce::PopupMenu humanize;
        humanize.addItem(11, "Subtle");
        humanize.addItem(12, "Natural");
        humanize.addItem(13, "Loose");
        juce::PopupMenu menu;
        const auto n = static_cast<int>(perf_sel_.size());
        menu.addSectionHeader(n == 1 ? juce::String("Score note: how it is played")
                                     : juce::String(n) + " score notes: how they are played");
        menu.addItem(1, "Performance...");
        menu.addSubMenu("Humanize", humanize);
        menu.addItem(2, n == 1 ? "Reset to as written" : "Reset these to as written", any);
        menu.addSeparator();
        menu.addItem(3, "Select all score notes");
        menu.addItem(4, "Reset the whole track to as written", !l->performance.empty());
        const std::string track = l->id;
        const auto ids = perf_sel_;
        juce::Component::SafePointer<PianoRoll> self(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
                           [self, track, ids](int r) {
                               if (self) self->performance_action(r, track, ids);
                           });
    }

    void performance_action(int r, const std::string& track, const std::vector<notation::EventId>& ids)
    {
        auto& session = m_.ctx.session;
        switch (r) {
            case 1: open_performance_panel(); break;
            case 2:
                if (session.clear_performance(track, ids)) status("Reset to as written");
                break;
            case 3:
                if (const auto* l = m_.selected_lane()) {
                    selection_.clear();
                    perf_sel_.clear();
                    for (auto sid : l->score_ids)
                        if (sid != notation::kInvalidEventId) perf_sel_.push_back(sid);
                    repaint();
                }
                break;
            case 4:
                if (session.clear_performance(track)) status("The whole track plays as written");
                break;
            case 11:
            case 12:
            case 13: {
                auto settings = project::humanize_preset(r == 11 ? "subtle" : r == 12 ? "natural" : "loose");
                settings.seed = ++humanize_seed_;
                if (session.humanize(track, ids, settings))
                    status("Humanized " + juce::String(static_cast<int>(ids.size())) + " note(s) - Undo to try again");
                break;
            }
            default: break;
        }
    }

    void open_performance_panel()
    {
        const auto* l = m_.selected_lane();
        if (!l || perf_sel_.empty()) return;
        juce::Rectangle<int> area;
        for (auto sid : perf_sel_)
            if (const int i = score_index(*l, sid); i >= 0) {
                const auto r = note_rect(l->score_notes[static_cast<std::size_t>(i)]).toNearestInt();
                area = area.isEmpty() ? r : area.getUnion(r);
            }
        if (area.isEmpty()) area = grid_area().withSizeKeepingCentre(10, 10);
        juce::CallOutBox::launchAsynchronously(std::make_unique<PerformancePanel>(m_.ctx, l->id, perf_sel_, m_.bpm),
                                               area.getIntersection(grid_area()), this);
    }

    // --- Painting ---------------------------------------------------------------------------------------

    void paint_header(juce::Graphics& g, const LaneView* l)
    {
        auto r = getLocalBounds().removeFromTop(kRollHeader);
        g.setColour(colours::panel);
        g.fillRect(r);
        g.setColour(colours::outline);
        g.drawHorizontalLine(r.getBottom() - 1, 0.0f, static_cast<float>(getWidth()));
        auto title = r.removeFromLeft(kHeader).reduced(10, 0);
        if (l) {
            g.setColour(l->colour);
            g.fillRect(title.removeFromLeft(4).reduced(0, 8));
            title.removeFromLeft(6);
        }
        g.setColour(colours::text);
        g.setFont(juce::FontOptions(12.5f, juce::Font::bold));
        g.drawText(l ? l->name : juce::String("Piano Roll"), title, juce::Justification::centredLeft, true);

        auto info = r.withTrimmedLeft(snap_.getWidth() + 14).withTrimmedRight(262);
        g.setFont(juce::FontOptions(11.5f));
        if (hint_.isNotEmpty()) {
            g.setColour(colours::accent_warm);
            g.drawText(hint_, info, juce::Justification::centredLeft, true);
        } else if (l && !l->reference) {
            g.setColour(colours::text_dim);
            const auto n = selection_.size();
            juce::String text = n > 0 ? juce::String(static_cast<int>(n)) + " selected  -  drag to move, edge to resize, Delete to remove"
                                : !perf_sel_.empty()
                                    ? juce::String(static_cast<int>(perf_sel_.size())) +
                                          " score note(s)  -  drag to play early or late, end to hold, right-click to humanize or reset"
                                    : juce::String("Click and drag to draw notes  -  drag a score note to shape how it is played");
            g.drawText(text, info, juce::Justification::centredLeft, true);
        }
        // Legend.
        if (l && !l->reference) {
            auto legend = r.removeFromRight(250).reduced(8, 9);
            g.setFont(juce::FontOptions(11.0f));
            auto item = [&](const juce::String& label, bool score) {
                auto box = legend.removeFromLeft(14).toFloat().reduced(0.0f, 1.0f);
                if (score) {
                    g.setColour(l->colour.withAlpha(0.28f));
                    g.fillRoundedRectangle(box, 2.0f);
                    g.setColour(l->colour.withAlpha(0.7f));
                    g.drawRoundedRectangle(box, 2.0f, 1.0f);
                } else {
                    g.setColour(l->colour.brighter(0.5f));
                    g.fillRoundedRectangle(box, 2.0f);
                }
                legend.removeFromLeft(5);
                g.setColour(colours::text_dim);
                g.drawText(label, legend.removeFromLeft(100), juce::Justification::centredLeft);
            };
            item("MIDI", false);
            item("Score (performance)", true);
        }
    }

    void paint_rows(juce::Graphics& g, juce::Rectangle<int> grid)
    {
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(grid);
        g.setColour(colours::panel.darker(0.05f));
        g.fillRect(grid);
        const auto* l = m_.selected_lane();
        for (int p = 0; p < 128; ++p) {
            const float y = y_of(p);
            if (y + kKeyHeight < grid.getY() || y > grid.getBottom()) continue;
            const bool out_of_range = l && (p < l->range_lo || p > l->range_hi);
            if (is_black(p) || out_of_range) {
                g.setColour(out_of_range ? juce::Colours::black.withAlpha(0.28f) : colours::background);
                g.fillRect(juce::Rectangle<float>(static_cast<float>(grid.getX()), y, static_cast<float>(grid.getWidth()),
                                                  static_cast<float>(kKeyHeight)));
            }
            if (p % 12 == 0) {
                g.setColour(colours::outline.withAlpha(0.9f));
                g.drawHorizontalLine(juce::roundToInt(y + kKeyHeight) - 1, static_cast<float>(grid.getX()),
                                     static_cast<float>(grid.getRight()));
            } else if (p % 12 == 5) {
                g.setColour(colours::outline.withAlpha(0.35f));
                g.drawHorizontalLine(juce::roundToInt(y + kKeyHeight) - 1, static_cast<float>(grid.getX()),
                                     static_cast<float>(grid.getRight()));
            }
        }
    }

    void paint_regions(juce::Graphics& g, const LaneView& l, juce::Rectangle<int> grid)
    {
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(grid);
        for (const auto& r : l.regions) {
            const float x0 = static_cast<float>(m_.x_of(static_cast<double>(r.start)));
            const float x1 = static_cast<float>(m_.x_of(static_cast<double>(r.end)));
            if (x1 < grid.getX() || x0 > grid.getRight()) continue;
            const auto area = juce::Rectangle<float>(x0, static_cast<float>(grid.getY()), x1 - x0,
                                                     static_cast<float>(grid.getHeight()));
            g.setColour(l.colour.withAlpha(0.05f));
            g.fillRect(area);
            g.setColour(l.colour.withAlpha(0.45f));
            g.drawVerticalLine(juce::roundToInt(x0), area.getY(), area.getBottom());
            g.drawVerticalLine(juce::roundToInt(x1) - 1, area.getY(), area.getBottom());
            auto label = area.withHeight(14.0f);
            g.setColour(l.colour.withAlpha(0.3f));
            g.fillRect(label);
            g.setColour(colours::text);
            g.setFont(juce::FontOptions(10.0f));
            g.drawText(short_source(r.source), label.reduced(4.0f, 0.0f), juce::Justification::centredLeft, true);
        }
    }

    void paint_notes(juce::Graphics& g, const LaneView& l, juce::Rectangle<int> grid)
    {
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(grid);
        const auto gf = grid.toFloat();

        // Score notes, as performed. Their pitch and rhythm belong to the
        // score; dragging them changes how they are played. A note played
        // away from how it is written keeps a dashed outline at the written
        // place and a line joining the two.
        for (std::size_t i = 0; i < l.score_notes.size(); ++i) {
            const notation::EventId sid = i < l.score_ids.size() ? l.score_ids[i] : notation::kInvalidEventId;
            const bool sel = sid != notation::kInvalidEventId && is_perf_selected(sid);
            NoteEvent shown = l.score_notes[i];
            if (sel && drag_ == Drag::PerfMove) {
                shown.start = std::max<Ticks>(0, shown.start + dt_);
                shown.pitch = juce::jlimit(0, 127, shown.pitch + dp_);
            }
            if (sel && drag_ == Drag::PerfLength) shown.duration = std::max<Ticks>(1, shown.duration + dt_);
            const auto r = note_rect(shown);
            const auto w = l.score_written.find(sid);
            const bool moved = w != l.score_written.end() && (w->second.start != shown.start || w->second.duration != shown.duration);
            if (moved) {
                const auto wr = note_rect(w->second);
                if (wr.intersects(gf) || r.intersects(gf)) {
                    juce::Path outline;
                    outline.addRoundedRectangle(wr.reduced(0.5f), 2.0f);
                    juce::Path dashed;
                    const float dashes[] = {3.0f, 2.5f};
                    juce::PathStrokeType(1.0f).createDashedStroke(dashed, outline, dashes, 2);
                    g.setColour(colours::text_dim.withAlpha(0.7f));
                    g.fillPath(dashed);
                    g.drawLine(wr.getX(), wr.getCentreY(), r.getX(), r.getCentreY(), 1.0f);
                }
            }
            if (!r.intersects(gf)) continue;
            const bool performed = sid != notation::kInvalidEventId && l.performance.count(sid) > 0;
            g.setColour(sel ? colours::accent_warm.withAlpha(0.55f) : l.colour.withAlpha(performed ? 0.38f : 0.22f));
            g.fillRoundedRectangle(r, 2.0f);
            g.setColour(sel ? juce::Colours::white : l.colour.withAlpha(0.75f));
            g.drawRoundedRectangle(r.reduced(0.5f), 2.0f, sel ? 1.4f : 1.0f);
            if (performed && r.getWidth() > 8.0f) {
                // A small mark: this note has a performance of its own.
                g.setColour(colours::accent_warm);
                g.fillEllipse(r.getRight() - 6.0f, r.getY() + 2.0f, 4.0f, 4.0f);
            }
        }

        // MIDI-region notes.
        const auto base = l.colour.brighter(0.35f);
        for (const auto& reg : l.regions)
            for (const auto& n : reg.notes) {
                const NoteKey k{reg.id, n.start, n.pitch};
                const bool sel = is_selected(k);
                NoteEvent shown = n;
                if (sel && drag_ == Drag::Move) {
                    // Original position stays as a faint outline while dragging.
                    g.setColour(colours::text.withAlpha(0.25f));
                    g.drawRoundedRectangle(note_rect(n), 2.0f, 1.0f);
                    shown.start = std::max<Ticks>(0, n.start + dt_);
                    shown.pitch = juce::jlimit(0, 127, n.pitch + dp_);
                } else if (sel && drag_ == Drag::Resize) {
                    shown.duration = std::max(grid_step() / 2, n.duration + dt_);
                } else if (drag_ == Drag::Velocity &&
                           std::find(vel_targets_.begin(), vel_targets_.end(), k) != vel_targets_.end()) {
                    shown.velocity = vel_value_;
                }
                const auto r = note_rect(shown);
                if (!r.intersects(gf)) continue;
                const float shade = 0.55f + 0.45f * static_cast<float>(shown.velocity) / 127.0f;
                g.setColour(sel ? colours::accent_warm : base.withMultipliedBrightness(shade));
                g.fillRoundedRectangle(r, 2.0f);
                g.setColour(sel ? juce::Colours::white : juce::Colours::black.withAlpha(0.55f));
                g.drawRoundedRectangle(r.reduced(0.5f), 2.0f, sel ? 1.4f : 1.0f);
                if (r.getWidth() > 30.0f) {
                    g.setColour(juce::Colours::black.withAlpha(0.7f));
                    g.setFont(juce::FontOptions(9.0f));
                    g.drawText(note_name(shown.pitch), r.reduced(3.0f, 0.0f), juce::Justification::centredLeft, false);
                }
            }

        if (drag_ == Drag::Create) {
            NoteEvent n;
            n.start = create_start_;
            n.duration = create_end_ - create_start_;
            n.pitch = create_pitch_;
            const auto r = note_rect(n);
            g.setColour(colours::accent_warm.withAlpha(0.75f));
            g.fillRoundedRectangle(r, 2.0f);
            g.setColour(juce::Colours::white);
            g.drawRoundedRectangle(r.reduced(0.5f), 2.0f, 1.2f);
        }
    }

    void paint_keyboard(juce::Graphics& g, juce::Rectangle<int> keys)
    {
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(keys);
        g.setColour(colours::panel);
        g.fillRect(keys);
        const int piano_x = kHeader - 62;
        const auto* l = m_.selected_lane();
        for (int p = 0; p < 128; ++p) {
            const float y = y_of(p);
            if (y + kKeyHeight < keys.getY() || y > keys.getBottom()) continue;
            const float kh = static_cast<float>(kKeyHeight);
            if (!is_black(p)) {
                g.setColour(juce::Colour(0xffd9dce1));
                g.fillRect(juce::Rectangle<float>(static_cast<float>(piano_x), y, 61.0f, kh));
                g.setColour(juce::Colour(0xff9aa0a8));
                g.drawHorizontalLine(juce::roundToInt(y + kh) - 1, static_cast<float>(piano_x), static_cast<float>(kHeader - 1));
            } else {
                g.setColour(juce::Colour(0xffd9dce1));
                g.fillRect(juce::Rectangle<float>(static_cast<float>(piano_x), y, 61.0f, kh));
                g.setColour(juce::Colour(0xff1a1c20));
                g.fillRect(juce::Rectangle<float>(static_cast<float>(piano_x), y + 1.0f, 38.0f, kh - 2.0f));
            }
            if (l && (p < l->range_lo || p > l->range_hi)) {
                g.setColour(juce::Colours::black.withAlpha(0.35f));
                g.fillRect(juce::Rectangle<float>(static_cast<float>(piano_x), y, 61.0f, kh));
            }
            if (p % 12 == 0) {
                g.setColour(colours::text);
                g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
                g.drawText(note_name(p), juce::Rectangle<float>(8.0f, y, static_cast<float>(piano_x - 14), kh),
                           juce::Justification::centredRight);
                g.setColour(colours::outline);
                g.drawHorizontalLine(juce::roundToInt(y + kh) - 1, 0.0f, static_cast<float>(piano_x));
                g.setColour(juce::Colour(0xff6a7079));
                g.setFont(juce::FontOptions(8.5f));
                g.drawText("C", juce::Rectangle<float>(static_cast<float>(kHeader - 16), y, 12.0f, kh),
                           juce::Justification::centred);
            }
        }
        if (l) {
            g.setColour(colours::text_dim.withAlpha(0.6f));
            g.setFont(juce::FontOptions(9.5f));
            const float top = y_of(l->range_hi);
            if (top > keys.getY() && top < keys.getBottom())
                g.drawText("range", juce::Rectangle<float>(8.0f, top - 12.0f, static_cast<float>(piano_x - 14), 11.0f),
                           juce::Justification::centredRight);
        }
    }

    void paint_velocity(juce::Graphics& g, const LaneView& l, juce::Rectangle<int> vel)
    {
        g.setColour(colours::panel);
        g.fillRect(0, vel.getY(), kHeader, vel.getHeight());
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(11.0f));
        g.drawText("Velocity", juce::Rectangle<int>(10, vel.getY() + 6, kHeader - 20, 14), juce::Justification::centredLeft);
        g.setFont(juce::FontOptions(9.5f));
        g.drawText("127", juce::Rectangle<int>(10, vel.getY() + 2, kHeader - 18, 12), juce::Justification::centredRight);
        g.drawText("1", juce::Rectangle<int>(10, vel.getBottom() - 14, kHeader - 18, 12), juce::Justification::centredRight);

        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(vel);
        g.setColour(colours::background.darker(0.15f));
        g.fillRect(vel);
        paint_grid(g, m_, vel, 0, false);
        g.setColour(colours::outline);
        g.drawHorizontalLine(vel.getY(), static_cast<float>(vel.getX()), static_cast<float>(vel.getRight()));
        const float bottom = static_cast<float>(vel.getBottom()) - 4.0f;
        const float span = static_cast<float>(vel.getHeight() - 10);
        auto bar = [&](Ticks start, int v, juce::Colour c) {
            const float x = static_cast<float>(m_.x_of(static_cast<double>(start)));
            const float top = bottom - span * static_cast<float>(v) / 127.0f;
            g.setColour(c);
            g.fillRect(juce::Rectangle<float>(x, top, 2.0f, bottom - top));
            g.fillEllipse(x - 2.5f, top - 3.0f, 7.0f, 7.0f);
        };
        for (std::size_t i = 0; i < l.score_notes.size(); ++i) {
            const auto& n = l.score_notes[i];
            const notation::EventId sid = i < l.score_ids.size() ? l.score_ids[i] : notation::kInvalidEventId;
            const bool sel = sid != notation::kInvalidEventId && is_perf_selected(sid);
            int v = n.velocity;
            if (sel && drag_ == Drag::PerfVelocity) {
                const int a = score_index(l, perf_anchor_);
                if (a >= 0) v = juce::jlimit(1, 127, v + vel_value_ - l.score_notes[static_cast<std::size_t>(a)].velocity);
            }
            if (const auto w = l.score_written.find(sid); w != l.score_written.end() && w->second.velocity != v) {
                // The written velocity, as a short tick.
                const float x = static_cast<float>(m_.x_of(static_cast<double>(n.start)));
                const float wy = bottom - span * static_cast<float>(w->second.velocity) / 127.0f;
                g.setColour(colours::text_dim.withAlpha(0.8f));
                g.fillRect(juce::Rectangle<float>(x - 4.0f, wy, 10.0f, 1.5f));
            }
            bar(n.start, v, sel ? colours::accent_warm : l.colour.withAlpha(l.performance.count(sid) ? 0.6f : 0.3f));
        }
        for (const auto& r : l.regions)
            for (const auto& n : r.notes) {
                const NoteKey k{r.id, n.start, n.pitch};
                int v = n.velocity;
                if (drag_ == Drag::Velocity && std::find(vel_targets_.begin(), vel_targets_.end(), k) != vel_targets_.end())
                    v = vel_value_;
                bar(n.start, v, is_selected(k) ? colours::accent_warm : l.colour.brighter(0.35f));
            }
    }

    Model& m_;
    juce::ScrollBar vscroll_{true};
    juce::ComboBox snap_;
    int scroll_y_ = 0;
    std::string track_;
    std::vector<NoteKey> selection_;
    juce::String hint_;

    Drag drag_ = Drag::None;
    NoteEvent anchor_;
    double down_tick_ = 0.0;
    int down_pitch_ = 60;
    Ticks dt_ = 0;
    int dp_ = 0;
    Ticks create_start_ = 0, create_end_ = 0;
    int create_pitch_ = 60;
    std::vector<NoteKey> vel_targets_;
    int vel_value_ = 80;
    int last_velocity_ = 90;
    // Score notes selected for performance editing, by score note id.
    std::vector<notation::EventId> perf_sel_;
    notation::EventId perf_anchor_ = notation::kInvalidEventId;
    bool flip_ = false;
    bool free_ = false;  ///< The current score-note drag is Alt (performance only).
    std::uint64_t humanize_seed_ = 0;
};

}  // namespace

// --- SequencerView ----------------------------------------------------------------------------------

struct SequencerView::Impl : private juce::ScrollBar::Listener {
    explicit Impl(SequencerView& o, AppContext& c)
        : owner(o), model(c), toolbar(model), ruler(model), lanes(model), roll(model)
    {
        for (juce::Component* comp : std::initializer_list<juce::Component*>{&toolbar, &ruler, &lanes, &divider, &roll, &hscroll})
            owner.addAndMakeVisible(comp);
        style_scrollbar(hscroll);
        hscroll.addListener(this);
        toolbar.roll_button().setToggleState(true, juce::dontSendNotification);
        toolbar.roll_button().onClick = [this] { set_roll_visible(toolbar.roll_button().getToggleState()); };
        model.view_changed = [this] { view_changed(); };
        model.open_piano_roll = [this] { set_roll_visible(true); };
        model.range_changed = [this] {
            if (owner.on_range_changed) owner.on_range_changed();
        };
        model.automation_layout_changed = [this] {
            lanes.update_scroll();
            lanes.repaint();
        };
        model.pencil_changed = [this] {
            toolbar.pencil_button().setToggleState(model.pencil, juce::dontSendNotification);
        };
        ruler.setComponentID("sequencer_ruler");
        lanes.setComponentID("sequencer_lanes");
        divider.on_begin = [this] { drag_start_height = roll_height; };
        divider.on_drag = [this](int dy) {
            roll_height = juce::jlimit(kRollHeader + kVelocity + 4 * kKeyHeight, std::max(200, owner.getHeight() - 160),
                                       drag_start_height - dy);
            owner.resized();
        };
        listener = c.session.add_listener([this](session::Change change) {
            if (change == session::Change::Document) {
                model.rebuild();
                lanes.update_scroll();
                lanes.layout_automation(true);
                roll.document_changed();
                update_hscroll();
                owner.repaint();
            } else if (change == session::Change::Selection) {
                roll.document_changed();
                lanes.ensure_visible(model.ctx.session.selected_track());
                owner.repaint();
            }
        });
        model.rebuild();
        lanes.layout_automation(true);
    }

    ~Impl() override { model.ctx.session.remove_listener(listener); }

    void set_roll_visible(bool visible)
    {
        roll_visible = visible;
        toolbar.roll_button().setToggleState(visible, juce::dontSendNotification);
        owner.resized();
        if (visible) roll.document_changed();
    }

    void layout()
    {
        auto r = owner.getLocalBounds();
        model.timeline_width = std::max(10, r.getWidth() - kHeader - kScroll);
        toolbar.setBounds(r.removeFromTop(kToolbar));
        ruler.setBounds(r.removeFromTop(kRuler));
        hscroll.setBounds(r.removeFromBottom(kHScroll).withTrimmedLeft(kHeader).withTrimmedRight(kScroll));
        divider.setVisible(roll_visible);
        roll.setVisible(roll_visible);
        if (roll_visible) {
            if (roll_height <= 0) roll_height = juce::jmax(260, r.getHeight() * 52 / 100);
            const int h = juce::jlimit(kRollHeader + kVelocity + 40, std::max(kRollHeader + kVelocity + 40, r.getHeight() - 90),
                                       roll_height);
            roll.setBounds(r.removeFromBottom(h));
            divider.setBounds(r.removeFromBottom(kDivider));
        }
        lanes.setBounds(r);
        update_hscroll();
    }

    void update_hscroll()
    {
        const double visible = model.visible_ticks();
        const double total = std::max(static_cast<double>(model.end), model.view_start + visible);
        hscroll.setRangeLimits(0.0, total, juce::dontSendNotification);
        hscroll.setCurrentRange(model.view_start, visible, juce::dontSendNotification);
    }

    void view_changed()
    {
        update_hscroll();
        toolbar.repaint();
        ruler.repaint();
        lanes.repaint();
        roll.repaint();
    }

    void scrollBarMoved(juce::ScrollBar*, double start) override
    {
        model.view_start = std::max(0.0, start);
        ruler.repaint();
        lanes.repaint();
        roll.repaint();
    }

    void set_playhead(Ticks tick, bool playing)
    {
        // Recording: the pass started record_seconds() before the playhead.
        const bool recording = model.ctx.engine.recording();
        if (recording) {
            const double start = model.ctx.engine.position_seconds() - model.ctx.engine.record_seconds();
            model.record_start = model.tempo.tick_at_seconds(std::max(0.0, start));
        }
        if (recording != model.recording) {
            model.recording = recording;
            lanes.repaint();
        }
        if (tick == model.playhead && playing == model.playing) return;
        const Ticks old = model.playhead;
        model.playhead = tick;
        model.playing = playing;
        if (recording) lanes.repaint();
        lanes.automation_playhead();
        if (playing) {
            const double x = model.x_of(static_cast<double>(tick));
            const double right = kHeader + model.timeline_width;
            if (x > right - 24 || x < kHeader) {
                model.set_view_start(static_cast<double>(tick) - model.visible_ticks() * 0.08);
                return;
            }
        }
        // Repaint only the strips the playhead crossed.
        const int x0 = juce::roundToInt(model.x_of(static_cast<double>(std::min(old, tick)))) - 8;
        const int x1 = juce::roundToInt(model.x_of(static_cast<double>(std::max(old, tick)))) + 8;
        for (juce::Component* c : std::initializer_list<juce::Component*>{&lanes, &roll})
            c->repaint(x0, 0, x1 - x0, c->getHeight());
        ruler.repaint();
    }

    SequencerView& owner;
    Model model;
    Toolbar toolbar;
    Ruler ruler;
    LaneArea lanes;
    Divider divider;
    PianoRoll roll;
    juce::ScrollBar hscroll{false};
    int listener = 0;
    bool roll_visible = true;
    int roll_height = 0;
    int drag_start_height = 0;
};

namespace {
std::vector<SequencerView*>& instances()
{
    static std::vector<SequencerView*> list;
    return list;
}
}  // namespace

SequencerView::SequencerView(AppContext& context) : impl_(std::make_unique<Impl>(*this, context)), context_(context)
{
    instances().push_back(this);
}

SequencerView::~SequencerView()
{
    auto& list = instances();
    list.erase(std::remove(list.begin(), list.end(), this), list.end());
}

SequencerView* SequencerView::find(const AppContext& context)
{
    for (auto* v : instances())
        if (&v->context_ == &context) return v;
    return nullptr;
}

void SequencerView::show_automation(const std::string& owner_id, bool show) { impl_->model.set_automation_shown(owner_id, show); }

bool SequencerView::automation_shown(const std::string& owner_id) const
{
    return impl_->model.show_automation.count(owner_id) > 0;
}

void SequencerView::hide_all_automation()
{
    impl_->model.show_automation.clear();
    if (impl_->model.automation_layout_changed) impl_->model.automation_layout_changed();
}

bool SequencerView::pencil_mode() const { return impl_->model.pencil; }

void SequencerView::set_pencil_mode(bool on) { impl_->model.set_pencil_mode(on); }

void SequencerView::notate_region(const std::string& region_id) { impl_->roll.open_region_notation(region_id); }

void SequencerView::set_playhead(Ticks tick, bool playing) { impl_->set_playhead(tick, playing); }

std::pair<Ticks, Ticks> SequencerView::range() const { return {impl_->model.range_start, impl_->model.range_end}; }

void SequencerView::set_range(Ticks start, Ticks end)
{
    impl_->model.range_start = std::max<Ticks>(0, std::min(start, end));
    impl_->model.range_end = std::max<Ticks>(0, std::max(start, end));
    repaint();
    if (on_range_changed) on_range_changed();
}

void SequencerView::set_punch_display(bool on)
{
    impl_->model.punch = on;
    repaint();
}

bool SequencerView::promote_take(session::Session& session, const std::string& track_id, const std::string& clip_id)
{
    return LaneArea::promote_take(session, track_id, clip_id);
}

std::pair<std::string, std::string> SequencerView::selected_clip() const
{
    return {impl_->model.clip_track, impl_->model.clip_id};
}

void SequencerView::paint(juce::Graphics& g) { g.fillAll(colours::background); }

void SequencerView::resized() { impl_->layout(); }

}  // namespace vcc::desktop
