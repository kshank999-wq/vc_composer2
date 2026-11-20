#include "ui/SequencerView.h"

#include "ui/Deferred.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <set>

#include "audio/Consolidate.h"
#include "ui/Import.h"
#include "ui/AudioTrackStyle.h"
#include "ui/AutomationLaneView.h"
#include "ui/Theme.h"
#include "ui/TrackPanel.h"
#include "ui/WaveformCache.h"
#include "ui/hw/HwControls.h"
#include "ui/hw/HwTheme.h"
#include "ui/shell/TopDeck.h"
#include "vcc/picture/Picture.h"
#include "vcc/project/ArticulationMap.h"
#include "vcc/project/Automation.h"
#include "vcc/project/MidiTools.h"
#include "vcc/project/Performance.h"
#include "vcc/project/Render.h"
#include "vcc/project/RenderCache.h"
#include "vcc/session/Notate.h"

namespace vcc::desktop {

namespace {

constexpr int kHeader = 220;      ///< Track header / keyboard column width (docs/spec/ui, Producer track header).
constexpr int kScroll = 10;       ///< Vertical scroll bar width (right edge of every band).
constexpr int kToolbar = 32;
constexpr int kRuler = 34;
constexpr int kLaneHeight = 72;   ///< An instrument or MIDI track, collapsed.
constexpr int kAudioRow = 84;     ///< An audio track.
constexpr int kExpandedRow = 272; ///< An instrument track expanded: piano roll and velocity inline.
constexpr int kAudioLane = 26;    ///< An instrument track's own audio lane, at the bottom of its row.
constexpr int kHeaderTop = 52;    ///< Header rows 1-2; an expanded row's piano roll starts under them.
constexpr int kRollInset = kHeader - 62;  ///< The inline piano roll leaves the header's left part to it.
constexpr int kMinRow = 48;
constexpr int kDivider = 6;
constexpr int kHScroll = 12;
constexpr int kRollHeader = 30;
constexpr int kVelocity = 48;
constexpr int kKeyHeight = 11;
constexpr int kTakeHeight = 40;   ///< One take lane under an audio track.
constexpr int kEdge = 6;          ///< Grab width of a clip's trim edges and handles.
constexpr double kMinClip = 0.01; ///< Shortest clip a trim leaves, seconds.
constexpr int kBusHeight = 60;    ///< An aux / bus row (automation only) at the bottom.

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
    std::vector<ControllerEvent> controllers;
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
    std::vector<RegionView> regions;       ///< Main lane: what plays.
    std::vector<RegionView> take_regions;  ///< MIDI takes on take lanes (silent).
    int lo = 48, hi = 72;  ///< Pitch span for the lane's mini roll.
    int range_lo = 21, range_hi = 108;
    // Audio (Main Spec §11).
    bool audio = false;       ///< An audio track: clips, no MIDI.
    bool stereo = false;
    bool armed = false;
    std::vector<project::AudioClip> clips;  ///< Every lane, sorted by lane then start.
    std::vector<std::string> clip_paths;    ///< Resolved file of each clip.
    std::vector<int> takes;                 ///< Distinct take lanes (> 0) in use, ascending.
    // Producer track header (docs/spec/ui).
    int number = 0;
    bool midi_only = false;  ///< A MIDI track (external instrument) rather than an Instrument track.
    bool mute = false, solo = false, monitor_input = false;
    double gain_db = 0.0;
    std::string playback = "both";
    std::string record_source = "midi";
    bool printed_fx = false;
    juce::String articulation;  ///< An articulation track: its sound ("Staccato").
    bool expanded = false;      ///< A part expanded to articulation tracks: they play it.
    int articulation_count = 0; ///< An expanded part: how many tracks it has.
    bool folded = false;        ///< An expanded part whose tracks are hidden here.
    std::vector<int> inserts;  ///< Per insert slot: 0 empty, 1 active, 2 bypassed.
    juce::String route;        ///< "IN 1 > MASTER".
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
    std::vector<project::Marker> markers;
    std::vector<project::Comment> comments;               ///< hits, sync points (picture time)
    std::vector<project::FlaggedRegion> changed_regions; ///< from comparing cuts
    Ticks end = kTicksPerWhole * 32;
    std::vector<LaneView> lanes;

    std::function<void()> view_changed;   ///< zoom / scroll moved
    std::function<void()> open_piano_roll;
    std::function<void()> range_changed;  ///< the ruler selection changed

    // Audio clips and recording.
    WaveformCache waveforms;
    std::set<std::string> show_takes;     ///< tracks whose take lanes are open
    std::string clip_track, clip_id;      ///< the selected audio clip (the one menus and keys start from)
    /// Clips selected with it (Shift-click, or a box dragged across the lanes).
    std::set<std::pair<std::string, std::string>> more_clips;
    bool is_clip_selected(const std::string& track, const std::string& id) const
    {
        return (clip_track == track && clip_id == id) || more_clips.count({track, id}) > 0;
    }
    /// Everything selected, the main clip first.
    std::vector<std::pair<std::string, std::string>> selected_clips() const
    {
        std::vector<std::pair<std::string, std::string>> out;
        if (!clip_id.empty()) out.emplace_back(clip_track, clip_id);
        for (const auto& k : more_clips)
            if (!(k.first == clip_track && k.second == clip_id)) out.push_back(k);
        return out;
    }
    Ticks range_start = 0, range_end = 0; ///< ruler selection (punch range); empty when equal
    bool punch = false;
    bool recording = false;
    Ticks record_start = 0;

    // Automation (view state: which owners show their lanes, row heights, pencil).
    std::set<std::string> show_automation;
    std::map<std::string, int> lane_heights;  ///< "owner/lane id" -> row height
    bool pencil = false;
    /// Edit mode and the Smart tool (view state, like the pencil).
    using EditMode = SequencerView::EditMode;
    EditMode edit_mode = EditMode::Grid;
    bool smart = false;
    /// A Smart-tool range: the tracks it covers, and the range it was made
    /// as (a range set any other way, on the ruler, covers no track).
    std::set<std::string> range_tracks;
    Ticks smart_from = 0, smart_to = 0;
    int spot_scale = 0;  ///< the Spot dialog's time scale, remembered
    bool smart_range_live() const
    {
        return !range_tracks.empty() && has_range() && range_start == smart_from && range_end == smart_to;
    }
    /// Whether a drag snaps: in Grid mode, unless Cmd/Ctrl is held (and the
    /// other way round in the other modes).
    bool snaps(bool command_down) const { return (edit_mode == EditMode::Grid) != command_down; }
    void set_edit_mode(EditMode mode)
    {
        if (edit_mode == mode) return;
        edit_mode = mode;
        static const char* what[] = {"Shuffle: clips stay butted together; moving, trimming or deleting closes the gap",
                                     "Slip: clips move freely", "Spot: click a clip to type where it starts",
                                     "Grid: moves and trims snap to the grid"};
        ctx.status(what[static_cast<int>(mode)]);
    }
    void set_smart(bool on)
    {
        if (smart == on) return;
        smart = on;
        ctx.status(on ? "Smart tool: the top half of a clip selects a range, the bottom half moves it"
                      : "Smart tool off");
    }
    /// MIDI tools: the quantize settings last used, and what the piano
    /// roll's bottom lane shows (-1 velocity, else a controller number).
    project::QuantizeSettings quantize{kTicksPerQuarter / 4, 1.0, 0.0, false};
    int cc_lane = -1;
    /// The piano roll's tools (set by the view): on its selected notes, or all.
    std::function<int(const project::QuantizeSettings&)> quantize_action;
    std::function<int(const project::VelocityChange&)> velocity_action;
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
    /// The track whose piano roll is open inline (one at a time; it follows the selection).
    std::string expanded;
    /// Expanded parts whose articulation tracks are folded away (a view
    /// setting: they still play).
    std::set<std::string> folded_parts;
    /// Rows the user resized: track id, or id + "#x" for its expanded height.
    std::map<std::string, int> heights;
    std::function<void()> expanded_changed;
    /// The header's Print (false) and Freeze (true) keys.
    std::function<void(const std::string& track, bool freeze)> print_track;

    bool has_audio_lane(const LaneView& l) const { return !l.audio && !l.bus && !l.reference; }
    bool is_expanded(const LaneView& l) const { return has_audio_lane(l) && expanded == l.id; }
    /// The height slider (bottom right): every row taller or shorter.
    double height_zoom = 1.0;
    int base_height(const LaneView& l) const
    {
        if (l.bus) return kBusHeight;
        const std::string key = is_expanded(l) ? l.id + "#x" : l.id;
        int h = is_expanded(l) ? kExpandedRow : l.audio ? kAudioRow : kLaneHeight;
        if (auto it = heights.find(key); it != heights.end()) h = it->second;
        return std::max(min_height(l), static_cast<int>(std::lround(h * height_zoom)));
    }
    /// A row's height as stored (before the height slider).
    int stored_height(int shown) const { return static_cast<int>(std::lround(shown / std::max(0.1, height_zoom))); }
    /// The smallest a row can be dragged to.
    int min_height(const LaneView& l) const
    {
        return is_expanded(l) ? kHeaderTop + kRollHeader + kVelocity + 4 * kKeyHeight + kAudioLane : kMinRow;
    }
    /// Where a track's main-lane clips sit: the whole row of an audio track,
    /// the audio lane at the bottom of an instrument track's.
    juce::Rectangle<int> clip_row(const LaneView& l, juce::Rectangle<int> base) const
    {
        return has_audio_lane(l) ? base.withTop(base.getBottom() - kAudioLane) : base;
    }
    void set_expanded(const std::string& id)
    {
        if (expanded == id) return;
        expanded = id;
        if (expanded_changed) expanded_changed();
    }
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
        comments = d.comments;
        changed_regions = d.active_picture() ? d.active_picture()->changed_regions : std::vector<project::FlaggedRegion>{};
        end = session.content_end();
        double audio_end = 0.0;
        lanes.clear();
        int number = 0;
        for (const auto* t : session.music_tracks()) {
            ++number;
            if (!t->articulation_parent.empty() && folded_parts.count(t->articulation_parent)) continue;
            LaneView l;
            l.id = t->id;
            l.number = number;
            l.midi_only = t->kind == project::TrackKind::MidiOnly;
            l.mute = t->mute;
            l.solo = t->solo;
            l.monitor_input = t->monitor == "input";
            l.gain_db = t->gain_db;
            l.playback = t->playback;
            l.printed_fx = t->printed_fx;
            for (const auto& slot : t->inserts)
                l.inserts.push_back(slot.kind.empty() ? 0 : (slot.bypass || slot.suggestion) ? 2 : 1);
            {
                juce::String in = t->input < 0 ? juce::String("NO IN")
                                               : "IN " + juce::String(t->input + 1) + (t->input_stereo ? "-" + juce::String(t->input + 2) : "");
                juce::String out = t->output_bus_id.empty() ? juce::String("MASTER") : juce::String(t->output_bus_id).toUpperCase();
                if (const auto* b = d.find_bus(t->output_bus_id)) out = juce::String(b->name).toUpperCase();
                l.route = in + juce::String::fromUTF8(" \xe2\x80\xba ") + out;
            }
            l.name = juce::String(t->name);
            if (!t->articulation_parent.empty())
                l.articulation = juce::String(project::articulation_group_name(t->articulation_group));
            l.expanded = t->articulations_expanded;
            if (l.expanded) {
                l.articulation_count = static_cast<int>(session.articulation_tracks(t->id).size());
                l.folded = folded_parts.count(t->id) > 0;
            }
            l.colour = colour_from_hex(t->color);
            // An articulation track wears its part's colour: violins purple, every violin articulation purple.
            if (!t->articulation_parent.empty())
                if (const auto* part = session.data().find_track(t->articulation_parent)) l.colour = colour_from_hex(part->color);
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
            l.record_source = t->record_source;
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
                for (const auto& r : v->regions) {
                    (r.lane == 0 ? l.regions : l.take_regions)
                        .push_back(RegionView{r.id, r.source, r.start, r.end, r.lane, r.notes, r.controllers});
                    if (r.lane > 0 && std::find(l.takes.begin(), l.takes.end(), r.lane) == l.takes.end()) l.takes.push_back(r.lane);
                }
                std::sort(l.takes.begin(), l.takes.end());
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
juce::Rectangle<int> automation_button(const LaneView&, juce::Rectangle<int> row)
{
    // Row 2 of the header, left of the volume readout.
    return {kHeader - 96, row.getY() + 30, 24, 18};
}

void paint_automation_button(juce::Graphics& g, const LaneView& l, juce::Rectangle<int> row, bool open)
{
    const auto r = automation_button(l, row).toFloat();
    const bool has = std::any_of(l.automation.begin(), l.automation.end(), [](const auto& a) { return a.points.size() > 1; });
    const juce::Colour mc = automation_ui::mode_colour(l.mode);
    hw::paint::keycap(g, r, open, {}, open ? mc : juce::Colour());
    g.setColour(open ? juce::Colours::white : (has ? mc : hw::tok::text));
    g.setFont(hw::fonts::silk(11.0f, 0.06f));
    g.drawText("A", r, juce::Justification::centred, false);
}

// --- Toolbar ---------------------------------------------------------------------------------------

class Toolbar final : public juce::Component {
public:
    explicit Toolbar(Model& m) : m_(m)
    {
        for (auto* b : {&zoom_out_, &zoom_in_, &fit_, &roll_, &pencil_, &quantize_, &velocity_}) addAndMakeVisible(*b);
        quantize_.setTooltip("Quantize the selected notes, or all played-in notes of the track (Q in the piano roll)");
        quantize_.setComponentID("sequencer_quantize");
        quantize_.onClick = [this] { show_quantize_menu(); };
        velocity_.setTooltip("Change the velocity of the selected notes, or all played-in notes of the track");
        velocity_.setComponentID("sequencer_velocity");
        velocity_.onClick = [this] { show_velocity_menu(); };
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
        g.drawText(info, getLocalBounds().withTrimmedLeft(velocity_.getRight() + 16).withTrimmedRight(12),
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
        r.removeFromLeft(12);
        quantize_.setBounds(r.removeFromLeft(74));
        r.removeFromLeft(6);
        velocity_.setBounds(r.removeFromLeft(70));
    }

    /// The grids the MIDI tools offer.
    static std::vector<std::pair<Ticks, juce::String>> quantize_grids()
    {
        const Ticks q = kTicksPerQuarter;
        return {{q, "1/4"}, {q / 2, "1/8"}, {q / 3, "1/8 triplet"}, {q / 4, "1/16"}, {q / 6, "1/16 triplet"}, {q / 8, "1/32"}};
    }

    void show_quantize_menu()
    {
        auto& q = m_.quantize;
        juce::PopupMenu menu;
        menu.addSectionHeader("Quantize to");
        for (const auto& [ticks, name] : quantize_grids())
            menu.addItem(name, true, q.grid == ticks, [this, t = ticks] {
                m_.quantize.grid = t;
                if (m_.quantize_action) m_.quantize_action(m_.quantize);
            });
        menu.addSeparator();
        juce::PopupMenu strength, swing;
        for (int s : {100, 90, 75, 50, 25})
            strength.addItem(juce::String(s) + "%", true, std::abs(q.strength * 100.0 - s) < 0.5,
                             [this, s] { m_.quantize.strength = s / 100.0; });
        for (int s : {0, 16, 33, 50})
            swing.addItem(s == 0 ? juce::String("Straight") : juce::String(s) + "% of a step late", true,
                          std::abs(q.swing * 100.0 - s) < 1.0, [this, s] { m_.quantize.swing = s / 100.0; });
        menu.addSubMenu("Strength (" + juce::String(juce::roundToInt(q.strength * 100.0)) + "%)", strength);
        menu.addSubMenu("Swing", swing);
        menu.addItem("Quantize note ends too", true, q.ends, [this] { m_.quantize.ends = !m_.quantize.ends; });
        menu.addItem("Quantize now (Q)", [this] {
            if (m_.quantize_action) m_.quantize_action(m_.quantize);
        });
        // Input quantize: the selected track's recordings land on a grid.
        if (const auto* t = m_.ctx.session.data().find_track(m_.ctx.session.selected_track());
            t && (t->kind == project::TrackKind::Instrument || t->kind == project::TrackKind::MidiOnly)) {
            juce::PopupMenu input;
            const std::string id = t->id;
            input.addItem("Off (as played)", true, t->input_quantize == 0, [this, id] { m_.ctx.session.set_input_quantize(id, 0); });
            for (const auto& [ticks, name] : quantize_grids())
                input.addItem(name, true, t->input_quantize == ticks,
                              [this, id, t2 = ticks] { m_.ctx.session.set_input_quantize(id, t2); });
            menu.addSeparator();
            menu.addSubMenu("Input quantize while recording " + juce::String(t->name), input);
        }
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&quantize_));
    }

    void show_velocity_menu()
    {
        auto apply = [this](project::VelocityChange v) {
            if (m_.velocity_action) m_.velocity_action(v);
        };
        juce::PopupMenu menu;
        menu.addItem("Louder (+10)", [apply] { project::VelocityChange v; v.offset = 10; apply(v); });
        menu.addItem("Softer (-10)", [apply] { project::VelocityChange v; v.offset = -10; apply(v); });
        menu.addItem("Scale up 120%", [apply] { project::VelocityChange v; v.scale = 1.2; apply(v); });
        menu.addItem("Scale down 80%", [apply] { project::VelocityChange v; v.scale = 0.8; apply(v); });
        menu.addItem("Even out (halfway to the average)", [apply] { project::VelocityChange v; v.compress = 0.5; apply(v); });
        menu.addSeparator();
        for (int s : {64, 90, 100, 127})
            menu.addItem("Set all to " + juce::String(s), [apply, s] { project::VelocityChange v; v.set = s; apply(v); });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&velocity_));
    }

private:
    Model& m_;
    juce::TextButton zoom_out_{"-"}, zoom_in_{"+"}, fit_{"Fit"}, roll_{"Piano Roll"}, pencil_{"Pencil"},
        quantize_{"Quantize"}, velocity_{"Velocity"};
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

