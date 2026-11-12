#include "ui/SequencerView.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>

#include "ui/Theme.h"
#include "vcc/project/Render.h"

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

const juce::String kScoreHint = "Written in the score - edit in the Composer view";

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
    std::vector<NoteEvent> score_notes;
    std::vector<RegionView> regions;
    int lo = 48, hi = 72;  ///< Pitch span for the lane's mini roll.
    int range_lo = 21, range_hi = 108;
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

struct Model {
    explicit Model(AppContext& c) : ctx(c) {}

    AppContext& ctx;
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
            if (v && !l.reference) {
                project::Track score_only = *t;
                score_only.active_version()->regions.clear();
                try {
                    l.score_notes = project::render_track(score_only).notes;
                } catch (...) {
                    l.score_notes.clear();
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

// --- Toolbar ---------------------------------------------------------------------------------------

class Toolbar final : public juce::Component {
public:
    explicit Toolbar(Model& m) : m_(m)
    {
        for (auto* b : {&zoom_out_, &zoom_in_, &fit_, &roll_}) addAndMakeVisible(*b);
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
        int lanes = static_cast<int>(m_.lanes.size());
        juce::String info = juce::String(lanes) + (lanes == 1 ? " track" : " tracks") + juce::String::fromUTF8("  \xc2\xb7  ") +
                            juce::String(m_.meter.bar_at(std::max<Ticks>(0, m_.end - 1))) + " bars";
        g.drawText(info, getLocalBounds().withTrimmedLeft(roll_.getRight() + 16).withTrimmedRight(12),
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
    }

private:
    Model& m_;
    juce::TextButton zoom_out_{"-"}, zoom_in_{"+"}, fit_{"Fit"}, roll_{"Piano Roll"};
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

    void mouseDown(const juce::MouseEvent& e) override { locate(e); }
    void mouseDrag(const juce::MouseEvent& e) override { locate(e); }

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
    void locate(const juce::MouseEvent& e)
    {
        if (e.x < kHeader) return;
        const Ticks tick = m_.snap(static_cast<Ticks>(m_.tick_of(e.x)), kTicksPerQuarter / 4, false);
        m_.ctx.engine.locate(tick);
        m_.playhead = tick;
        if (auto* p = getParentComponent()) p->repaint();
    }

    Model& m_;
};

// --- Track lanes -------------------------------------------------------------------------------------

class LaneArea final : public juce::Component, private juce::ScrollBar::Listener {
public:
    explicit LaneArea(Model& m) : m_(m)
    {
        addAndMakeVisible(vscroll_);
        style_scrollbar(vscroll_);
        vscroll_.addListener(this);
    }

    void update_scroll()
    {
        const int content = static_cast<int>(m_.lanes.size()) * kLaneHeight;
        const int visible = getHeight();
        scroll_y_ = juce::jlimit(0, std::max(0, content - visible), scroll_y_);
        vscroll_.setRangeLimits(0.0, std::max(content, visible));
        vscroll_.setCurrentRange(scroll_y_, visible, juce::dontSendNotification);
    }

    void ensure_visible(const std::string& id)
    {
        for (std::size_t i = 0; i < m_.lanes.size(); ++i) {
            if (m_.lanes[i].id != id) continue;
            const int y = static_cast<int>(i) * kLaneHeight;
            if (y < scroll_y_) scroll_y_ = y;
            else if (y + kLaneHeight > scroll_y_ + getHeight()) scroll_y_ = y + kLaneHeight - getHeight();
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
        for (std::size_t i = 0; i < m_.lanes.size(); ++i) {
            const int y = static_cast<int>(i) * kLaneHeight - scroll_y_;
            if (y + kLaneHeight < 0 || y > h) continue;
            paint_lane(g, m_.lanes[i], juce::Rectangle<int>(0, y, w - kScroll, kLaneHeight), timeline,
                       m_.lanes[i].id == selected);
        }
        if (m_.lanes.empty()) {
            g.setColour(colours::text_dim);
            g.setFont(juce::FontOptions(14.0f));
            g.drawText("No tracks yet - add a track or a template", timeline, juce::Justification::centred);
        }
        g.setColour(colours::outline);
        g.drawVerticalLine(kHeader - 1, 0.0f, static_cast<float>(h));
        paint_playhead(g, m_, timeline);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (const auto* l = lane_at(e.y)) m_.ctx.session.select_track(l->id);
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (const auto* l = lane_at(e.y)) {
            m_.ctx.session.select_track(l->id);
            if (m_.open_piano_roll) m_.open_piano_roll();
        }
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCommandDown() || e.mods.isCtrlDown()) {
            m_.zoom_about(w.deltaY > 0 ? 1.2 : 1.0 / 1.2, std::max(e.x, kHeader));
            return;
        }
        const int content = static_cast<int>(m_.lanes.size()) * kLaneHeight;
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

private:
    void scrollBarMoved(juce::ScrollBar*, double start) override
    {
        scroll_y_ = juce::roundToInt(start);
        repaint();
    }

    const LaneView* lane_at(int y) const
    {
        const int i = (y + scroll_y_) / kLaneHeight;
        if (y + scroll_y_ < 0 || i >= static_cast<int>(m_.lanes.size())) return nullptr;
        return &m_.lanes[static_cast<std::size_t>(i)];
    }

    void paint_lane(juce::Graphics& g, const LaneView& l, juce::Rectangle<int> row, juce::Rectangle<int> timeline,
                    bool selected)
    {
        // Header.
        auto header = row.withWidth(kHeader);
        g.setColour(selected ? colours::panel_raised : colours::panel);
        g.fillRect(header.withTrimmedRight(1));
        g.setColour(l.colour);
        g.fillRect(header.withWidth(5));
        auto text = header.reduced(14, 7).withTrimmedLeft(-2);
        g.setColour(l.reference ? colours::accent_warm : (selected ? colours::text : colours::text.withAlpha(0.85f)));
        g.setFont(juce::FontOptions(13.0f, selected ? juce::Font::bold : juce::Font::plain));
        g.drawText(l.name, text.removeFromTop(18), juce::Justification::centredLeft, true);
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(l.reference ? juce::String("Temp / reference") : l.version, text.removeFromTop(15),
                   juce::Justification::centredLeft, true);
        if (!l.reference) {
            int count = static_cast<int>(l.score_notes.size());
            for (const auto& r : l.regions) count += static_cast<int>(r.notes.size());
            if (count > 0) {
                g.setFont(juce::FontOptions(10.0f));
                g.drawText(juce::String(count) + (count == 1 ? " note" : " notes"), text, juce::Justification::centredLeft);
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
        g.drawHorizontalLine(row.getBottom() - 1, 0.0f, static_cast<float>(row.getRight()));

        if (l.reference) {
            g.setColour(colours::accent_warm.withAlpha(0.06f));
            for (int x = body.getX() - body.getHeight(); x < body.getRight(); x += 14)
                g.drawLine(static_cast<float>(x), static_cast<float>(body.getBottom()),
                           static_cast<float>(x + body.getHeight()), static_cast<float>(body.getY()), 1.0f);
            g.setColour(colours::accent_warm.withAlpha(0.6f));
            g.setFont(juce::FontOptions(11.0f, juce::Font::italic));
            g.drawText("Temp / reference - no MIDI", body.reduced(10, 0), juce::Justification::centredLeft);
            return;
        }

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

class PianoRoll final : public juce::Component, private juce::ScrollBar::Listener {
public:
    explicit PianoRoll(Model& m) : m_(m)
    {
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
            drag_ = Drag::None;
            centre_on_notes();
        }
        // Drop selected notes that no longer exist.
        if (l) {
            std::vector<NoteKey> kept;
            for (const auto& k : selection_)
                if (region_note(*l, k)) kept.push_back(k);
            selection_ = std::move(kept);
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

        if (!l || l->reference) {
            g.setColour(colours::panel);
            g.fillRect(getLocalBounds().withTrimmedTop(kRollHeader));
            g.setColour(colours::text_dim);
            g.setFont(juce::FontOptions(14.0f));
            g.drawText(l ? "Temp / reference tracks hold no MIDI - select an instrument track"
                         : "Select an instrument track to edit its MIDI",
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
        if (l && !l->reference && grid_area().contains(e.getPosition())) {
            bool edge = false;
            if (hit_region_note(*l, e.position, edge)) {
                shape = edge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::DraggingHandCursor;
            } else if (hit_score_note(*l, e.position)) {
                hint = kScoreHint;
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
        if (!l || l->reference) return;
        const auto pos = e.position;

        if (keyboard_area().contains(e.getPosition())) {
            audition(*l, pitch_at(pos.y));
            return;
        }

        if (velocity_area().contains(e.getPosition())) {
            if (auto key = velocity_hit(*l, pos.x)) {
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
        if (auto hit = hit_region_note(*l, pos, edge)) {
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
        if (hit_score_note(*l, pos)) {
            if (m_.ctx.status) m_.ctx.status(kScoreHint);
            hint_ = kScoreHint;
            repaint();
            return;
        }
        if (!e.mods.isShiftDown()) selection_.clear();
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
            case Drag::Velocity: vel_value_ = velocity_at(pos.y); break;
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
            if (selection_.empty()) return false;
            commit_delete();
            return true;
        }
        if (key == juce::KeyPress::escapeKey) {
            selection_.clear();
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
    enum class Drag { None, Create, Move, Resize, Velocity };

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

    bool hit_score_note(const LaneView& l, juce::Point<float> p) const
    {
        for (const auto& n : l.score_notes)
            if (note_rect(n).contains(p)) return true;
        return false;
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
                                      : juce::String("Click and drag to draw notes  -  Shift-click to add to the selection");
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
            item("MIDI (editable)", false);
            item("Score (read-only)", true);
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

        // Score-linked notes: ghosted, read-only.
        for (const auto& n : l.score_notes) {
            const auto r = note_rect(n);
            if (!r.intersects(gf)) continue;
            g.setColour(l.colour.withAlpha(0.22f));
            g.fillRoundedRectangle(r, 2.0f);
            g.setColour(l.colour.withAlpha(0.6f));
            g.drawRoundedRectangle(r.reduced(0.5f), 2.0f, 1.0f);
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
        for (const auto& n : l.score_notes) bar(n.start, n.velocity, l.colour.withAlpha(0.3f));
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
        if (tick == model.playhead && playing == model.playing) return;
        const Ticks old = model.playhead;
        model.playhead = tick;
        model.playing = playing;
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

SequencerView::SequencerView(AppContext& context) : impl_(std::make_unique<Impl>(*this, context)), context_(context) {}

SequencerView::~SequencerView() = default;

void SequencerView::set_playhead(Ticks tick, bool playing) { impl_->set_playhead(tick, playing); }

void SequencerView::paint(juce::Graphics& g) { g.fillAll(colours::background); }

void SequencerView::resized() { impl_->layout(); }

}  // namespace vcc::desktop
