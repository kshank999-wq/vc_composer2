#include "ui/composer/ComposerRuler.h"

#include <algorithm>
#include <cmath>

#include "ui/Theme.h"
#include "ui/composer/ChordDetect.h"
#include "ui/hw/HwControls.h"
#include "vcc/theory/Pitch.h"

namespace vcc::desktop::composer_ui {

ComposerRuler::ComposerRuler(AppContext& context, TimelineAccess access) : context_(context), access_(std::move(access))
{
    setOpaque(true);
    hw::Theme::get().addChangeListener(this);
    listener_ = context_.session.add_listener([this](session::Change c) {
        if (c == session::Change::Document) refresh();
        if (c == session::Change::Selection) repaint();
    });
    refresh();
}

ComposerRuler::~ComposerRuler()
{
    hw::Theme::get().removeChangeListener(this);
    context_.session.remove_listener(listener_);
}

void ComposerRuler::set_playhead(Ticks tick)
{
    if (tick == playhead_) return;
    playhead_ = tick;
    repaint();
}

Ticks ComposerRuler::song_end() const
{
    return std::max<Ticks>(context_.session.content_end(), 8 * kTicksPerWhole);
}

void ComposerRuler::refresh()
{
    const auto& session = context_.session;
    const auto& d = session.data();
    // Chords: every pitched track's sounding notes, bar by bar.
    std::vector<NoteEvent> all;
    lanes_.clear();
    for (const auto* t : session.music_tracks()) {
        if (t->kind != project::TrackKind::Instrument && t->kind != project::TrackKind::MidiOnly) continue;
        const auto& r = render_.render(*t);
        if (t->midi_channel != 9) all.insert(all.end(), r.notes.begin(), r.notes.end());
        Lane lane;
        lane.colour = colour_from_hex(t->color);
        if (const auto* v = t->active_version(); v && !v->phrases.empty()) {
            for (const auto& p : v->phrases) lane.spans.emplace_back(p.start, p.end);
        } else {
            // Bars that have notes, merged into runs.
            for (const auto& n : r.notes) {
                const Ticks a = d.meter.bar_start(d.meter.bar_at(n.start));
                const Ticks b = d.meter.bar_start(d.meter.bar_at(std::max<Ticks>(n.start, n.end() - 1)) + 1);
                if (!lane.spans.empty() && a <= lane.spans.back().second)
                    lane.spans.back().second = std::max(lane.spans.back().second, b);
                else
                    lane.spans.emplace_back(a, b);
            }
        }
        lanes_.push_back(std::move(lane));
    }
    std::sort(all.begin(), all.end());
    const int bars = d.meter.bar_at(song_end()) + 1;
    // Roots spelled as the key signature does: flats in a flat key.
    const auto key = theory::Key::parse(d.key);
    const bool flats = key && key->fifths() < 0;
    chords_ = chord_symbols(all, d.meter, bars, flats);
    repaint();
}

juce::Rectangle<int> ComposerRuler::overview_area() const
{
    return {kLabelWidth, 2, std::max(0, getWidth() - kLabelWidth - 4), kOverview - 4};
}

juce::Rectangle<int> ComposerRuler::row(int i) const
{
    return {kLabelWidth, kOverview + 6 + i * kRow, std::max(0, getWidth() - kLabelWidth), kRow};
}

void ComposerRuler::paint(juce::Graphics& g)
{
    const auto& theme = hw::Theme::get();
    const auto& d = context_.session.data();
    g.fillAll(hw::tok::panel_bottom);

    // --- Overview: the whole piece, one thin row per track ------------------
    const auto ov = overview_area();
    hw::paint::silk(g, "Overview", {12.0f, 0.0f, 120.0f, static_cast<float>(kOverview)}, 10.0f);
    hw::paint::well(g, ov.toFloat(), 3.0f);
    const double end = static_cast<double>(song_end());
    auto ox = [&](Ticks t) { return static_cast<float>(ov.getX() + 2 + (ov.getWidth() - 4) * static_cast<double>(t) / end); };
    if (!lanes_.empty()) {
        const float h = std::max(1.0f, std::min(3.0f, (static_cast<float>(ov.getHeight()) - 6.0f) / static_cast<float>(lanes_.size())));
        float y = static_cast<float>(ov.getY()) + 3.0f;
        for (const auto& lane : lanes_) {
            g.setColour(lane.colour.withAlpha(0.85f));
            for (const auto& [a, b] : lane.spans) g.fillRect(juce::Rectangle<float>(ox(a), y, std::max(1.0f, ox(b) - ox(a)), h));
            y += h + (h >= 2.0f ? 1.0f : 0.0f);
        }
    }
    {
        const Ticks o = access_.origin ? access_.origin() : 0;
        const Ticks v = access_.visible ? access_.visible() : 0;
        const auto box = juce::Rectangle<float>(ox(o), static_cast<float>(ov.getY()) + 1.0f, std::max(4.0f, ox(o + v) - ox(o)),
                                                static_cast<float>(ov.getHeight()) - 2.0f);
        g.setColour(theme.lcd().withAlpha(0.08f));
        g.fillRect(box);
        g.setColour(theme.lcd());
        g.drawRect(box, 1.2f);
    }

    // --- The four rows -----------------------------------------------------
    const char* labels[] = {"Bars | Beats", "Markers", "Chords", "Tempo"};
    for (int i = 0; i < 4; ++i) {
        const auto r = row(i);
        hw::paint::silk(g, labels[i], {12.0f, static_cast<float>(r.getY()), 200.0f, static_cast<float>(r.getHeight())}, 10.0f);
        g.setColour(hw::tok::edge);
        g.drawHorizontalLine(r.getBottom() - 1, 0.0f, static_cast<float>(getWidth()));
    }
    g.setColour(hw::tok::edge);
    g.drawVerticalLine(kLabelWidth - 1, static_cast<float>(row(0).getY()), static_cast<float>(getHeight()));

    juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(juce::Rectangle<int>(kLabelWidth, row(0).getY(), getWidth() - kLabelWidth, 4 * kRow));
    if (!access_.x_of || !access_.tick_at) return;
    const Ticks first = std::max<Ticks>(0, access_.tick_at(static_cast<float>(kLabelWidth)));
    const Ticks last = access_.tick_at(static_cast<float>(getWidth()));
    const int bar0 = d.meter.bar_at(first), bar1 = d.meter.bar_at(std::max(first, last)) + 1;

    // Bars | Beats, with the selection's range along the bottom.
    const auto bars = row(0);
    const auto& sel = context_.session.selection();
    if (sel.has_range()) {
        const float a = access_.x_of(sel.from), b = access_.x_of(sel.to);
        g.setColour(theme.led());
        g.fillRect(juce::Rectangle<float>(a, static_cast<float>(bars.getBottom() - 4), std::max(2.0f, b - a), 3.0f));
    }
    for (int bar = bar0; bar <= bar1; ++bar) {
        const Ticks start = d.meter.bar_start(bar);
        const auto sig = d.meter.at_bar(bar);
        const float x = access_.x_of(start);
        g.setColour(hw::tok::silk.withAlpha(0.7f));
        g.drawVerticalLine(juce::roundToInt(x), static_cast<float>(bars.getY() + 2), static_cast<float>(bars.getBottom()));
        g.setColour(hw::tok::text);
        g.setFont(hw::fonts::lcd(12.0f));
        g.drawText(juce::String(bar), juce::Rectangle<float>(x + 4.0f, static_cast<float>(bars.getY()), 40.0f, 16.0f),
                   juce::Justification::centredLeft, false);
        for (int beat = 1; beat < sig.numerator; ++beat) {
            const float bx = access_.x_of(start + beat * sig.beat_ticks());
            g.setColour(hw::tok::silk.withAlpha(0.4f));
            g.drawVerticalLine(juce::roundToInt(bx), static_cast<float>(bars.getBottom() - 7), static_cast<float>(bars.getBottom() - 1));
        }
    }

    // Markers: cues as yellow flags, hit points in grey.
    const auto markers = row(1);
    auto flag = [&](Ticks at, const juce::String& text, juce::Colour fill) {
        const float x = access_.x_of(at);
        g.setFont(hw::fonts::silk(10.5f, 0.06f, true));
        const float w = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), text) + 18.0f;
        juce::Path p;
        const float y = static_cast<float>(markers.getY()) + 3.0f, h = static_cast<float>(markers.getHeight()) - 6.0f;
        p.startNewSubPath(x, y);
        p.lineTo(x + w, y);
        p.lineTo(x + w + 6.0f, y + h / 2.0f);
        p.lineTo(x + w, y + h);
        p.lineTo(x, y + h);
        p.closeSubPath();
        g.setColour(fill);
        g.fillPath(p);
        g.setColour(juce::Colour(0xff1b1a18));
        g.drawText(text, juce::Rectangle<float>(x + 6.0f, y, w - 6.0f, h), juce::Justification::centredLeft, false);
    };
    for (const auto& c : d.cues) {
        const juce::String title = juce::String(c.title).isNotEmpty() ? juce::String(c.title) : juce::String(c.code);
        flag(c.start, title.toUpperCase(), hw::tok::marker);
    }
    for (const auto& m : d.markers) {
        const Ticks at = d.tempo.tick_at_seconds(m.seconds);
        flag(at, juce::String(m.label.empty() ? m.kind : m.label).toUpperCase(), juce::Colour(0xff8e98a3));
    }

    // Chords: one pill per bar where the music spells one.
    const auto chords = row(2);
    for (int bar = bar0; bar <= bar1; ++bar) {
        if (bar < 1 || bar > static_cast<int>(chords_.size())) continue;
        const auto& symbol = chords_[static_cast<std::size_t>(bar - 1)];
        if (symbol.empty()) continue;
        const float x = access_.x_of(d.meter.bar_start(bar)) + 6.0f;
        g.setFont(hw::fonts::ui(11.5f, true));
        const float w = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), symbol) + 14.0f;
        const auto pill = juce::Rectangle<float>(x, static_cast<float>(chords.getY()) + 3.0f, w, static_cast<float>(kRow) - 6.0f);
        g.setColour(juce::Colour(0xff2a2e34));
        g.fillRoundedRectangle(pill, pill.getHeight() / 2.0f);
        g.setColour(hw::tok::edge);
        g.drawRoundedRectangle(pill, pill.getHeight() / 2.0f, 1.0f);
        g.setColour(hw::tok::text_bright);
        g.drawText(juce::String(symbol), pill, juce::Justification::centred, false);
    }

    // Tempo: a line at the tempo, labelled where it changes.
    const auto tempo = row(3);
    g.setColour(theme.lcd());
    const float ty = static_cast<float>(tempo.getBottom()) - 6.0f;
    g.drawHorizontalLine(juce::roundToInt(ty), static_cast<float>(kLabelWidth), static_cast<float>(getWidth()));
    g.setFont(hw::fonts::lcd(11.0f));
    const auto& changes = d.tempo.changes();
    for (std::size_t i = 0; i < changes.size(); ++i) {
        const float x = std::max(static_cast<float>(kLabelWidth) + 4.0f, access_.x_of(changes[i].tick) + 4.0f);
        g.drawText(juce::String::fromUTF8("\xe2\x99\xa9 = ") + juce::String(changes[i].bpm, changes[i].bpm == std::floor(changes[i].bpm) ? 0 : 1),
                   juce::Rectangle<float>(x, static_cast<float>(tempo.getY()), 90.0f, 14.0f), juce::Justification::centredLeft, false);
    }

    // The playhead: a line through the rows with a cap in the bars row.
    const float px = access_.x_of(playhead_);
    if (px >= kLabelWidth) {
        g.setColour(theme.lcd());
        g.fillRect(juce::Rectangle<float>(px - 0.8f, static_cast<float>(bars.getY()), 1.6f, 4.0f * kRow));
        juce::Path cap;
        cap.addTriangle(px - 6.0f, static_cast<float>(bars.getY()), px + 6.0f, static_cast<float>(bars.getY()), px,
                        static_cast<float>(bars.getY()) + 8.0f);
        g.fillPath(cap);
    }
}