        // Review comments: small bubbles, amber while open.
        for (const auto& cm : m_.comments) {
            const float x = static_cast<float>(m_.x_of(static_cast<double>(m_.tempo.tick_at_seconds(std::max(0.0, cm.seconds)))));
            if (x < area.getX() - 6 || x > area.getRight() + 6) continue;
            const juce::Rectangle<float> r(x - 1.0f, fh - 17.0f, 12.0f, 9.0f);
            g.setColour(cm.resolved ? juce::Colour(0xff6b737c) : juce::Colour(0xffe0a040));
            g.fillRoundedRectangle(r, 2.5f);
            juce::Path tail;
            tail.addTriangle(r.getX() + 2.0f, r.getBottom(), r.getX() + 6.0f, r.getBottom(), r.getX() + 2.0f, r.getBottom() + 3.0f);
            g.fillPath(tail);
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
        const bool free = !m_.snaps(e.mods.isCommandDown());
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

class LaneArea final : public juce::Component, public juce::FileDragAndDropTarget, private juce::ScrollBar::Listener {
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
        layout_roll();
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
        // A file being dragged over: the track it lands on and where.
        if (drop_x_ >= 0) {
            juce::Graphics::ScopedSaveState state(g);
            int top = -scroll_y_;
            juce::Rectangle<int> row;
            for (const auto& l : m_.lanes) {
                const int rh = m_.row_height(l);
                if (!l.bus && l.id == drop_track_) row = {0, top, w - kScroll, m_.base_height(l)};
                top += rh;
            }
            if (row.isEmpty()) row = {0, top, w - kScroll, 40};  // below the tracks: a new one
            g.setColour(colours::accent.withAlpha(0.14f));
            g.fillRect(row);
            const float x = static_cast<float>(m_.x_of(static_cast<double>(drop_tick(drop_x_))));
            g.setColour(colours::accent);
            g.fillRect(juce::Rectangle<float>(x - 1.0f, static_cast<float>(row.getY()), 2.0f, static_cast<float>(row.getHeight())));
            if (drop_track_.empty()) {
                g.setFont(juce::FontOptions(12.0f));
                g.drawText("Drop here for a new track", row.withTrimmedLeft(kHeader + 8), juce::Justification::centredLeft);
            }
        }
        // The Smart tool's range, on the clip rows of its tracks.
        if (m_.smart_range_live()) {
            juce::Graphics::ScopedSaveState state(g);
            g.reduceClipRegion(timeline);
            const float x0 = static_cast<float>(m_.x_of(static_cast<double>(m_.range_start)));
            const float x1 = static_cast<float>(m_.x_of(static_cast<double>(m_.range_end)));
            for (std::size_t i = 0; i < m_.lanes.size(); ++i) {
                const auto& l = m_.lanes[i];
                if (!m_.range_tracks.count(l.id)) continue;
                const auto base = juce::Rectangle<int>(0, m_.lane_top(i) - scroll_y_, getWidth(), m_.base_height(l));
                const auto row = m_.clip_row(l, base).toFloat();
                g.setColour(colours::accent.withAlpha(0.30f));
                g.fillRect(juce::Rectangle<float>(x0, row.getY(), std::max(1.0f, x1 - x0), row.getHeight()));
            }
        }
        // A clip being shuffled: its ghost where the mouse is.
        if (drag_ == Drag::Move && shuffle_live_) {
            juce::Graphics::ScopedSaveState state(g);
            g.reduceClipRegion(timeline);
            const float x = static_cast<float>(m_.x_of_seconds(shuffle_target_));
            const float x1 = static_cast<float>(m_.x_of_seconds(shuffle_target_ + drag_clip_.length));
            const auto ghost = juce::Rectangle<float>(x, drag_row_.getY(), std::max(2.0f, x1 - x), drag_row_.getHeight());
            g.setColour(colours::accent.withAlpha(0.18f));
            g.fillRect(ghost);
            g.setColour(colours::accent);
            g.drawRect(ghost, 1.5f);
        }
        // A selection box being dragged.
        if (drag_ == Drag::Band) {
            const auto box = juce::Rectangle<float>(band_start_, band_end_);
            g.setColour(colours::accent.withAlpha(0.12f));
            g.fillRect(box);
            g.setColour(colours::accent.withAlpha(0.8f));
            g.drawRect(box, 1.0f);
        }
        // A comp swipe in progress, over its take.
        if (drag_ == Drag::Comp) {
            const float x0 = static_cast<float>(m_.x_of(static_cast<double>(m_.tempo.tick_at_seconds(std::min(comp_from_, comp_to_)))));
            const float x1 = static_cast<float>(m_.x_of(static_cast<double>(m_.tempo.tick_at_seconds(std::max(comp_from_, comp_to_)))));
            juce::Graphics::ScopedSaveState state(g);
            g.reduceClipRegion(timeline);
            const auto r = juce::Rectangle<float>(x0, comp_row_.getY(), std::max(1.0f, x1 - x0), comp_row_.getHeight());
            g.setColour(colours::accent.withAlpha(0.28f));
            g.fillRect(r);
            g.setColour(colours::accent);
            g.drawRect(r, 1.5f);
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
        resize_track_.clear();
        // The bottom edge of a header resizes its row.
        if (e.x < kHeader && !e.mods.isPopupMenu())
            if (const auto* l = resize_edge_at(e.getPosition())) {
                resize_track_ = l->id;
                resize_key_ = m_.is_expanded(*l) ? l->id + "#x" : l->id;
                resize_start_ = m_.base_height(*l);
                resize_min_ = m_.min_height(*l);
                return;
            }
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
            if (e.mods.isPopupMenu()) {
                TrackPanel::show_track_menu(m_.ctx, track);
                return;
            }
            if (hit.take == 0 && header_click(lane, hit.row, e.getPosition())) return;
            if (!lane.takes.empty() && !m_.has_audio_lane(lane) && disclosure_rect(hit.row).contains(e.getPosition()))
                toggle_takes(track);
            return;
        }
        const int index = clip_at(lane, hit, e.position);
        if (index >= 0 && e.mods.isShiftDown() && !e.mods.isPopupMenu()) {
            toggle_clip(track, lane.clips[static_cast<std::size_t>(index)].id);
            grabKeyboardFocus();
            return;
        }
        if (index < 0 && !take_region_at(lane, hit, e.position) && !e.mods.isPopupMenu()) {
            // A drag across empty lanes selects the clips it touches (Shift adds).
            band_start_ = e.position;
            band_end_ = e.position;
            band_add_ = e.mods.isShiftDown();
            drag_ = Drag::Band;
            if (!band_add_) select_clip({}, {});
            grabKeyboardFocus();
            return;
        }
        if (index < 0) {
            select_clip({}, {});
            // A MIDI take: drag across it to comp that part onto the main lane.
            if (const auto* take = take_region_at(lane, hit, e.position)) {
                grabKeyboardFocus();
                if (e.mods.isPopupMenu()) {
                    show_take_region_menu(lane, *take);
                    return;
                }
                drag_ = Drag::Comp;
                drag_track_ = track;
                comp_midi_ = take->id;
                comp_lo_ = m_.tempo.seconds_at(take->start);
                comp_hi_ = m_.tempo.seconds_at(take->end);
                comp_row_ = hit.row.toFloat().withX(static_cast<float>(kHeader)).withWidth(static_cast<float>(getWidth() - kHeader));
                comp_from_ = comp_to_ = comp_seconds(e.x, e.mods.isCommandDown());
            }
            return;
        }
        const auto& c = lane.clips[static_cast<std::size_t>(index)];
        if (!m_.is_clip_selected(track, c.id)) select_clip(track, c.id);  // a click in the selection keeps it
        grabKeyboardFocus();
        if (e.mods.isPopupMenu()) {
            show_clip_menu(lane, c);
            return;
        }
        const auto rect = clip_rect(c, clip_row_of(lane, hit));
        drag_ = zone_for(c, rect, e.position);
        drag_clip_ = c;
        // Spot: a click on a clip asks where it starts.
        if (m_.edit_mode == Model::EditMode::Spot && hit.take == 0 && drag_ == Drag::Move) {
            drag_ = Drag::None;
            show_spot_dialog(track, c.id);
            return;
        }
        // The Smart tool: the top half of a clip selects a range on its track.
        if (m_.smart && hit.take == 0 && drag_ == Drag::Move && e.position.y < rect.getCentreY()) {
            select_clip({}, {});
            clear_smart_range();
            drag_ = Drag::Select;
            drag_track_ = track;
            return;
        }
        shuffle_live_ = false;
        drag_row_ = rect;
        // Swipe comping: dragging across a take picks that part of it for
        // the main lane. Alt-drag moves the take instead.
        if (hit.take > 0 && drag_ == Drag::Move && !e.mods.isAltDown()) {
            drag_ = Drag::Comp;
            comp_midi_.clear();
            comp_lo_ = c.start_seconds;
            comp_hi_ = c.end_seconds();
            comp_row_ = rect.withX(static_cast<float>(kHeader)).withWidth(static_cast<float>(getWidth() - kHeader));
            comp_from_ = comp_to_ = comp_seconds(e.x, e.mods.isCommandDown());
        }
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
        if (!resize_track_.empty()) {
            m_.heights[resize_key_] = m_.stored_height(juce::jlimit(resize_min_, 600, resize_start_ + e.getDistanceFromDragStartY()));
            update_scroll();
            repaint();
            return;
        }
        if (drag_ == Drag::None) return;
        if (drag_ == Drag::Band) {
            band_end_ = e.position;
            repaint();
            return;
        }
        if (drag_ == Drag::Comp) {
            comp_to_ = comp_seconds(e.x, e.mods.isCommandDown());
            const double a = std::min(comp_from_, comp_to_), b = std::max(comp_from_, comp_to_);
            m_.ctx.status("Comp " + juce::String(std::max(0.0, b - a), 2) + " s from this take");
            repaint();
            return;
        }
        if (drag_ == Drag::Select) {
            const bool loose = !m_.snaps(e.mods.isCommandDown());
            auto at = [&](double x) {
                const Ticks t = std::max<Ticks>(0, static_cast<Ticks>(std::llround(m_.tick_of(std::max<double>(x, kHeader)))));
                return loose ? t : m_.snap(t, m_.clip_grid(), false);
            };
            const Ticks a = at(e.mouseDownPosition.x), b = at(e.x);
            set_smart_range({drag_track_}, std::min(a, b), std::max(a, b));
            return;
        }
        const project::AudioClip o = drag_clip_;
        const bool free = !m_.snaps(e.mods.isCommandDown());  // Grid snaps; Cmd/Ctrl inverts
        auto snapped = [&](double tick) {
            const Ticks t = std::max<Ticks>(0, static_cast<Ticks>(std::llround(tick)));
            return free ? t : m_.snap(t, m_.clip_grid(), false);
        };
        const double dticks = m_.tick_of(e.x) - m_.tick_of(e.mouseDownPosition.x);
        // Shuffle: the clip follows the mouse as a ghost and goes in at the
        // nearest clip edge on release.
        if (drag_ == Drag::Move && m_.edit_mode == Model::EditMode::Shuffle) {
            if (!shuffle_live_ && std::abs(e.getDistanceFromDragStartX()) < 3) return;
            const Ticks t0 = m_.tempo.tick_at_seconds(o.start_seconds);
            shuffle_target_ = m_.tempo.seconds_at(std::max<Ticks>(0, static_cast<Ticks>(std::llround(static_cast<double>(t0) + dticks))));
            shuffle_live_ = true;
            m_.ctx.status("Shuffle: release to put the clip at the nearest clip edge");
            repaint();
            return;
        }
        const bool shuffle_trim = m_.edit_mode == Model::EditMode::Shuffle && (drag_ == Drag::TrimLeft || drag_ == Drag::TrimRight);
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
                // In Shuffle the clip's start stays and what follows closes up.
                c.start_seconds = shuffle_trim ? o.start_seconds : o.start_seconds + d;
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
            case Drag::Comp:
            case Drag::Band:
            case Drag::Select:
            case Drag::None: return;
        }
        const project::AudioClip* now = m_.clip(drag_track_, o.id);
        if (!now) return;
        const auto same = [](double a, double b) { return std::abs(a - b) < 1e-9; };
        if (same(now->start_seconds, c.start_seconds) && same(now->source_offset, c.source_offset) &&
            same(now->length, c.length) && same(now->fade_in, c.fade_in) && same(now->fade_out, c.fade_out) &&
            same(now->gain_db, c.gain_db))
            return;
        const auto apply = [c](project::AudioClip& x) {
            x.start_seconds = c.start_seconds;
            x.source_offset = c.source_offset;
            x.length = c.length;
            x.fade_in = c.fade_in;
            x.fade_out = c.fade_out;
            x.gain_db = c.gain_db;
        };
        if (shuffle_trim) m_.ctx.session.update_audio_clip_shuffle(drag_track_, o.id, drag_label_, apply);
        else m_.ctx.session.update_audio_clip(drag_track_, o.id, drag_label_, apply);
        if (drag_ == Drag::Gain) m_.ctx.status("Clip gain " + db_text(c.gain_db));
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (drag_ == Drag::Select) {
            drag_ = Drag::None;
            if (!m_.smart_range_live()) {
                // A click, no drag: the playhead goes there; any range goes.
                const bool loose = !m_.snaps(e.mods.isCommandDown());
                const Ticks t = std::max<Ticks>(0, static_cast<Ticks>(std::llround(m_.tick_of(std::max<double>(e.x, kHeader)))));
                clear_smart_range();
                m_.ctx.engine.locate(loose ? t : m_.snap(t, m_.clip_grid(), false));
            } else {
                const double secs = m_.tempo.seconds_at(m_.range_end) - m_.tempo.seconds_at(m_.range_start);
                m_.ctx.status("Selected " + juce::String(secs, 2) + " s: Delete removes it" +
                              (m_.edit_mode == Model::EditMode::Shuffle ? juce::String(" and closes the gap") : juce::String()) +
                              ", Cmd/Ctrl+T splits at its edges");
            }
            return;
        }
        if (drag_ == Drag::Move && shuffle_live_) {
            drag_ = Drag::None;
            shuffle_live_ = false;
            repaint();
            if (m_.ctx.session.shuffle_move_audio_clip(drag_track_, drag_clip_.id, shuffle_target_))
                m_.ctx.status("Shuffled the clip into place");
            return;
        }
        if (drag_ == Drag::Band) {
            drag_ = Drag::None;
            const auto box = juce::Rectangle<float>(band_start_, band_end_);
            if (box.getWidth() > 3.0f || box.getHeight() > 3.0f) {
                const int n = select_in_box(box, band_add_);
                if (n > 0) m_.ctx.status(juce::String(static_cast<int>(m_.selected_clips().size())) + " clips selected");
            }
            repaint();
            return;
        }
        if (drag_ == Drag::Comp) {
            drag_ = Drag::None;
            repaint();
            const double a = std::min(comp_from_, comp_to_), b = std::max(comp_from_, comp_to_);
            if (!comp_midi_.empty()) {
                const Ticks ta = m_.tempo.tick_at_seconds(a), tb = m_.tempo.tick_at_seconds(b);
                if (tb > ta && m_.ctx.session.comp_region_take(drag_track_, comp_midi_, ta, tb))
                    m_.ctx.status("Comped " + juce::String(b - a, 2) + " s of this MIDI take onto the main lane");
                comp_midi_.clear();
            } else if (b - a > 0.01 && m_.ctx.session.comp_take(drag_track_, drag_clip_.id, a, b)) {
                m_.ctx.status("Comped " + juce::String(b - a, 2) + " s onto the main lane");
            }
        }
        drag_ = Drag::None;
        resize_track_.clear();
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        auto shape = juce::MouseCursor::NormalCursor;
        if (e.x < kHeader && resize_edge_at(e.getPosition())) {
            setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
            return;
        }
        const Hit hit = hit_at(e.y);
        if (hit.lane && e.x >= kHeader) {
            const int index = clip_at(*hit.lane, hit, e.position);
            if (index >= 0) {
                const auto& c = hit.lane->clips[static_cast<std::size_t>(index)];
                const auto r = clip_rect(c, clip_row_of(*hit.lane, hit));
                const Drag zone = zone_for(c, r, e.position);
                const bool main = hit.take == 0 && zone == Drag::Move;
                if (main && m_.edit_mode == Model::EditMode::Spot) {
                    setMouseCursor(juce::MouseCursor::PointingHandCursor);
                    return;
                }
                if (main && m_.smart && e.position.y < r.getCentreY()) {
                    setMouseCursor(juce::MouseCursor::IBeamCursor);
                    return;
                }
                switch (zone) {
                    case Drag::TrimLeft:
                    case Drag::TrimRight: shape = juce::MouseCursor::LeftRightResizeCursor; break;
                    case Drag::FadeIn:
                    case Drag::FadeOut: shape = juce::MouseCursor::PointingHandCursor; break;
                    case Drag::Gain: shape = juce::MouseCursor::UpDownResizeCursor; break;
                    case Drag::Move:
                        shape = hit.take > 0 ? juce::MouseCursor::IBeamCursor : juce::MouseCursor::NormalCursor;
                        break;
                    case Drag::Comp:
                    case Drag::Band:
                    case Drag::Select:
                    case Drag::None: shape = juce::MouseCursor::NormalCursor; break;
                }
            } else if (take_region_at(*hit.lane, hit, e.position)) {
                shape = juce::MouseCursor::IBeamCursor;  // a MIDI take: drag to comp
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
        if (hit.lane && hit.take > 0 && e.x >= kHeader) {
            // Double-click a take: the whole take onto the main lane.
            const int index = clip_at(*hit.lane, hit, e.position);
            if (index >= 0) promote_take(m_.ctx.session, hit.lane->id, hit.lane->clips[static_cast<std::size_t>(index)].id);
            else if (const auto* take = take_region_at(*hit.lane, hit, e.position))
                m_.ctx.session.promote_region_take(hit.lane->id, take->id);
            return;
        }
        if (hit.lane && m_.has_audio_lane(*hit.lane) && hit.take == 0) {
            // Double-click a track to open (or close) its piano roll inline.
            m_.ctx.session.select_track(hit.lane->id);
            m_.set_expanded(m_.is_expanded(*hit.lane) ? std::string() : hit.lane->id);
        }
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (!key.getModifiers().isAnyModifierKeyDown() && (key.getKeyCode() == 'P' || key.getKeyCode() == 'p')) {
            m_.set_pencil_mode(!m_.pencil);
            return true;
        }
        // Cmd/Ctrl+T: split at the playhead (clips and MIDI regions; with no
        // clip selected, the selected track's).
        if (key.getModifiers().isCommandDown() && (key.getKeyCode() == 'T' || key.getKeyCode() == 't')) {
            if (m_.smart_range_live()) split_smart_range();
            else split_at_playhead();
            return true;
        }
        if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) return delete_selection();
        if (key == juce::KeyPress::escapeKey && m_.smart_range_live()) {
            clear_smart_range();
            return true;
        }
        if (m_.clip_id.empty()) return false;
        if (key == juce::KeyPress::escapeKey) {
            select_clip({}, {});
            return true;
        }
        const auto mods = key.getModifiers();
        // Alt+Left/Right: nudge 10 ms; with Shift, a grid step.
        if (mods.isAltDown() && (key.getKeyCode() == juce::KeyPress::leftKey || key.getKeyCode() == juce::KeyPress::rightKey)) {
            const double sign = key.getKeyCode() == juce::KeyPress::leftKey ? -1.0 : 1.0;
            nudge_selected(sign * (mods.isShiftDown() ? grid_seconds() : kNudge));
            return true;
        }
        return false;
    }

    // --- Dropping audio and MIDI files onto the lanes ----------------------------------------

    bool isInterestedInFileDrag(const juce::StringArray& files) override
    {
        for (const auto& f : files)
            if (is_audio_file(juce::File(f)) || is_midi_file(juce::File(f))) return true;
        return false;
    }
    void fileDragEnter(const juce::StringArray&, int x, int y) override { set_drop_hint(x, y); }
    void fileDragMove(const juce::StringArray&, int x, int y) override { set_drop_hint(x, y); }
    void fileDragExit(const juce::StringArray&) override
    {
        drop_x_ = -1;
        repaint();
    }

    /// The track under `y` that a drop lands on (empty: below the tracks, a new one).
    std::string drop_track(int y) const
    {
        const Hit hit = hit_at(y);
        return hit.lane && !hit.lane->bus ? hit.lane->id : std::string();
    }
    /// The tick under `x`, snapped to the clip grid.
    Ticks drop_tick(int x) const
    {
        const Ticks t = std::max<Ticks>(0, static_cast<Ticks>(std::llround(m_.tick_of(std::max(x, kHeader)))));
        return m_.snaps(false) ? m_.snap(t, m_.clip_grid(), false) : t;
    }

    void filesDropped(const juce::StringArray& files, int x, int y) override
    {
        drop_x_ = -1;
        repaint();
        juce::Array<juce::File> audio, midi;
        juce::StringArray skipped;
        for (const auto& f : files) {
            const juce::File file(f);
            if (is_midi_file(file)) midi.add(file);
            else if (is_audio_file(file)) audio.add(file);
            else skipped.add(file.getFileName());
        }
        const std::string track = drop_track(y);
        const Ticks tick = drop_tick(x);
        juce::StringArray messages, unreadable = skipped;
        if (!audio.isEmpty()) {
            const auto r = import_audio_files(m_.ctx, audio, track, m_.tempo.seconds_at(tick), false);
            if (r.message.isNotEmpty()) messages.add(r.message);
            unreadable.addArray(r.unreadable);
        }
        if (!midi.isEmpty()) {
            const auto r = import_midi_files(m_.ctx, midi, track, tick);
            if (r.message.isNotEmpty()) messages.add(r.message);
            unreadable.addArray(r.unreadable);
        }
        juce::String msg = messages.joinIntoString("; ");
        if (!unreadable.isEmpty()) msg << (msg.isEmpty() ? "Could not import " : "; could not read ") << unreadable.joinIntoString(", ");
        m_.ctx.status(msg);
    }

    void set_drop_hint(int x, int y)
    {
        drop_x_ = std::max(x, kHeader);
        drop_track_ = drop_track(y);
        repaint();
    }

    // --- Clip editing (the clip menu, keys, and SequencerView's entry points) -------------

    static constexpr double kNudge = 0.010;  ///< Alt+arrow: 10 ms.

    /// The selected clip's track, split where the playhead is (every lane).
    bool split_at_playhead()
    {
        // The tracks of the selected clips, else the selected track: their
        // audio clips and MIDI regions under the playhead.
        std::vector<std::string> tracks;
        for (const auto& [t, id] : m_.selected_clips())
            if (std::find(tracks.begin(), tracks.end(), t) == tracks.end()) tracks.push_back(t);
        if (tracks.empty() && !m_.ctx.session.selected_track().empty()) tracks.push_back(m_.ctx.session.selected_track());
        if (tracks.empty()) return false;
        const auto clips = m_.ctx.session.split_audio_clips(tracks, m_.tempo.seconds_at(m_.playhead));
        const auto regions = m_.ctx.session.split_regions(tracks, m_.playhead);
        if (clips.empty() && regions.empty()) {
            m_.ctx.status("The playhead is not inside a clip or region on the selected track");
            return false;
        }
        juce::StringArray parts;
        if (!clips.empty()) parts.add(juce::String(static_cast<int>(clips.size())) + (clips.size() == 1 ? " clip" : " clips"));
        if (!regions.empty()) parts.add(juce::String(static_cast<int>(regions.size())) + (regions.size() == 1 ? " region" : " regions"));
        m_.ctx.status("Split " + parts.joinIntoString(" and ") + " at the playhead");
        return true;
    }

    /// The clip after the selected one on its lane that touches or overlaps it.
    std::string next_clip() const
    {
        const auto* sel = m_.clip(m_.clip_track, m_.clip_id);
        const auto* t = m_.ctx.session.data().find_track(m_.clip_track);
        if (!sel || !t || !t->active_version()) return {};
        std::string best;
        double best_start = 1e300;
        for (const auto& c : t->active_version()->audio_clips)
            if (c.id != sel->id && c.lane == sel->lane && c.start_seconds > sel->start_seconds &&
                c.start_seconds <= sel->end_seconds() + 0.001 && c.start_seconds < best_start) {
                best = c.id;
                best_start = c.start_seconds;
            }
        return best;
    }

    bool crossfade_selected(double seconds)
    {
        const std::string next = next_clip();
        if (next.empty()) {
            m_.ctx.status("Crossfade: no clip meets this one's end on its lane");
            return false;
        }
        if (!m_.ctx.session.crossfade_audio_clips(m_.clip_track, m_.clip_id, next, seconds)) {
            m_.ctx.status("Crossfade: the next clip has no audio before its start to fade in from");
            return false;
        }
        m_.ctx.status("Crossfade " + juce::String(juce::roundToInt(seconds * 1000.0)) + " ms");
        return true;
    }

    bool consolidate_selected()
    {
        if (m_.clip_track.empty()) return false;
        // Several clips selected on the main clip's track: those; else the clips touching it.
        std::vector<std::string> ids;
        for (const auto& [t, id] : m_.selected_clips())
            if (t == m_.clip_track) ids.push_back(id);
        if (ids.size() < 2) ids = touching_clips(m_.ctx, m_.clip_track, m_.clip_id);
        const auto r = consolidate_clips(m_.ctx, m_.clip_track, ids);
        m_.ctx.status(r.message);
        if (r.ok) select_clip(m_.clip_track, r.clip_id);
        return r.ok;
    }

    bool nudge_selected(double seconds)
    {
        if (m_.clip_track.empty()) return false;
        // Every selected clip, track by track (the nudges merge into one undo step).
        std::map<std::string, std::vector<std::string>> by_track;
        for (const auto& [t, id] : m_.selected_clips()) by_track[t].push_back(id);
        bool ok = false;
        for (const auto& [t, ids] : by_track) ok = m_.ctx.session.nudge_audio_clips(t, ids, seconds) || ok;
        if (ok) m_.ctx.status("Nudge " + juce::String(seconds * 1000.0, 1) + " ms");
        return ok;
    }

    /// One step of the clip grid, in seconds at the selected clip.
    double grid_seconds() const
    {
        const auto* sel = m_.clip(m_.clip_track, m_.clip_id);
        const Ticks at = sel ? m_.tempo.tick_at_seconds(sel->start_seconds) : m_.playhead;
        return m_.tempo.seconds_at(at + m_.clip_grid()) - m_.tempo.seconds_at(at);
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
    enum class Drag { None, Move, TrimLeft, TrimRight, FadeIn, FadeOut, Gain, Comp, Band, Select };

    /// The track whose header's bottom edge is under `p`, or null.
    const LaneView* resize_edge_at(juce::Point<int> p) const
    {
        int top = -scroll_y_;
        for (const auto& l : m_.lanes) {
            if (!l.bus) {
                const int bottom = top + m_.base_height(l);
                if (p.y >= bottom - 4 && p.y <= bottom) return &l;
            }
            top += m_.row_height(l);
        }
        return nullptr;
    }

    /// A click on a header's keys. True when it hit one.
    bool header_click(const LaneView& l, juce::Rectangle<int> row, juce::Point<int> pt)
    {
        auto& session = m_.ctx.session;
        const auto p = header_parts(l, row);
        const std::string id = l.id;
        if (p.chevron.contains(pt)) {
            m_.set_expanded(m_.is_expanded(l) ? std::string() : id);
            return true;
        }
        if (p.fold.contains(pt)) {
            set_part_folded(id, !l.folded);
            return true;
        }
        if (l.bus) return false;
        if (p.rec.contains(pt)) {
            session.set_record_armed(id, !l.armed);
            return true;
        }
        if (p.solo.contains(pt)) {
            session.set_solo(id, !l.solo);
            return true;
        }
        if (p.mute.contains(pt)) {
            session.set_mute(id, !l.mute);
            return true;
        }
        if (p.input.contains(pt)) {
            const std::string mode = l.monitor_input ? "auto" : "input";
            session.update_track_mix(id, "Monitor " + id, [mode](project::Track& t) { t.monitor = mode; });
            return true;
        }
        if (p.takes.contains(pt)) {
            toggle_takes(id);
            return true;
        }
        const char* values[] = {"midi", "audio", "both"};
        for (std::size_t i = 0; i < 3; ++i)
            if (p.source[i].contains(pt)) {
                session.set_playback(id, values[i]);
                return true;
            }
        if (p.roll.contains(pt)) {
            m_.set_expanded({});
            return true;
        }
        if (p.score.contains(pt)) {
            m_.ctx.commands.invokeDirectly(cmd::showComposer, true);
            return true;
        }
        if (p.print.contains(pt)) {
            if (m_.print_track) m_.print_track(id, false);
            return true;
        }
        if (p.freeze.contains(pt)) {
            // The freeze choices (Memory Management spec §5).
            session.select_track(id);
            juce::PopupMenu m;
            auto& cmds = m_.ctx.commands;
            m.addCommandItem(&cmds, cmd::freezeTrack);
            m.addCommandItem(&cmds, cmd::freezeUnload);
            m.addSeparator();
            m.addCommandItem(&cmds, cmd::unfreezeTrack);
            m.addCommandItem(&cmds, cmd::rerenderTrack);
            m.showMenuAsync(juce::PopupMenu::Options());
            return true;
        }
        return false;
    }

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
        if (m_.clip_track == track && m_.clip_id == id && m_.more_clips.empty()) return;
        m_.clip_track = track;
        m_.clip_id = id;
        m_.more_clips.clear();
        repaint();
    }

    /// Shift-click: adds a clip to the selection, or takes it out.
    void toggle_clip(const std::string& track, const std::string& id)
    {
        if (m_.clip_id.empty()) {
            select_clip(track, id);
            return;
        }
        if (m_.clip_track == track && m_.clip_id == id) {
            // The main clip leaves: the next one leads.
            if (m_.more_clips.empty()) {
                select_clip({}, {});
                return;
            }
            const auto next = *m_.more_clips.begin();
            m_.more_clips.erase(next);
            m_.clip_track = next.first;
            m_.clip_id = next.second;
        } else if (!m_.more_clips.erase({track, id})) {
            m_.more_clips.insert({track, id});
        }
        repaint();
    }

    /// Selects every clip whose rectangle touches `box` (a drag across empty lanes).
    int select_in_box(juce::Rectangle<float> box, bool add)
    {
        if (!add) {
            m_.clip_track.clear();
            m_.clip_id.clear();
            m_.more_clips.clear();
        }
        int n = 0;
        int top = -scroll_y_;
        for (const auto& l : m_.lanes) {
            const int rh = m_.row_height(l);
            if (!l.bus) {
                const int base = m_.base_height(l);
                const juce::Rectangle<int> row{0, top, getWidth() - kScroll, base};
                for (const auto& c : l.clips) {
                    juce::Rectangle<int> lane_row;
                    if (c.lane == 0) {
                        lane_row = m_.clip_row(l, row);
                    } else {
                        const auto it = std::find(l.takes.begin(), l.takes.end(), c.lane);
                        if (it == l.takes.end() || m_.takes_height(l) == 0) continue;
                        lane_row = {0, top + base + static_cast<int>(it - l.takes.begin()) * kTakeHeight, getWidth() - kScroll, kTakeHeight};
                    }
                    if (!clip_rect(c, lane_row).intersects(box)) continue;
                    if (m_.clip_id.empty()) {
                        m_.clip_track = l.id;
                        m_.clip_id = c.id;
                    } else {
                        m_.more_clips.insert({l.id, c.id});
                    }
                    ++n;
                }
            }
            top += rh;
        }
        repaint();
        return n;
    }

public:
    /// An expanded part's articulation tracks folded away, or shown again.
    void set_part_folded(const std::string& part, bool folded)
    {
        if (folded) m_.folded_parts.insert(part);
        else m_.folded_parts.erase(part);
        auto& session = m_.ctx.session;
        if (const auto* sel = session.data().find_track(session.selected_track()); folded && sel && sel->articulation_parent == part)
            session.select_track(part);
        m_.rebuild();
        update_scroll();
        layout_automation(true);
        repaint();
    }

private:
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
        layout_roll();
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
        return {26, row.getY() + kHeaderTop, 130, 16};
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
        const auto row = clip_row_of(l, hit);
        for (int i = static_cast<int>(l.clips.size()); --i >= 0;) {
            const auto& c = l.clips[static_cast<std::size_t>(i)];
            if (c.lane != hit.take) continue;
            if (clip_rect(c, row).expanded(2.0f, 0.0f).contains(p)) return i;
        }
        return -1;
    }

    /// The row a hit's clips sit in: the take row, or the main clip row
    /// (an instrument track's audio lane).
    juce::Rectangle<int> clip_row_of(const LaneView& l, const Hit& hit) const
    {
        return hit.take == 0 ? m_.clip_row(l, hit.row) : hit.row;
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

    void show_take_region_menu(const LaneView& lane, const RegionView& r)
    {
        const std::string track = lane.id, id = r.id;
        juce::PopupMenu menu;
        menu.addSectionHeader("MIDI take (lane " + juce::String(r.lane) + ")");
        menu.addItem("Promote take", [this, track, id] { m_.ctx.session.promote_region_take(track, id); });
        menu.addItem("Comp all of it onto the main lane", [this, track, id, from = r.start, to = r.end] {
            m_.ctx.session.comp_region_take(track, id, from, to);
        });
        const bool open = m_.show_takes.count(track) > 0;
        menu.addItem(open ? "Hide take lanes" : "Show take lanes", [this, track] { toggle_takes(track); });
        menu.showMenuAsync({});
    }

public:
    /// A main-lane clip's rectangle in this component (empty when not shown).
    juce::Rectangle<float> clip_bounds(const std::string& track, const std::string& id) const
    {
        int top = -scroll_y_;
        for (const auto& l : m_.lanes) {
            if (l.id == track)
                for (const auto& c : l.clips)
                    if (c.id == id && c.lane == 0)
                        return clip_rect(c, m_.clip_row(l, {0, top, getWidth() - kScroll, m_.base_height(l)}));
            top += m_.row_height(l);
        }
        return {};
    }

    // --- Edit modes ----------------------------------------------------------------------

    void set_smart_range(const std::set<std::string>& tracks, Ticks from, Ticks to)
    {
        m_.range_tracks = tracks;
        m_.range_start = m_.smart_from = from;
        m_.range_end = m_.smart_to = to;
        if (m_.range_changed) m_.range_changed();
        if (auto* p = getParentComponent()) p->repaint();
    }
    void clear_smart_range()
    {
        const bool had = m_.smart_range_live();
        m_.range_tracks.clear();
        if (had) {
            m_.range_start = m_.range_end = 0;
            if (m_.range_changed) m_.range_changed();
        }
        if (auto* p = getParentComponent()) p->repaint();
    }
    std::vector<std::string> smart_tracks() const { return {m_.range_tracks.begin(), m_.range_tracks.end()}; }

    /// Delete: the Smart range, else the selected clips (Shuffle closes the gaps).
    bool delete_selection()
    {
        if (m_.smart_range_live()) {
            const double from = m_.tempo.seconds_at(m_.range_start), to = m_.tempo.seconds_at(m_.range_end);
            const bool shuffle = m_.edit_mode == Model::EditMode::Shuffle;
            const bool ok = m_.ctx.session.delete_audio_range(smart_tracks(), from, to, shuffle);
            clear_smart_range();
            if (ok) m_.ctx.status(shuffle ? "Deleted the range and closed the gap" : "Deleted the range");
            return ok;
        }
        if (m_.clip_id.empty()) return false;
        const auto clips = m_.selected_clips();
        select_clip({}, {});
        return m_.edit_mode == Model::EditMode::Shuffle ? m_.ctx.session.remove_audio_clips_shuffle(clips)
                                                        : m_.ctx.session.remove_audio_clips(clips);
    }

    /// Cmd/Ctrl+T on a Smart range: splits its tracks' clips at both edges.
    void split_smart_range()
    {
        const auto tracks = smart_tracks();
        int n = static_cast<int>(m_.ctx.session.split_audio_clips(tracks, m_.tempo.seconds_at(m_.range_end)).size());
        n += static_cast<int>(m_.ctx.session.split_audio_clips(tracks, m_.tempo.seconds_at(m_.range_start)).size());
        m_.ctx.status(n > 0 ? "Split at the range's edges" : "No clip crosses the range's edges");
    }

    bool spot(const std::string& track, const std::string& id, double seconds)
    {
        return m_.ctx.session.update_audio_clip(track, id, unique_label("Spot clip"),
                                                [seconds](project::AudioClip& x) { x.start_seconds = std::max(0.0, seconds); });
    }

    void show_spot_dialog(const std::string& track, const std::string& id)
    {
        const project::AudioClip* c = m_.clip(track, id);
        if (!c) return;
        const auto& d = m_.ctx.session.data();
        const double start = c->start_seconds;
        auto* w = new juce::AlertWindow("Spot", "Where \"" + juce::String(c->name.empty() ? std::string("the clip") : c->name) +
                                                    "\" starts. Now " + SequencerView::spot_text(d, start, m_.spot_scale) +
                                                    ", " + juce::String(c->length, 3) + " s long.",
                                        juce::MessageBoxIconType::NoIcon);
        w->addComboBox("scale", {"Bars|Beats", "Min:Sec", "Timecode"}, "Time scale");
        w->addTextEditor("start", SequencerView::spot_text(d, start, m_.spot_scale), "Start");
        if (auto* combo = w->getComboBoxComponent("scale")) {
            combo->setSelectedItemIndex(m_.spot_scale, juce::dontSendNotification);
            combo->onChange = [this, w, combo, start] {
                m_.spot_scale = combo->getSelectedItemIndex();
                if (auto* t = w->getTextEditor("start"))
                    t->setText(SequencerView::spot_text(m_.ctx.session.data(), start, m_.spot_scale), false);
            };
        }
        w->addButton("Spot", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<LaneArea> self(this);
        w->enterModalState(true, juce::ModalCallbackFunction::create([self, w, track, id](int r) {
            if (r != 1 || !self) return;
            const auto text = w->getTextEditorContents("start");
            if (const auto s = SequencerView::parse_spot_text(self->m_.ctx.session.data(), text, self->m_.spot_scale))
                self->spot(track, id, *s);
            else
                self->m_.ctx.status("Could not read \"" + text + "\" as a time");
        }), true);
    }

private:
    void show_clip_menu(const LaneView& lane, const project::AudioClip& c)
    {
        auto& session = m_.ctx.session;
        const std::string track = lane.id, id = c.id;
        juce::PopupMenu menu;
        menu.addSectionHeader(juce::String(c.name.empty() ? std::string("Clip") : c.name));
        if (c.lane > 0)
            menu.addItem("Promote take", [&session, track, id] { promote_take(session, track, id); });
        juce::Component::SafePointer<LaneArea> self(this);
        menu.addItem("Spot...", [self, track, id] {
            if (self) self->show_spot_dialog(track, id);
        });
        const auto chosen = m_.is_clip_selected(track, id) ? m_.selected_clips()
                                                           : std::vector<std::pair<std::string, std::string>>{{track, id}};
        const juce::String what = chosen.size() > 1 ? juce::String(static_cast<int>(chosen.size())) + " clips" : juce::String("clip");
        menu.addItem((c.muted ? "Unmute " : "Mute ") + what, [this, chosen, muted = c.muted] {
            m_.ctx.session.update_audio_clips(chosen, unique_label(muted ? "Unmute clip" : "Mute clip"),
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
        menu.addSeparator();
        const double playhead = m_.tempo.seconds_at(m_.playhead);
        {
            juce::PopupMenu::Item split("Split at playhead");
            split.setEnabled(playhead > c.start_seconds + 0.005 && playhead < c.end_seconds() - 0.005)
                .setAction([this] { split_at_playhead(); });
            split.shortcutKeyDescription = juce::KeyPress('t', juce::ModifierKeys::commandModifier, 0).getTextDescriptionWithIcons();
            menu.addItem(std::move(split));
        }
        juce::PopupMenu fades;
        for (int ms : {10, 20, 50, 100, 250})
            fades.addItem(juce::String(ms) + " ms", [this, ms] { crossfade_selected(ms / 1000.0); });
        menu.addSubMenu("Crossfade with the next clip", fades, !next_clip().empty());
        const auto chain = touching_clips(m_.ctx, track, id);
        menu.addItem("Consolidate " + juce::String(static_cast<int>(chain.size())) + " touching clips", chain.size() > 1, false,
                     [this] { consolidate_selected(); });
        juce::PopupMenu nudge;
        nudge.addItem("Earlier 10 ms (Alt+Left)", [this] { nudge_selected(-kNudge); });
        nudge.addItem("Later 10 ms (Alt+Right)", [this] { nudge_selected(kNudge); });
        nudge.addItem("Earlier one grid step (Alt+Shift+Left)", [this] { nudge_selected(-grid_seconds()); });
        nudge.addItem("Later one grid step (Alt+Shift+Right)", [this] { nudge_selected(grid_seconds()); });
        menu.addSubMenu("Nudge", nudge);
        menu.addSeparator();
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
        menu.addItem("Delete " + what, [this, chosen] {
            select_clip({}, {});
            m_.ctx.session.remove_audio_clips(chosen);
        });
        menu.showMenuAsync({});
    }

    /// The Producer track header's parts (docs/spec/ui, "Producer track header"),
    /// for a track's base row.
    struct HeaderParts {
        juce::Rectangle<int> chevron, tag, number, name, rec, solo, mute, input, volume, resize;
        juce::Rectangle<int> takes;  ///< An instrument track with MIDI or audio takes: show or hide them.
        juce::Rectangle<int> fold;   ///< An expanded part: fold or show its articulation tracks.
        std::array<juce::Rectangle<int>, 3> source;  ///< MIDI, Audio, Both.
        // Expanded only.
        juce::Rectangle<int> route, inserts, print, freeze, roll, score;
    };

    HeaderParts header_parts(const LaneView& l, juce::Rectangle<int> row) const
    {
        HeaderParts p;
        const int y = row.getY();
        if (m_.has_audio_lane(l)) p.chevron = {9, y + 8, 14, 16};
        p.tag = {26, y + 8, 44, 16};
        p.number = {74, y + 6, 24, 20};
        p.name = {102, y + 6, kHeader - 110, 20};
        if (l.expanded) p.fold = p.name.removeFromRight(40).translated(4, 0).withTrimmedLeft(4);
        if (!l.bus) {
            p.rec = {26, y + 30, 22, 18};
            p.solo = {51, y + 30, 22, 18};
            p.mute = {76, y + 30, 22, 18};
            p.input = {101, y + 30, 22, 18};
            p.volume = {kHeader - 66, y + 30, 56, 18};
        }
        if (m_.has_audio_lane(l)) {
            for (int i = 0; i < 3; ++i) p.source[static_cast<std::size_t>(i)] = {26 + i * 40, y + kHeaderTop, 38, 16};
            if (!l.takes.empty()) p.takes = {148, y + kHeaderTop, kHeader - 156, 16};
        }
        if (m_.is_expanded(l) && row.getHeight() >= kHeaderTop + 120) {
            p.route = {10, y + 76, kRollInset - 16, 18};
            p.inserts = {10, y + 100, kRollInset - 16, 14};
            p.print = {10, y + 120, 68, 20};
            p.freeze = {82, y + 120, kRollInset - 92, 20};
            p.roll = {10, y + 146, 68, 20};
            p.score = {82, y + 146, kRollInset - 92, 20};
        }
        p.resize = {0, row.getBottom() - 4, kHeader, 5};
        return p;
    }

    /// The lettering and box of an articulation track: its part's colour, lighter.
    static juce::Colour articulation_ink(juce::Colour part) { return part.interpolatedWith(juce::Colours::white, 0.55f); }

    static juce::Colour type_colour(const LaneView& l)
    {
        if (l.bus) return hw::tok::type_aux;
        if (l.audio || l.reference) return hw::tok::type_audio;
        return l.midi_only ? hw::tok::type_midi : hw::tok::type_instrument;
    }

    static void key(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& legend, bool on, juce::Colour solid = {},
                    bool enabled = true)
    {
        if (r.isEmpty()) return;
        hw::paint::keycap(g, r.toFloat(), on, solid);
        const bool solid_on = on && !solid.isTransparent();
        g.setColour((solid_on ? juce::Colour(0xff101214) : on ? juce::Colours::white : hw::tok::text)
                        .withAlpha(enabled ? 1.0f : 0.35f));
        g.setFont(hw::fonts::silk(10.0f, 0.06f));
        g.drawText(legend.toUpperCase(), r.toFloat().translated(0.0f, on ? 0.5f : -0.5f), juce::Justification::centred, false);
    }

    void paint_header(juce::Graphics& g, const LaneView& l, juce::Rectangle<int> rows, bool selected)
    {
        const auto row = rows.withHeight(m_.base_height(l));
        const auto column = rows.withWidth(kHeader);
        g.setGradientFill(juce::ColourGradient::vertical(selected ? hw::tok::panel_top.brighter(0.08f) : hw::tok::panel_top,
                                                         static_cast<float>(column.getY()), hw::tok::panel_bottom,
                                                         static_cast<float>(column.getBottom())));
        g.fillRect(column);
        g.setColour(hw::tok::edge);
        g.drawHorizontalLine(column.getBottom() - 1, 0.0f, static_cast<float>(kHeader));
        // The colour stripe.
        g.setColour(l.colour);
        g.fillRect(column.withWidth(5));
        if (selected) {
            g.setColour(hw::Theme::get().led().withAlpha(0.7f));
            g.drawRect(row.withWidth(kHeader).withTrimmedLeft(5), 1);
        }
        const auto p = header_parts(l, row);
        // Chevron: expand or collapse the piano roll.
        if (!p.chevron.isEmpty()) {
            juce::Path tri;
            const auto cr = p.chevron.toFloat();
            const float cx = cr.getCentreX(), cy = cr.getCentreY();
            if (m_.is_expanded(l)) tri.addTriangle(cx - 5.0f, cy - 3.0f, cx + 5.0f, cy - 3.0f, cx, cy + 4.0f);
            else tri.addTriangle(cx - 3.0f, cy - 5.0f, cx - 3.0f, cy + 5.0f, cx + 4.0f, cy);
            g.setColour(hw::tok::text);
            g.fillPath(tri);
        }
        // Type tag, number and name.
        const auto tc = type_colour(l);
        g.setColour(tc.withAlpha(0.18f));
        g.fillRoundedRectangle(p.tag.toFloat(), 3.0f);
        g.setColour(tc);
        g.drawRoundedRectangle(p.tag.toFloat().reduced(0.5f), 3.0f, 1.0f);
        g.setFont(hw::fonts::silk(9.5f, 0.08f));
        const juce::String tag = l.bus ? (l.bus_kind == "master" ? "MASTER" : "AUX")
                                 : (l.audio || l.reference) ? "AUDIO"
                                 : l.articulation.isNotEmpty() ? "ARTIC"
                                 : l.expanded ? "PART"
                                 : l.midi_only ? "MIDI"
                                               : "INST";
        g.drawText(tag, p.tag, juce::Justification::centred, false);
        hw::paint::lcd_window(g, p.number.toFloat());
        hw::paint::lcd_text(g, l.bus ? juce::String("--") : juce::String(l.number).paddedLeft('0', 2), p.number.toFloat(), 11.0f,
                            juce::Justification::centred);
        hw::paint::lcd_window(g, p.name.toFloat());
        // An articulation track reads as a branch of its part: an elbow, then
        // the sound; the part says its notes play on the tracks below.
        const juce::String shown = l.articulation.isNotEmpty() ? juce::String::fromUTF8("\xe2\x94\x94 ") + l.articulation
                                                               : l.name;
        hw::paint::lcd_text(g, shown.toUpperCase(), p.name.toFloat().reduced(5.0f, 0.0f), 11.0f, juce::Justification::centredLeft,
                            l.reference ? 0.7f : 1.0f);
        if (l.articulation.isNotEmpty()) {
            // An articulation: boxed in its part's colour, so it reads as a branch of that part.
            g.setColour(articulation_ink(l.colour));
            g.drawRoundedRectangle(p.name.toFloat().reduced(0.5f), 3.0f, 1.5f);
        }
        if (!p.fold.isEmpty()) {
            // The part's articulation tracks: how many, and a key to fold them.
            hw::paint::keycap(g, p.fold.toFloat(), !l.folded, {});
            g.setColour(hw::tok::text);
            g.setFont(hw::fonts::silk(9.5f, 0.04f));
            g.drawText(juce::String(l.articulation_count) + juce::String::fromUTF8(l.folded ? " \xe2\x96\xb8" : " \xe2\x96\xbe"),
                       p.fold, juce::Justification::centred, false);
        }
        paint_automation_button(g, l, row, m_.show_automation.count(l.id) > 0);
        if (l.bus) return;
        // R S M I, and the volume.
        key(g, p.rec, "R", l.armed, hw::tok::rec);
        key(g, p.solo, "S", l.solo, hw::tok::solo);
        key(g, p.mute, "M", l.mute, hw::tok::mute);
        key(g, p.input, "I", l.monitor_input, hw::tok::input_monitor);
        hw::paint::lcd_window(g, p.volume.toFloat());
        hw::paint::lcd_text(g, l.gain_db <= -59.95 ? juce::String("-INF") : juce::String(l.gain_db, 1) + " dB",
                            p.volume.toFloat().reduced(4.0f, 0.0f), 10.0f, juce::Justification::centredRight);
        // What the track plays: its MIDI, its audio lane, or both.
        if (m_.has_audio_lane(l)) {
            const char* names[] = {"MIDI", "Audio", "Both"};
            const char* values[] = {"midi", "audio", "both"};
            for (std::size_t i = 0; i < 3; ++i) key(g, p.source[i], names[i], l.playback == values[i]);
            if (!p.takes.isEmpty())
                key(g, p.takes, juce::String(static_cast<int>(l.takes.size())) + (l.takes.size() == 1 ? " take" : " takes"),
                    m_.show_takes.count(l.id) > 0);
        } else if (!l.takes.empty()) {
            const bool open = m_.show_takes.count(l.id) > 0;
            auto d = disclosure_rect(row);
            juce::Path tri;
            const float cx = static_cast<float>(d.getX()) + 4.0f, cy = static_cast<float>(d.getCentreY());
            if (open) tri.addTriangle(cx - 4.0f, cy - 2.5f, cx + 4.0f, cy - 2.5f, cx, cy + 3.0f);
            else tri.addTriangle(cx - 2.5f, cy - 4.0f, cx - 2.5f, cy + 4.0f, cx + 3.0f, cy);
            g.setColour(hw::tok::text);
            g.fillPath(tri);
            const int n = static_cast<int>(l.takes.size());
            hw::paint::silk(g, juce::String(n) + (n == 1 ? " take lane" : " take lanes"), d.withTrimmedLeft(12).toFloat(), 9.5f);
        } else if (l.audio) {
            int n = 0;
            for (const auto& clip : l.clips) n += clip.lane == 0 ? 1 : 0;
            hw::paint::silk(g, juce::String(l.stereo ? "Stereo" : "Mono") + (n > 0 ? juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(n) + (n == 1 ? " clip" : " clips") : juce::String()),
                            disclosure_rect(row).toFloat(), 9.5f);
        }
        // Expanded: routing, inserts, print and freeze, piano roll | score.
        if (!p.route.isEmpty()) {
            hw::paint::lcd_window(g, p.route.toFloat());
            hw::paint::lcd_text(g, l.route, p.route.toFloat().reduced(5.0f, 0.0f), 9.5f);
            auto ins = p.inserts;
            hw::paint::silk(g, "Ins", ins.removeFromLeft(26).toFloat(), 9.0f);
            for (int i = 0; i < 5; ++i) {
                const int state = i < static_cast<int>(l.inserts.size()) ? l.inserts[static_cast<std::size_t>(i)] : 0;
                auto cell = ins.removeFromLeft(ins.getWidth() / (5 - i));
                hw::paint::led(g, {static_cast<float>(cell.getX()) + 5.0f, static_cast<float>(cell.getCentreY())}, state == 1,
                               state == 2 ? hw::tok::meter_amber : hw::Theme::get().led(), 5.0f);
                hw::paint::silk(g, juce::String::charToString(static_cast<juce::juce_wchar>('A' + i)),
                                cell.withTrimmedLeft(10).toFloat(), 8.5f);
            }
            key(g, p.print, "Print", false, hw::tok::rec);
            key(g, p.freeze, "Freeze", l.printed_fx && l.playback == "audio", hw::tok::mute);  // opens the freeze choices
            key(g, p.roll, "Piano roll", true);
            key(g, p.score, "Score", false);
        }
    }

    void paint_clip(juce::Graphics& g, const LaneView& l, std::size_t index, juce::Rectangle<int> row,
                    juce::Rectangle<int> body, bool take_row)
    {
        const auto& c = l.clips[index];
        const auto r = clip_rect(c, row);
        if (r.getRight() < body.getX() || r.getX() > body.getRight()) return;
        const bool selected = m_.is_clip_selected(l.id, c.id);
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
        const auto row = rows.withHeight(m_.base_height(l));
        paint_header(g, l, rows, selected);

        // Take lanes under the track.
        if (m_.takes_height(l) > 0) {
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
                for (const auto& r : l.take_regions)
                    if (r.lane == l.takes[k]) paint_take_region(g, l, r, take_row);
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
            // The MIDI lane above, the track's own audio lane below.
            const auto audio_lane = m_.clip_row(l, row).withLeft(kHeader).withRight(timeline.getRight());
            auto midi = body.withBottom(audio_lane.getY());
            if (m_.is_expanded(l)) midi = midi.withHeight(kHeaderTop);  // the piano roll fills the rest
            const float dim = l.playback == "audio" ? 0.35f : 1.0f;
            {
                juce::Graphics::ScopedSaveState s(g);
                g.reduceClipRegion(midi);
                if (dim < 1.0f) g.beginTransparencyLayer(dim);
                paint_notes(g, l, midi);
                if (dim < 1.0f) g.endTransparencyLayer();
            }
            g.setColour(juce::Colours::black.withAlpha(0.28f));
            g.fillRect(audio_lane);
            g.setColour(hw::tok::type_audio.withAlpha(0.22f));
            g.drawHorizontalLine(audio_lane.getY(), static_cast<float>(audio_lane.getX()), static_cast<float>(audio_lane.getRight()));
            if (!has_main_clips && !(l.armed && m_.recording)) {
                hw::paint::silk(g, l.record_source == "print"
                                       ? (l.armed ? "PRINT ARMED - press Record: the instrument goes onto this lane as a WAV, then unloads"
                                                  : "Records by printing: arm, then Record puts the instrument here as a WAV")
                                   : l.playback == "midi" ? "Audio lane (off: this track plays its MIDI)"
                                                          : "Audio lane - print the instrument here as a WAV (arm with Print, then Record)",
                                audio_lane.reduced(10, 0).toFloat(), 9.0f, juce::Justification::centredLeft,
                                hw::tok::silk.withAlpha(0.6f));
            }
            // The track's freeze state, in words (spec: labels, not colour alone).
            if (m_.ctx.track_state) {
                const auto state = m_.ctx.track_state(l.id);
                if (state.isNotEmpty() && state != "LIVE") {
                    const auto tag = audio_lane.withWidth(state.length() * 7 + 18).withX(audio_lane.getRight() - (state.length() * 7 + 26))
                                         .reduced(0, 3).toFloat();
                    const auto c = state.startsWith("STALE") || state.startsWith("MISSING") ? hw::tok::meter_amber
                                   : state == "RENDERING"                                    ? hw::tok::rec
                                                                                             : hw::tok::type_audio;
                    g.setColour(juce::Colours::black.withAlpha(0.55f));
                    g.fillRoundedRectangle(tag, 3.0f);
                    g.setColour(c);
                    g.drawRoundedRectangle(tag, 3.0f, 1.0f);
                    hw::paint::silk(g, state, tag, 8.5f, juce::Justification::centred, c);
                }
            }
        }

        const auto clips_row = m_.clip_row(l, row);
        if (l.playback == "midi" && m_.has_audio_lane(l)) g.beginTransparencyLayer(0.4f);
        for (std::size_t i = 0; i < l.clips.size(); ++i)
            if (l.clips[i].lane == 0) paint_clip(g, l, i, clips_row, body, false);
        if (l.playback == "midi" && m_.has_audio_lane(l)) g.endTransparencyLayer();

        // A pass being recorded grows from where it started to the playhead.
        if (l.armed && m_.recording && m_.playhead > m_.record_start) {
            const float x0 = static_cast<float>(m_.x_of(static_cast<double>(m_.record_start)));
            const float x1 = static_cast<float>(m_.x_of(static_cast<double>(m_.playhead)));
            auto rr = juce::Rectangle<float>(x0, static_cast<float>(clips_row.getY()) + 3.0f, x1 - x0,
                                             static_cast<float>(clips_row.getHeight()) - 6.0f);
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

        // An articulation track: each bar it plays boxed in the part's
        // colour and lettered with its articulation, in a lighter ink.
        if (l.articulation.isNotEmpty()) {
            std::set<int> bars;
            for (const auto& n : l.score_notes) bars.insert(m_.meter.bar_at(n.start));
            const auto ink = articulation_ink(l.colour);
            for (const int bar : bars) {
                const float x0 = static_cast<float>(m_.x_of(static_cast<double>(m_.meter.bar_start(bar))));
                const float x1 = static_cast<float>(m_.x_of(static_cast<double>(m_.meter.bar_start(bar + 1))));
                if (x1 < body.getX() || x0 > body.getRight()) continue;
                auto box = juce::Rectangle<float>(x0 + 1.0f, static_cast<float>(body.getY()) + 2.0f, x1 - x0 - 2.0f,
                                                  static_cast<float>(body.getHeight()) - 4.0f);
                g.setColour(l.colour.withAlpha(0.12f));
                g.fillRoundedRectangle(box, 3.0f);
                g.setColour(ink.withAlpha(0.85f));
                g.drawRoundedRectangle(box, 3.0f, 1.2f);
                g.setColour(ink);
                g.setFont(hw::fonts::silk(9.0f, 0.06f));
                g.drawText(l.articulation.toUpperCase(), box.withHeight(12.0f).reduced(4.0f, 0.0f), juce::Justification::centredLeft,
                           true);
            }
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

    /// A MIDI take in its take lane: the region and a mini roll of its notes.
    void paint_take_region(juce::Graphics& g, const LaneView& l, const RegionView& r, juce::Rectangle<int> take_row) const
    {
        const auto rr = take_region_rect(r, take_row);
        g.setColour(l.colour.withAlpha(0.10f));
        g.fillRoundedRectangle(rr, 3.0f);
        g.setColour(l.colour.withAlpha(0.55f));
        g.drawRoundedRectangle(rr, 3.0f, 1.0f);
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(9.5f));
        g.drawText(short_source(r.source), rr.withHeight(11.0f).reduced(4.0f, 0.0f), juce::Justification::centredLeft, true);
        const auto area = rr.withTrimmedTop(11.0f).reduced(0.0f, 2.0f);
        const float span = static_cast<float>(std::max(12, l.hi - l.lo + 1));
        g.setColour(l.colour.brighter(0.4f).withAlpha(0.85f));
        for (const auto& n : r.notes) {
            const float x0 = static_cast<float>(m_.x_of(static_cast<double>(n.start)));
            const float x1 = static_cast<float>(m_.x_of(static_cast<double>(n.end())));
            const float y = area.getBottom() - (static_cast<float>(juce::jlimit(l.lo, l.hi, n.pitch) - l.lo) + 0.5f) /
                                                   span * area.getHeight();
            g.fillRect(juce::Rectangle<float>(x0, y - 1.0f, std::max(1.5f, x1 - x0 - 0.5f), 2.0f));
        }
    }

    juce::Rectangle<float> take_region_rect(const RegionView& r, juce::Rectangle<int> take_row) const
    {
        const float x0 = static_cast<float>(m_.x_of(static_cast<double>(r.start)));
        const float x1 = static_cast<float>(m_.x_of(static_cast<double>(r.end)));
        return {x0 + 0.5f, static_cast<float>(take_row.getY()) + 2.0f, std::max(2.0f, x1 - x0 - 1.0f),
                static_cast<float>(take_row.getHeight()) - 5.0f};
    }

    /// The MIDI take under `p` in a take row, or null.
    const RegionView* take_region_at(const LaneView& l, const Hit& hit, juce::Point<float> p) const
    {
        if (hit.take <= 0) return nullptr;
        for (auto it = l.take_regions.rbegin(); it != l.take_regions.rend(); ++it)
            if (it->lane == hit.take && take_region_rect(*it, hit.row).contains(p)) return &*it;
        return nullptr;
    }

    Model& m_;
    juce::ScrollBar vscroll_{true};
    int scroll_y_ = 0;
    Drag drag_ = Drag::None;
    bool shuffle_live_ = false;        ///< a Shuffle move under way
    double shuffle_target_ = 0.0;      ///< where its ghost is (seconds)
    juce::Rectangle<float> drag_row_;  ///< the dragged clip at the click
    std::string drag_track_, drag_label_;
    project::AudioClip drag_clip_;
    double file_length_ = 0.0;  ///< Of the dragged clip's file, when known.
    // A selection box being dragged across the lanes.
    juce::Point<float> band_start_, band_end_;
    bool band_add_ = false;
    // A file drag over the lanes: where it would land (-1: none).
    int drop_x_ = -1;
    std::string drop_track_;
    // Swipe comping: the range in project seconds, inside the take.
    double comp_from_ = 0.0, comp_to_ = 0.0;
    double comp_lo_ = 0.0, comp_hi_ = 0.0;  ///< The take's span.
    std::string comp_midi_;                 ///< A MIDI take being comped (empty: an audio take).
    juce::Rectangle<float> comp_row_;
    double comp_seconds(int x, bool free) const
    {
        const Ticks t = std::max<Ticks>(0, static_cast<Ticks>(std::llround(m_.tick_of(x))));
        const double s = m_.tempo.seconds_at(free ? t : m_.snap(t, m_.clip_grid(), false));
        return juce::jlimit(comp_lo_, comp_hi_, s);
    }
    std::string resize_track_, resize_key_;
    int resize_start_ = 0, resize_min_ = kMinRow;
    juce::Component* roll_ = nullptr;
    std::function<void(int inset)> roll_inset_;

public:
    /// The piano roll, placed inline in the expanded track's row.
    void set_roll(juce::Component* roll, std::function<void(int)> set_inset)
    {
        roll_ = roll;
        roll_inset_ = std::move(set_inset);
        if (roll_) addChildComponent(*roll_);
        layout_roll();
    }
    void layout_roll()
    {
        if (!roll_) return;
        int top = -scroll_y_;
        for (const auto& l : m_.lanes) {
            if (m_.is_expanded(l)) {
                const int h = m_.base_height(l) - kHeaderTop - kAudioLane;
                roll_->setBounds(0, top + kHeaderTop, getWidth() - kScroll, std::max(0, h));
                if (roll_inset_) roll_inset_(kRollInset);
                roll_->setVisible(h > 0);
                return;
            }
            top += m_.row_height(l);
        }
        roll_->setVisible(false);
    }

private:
    std::map<std::string, std::unique_ptr<AutomationLaneView>> views_;  ///< "owner/lane id" ("owner/+": empty row)
    std::vector<std::unique_ptr<AutomationLaneView>> graveyard_;
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
        grid_.setTitle("Grid");
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
        snap_.setTitle("Snap");
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
        std::size_t count = 0;
        if (l) {
            count = l->score_notes.size();
            for (const auto& r : l->regions) count += r.notes.size();
        }
        if (id != track_) {
            track_ = id;
            selection_.clear();
            perf_sel_.clear();
            drag_ = Drag::None;
            centre_on_notes();
        } else if (note_count_ == 0 && count > 0) {
            centre_on_notes();  // the track's first notes: bring them into view
        }
        note_count_ = count;
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
            // The shared selection (docs/spec/ui): notes selected in the
            // Composer view or the inspector are selected here.
            const auto& shared = m_.ctx.session.selection();
            if (shared.track == l->id) {
                std::vector<notation::EventId> want;
                for (auto sid : shared.notes)
                    if (score_index(*l, sid) >= 0) want.push_back(sid);
                auto mine = perf_sel_;
                std::sort(mine.begin(), mine.end());
                if (mine != want) {
                    perf_sel_ = want;
                    if (!want.empty()) selection_.clear();
                }
            }
        }
        repaint();
    }

    /// Hands the score notes selected here to the session's shared selection.
    void publish_selection()
    {
        const auto* l = m_.selected_lane();
        if (!l || l->reference || l->audio) return;
        auto mine = perf_sel_;
        std::sort(mine.begin(), mine.end());
        auto& session = m_.ctx.session;
        const auto& shared = session.selection();
        if (shared.track == l->id && shared.notes == mine) return;
        if (mine.empty()) {
            auto next = shared;
            next.track = l->id;
            next.notes.clear();
            session.set_selection(std::move(next));
        } else {
            session.select_notes(l->id, mine);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto header = r.removeFromTop(kRollHeader);
        snap_.setBounds(header.withTrimmedLeft(kHeader).removeFromLeft(110).reduced(0, 4));
        vscroll_.setBounds(getWidth() - kScroll, kRollHeader, kScroll, grid_area().getHeight());
        // Opened or resized inline: the track's notes back in the middle.
        if (grid_area().getHeight() != last_grid_height_) {
            last_grid_height_ = grid_area().getHeight();
            centre_on_notes();
        }
        update_scroll();
    }

    /// Inline in a track row: the header's left part belongs to the row.
    void set_inset(int inset)
    {
        if (inset == inset_) return;
        inset_ = inset;
        repaint();
    }
    bool hitTest(int x, int y) override { return x >= inset_ && juce::Component::hitTest(x, y); }

    void paint(juce::Graphics& g) override
    {
        if (inset_ > 0) g.reduceClipRegion(inset_, 0, getWidth() - inset_, getHeight());
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

        if (lane_label_area().contains(e.getPosition())) {
            show_lane_menu();
            return;
        }
        if (velocity_area().contains(e.getPosition()) && m_.cc_lane == 11) {
            // A crescendo's or diminuendo's swell: its start or end handle.
            if (const auto h = swell_handle_at(*l, pos)) {
                swell_ = *h;
                drag_ = Drag::Swell;
                drag_swell(pos);
                return;
            }
        }
        if (velocity_area().contains(e.getPosition()) && m_.cc_lane >= 0) {
            // Drawing a controller: points along the drag, committed on release.
            cc_points_.clear();
            add_cc_point(pos);
            drag_ = Drag::Cc;
            repaint();
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
            case Drag::Cc: add_cc_point(pos); break;
            case Drag::Swell: drag_swell(pos); break;
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

    void mouseUp(const juce::MouseEvent& e) override
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
                else if (!e.mods.isShiftDown()) perf_sel_ = {perf_anchor_};  // a click, not a drag
                break;
            case Drag::PerfLength:
                if (dt_ != 0) commit_score_edit(Drag::PerfLength);
                break;
            case Drag::PerfVelocity: commit_performance(Drag::PerfVelocity); break;
            case Drag::Cc: commit_cc(); break;
            case Drag::Swell: break;  // each move was already one (coalesced) undo step
            case Drag::None: break;
        }
        publish_selection();
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
            publish_selection();
            repaint();
            return true;
        }
        if (!key.getModifiers().isAnyModifierKeyDown() && (key.getKeyCode() == 'Q' || key.getKeyCode() == 'q')) {
            quantize(m_.quantize);
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

    // --- MIDI tools (the toolbar's Quantize and Velocity, Q, the CC lane) ---------------

    /// The selected played-in notes, for the session's MIDI tools (empty: all).
    std::vector<session::Session::RegionNote> chosen_notes() const
    {
        std::vector<session::Session::RegionNote> out;
        for (const auto& k : selection_) out.push_back({k.region, k.start, k.pitch});
        return out;
    }

    int quantize(const project::QuantizeSettings& q)
    {
        const auto* l = m_.selected_lane();
        if (!l || l->audio || l->reference) return 0;
        const bool some = !selection_.empty();
        const int moved = m_.ctx.session.quantize_regions(l->id, chosen_notes(), q);
        selection_.clear();  // the notes moved: their keys are stale
        publish_selection();
        status(moved > 0 ? "Quantized " + juce::String(moved) + (moved == 1 ? " note" : " notes")
                         : juce::String(some ? "The selected notes are already on the grid"
                                             : "No played-in notes to quantize (score notes follow the score)"));
        repaint();
        return moved;
    }

    int change_velocity(const project::VelocityChange& v)
    {
        const auto* l = m_.selected_lane();
        if (!l || l->audio || l->reference) return 0;
        const int changed = m_.ctx.session.change_region_velocities(l->id, chosen_notes(), v);
        status(changed > 0 ? "Velocity of " + juce::String(changed) + (changed == 1 ? " note" : " notes") + " changed"
                           : juce::String("No velocities changed"));
        repaint();
        return changed;
    }

    /// Draws controller `cc` from `from` to `to` with a straight ramp of
    /// values (the check, and a click-drag in the CC lane, come here).
    bool draw_cc(int cc, Ticks from, Ticks to, int v0, int v1)
    {
        const auto* l = m_.selected_lane();
        if (!l || to <= from) return false;
        const RegionView* region = nullptr;
        for (const auto& r : l->regions)
            if (r.lane == 0 && r.start <= from && from < r.end) region = &r;
        if (!region) {
            status("Draw controllers inside a MIDI region (record or draw notes first)");
            return false;
        }
        const Ticks step = std::max<Ticks>(1, grid() / 2);
        std::vector<ControllerEvent> pts;
        int last = -1;
        for (Ticks t = from; t < to; t += step) {
            const double f = static_cast<double>(t - from) / static_cast<double>(std::max<Ticks>(1, to - from - 1));
            int v = juce::roundToInt(v0 + (v1 - v0) * std::min(1.0, f));
            if (cc == 64) v = v >= 64 ? 127 : 0;  // the sustain pedal is on or off
            if (v == last) continue;
            pts.push_back(ControllerEvent{t, 0, cc, v});
            last = v;
        }
        const bool ok = m_.ctx.session.draw_region_controller(l->id, region->id, cc, from, to, pts);
        if (ok) status("Drew " + lane_name(cc));
        return ok;
    }

    void set_cc_lane(int cc)
    {
        m_.cc_lane = cc;
        repaint();
    }

    static juce::String lane_name(int cc)
    {
        switch (cc) {
            case -1: return "Velocity";
            case 1: return "CC1 Modulation";
            case 7: return "CC7 Volume";
            case 10: return "CC10 Pan";
            case 11: return "CC11 Expression";
            case 64: return "CC64 Sustain";
            case kPitchBendController: return "Pitch bend";
            case kChannelPressureController: return "Aftertouch";
            default: return "CC" + juce::String(cc);
        }
    }

private:
    void show_lane_menu()
    {
        juce::PopupMenu menu;
        for (int cc : {-1, 1, 11, 7, 10, 64, kPitchBendController, kChannelPressureController})
            menu.addItem(lane_name(cc), true, m_.cc_lane == cc, [this, cc] { set_cc_lane(cc); });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
    }

    int cc_value_at(float y) const
    {
        const auto v = velocity_area();
        const float frac = (static_cast<float>(v.getBottom()) - 4.0f - y) / static_cast<float>(v.getHeight() - 10);
        const int top = controller_max(m_.cc_lane);
        return juce::jlimit(0, top, juce::roundToInt(frac * static_cast<float>(top)));
    }

    void add_cc_point(juce::Point<float> p)
    {
        const Ticks t = std::max<Ticks>(0, static_cast<Ticks>(std::llround(m_.tick_of(p.x))));
        cc_points_.push_back({t, cc_value_at(p.y)});
    }

    /// A drag in the CC lane: its points, one per half grid step, replace
    /// the controller across the dragged span.
    void commit_cc()
    {
        const auto* l = m_.selected_lane();
        if (!l || cc_points_.empty()) return;
        auto pts = cc_points_;
        cc_points_.clear();
        std::sort(pts.begin(), pts.end());
        const Ticks step = std::max<Ticks>(1, grid() / 2);
        const Ticks from = (pts.front().first / step) * step;
        const Ticks to = std::max(from + step, (pts.back().first / step + 1) * step);
        const RegionView* region = nullptr;
        for (const auto& r : l->regions)
            if (r.lane == 0 && r.start <= from && from < r.end) region = &r;
        if (!region) {
            status("Draw controllers inside a MIDI region (record or draw notes first)");
            return;
        }
        std::vector<ControllerEvent> events;
        int last = -1;
        std::size_t k = 0;
        for (Ticks t = from; t < to; t += step) {
            while (k + 1 < pts.size() && pts[k + 1].first <= t) ++k;
            int v = pts[k].second;
            if (m_.cc_lane == 64) v = v >= 64 ? 127 : 0;
            if (v == last) continue;
            events.push_back(ControllerEvent{t, 0, m_.cc_lane, v});
            last = v;
        }
        if (m_.ctx.session.draw_region_controller(l->id, region->id, m_.cc_lane, from, to, events)) status("Drew " + lane_name(m_.cc_lane));
    }

    /// The CC lane: each main-lane region's values as steps.
    void paint_cc(juce::Graphics& g, const LaneView& l, juce::Rectangle<int> vel)
    {
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(vel);
        g.setColour(colours::background.darker(0.15f));
        g.fillRect(vel);
        paint_grid(g, m_, vel, 0, false);
        g.setColour(colours::outline);
        g.drawHorizontalLine(vel.getY(), static_cast<float>(vel.getX()), static_cast<float>(vel.getRight()));
        const float bottom = static_cast<float>(vel.getBottom()) - 4.0f;
        const float span = static_cast<float>(vel.getHeight() - 10);
        const float top_value = static_cast<float>(controller_max(m_.cc_lane));
        auto y_of_value = [&](int v) { return bottom - span * static_cast<float>(v) / top_value; };
        if (m_.cc_lane == kPitchBendController) {
            // Pitch bend: the centre line is "no bend".
            g.setColour(colours::text_dim.withAlpha(0.6f));
            g.drawHorizontalLine(juce::roundToInt(y_of_value(8192)), static_cast<float>(vel.getX()), static_cast<float>(vel.getRight()));
        }
        for (const auto& r : l.regions) {
            if (r.lane != 0) continue;
            std::vector<ControllerEvent> ev;
            for (const auto& e : r.controllers)
                if (e.controller == m_.cc_lane) ev.push_back(e);
            if (ev.empty()) continue;
            juce::Path p;
            const float x_end = static_cast<float>(m_.x_of(static_cast<double>(r.end)));
            for (std::size_t i = 0; i < ev.size(); ++i) {
                const float x0 = static_cast<float>(m_.x_of(static_cast<double>(ev[i].tick)));
                const float x1 = i + 1 < ev.size() ? static_cast<float>(m_.x_of(static_cast<double>(ev[i + 1].tick))) : x_end;
                const float y = y_of_value(ev[i].value);
                if (i == 0) p.startNewSubPath(x0, bottom);
                p.lineTo(x0, y);
                p.lineTo(x1, y);
            }
            p.lineTo(x_end, bottom);
            p.closeSubPath();
            g.setColour(colours::accent.withAlpha(0.25f));
            g.fillPath(p);
            g.setColour(colours::accent);
            g.strokePath(p, juce::PathStrokeType(1.4f));
        }
        // The score's crescendos and diminuendos: each swell a ramp from its
        // start value to its end value, with a handle at each end to drag.
        if (m_.cc_lane == 11) {
            for (const auto& h : swells(l.id)) {
                const float x0 = static_cast<float>(m_.x_of(static_cast<double>(h.start)));
                const float x1 = static_cast<float>(m_.x_of(static_cast<double>(h.end)));
                const float y0 = y_of_value(h.start_value), y1 = y_of_value(h.end_value);
                juce::Path ramp;
                ramp.startNewSubPath(x0, bottom);
                ramp.lineTo(x0, y0);
                ramp.lineTo(x1, y1);
                ramp.lineTo(x1, bottom);
                ramp.closeSubPath();
                g.setColour(colours::playhead.withAlpha(0.18f));
                g.fillPath(ramp);
                g.setColour(colours::playhead);
                g.drawLine(x0, y0, x1, y1, 1.8f);
                for (const auto& [x, y, val] : {std::tuple<float, float, int>{x0, y0, h.start_value}, {x1, y1, h.end_value}}) {
                    g.fillEllipse(x - 4.5f, y - 4.5f, 9.0f, 9.0f);
                    g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
                    g.drawText(juce::String(val), juce::Rectangle<float>(x - 16.0f, y - 18.0f, 32.0f, 12.0f),
                               juce::Justification::centred);
                }
            }
        }
        // The stroke being drawn.
        if (drag_ == Drag::Cc && cc_points_.size() > 1) {
            juce::Path p;
            for (std::size_t i = 0; i < cc_points_.size(); ++i) {
                const float x = static_cast<float>(m_.x_of(static_cast<double>(cc_points_[i].first)));
                const float y = y_of_value(cc_points_[i].second);
                if (i == 0) p.startNewSubPath(x, y);
                else p.lineTo(x, y);
            }
            g.setColour(colours::playhead);
            g.strokePath(p, juce::PathStrokeType(1.5f));
        }
    }

    std::vector<std::pair<Ticks, int>> cc_points_;

    enum class Drag { None, Create, Move, Resize, Velocity, PerfMove, PerfLength, PerfVelocity, Cc, Swell };

public:
    /// A hairpin's swell in the CC11 lane: which hairpin, and which end.
    struct SwellHandle {
        std::size_t staff = 0;
        Ticks start = 0, end = 0;
        bool at_end = false;
        int start_value = 0, end_value = 0;
    };
    /// The score's hairpins on a track, as the CC11 lane shows them.
    std::vector<SwellHandle> swells(const std::string& track_id) const
    {
        std::vector<SwellHandle> out;
        const auto* t = m_.ctx.session.data().find_track(track_id);
        const auto* v = t ? t->active_version() : nullptr;
        if (!v) return out;
        for (const auto& h : v->score.hairpins())
            out.push_back({h.staff, h.start_tick, h.end_tick, false, notation::hairpin_expression_start(h),
                           notation::hairpin_expression_end(h)});
        return out;
    }
    /// Sets a swell's value at one end (the check, and a handle drag, come here).
    bool set_swell(const std::string& track_id, const SwellHandle& h, int value)
    {
        const bool ok = m_.ctx.session.set_hairpin_expression(track_id, h.staff, h.start, h.at_end ? h.start_value : value,
                                                               h.at_end ? value : h.end_value);
        if (ok) status(juce::String(h.at_end ? "Swell ends at " : "Swell starts at ") + juce::String(value) + " (CC11)");
        repaint();
        return ok;
    }

private:
    SwellHandle swell_;

    std::optional<SwellHandle> swell_handle_at(const LaneView& l, juce::Point<float> p) const
    {
        const auto vel = velocity_area();
        const float bottom = static_cast<float>(vel.getBottom()) - 4.0f, span = static_cast<float>(vel.getHeight() - 10);
        for (auto h : swells(l.id))
            for (bool end : {true, false}) {
                const float x = static_cast<float>(m_.x_of(static_cast<double>(end ? h.end : h.start)));
                const float y = bottom - span * static_cast<float>(end ? h.end_value : h.start_value) / 127.0f;
                if (std::abs(p.x - x) <= 7.0f && std::abs(p.y - y) <= 9.0f) {
                    h.at_end = end;
                    return h;
                }
            }
        return std::nullopt;
    }

    void drag_swell(juce::Point<float> p)
    {
        const auto* l = m_.selected_lane();
        if (!l) return;
        const int v = cc_value_at(p.y);
        set_swell(l->id, swell_, v);
        (swell_.at_end ? swell_.end_value : swell_.start_value) = v;
    }

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
    /// The lane's name, a menu (inline, the roll's left part is under the track header).
    juce::Rectangle<int> lane_label_area() const { return {inset_, getHeight() - kVelocity, kHeader - inset_, 20}; }
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
            ok = m_.ctx.session.edit_performance(part_of(l->id), ids, gesture_label("Performance velocity"),
                                                 [delta](notation::EventId, project::NoteOverride& o) { o.velocity += delta; });
        } else {
            const bool onset = kind == Drag::PerfMove;
            ok = m_.ctx.session.edit_performance(part_of(l->id), ids, gesture_label(onset ? "Performance timing" : "Performance length"),
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
    using ScoreEdits = std::vector<std::pair<notation::EventId, session::NoteTarget>>;

    /// The track a score-note edit belongs to: the lane's own (articulation
    /// tracks hold their own notes).
    std::string part_of(const std::string& lane) const { return lane; }

    void commit_score_edit(Drag kind)
    {
        const auto* l = m_.selected_lane();
        if (!l || perf_sel_.empty()) return;
        auto& session = m_.ctx.session;
        const std::string track = l->id;
        const bool move = kind == Drag::PerfMove;
        const Ticks dt = dt_;
        const int dp = dp_;
        const bool free = free_;
        // Targets from what the roll showed when the drag began.
        ScoreEdits targets;
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
        hint_.clear();
        // Free (Alt): a performance, as before.
        if (free) {
            apply_score_edits(track, targets, label, false);
            return;
        }
        // Would the edit change the written music? (Memory Management spec
        // §2.3: never silently overwrite Composer content.)
        std::optional<session::NotatePlan> first;
        for (const auto& [sid, target] : targets) {
            auto plan = session.plan_note_edit(track, sid, target);
            if (plan.ok() && plan.alters_notation()) {
                first = std::move(plan);
                break;
            }
        }
        if (!first) {
            apply_score_edits(track, targets, label, true);
            return;
        }
        const auto policy = session.data().producer_edits;
        if (policy == "apply" || policy == "keep") {
            apply_score_edits(track, targets, label, policy == "apply");
            return;
        }
        ask_producer_edit(track, targets, label, *first);
    }

    /// Applies a gesture's note edits: written into the score (`to_score`:
    /// the recommended reading, asking which when one note can be read
    /// more than one way), or each kept as a Producer override.
    void apply_score_edits(const std::string& track, const ScoreEdits& targets, const std::string& label, bool to_score)
    {
        auto& session = m_.ctx.session;
        int written = 0, performed = 0;
        juce::String last;
        for (const auto& [sid, target] : targets) {
            auto plan = session.plan_note_edit(track, sid, target);
            if (!plan.ok()) continue;
            if (!to_score) {
                if (!plan.keep) continue;
                plan.options = {*plan.keep};
                plan.ambiguity.clear();
            } else if (targets.size() == 1 && !plan.ambiguity.empty()) {
                ask_reading(track, plan, label);
                return;
            }
            if (session.apply_notate(track, plan, 0, label, true)) {
                const auto& o = plan.options.front();
                (o.changes_notation ? written : performed)++;
                last = juce::String(session::describe(o));
            }
        }
        if (written + performed == 1)
            status(last);
        else if (written + performed > 1)
            status(juce::String(written) + " written into the score, " + juce::String(performed) + " kept as Producer overrides");
    }

    /// Decision 1: an edit made here that changes the written music asks
    /// whether the score takes it or Producer keeps it as an override; a
    /// tick makes the answer this project's rule (Edit Performance menu
    /// puts it back to asking).
    void ask_producer_edit(const std::string& track, const ScoreEdits& targets, const std::string& label,
                           const session::NotatePlan& plan)
    {
        const auto* t = m_.ctx.session.data().find_track(track);
        const juce::String what = juce::String(session::describe(plan.options.front()));
        auto* w = new juce::AlertWindow(
            "This changes the score",
            (targets.size() == 1 ? juce::String("The edit changes the written music") : juce::String(static_cast<int>(targets.size())) +
                                                                                            " edited notes change the written music") +
                (t ? " of " + juce::String(t->name) : juce::String()) + " (" + what +
                ").\n\nApply to Score: Composer shows it, and parts and exports print it.\nKeep Producer Override: the score "
                "stays as written; only playback changes, and Composer marks the note.",
            juce::MessageBoxIconType::QuestionIcon);
        w->setComponentID("producer_edit_question");
        auto always = std::make_shared<juce::ToggleButton>();
        always->setButtonText("Always do this for this project");  // no name: the dialog would print it as a label too
        always->setComponentID("always");
        always->setSize(320, 26);
        w->addCustomComponent(always.get());
        w->addButton("Apply to Score", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Keep Producer Override", 2);
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<PianoRoll> self(this);
        w->enterModalState(true, juce::ModalCallbackFunction::create([self, w, always, track, targets, label](int r) {
                               w->removeCustomComponent(0);
                               if (!self || r <= 0) {
                                   if (self) self->status("Nothing changed");
                                   return;
                               }
                               const bool to_score = r == 1;
                               if (always->getToggleState()) self->m_.ctx.session.set_producer_edits(to_score ? "apply" : "keep");
                               self->apply_score_edits(track, targets, label, to_score);
                           }),
                           true);
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
        menu.addItem(5, "Apply to Score (write them as they play)", any);
        menu.addSeparator();
        menu.addItem(3, "Select all score notes");
        menu.addItem(4, "Reset the whole track to as written", !l->performance.empty());
        // Decision 1: what an edit here that changes the written music does.
        const auto policy = m_.ctx.session.data().producer_edits;
        juce::PopupMenu rule;
        rule.addItem(21, "Ask each time", true, policy == "ask");
        rule.addItem(22, "Always apply to the score", true, policy == "apply");
        rule.addItem(23, "Always keep as Producer overrides", true, policy == "keep");
        menu.addSubMenu("Edits that change the score", rule);
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
                    publish_selection();
                    repaint();
                }
                break;
            case 4:
                if (session.clear_performance(track)) status("The whole track plays as written");
                break;
            case 5: {
                int left = 0;
                const int n = session.apply_overrides_to_score(track, ids, &left);
                status(n == 0 && left == 0 ? juce::String("Those notes play as written")
                       : juce::String(n) + " written into the score as they play" +
                             (left > 0 ? ", " + juce::String(left) + " cannot be written exactly and stay performances" : juce::String()));
                break;
            }
            case 21:
            case 22:
            case 23:
                session.set_producer_edits(r == 21 ? "ask" : r == 22 ? "apply" : "keep");
                status(r == 21 ? "Edits here that change the score will ask first"
                       : r == 22 ? "Edits here that change the score now apply to it (this project)"
                                 : "Edits here that change the score now stay Producer overrides (this project)");
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
        juce::CallOutBox::launchAsynchronously(std::make_unique<PerformancePanel>(m_.ctx, part_of(l->id), perf_sel_, m_.bpm),
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
            const bool moved = w != l.score_written.end() &&
                               (w->second.start != shown.start || w->second.duration != shown.duration || w->second.pitch != shown.pitch);
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
        // Lit keys: the pitches of the selected notes, in the LED colour.
        std::array<bool, 128> lit{};
        if (l) {
            for (auto sid : perf_sel_)
                if (const int i = score_index(*l, sid); i >= 0)
                    lit[static_cast<std::size_t>(juce::jlimit(0, 127, l->score_notes[static_cast<std::size_t>(i)].pitch))] = true;
            for (const auto& k : selection_) lit[static_cast<std::size_t>(juce::jlimit(0, 127, k.pitch))] = true;
        }
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
            if (lit[static_cast<std::size_t>(p)]) {
                const auto led = hw::Theme::get().led();
                g.setColour(led);
                g.fillRect(juce::Rectangle<float>(static_cast<float>(piano_x), y, 61.0f, kh));
                g.setColour(led.darker(0.6f));
                g.drawHorizontalLine(juce::roundToInt(y + kh) - 1, static_cast<float>(piano_x), static_cast<float>(kHeader - 1));
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
        g.drawFittedText(lane_name(m_.cc_lane) + juce::String::fromUTF8(" \xe2\x96\xbe"),
                         lane_label_area().reduced(4, 0).withTrimmedTop(4), juce::Justification::topLeft, 2);
        if (m_.cc_lane >= 0) {
            paint_cc(g, l, vel);
            return;
        }
        g.setFont(juce::FontOptions(9.5f));
        g.drawText("127", juce::Rectangle<int>(10, vel.getY() + 18, kHeader - 18, 12), juce::Justification::centredRight);  // under the name
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

    int last_grid_height_ = -1;
    std::size_t note_count_ = 0;
    int inset_ = 0;
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
        for (juce::Component* comp : std::initializer_list<juce::Component*>{&toolbar, &ruler, &lanes, &hscroll})
            owner.addAndMakeVisible(comp);
        // Width and height, bottom right - as in the Composer.
        for (auto* z : {&zoom_w, &zoom_h}) {
            z->setSliderStyle(juce::Slider::LinearHorizontal);
            z->setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            owner.addAndMakeVisible(*z);
        }
        zoom_w.setComponentID("producer_zoom_width");
        zoom_h.setComponentID("producer_zoom_height");
        zoom_w.setTitle("Zoom width");
        zoom_h.setTitle("Zoom height");
        zoom_w.setTooltip("Width: more or less time across the window (double-click: 100%)");
        zoom_h.setTooltip("Height: taller or shorter tracks (double-click: 100%)");
        zoom_w.setRange(3.0, 480.0);
        zoom_w.setSkewFactorFromMidPoint(24.0);
        zoom_w.setDoubleClickReturnValue(true, 24.0);
        zoom_h.setRange(0.5, 3.0);
        zoom_h.setSkewFactorFromMidPoint(1.0);
        zoom_h.setDoubleClickReturnValue(true, 1.0);
        zoom_w.setValue(model.px_per_quarter, juce::dontSendNotification);
        zoom_h.setValue(model.height_zoom, juce::dontSendNotification);
        zoom_w.onValueChange = [this] {
            const double anchor = model.view_start + model.visible_ticks() / 2.0;
            model.px_per_quarter = zoom_w.getValue();
            model.view_start = std::max(0.0, anchor - model.visible_ticks() / 2.0);
            view_changed();
        };
        zoom_h.onValueChange = [this] {
            model.height_zoom = zoom_h.getValue();
            lanes.update_scroll();
            lanes.layout_roll();
            lanes.repaint();
        };
        for (auto* l : {&zoom_w_label, &zoom_h_label}) {
            l->setColour(juce::Label::textColourId, colours::text_dim);
            l->setFont(hw::fonts::ui(10.5f));
            l->setJustificationType(juce::Justification::centredRight);
            owner.addAndMakeVisible(*l);
        }
        zoom_w_label.setText("W", juce::dontSendNotification);
        zoom_h_label.setText("H", juce::dontSendNotification);
        // The piano roll opens inline, in the expanded track's row (docs/spec/ui).
        lanes.set_roll(&roll, [this](int inset) { roll.set_inset(inset); });
        model.quantize_action = [this](const project::QuantizeSettings& q) { return roll.quantize(q); };
        model.velocity_action = [this](const project::VelocityChange& v) { return roll.change_velocity(v); };
        model.expanded_changed = [this] {
            toolbar.roll_button().setToggleState(!model.expanded.empty(), juce::dontSendNotification);
            lanes.update_scroll();
            lanes.ensure_visible(model.expanded);
            roll.document_changed();
            lanes.repaint();
        };
        style_scrollbar(hscroll);
        hscroll.addListener(this);
        toolbar.roll_button().onClick = [this] { set_roll_visible(toolbar.roll_button().getToggleState()); };
        model.view_changed = [this] { view_changed(); };
        model.print_track = [this](const std::string& track, bool freeze) {
            if (owner.on_print_track) owner.on_print_track(track, freeze);
        };
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
        listener = c.session.add_listener([this](session::Change change) {
            if (change == session::Change::Document) {
                // Showing: taken in at once (the view's own key and mouse
                // handlers act on its lanes straight after an edit - a
                // nudge, then another). Hidden behind the Composer view:
                // once, when it is shown again.
                if (owner.isShowing()) document_refresh();
                else deferred.request();
            } else if (change == session::Change::Selection) {
                deferred.flush();
                // An open piano roll follows the selected track.
                if (!model.expanded.empty()) {
                    const auto* sel = model.selected_lane();
                    if (sel && model.has_audio_lane(*sel)) model.set_expanded(sel->id);
                }
                roll.document_changed();
                lanes.ensure_visible(model.ctx.session.selected_track());
                owner.repaint();
            }
        });
        model.rebuild();
        lanes.layout_automation(true);
        generation = c.session.document_generation();
        // The selected instrument track starts with its piano roll open.
        if (const auto* sel = model.selected_lane(); sel && model.has_audio_lane(*sel)) model.set_expanded(sel->id);
        toolbar.roll_button().setToggleState(!model.expanded.empty(), juce::dontSendNotification);
    }

    ~Impl() override { model.ctx.session.remove_listener(listener); }

    /// Opens or closes the selected track's piano roll inline.
    void set_roll_visible(bool visible)
    {
        const auto* sel = model.selected_lane();
        model.set_expanded(visible && sel && model.has_audio_lane(*sel) ? sel->id : std::string());
        toolbar.roll_button().setToggleState(!model.expanded.empty(), juce::dontSendNotification);
    }

    void layout()
    {
        auto r = owner.getLocalBounds();
        model.timeline_width = std::max(10, r.getWidth() - kHeader - kScroll);
        toolbar.setBounds(r.removeFromTop(kToolbar));
        ruler.setBounds(r.removeFromTop(kRuler));
        // The bottom row: the scroll bar, then the W and H sliders at the right.
        auto bottom = r.removeFromBottom(std::max(kHScroll, 18));
        auto sliders = bottom.removeFromRight(260);
        zoom_h.setBounds(sliders.removeFromRight(110).reduced(2, 1));
        zoom_h_label.setBounds(sliders.removeFromRight(16));
        zoom_w.setBounds(sliders.removeFromRight(110).reduced(2, 1));
        zoom_w_label.setBounds(sliders.removeFromRight(16));
        hscroll.setBounds(bottom.withTrimmedLeft(kHeader).withSizeKeepingCentre(bottom.getWidth() - kHeader, kHScroll));
        lanes.setBounds(r);
        lanes.layout_roll();
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
        zoom_w.setValue(model.px_per_quarter, juce::dontSendNotification);
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
        // Called while the view is shown: anything deferred while hidden, now.
        if (deferred.pending()) deferred.flush();
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
        if (playing && model.ctx.follow) {
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
    PianoRoll roll;
    juce::ScrollBar hscroll{false};
    juce::Slider zoom_w, zoom_h;
    juce::Label zoom_w_label, zoom_h_label;
    int listener = 0;
    /// A document change taken in.
    void document_refresh()
    {
        model.rebuild();
        // A new document opens with its selected track's piano roll inline.
        if (model.ctx.session.document_generation() != generation) {
            generation = model.ctx.session.document_generation();
            const auto* sel = model.selected_lane();
            model.expanded = sel && model.has_audio_lane(*sel) ? sel->id : std::string();
            model.heights.clear();
            model.folded_parts.clear();
            toolbar.roll_button().setToggleState(!model.expanded.empty(), juce::dontSendNotification);
        }
        lanes.update_scroll();
        lanes.layout_automation(true);
        roll.document_changed();
        update_hscroll();
        owner.repaint();
    }
    Deferred deferred{[this] { document_refresh(); }};
    std::uint64_t generation = 0;
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

void SequencerView::show_automation(const std::string& owner_id, bool show)
{
    impl_->deferred.flush();
    impl_->model.set_automation_shown(owner_id, show);
}

void SequencerView::set_part_folded(const std::string& part_id, bool folded)
{
    impl_->deferred.flush();
    impl_->lanes.set_part_folded(part_id, folded);
}

bool SequencerView::part_folded(const std::string& part_id) const
{
    return impl_->model.folded_parts.count(part_id) > 0;
}

int SequencerView::shown_track_rows() const
{
    impl_->deferred.flush();
    int n = 0;
    for (const auto& l : impl_->model.lanes)
        if (!l.bus) ++n;
    return n;
}

bool SequencerView::automation_shown(const std::string& owner_id) const
{
    impl_->deferred.flush();
    return impl_->model.show_automation.count(owner_id) > 0;
}

void SequencerView::hide_all_automation()
{
    impl_->deferred.flush();
    impl_->model.show_automation.clear();
    if (impl_->model.automation_layout_changed) impl_->model.automation_layout_changed();
}

bool SequencerView::pencil_mode() const
{
    impl_->deferred.flush();
    return impl_->model.pencil;
}

void SequencerView::set_pencil_mode(bool on)
{
    impl_->deferred.flush();
    impl_->model.set_pencil_mode(on);
}

void SequencerView::zoom(bool in)
{
    impl_->deferred.flush();
    auto& m = impl_->model;
    m.zoom_about(in ? 1.25 : 0.8, kHeader + m.timeline_width / 2.0);
}

void SequencerView::notate_region(const std::string& region_id)
{
    impl_->deferred.flush();
    impl_->roll.open_region_notation(region_id);
}

void SequencerView::set_playhead(Ticks tick, bool playing) { impl_->set_playhead(tick, playing); }

void SequencerView::visibilityChanged()
{
    // Shown again: whatever changed while it was hidden, now.
    if (isShowing()) impl_->deferred.flush();
}

void SequencerView::bring_up_to_date() { impl_->deferred.flush(); }

void SequencerView::reveal_track(const std::string& track_id)
{
    impl_->lanes.ensure_visible(track_id);
    impl_->lanes.repaint();
}

void SequencerView::scroll_to_start()
{
    impl_->deferred.flush();
    impl_->model.set_view_start(0.0);
    impl_->lanes.repaint();
    repaint();
}

void SequencerView::reveal(Ticks tick)
{
    impl_->deferred.flush();
    auto& model = impl_->model;
    const double x = model.x_of(static_cast<double>(tick));
    if (x > kHeader + model.timeline_width - 24 || x < kHeader)
        model.set_view_start(static_cast<double>(tick) - model.visible_ticks() * 0.08);
}

std::pair<Ticks, Ticks> SequencerView::range() const
{
    impl_->deferred.flush();
    return {impl_->model.range_start, impl_->model.range_end};
}

void SequencerView::set_range(Ticks start, Ticks end)
{
    impl_->deferred.flush();
    impl_->model.range_start = std::max<Ticks>(0, std::min(start, end));
    impl_->model.range_end = std::max<Ticks>(0, std::max(start, end));
    repaint();
    if (on_range_changed) on_range_changed();
}

void SequencerView::set_punch_display(bool on)
{
    impl_->deferred.flush();
    impl_->model.punch = on;
    repaint();
}

bool SequencerView::promote_take(session::Session& session, const std::string& track_id, const std::string& clip_id)
{
    return LaneArea::promote_take(session, track_id, clip_id);
}

bool SequencerView::split_at_playhead()
{
    impl_->deferred.flush();
    return impl_->lanes.split_at_playhead();
}
int SequencerView::quantize_notes(const project::QuantizeSettings& q)
{
    impl_->deferred.flush();
    return impl_->roll.quantize(q);
}
int SequencerView::change_velocities(const project::VelocityChange& v)
{
    impl_->deferred.flush();
    return impl_->roll.change_velocity(v);
}
void SequencerView::set_cc_lane(int cc)
{
    impl_->deferred.flush();
    impl_->roll.set_cc_lane(cc);
}
int SequencerView::cc_lane() const
{
    impl_->deferred.flush();
    return impl_->model.cc_lane;
}
std::vector<SequencerView::Swell> SequencerView::swells(const std::string& track_id) const
{
    std::vector<Swell> out;
    for (const auto& h : impl_->roll.swells(track_id)) out.push_back({h.staff, h.start, h.end, h.start_value, h.end_value});
    return out;
}

bool SequencerView::set_swell(const std::string& track_id, std::size_t index, bool at_end, int value)
{
    auto all = impl_->roll.swells(track_id);
    if (index >= all.size()) return false;
    auto h = all[index];
    h.at_end = at_end;
    return impl_->roll.set_swell(track_id, h, value);
}

bool SequencerView::draw_cc(int cc, Ticks from, Ticks to, int v0, int v1)
{
    impl_->deferred.flush();
    return impl_->roll.draw_cc(cc, from, to, v0, v1);
}
bool SequencerView::crossfade_selected(double seconds)
{
    impl_->deferred.flush();
    return impl_->lanes.crossfade_selected(seconds);
}
bool SequencerView::consolidate_selected()
{
    impl_->deferred.flush();
    return impl_->lanes.consolidate_selected();
}
bool SequencerView::nudge_selected(double seconds)
{
    impl_->deferred.flush();
    return impl_->lanes.nudge_selected(seconds);
}

void SequencerView::show_take_lanes(const std::string& track_id, bool show)
{
    impl_->deferred.flush();
    auto& m = impl_->model;
    if (show) m.show_takes.insert(track_id);
    else m.show_takes.erase(track_id);
    impl_->lanes.update_scroll();
    impl_->lanes.repaint();
}

int SequencerView::selected_clip_count() const
{
    impl_->deferred.flush();
    return static_cast<int>(impl_->model.selected_clips().size());
}

std::vector<std::pair<std::string, std::string>> SequencerView::selected_clips() const
{
    impl_->deferred.flush();
    return impl_->model.selected_clips();
}

std::pair<std::string, std::string> SequencerView::selected_clip() const
{
    impl_->deferred.flush();
    return {impl_->model.clip_track, impl_->model.clip_id};
}

void SequencerView::select_clip(const std::string& track_id, const std::string& clip_id)
{
    impl_->deferred.flush();
    auto& m = impl_->model;
    context_.session.select_track(track_id);
    const LaneView* lane = nullptr;
    const auto* clip = m.clip(track_id, clip_id, &lane);
    if (!clip) return;
    m.clip_track = track_id;
    m.clip_id = clip_id;
    const double start = static_cast<double>(m.tempo.tick_at_seconds(clip->start_seconds));
    if (start < m.view_start || start > m.view_start + m.visible_ticks() * 0.9) m.set_view_start(start - m.visible_ticks() * 0.1);
    impl_->lanes.ensure_visible(track_id);
    impl_->lanes.repaint();
}

std::string SequencerView::expanded_track() const
{
    impl_->deferred.flush();
    return impl_->model.expanded;
}

double SequencerView::view_start() const
{
    impl_->deferred.flush();
    return impl_->model.view_start;
}

void SequencerView::set_expanded_track(const std::string& track_id)
{
    impl_->deferred.flush();
    if (!track_id.empty()) context_.session.select_track(track_id);
    impl_->model.set_expanded(track_id);
}

int SequencerView::row_height(const std::string& track_id) const
{
    impl_->deferred.flush();
    const auto* l = impl_->model.lane(track_id);
    return l ? impl_->model.base_height(*l) : 0;
}

juce::Colour SequencerView::lane_colour(const std::string& track_id) const
{
    impl_->deferred.flush();
    const auto* l = impl_->model.lane(track_id);
    return l ? l->colour : juce::Colour();
}

void SequencerView::set_row_height(const std::string& track_id, int height)
{
    impl_->deferred.flush();
    auto& m = impl_->model;
    const auto* l = m.lane(track_id);
    if (!l) return;
    m.heights[m.is_expanded(*l) ? track_id + "#x" : track_id] = m.stored_height(juce::jlimit(m.min_height(*l), 600, height));
    impl_->lanes.update_scroll();
    impl_->lanes.repaint();
}

SequencerView::EditMode SequencerView::edit_mode() const
{
    impl_->deferred.flush();
    return impl_->model.edit_mode;
}
void SequencerView::set_edit_mode(EditMode mode)
{
    impl_->deferred.flush();
    impl_->model.set_edit_mode(mode);
    impl_->lanes.repaint();
}
bool SequencerView::smart_tool() const
{
    impl_->deferred.flush();
    return impl_->model.smart;
}
void SequencerView::set_smart_tool(bool on)
{
    impl_->deferred.flush();
    impl_->model.set_smart(on);
}
void SequencerView::select_track_range(const std::vector<std::string>& track_ids, Ticks from, Ticks to)
{
    impl_->deferred.flush();
    impl_->lanes.set_smart_range({track_ids.begin(), track_ids.end()}, std::min(from, to), std::max(from, to));
}
bool SequencerView::delete_selection()
{
    impl_->deferred.flush();
    return impl_->lanes.delete_selection();
}
juce::Rectangle<float> SequencerView::clip_bounds(const std::string& track_id, const std::string& clip_id) const
{
    impl_->deferred.flush();
    return impl_->lanes.clip_bounds(track_id, clip_id);
}
bool SequencerView::spot_selected(double seconds)
{
    impl_->deferred.flush();
    const auto& m = impl_->model;
    return !m.clip_id.empty() && impl_->lanes.spot(m.clip_track, m.clip_id, seconds);
}

juce::String SequencerView::spot_text(const project::ProjectData& d, double seconds, int scale)
{
    seconds = std::max(0.0, seconds);
    if (scale == 2) return juce::String(picture::project_timecode(d, seconds));
    if (scale == 1) {
        const int minutes = static_cast<int>(seconds / 60.0);
        return juce::String(minutes) + ":" + juce::String(seconds - minutes * 60.0, 3).paddedLeft('0', 6);
    }
    return shell::bars_beats_ticks(d.meter, d.tempo.tick_at_seconds(seconds));
}

std::optional<double> SequencerView::parse_spot_text(const project::ProjectData& d, const juce::String& text, int scale)
{
    const auto t = text.trim();
    if (t.isEmpty()) return std::nullopt;
    if (scale == 2) {
        const auto s = picture::project_seconds_for_timecode(d, t.toStdString());
        if (!s) return std::nullopt;
        return std::max(0.0, *s);
    }
    juce::StringArray parts;
    if (scale == 1) {
        // [h:]m:ss.sss, or plain seconds.
        parts.addTokens(t, ":", "");
        double s = 0.0;
        for (const auto& p : parts) {
            if (!p.trim().containsOnly("0123456789.")) return std::nullopt;
            s = s * 60.0 + p.trim().getDoubleValue();
        }
        return s;
    }
    // Bars|Beats|ticks (ticks in 960ths of a quarter); "|", "." or ":" between.
    parts.addTokens(t, "|.: ", "");
    parts.removeEmptyStrings();
    if (parts.isEmpty() || parts.size() > 3) return std::nullopt;
    for (const auto& p : parts)
        if (!p.containsOnly("0123456789")) return std::nullopt;
    const int bar = std::max(1, parts[0].getIntValue());
    const int beat = parts.size() > 1 ? std::max(1, parts[1].getIntValue()) : 1;
    const int tick = parts.size() > 2 ? std::max(0, parts[2].getIntValue()) : 0;
    const auto sig = d.meter.at_bar(bar);
    const Ticks at = d.meter.bar_start(bar) + static_cast<Ticks>(beat - 1) * sig.beat_ticks() +
                     static_cast<Ticks>(tick) * (kTicksPerQuarter / 960);
    return d.tempo.seconds_at(at);
}

void SequencerView::paint(juce::Graphics& g) { g.fillAll(colours::background); }

void SequencerView::resized() { impl_->layout(); }

}  // namespace vcc::desktop