void ComposerRuler::overview_scroll(float x)
{
    const auto ov = overview_area();
    const double frac = juce::jlimit(0.0, 1.0, (static_cast<double>(x) - ov.getX()) / std::max(1, ov.getWidth()));
    const Ticks centre = static_cast<Ticks>(frac * static_cast<double>(song_end()));
    const Ticks vis = access_.visible ? access_.visible() : 0;
    if (access_.scroll_to) access_.scroll_to(std::max<Ticks>(0, centre - vis / 2));
    repaint();
}

void ComposerRuler::mouseDown(const juce::MouseEvent& e)
{
    drag_ = Drag::None;
    if (overview_area().contains(e.getPosition())) {
        drag_ = Drag::Overview;
        overview_scroll(e.position.x);
        return;
    }
    if (e.x < kLabelWidth || !access_.tick_at) return;
    const auto& d = context_.session.data();
    const Ticks t = std::max<Ticks>(0, access_.tick_at(e.position.x));
    if (row(0).contains(e.getPosition())) {
        // Click: locate. Drag: a range (on beats).
        const auto sig = d.meter.at_bar(d.meter.bar_at(t));
        const Ticks beat = std::max<Ticks>(1, sig.beat_ticks());
        drag_from_ = (t + beat / 2) / beat * beat;
        drag_ = Drag::Range;
        context_.engine.locate(t);
        return;
    }
    if (row(1).contains(e.getPosition())) {
        // The nearest marker or cue to the left of the click.
        Ticks best = -1;
        for (const auto& c : d.cues)
            if (c.start <= t && c.start > best) best = c.start;
        for (const auto& m : d.markers) {
            const Ticks at = d.tempo.tick_at_seconds(m.seconds);
            if (at <= t && at > best) best = at;
        }
        if (best >= 0) context_.engine.locate(best);
    }
}

void ComposerRuler::mouseDrag(const juce::MouseEvent& e)
{
    if (drag_ == Drag::Overview) {
        overview_scroll(e.position.x);
    } else if (drag_ == Drag::Range && access_.tick_at && e.getDistanceFromDragStart() > 3) {
        const auto& d = context_.session.data();
        const Ticks t = std::max<Ticks>(0, access_.tick_at(e.position.x));
        const auto sig = d.meter.at_bar(d.meter.bar_at(t));
        const Ticks beat = std::max<Ticks>(1, sig.beat_ticks());
        const Ticks to = (t + beat / 2) / beat * beat;
        context_.session.set_selection_range(std::min(drag_from_, to), std::max(drag_from_, to));
    }
}

void ComposerRuler::mouseUp(const juce::MouseEvent&) { drag_ = Drag::None; }

}  // namespace vcc::desktop::composer_ui
