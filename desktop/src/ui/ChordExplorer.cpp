#include "ui/ChordExplorer.h"

#include <algorithm>
#include <cmath>

#include "ui/Theme.h"
#include "ui/hw/HwTheme.h"
#include "vcc/theory/KeyReference.h"

namespace vcc::desktop {

namespace {

constexpr int kLeftWidth = 300;
constexpr int kTopBar = 52;
constexpr int kBottomBar = 54;

/// What a dragged chord carries: "chord:<numeral>|<symbol>".
juce::var chord_drag(const theory::Chord& chord, const std::string& numeral)
{
    return juce::String("chord:") + juce::String::fromUTF8(numeral.c_str()) + "|" + juce::String::fromUTF8(chord.name().c_str());
}

std::optional<std::pair<theory::Chord, std::string>> parse_chord_drag(const juce::var& v)
{
    const auto s = v.toString();
    if (!s.startsWith("chord:")) return std::nullopt;
    const auto body = s.fromFirstOccurrenceOf("chord:", false, false);
    const auto numeral = body.upToFirstOccurrenceOf("|", false, false).toStdString();
    const auto chord = theory::parse_chord_symbol(body.fromFirstOccurrenceOf("|", false, false).toStdString());
    if (!chord) return std::nullopt;
    return std::make_pair(*chord, numeral);
}

juce::String utf8(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

/// A voiced note spelled as the chord spells it ("F#4" in D major, "Gb4" in Ebm).
juce::String spelled(int midi, const theory::Chord& chord)
{
    auto tones = chord.tones();
    tones.push_back(chord.bass_note());
    for (const auto& t : tones)
        if (t.pitch_class() == ((midi % 12) + 12) % 12) {
            const int octave = (midi - t.alteration - theory::letter_pitch_class(t.letter)) / 12 - 1;
            return utf8(t.display_name()) + juce::String(octave);
        }
    return dialogs::note_name(midi);
}

juce::String voicing_text(const std::vector<int>& notes, const theory::Chord& chord)
{
    juce::StringArray names;
    for (int n : notes) names.add(spelled(n, chord));
    return names.joinIntoString(" ");
}

}  // namespace

// --- Chord matrix button ----------------------------------------------------------------------

class ChordExplorer::MatrixButton final : public juce::Button {
public:
    MatrixButton() : juce::Button("chord") {}

    void set(const theory::Chord& chord, const juce::String& numeral)
    {
        chord_ = chord;
        numeral_ = numeral;
        setTooltip("Audition " + utf8(chord.display_name()) + " (nothing is written)");
        repaint();
    }
    const theory::Chord& chord() const noexcept { return chord_; }
    std::string numeral() const { return numeral_.toStdString(); }
    std::function<void()> on_double_click;
    bool borrowed = false;

    void mouseDrag(const juce::MouseEvent& e) override
    {
        // Dragged into the build strip: the chord goes into that bar.
        if (e.getDistanceFromDragStart() > 6)
            if (auto* c = juce::DragAndDropContainer::findParentDragContainerFor(this); c && !c->isDragAndDropActive()) {
                dragged_ = true;
                c->startDragging(chord_drag(chord_, numeral()), this);
            }
        juce::Button::mouseDrag(e);
    }
    void mouseDown(const juce::MouseEvent& e) override
    {
        dragged_ = false;
        juce::Button::mouseDown(e);
    }
    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        if (on_double_click) on_double_click();
    }
    /// The press ended a drag, not a click (no audition then).
    bool was_dragged() const noexcept { return dragged_; }

    void paintButton(juce::Graphics& g, bool highlighted, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced(1.0f);
        g.setColour(down ? colours::accent.withAlpha(0.45f) : highlighted ? colours::panel_raised.brighter(0.08f) : colours::panel_raised);
        g.fillRoundedRectangle(r, 5.0f);
        g.setColour(highlighted ? colours::accent : borrowed ? colours::accent_warm.withAlpha(0.7f) : colours::outline);
        g.drawRoundedRectangle(r.reduced(0.5f), 5.0f, 1.0f);
        auto area = getLocalBounds().reduced(4, 5);
        g.setColour(colours::text);
        g.setFont(juce::FontOptions(16.0f, juce::Font::bold));
        g.drawText(numeral_, area.removeFromTop(area.getHeight() / 2 + 2), juce::Justification::centredBottom, true);
        g.setColour(borrowed ? colours::accent_warm : colours::accent);
        g.setFont(juce::FontOptions(13.0f));
        g.drawText(utf8(chord_.display_name()), area, juce::Justification::centredTop, true);
    }

private:
    theory::Chord chord_;
    juce::String numeral_;
    bool dragged_ = false;
};

// --- Chord cards ------------------------------------------------------------------------------

class ChordExplorer::Cards final : public juce::Component {
public:
    Cards(AppContext& c, ChordExplorer& owner) : context_(c), owner_(owner) {}

    int selected = -1;

    void paint(juce::Graphics& g) override
    {
        const auto* prog = owner_.chosen();
        auto r = getLocalBounds();
        if (!prog) {
            g.setColour(colours::text_dim);
            g.setFont(juce::FontOptions(14.0f));
            g.drawText("Choose a progression on the left.", r, juce::Justification::centred);
            return;
        }
        // Header: name, numerals, notes.
        auto header = r.removeFromTop(48);
        g.setColour(colours::text);
        g.setFont(juce::FontOptions(19.0f, juce::Font::bold));
        const juce::String name = utf8(prog->name);
        const int name_w = juce::GlyphArrangement::getStringWidthInt(juce::Font(juce::FontOptions(19.0f, juce::Font::bold)), name) + 14;
        g.drawText(name, header.removeFromTop(26), juce::Justification::centredLeft, true);
        g.setColour(colours::accent);
        g.setFont(juce::FontOptions(15.0f));
        g.drawText(utf8(prog->roman_text()), name_w, 0, getWidth() - name_w, 26, juce::Justification::centredLeft, true);
        juce::String sub = juce::String(prog->bars) + "-bar " + utf8(prog->family) + " pattern";
        if (!prog->cadence.empty() && prog->cadence != "none") sub << ", " << utf8(prog->cadence) << " cadence";
        if (!prog->notes.empty()) sub << ".  " << utf8(prog->notes);
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(12.5f));
        g.drawText(sub, header, juce::Justification::topLeft, true);
        r.removeFromTop(6);

        if (!owner_.render_) {
            auto box = r.withSizeKeepingCentre(std::min(r.getWidth(), 560), 96).toFloat();
            g.setColour(colours::accent_warm.withAlpha(0.12f));
            g.fillRoundedRectangle(box, 6.0f);
            g.setColour(colours::accent_warm.withAlpha(0.6f));
            g.drawRoundedRectangle(box.reduced(0.5f), 6.0f, 1.0f);
            g.setColour(colours::accent_warm);
            g.setFont(juce::FontOptions(14.0f));
            g.drawFittedText(owner_.error_, box.reduced(16.0f, 10.0f).toNearestInt(), juce::Justification::centred, 4);
            return;
        }
        const auto& chords = owner_.render_->chords;
        layout(r, static_cast<int>(chords.size()));
        const auto& meter = context_.session.data().meter;
        for (std::size_t i = 0; i < chords.size(); ++i) {
            const auto& rc = chords[i];
            auto card = rects_[i].toFloat();
            const bool sel = static_cast<int>(i) == selected;
            g.setColour(sel ? colours::accent.withAlpha(0.22f) : colours::panel);
            g.fillRoundedRectangle(card, 6.0f);
            g.setColour(sel ? colours::accent : colours::outline);
            g.drawRoundedRectangle(card.reduced(0.5f), 6.0f, sel ? 1.5f : 1.0f);
            auto inner = rects_[i].reduced(8, 6);
            const int bar = meter.bar_at(rc.start);
            const auto bar_start = meter.bar_start(bar);
            const double beat = static_cast<double>(rc.start - bar_start) / static_cast<double>(meter.at_bar(bar).beat_ticks());
            g.setColour(colours::text_dim);
            g.setFont(juce::FontOptions(11.0f));
            g.drawText("Bar " + juce::String(bar) + (beat > 0.01 ? "." + juce::String(beat + 1.0, 1) : juce::String()),
                       inner.removeFromTop(14), juce::Justification::centredLeft);
            const bool compact = inner.getHeight() < 70;
            g.setColour(colours::text);
            g.setFont(juce::FontOptions(compact ? 17.0f : 21.0f, juce::Font::bold));
            g.drawText(utf8(rc.numeral), inner.removeFromTop(compact ? 22 : 28), juce::Justification::centred, true);
            g.setColour(colours::accent);
            g.setFont(juce::FontOptions(compact ? 13.0f : 15.0f));
            g.drawText(utf8(rc.chord.display_name()), inner.removeFromTop(compact ? 17 : 22), juce::Justification::centred, true);
            g.setColour(colours::text_dim);
            g.setFont(juce::FontOptions(11.0f));
            g.drawFittedText(voicing_text(rc.voicing.notes, rc.chord), inner, juce::Justification::centred, 2, 0.85f);
        }

        // What Send to Track will write: the rendered notes, rhythm included
        // (the preset's, or your build's while My Build is chosen).
        const auto* sending = owner_.active_render();
        if (!rects_.empty() && sending) {
            auto rest = r.withTop(rects_.back().getBottom() + 14);
            if (rest.getHeight() >= 70) {
                g.setColour(colours::text_dim);
                g.setFont(juce::FontOptions(11.5f, juce::Font::bold));
                g.drawText(juce::String(owner_.use_build_ ? "YOUR BUILD - NOTES TO SEND  -  " : "NOTES TO SEND  -  ") +
                               juce::String(static_cast<int>(sending->notes.size())) + " notes, bars " +
                               juce::String(meter.bar_at(sending->start)) + juce::String::fromUTF8("–") +
                               juce::String(meter.bar_at(sending->end - 1)),
                           rest.removeFromTop(18), juce::Justification::centredLeft);
                rest.removeFromTop(4);
                dialogs::draw_mini_roll(g, rest.withHeight(std::min(rest.getHeight(), 170)).toFloat(), sending->notes,
                                        sending->start, sending->end, meter,
                                        owner_.use_build_ ? colours::accent_warm : colours::accent);
            }
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        for (std::size_t i = 0; i < rects_.size(); ++i)
            if (rects_[i].contains(e.getPosition())) {
                selected = static_cast<int>(i);
                owner_.audition_card(selected);
                repaint();
                return;
            }
    }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        // A progression's chord, dragged into the build strip.
        if (e.getDistanceFromDragStart() < 6 || !owner_.render_ || selected < 0 ||
            selected >= static_cast<int>(owner_.render_->chords.size()))
            return;
        if (owner_.isDragAndDropActive()) return;
        const auto& rc = owner_.render_->chords[static_cast<std::size_t>(selected)];
        owner_.startDragging(chord_drag(rc.chord, rc.numeral), this);
    }

private:
    void layout(juce::Rectangle<int> area, int n)
    {
        rects_.clear();
        if (n <= 0) return;
        constexpr int gap = 10;
        const int cols = std::max(1, std::min(n, (area.getWidth() + gap) / (104 + gap)));
        const int rows = (n + cols - 1) / cols;
        const int w = std::min(170, (area.getWidth() - gap * (cols - 1)) / cols);
        const int h = std::max(64, std::min(150, (area.getHeight() - gap * (rows - 1)) / rows));
        for (int i = 0; i < n; ++i)
            rects_.emplace_back(area.getX() + (i % cols) * (w + gap), area.getY() + (i / cols) * (h + gap), w, h);
    }

    AppContext& context_;
    ChordExplorer& owner_;
    std::vector<juce::Rectangle<int>> rects_;
};

// --- Build strip ------------------------------------------------------------------------------

/// The progression's bars, side by side, in a scrolling view: each bar's
/// header ("Bar 5", its division) over its cells.
class ChordExplorer::BuildStrip final : public juce::Component,
                                        public juce::DragAndDropTarget,
                                        public juce::SettableTooltipClient {
public:
    explicit BuildStrip(ChordExplorer& owner) : owner_(owner)
    {
        setComponentID("chord_build_strip");
        setTitle("Your progression");
        setTooltip("Drag chords into the cells, or click one to type a chord and edit its notes. Right-click for more. "
                   "Click a bar's division to split it into up to 16 chords. An empty cell holds the chord before it.");
    }

    static constexpr int kHeader = 18;
    int bar_width() const { return juce::roundToInt(112.0 * owner_.zoom_width_); }
    void update_size(int height) { setSize(owner_.bar_count() * bar_width(), height); }

    juce::Rectangle<int> bar_rect(int bar) const { return {bar * bar_width(), 0, bar_width(), getHeight()}; }
    juce::Rectangle<int> division_rect(int bar) const
    {
        const auto r = bar_rect(bar).reduced(3, 0);
        return r.withHeight(kHeader).removeFromRight(34);
    }
    juce::Rectangle<int> cell_rect(int bar, int cell) const
    {
        auto r = bar_rect(bar).reduced(3, 0).withTrimmedTop(kHeader + 2).withTrimmedBottom(2);
        const int n = owner_.divisions(bar);
        const int x0 = r.getX() + r.getWidth() * cell / n, x1 = r.getX() + r.getWidth() * (cell + 1) / n;
        return {x0 + (cell > 0 ? 1 : 0), r.getY(), x1 - x0 - (cell > 0 ? 1 : 0), r.getHeight()};
    }
    std::pair<int, int> cell_at(juce::Point<int> p) const
    {
        const int bar = p.x / std::max(1, bar_width());
        if (bar < 0 || bar >= owner_.bar_count() || p.y < kHeader) return {-1, -1};
        for (int c = 0; c < owner_.divisions(bar); ++c)
            if (cell_rect(bar, c).expanded(1, 0).contains(p)) return {bar, c};
        return {-1, -1};
    }

    bool isInterestedInDragSource(const SourceDetails& d) override { return d.description.toString().startsWith("chord:"); }
    void itemDragEnter(const SourceDetails& d) override { itemDragMove(d); }
    void itemDragMove(const SourceDetails& d) override
    {
        const auto h = cell_at(d.localPosition);
        if (h != hover_) {
            hover_ = h;
            repaint();
        }
    }
    void itemDragExit(const SourceDetails&) override
    {
        hover_ = {-1, -1};
        repaint();
    }
    void itemDropped(const SourceDetails& d) override
    {
        hover_ = {-1, -1};
        const auto [bar, c] = cell_at(d.localPosition);
        if (const auto ch = parse_chord_drag(d.description); ch && bar >= 0) owner_.set_cell(bar, c, ch->first, ch->second);
        repaint();
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const int bar = e.x / std::max(1, bar_width());
        if (bar >= 0 && bar < owner_.bar_count() && division_rect(bar).contains(e.getPosition())) {
            show_division_menu(bar);
            return;
        }
        const auto [b, c] = cell_at(e.getPosition());
        if (b < 0) return;
        if (e.mods.isPopupMenu()) {
            show_cell_menu(b, c);
            return;
        }
        owner_.edit_cell(b, c);
    }

    void show_division_menu(int bar)
    {
        juce::PopupMenu m;
        m.addSectionHeader("Bar " + juce::String(owner_.first_bar() + bar) + ": chords in the bar");
        for (int n = 1; n <= kMaxDivisions; ++n)
            m.addItem(n, n == 1 ? juce::String("1 (whole bar)") : juce::String(n), true, owner_.divisions(bar) == n);
        juce::Component::SafePointer<BuildStrip> self(this);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(localAreaToGlobal(division_rect(bar))), [self, bar](int r) {
            if (self != nullptr && r > 0) self->owner_.set_divisions(bar, r);
        });
    }
    void show_cell_menu(int bar, int c)
    {
        juce::PopupMenu m;
        const bool has = owner_.cell(bar, c).has_value();
        m.addItem("Edit Chord and Notes...", [this, bar, c] { owner_.edit_cell(bar, c); });
        m.addItem("Hear It", has, false, [this, bar, c] { owner_.audition_notes(owner_.cell_notes(bar, c)); });
        m.addItem("Empty This Cell", has, false, [this, bar, c] { owner_.clear_cell(bar, c); });
        m.addSeparator();
        juce::PopupMenu div;
        for (int n = 1; n <= kMaxDivisions; ++n)
            div.addItem(n == 1 ? juce::String("Whole bar") : juce::String(n) + " chords", true, owner_.divisions(bar) == n,
                        [this, bar, n] { owner_.set_divisions(bar, n); });
        m.addSubMenu("Divide Bar " + juce::String(owner_.first_bar() + bar), div);
        m.addItem("Empty Bar " + juce::String(owner_.first_bar() + bar), [this, bar] { owner_.clear_build_chord(bar); });
        m.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
    }

    void paint(juce::Graphics& g) override
    {
        const int first = owner_.first_bar();
        // The last filled cell: empty cells before it hold the chord before them.
        std::pair<int, int> last{-1, -1};
        for (int b = 0; b < owner_.bar_count(); ++b)
            for (int c = 0; c < owner_.divisions(b); ++c)
                if (owner_.cell(b, c)) last = {b, c};
        bool held = false;
        const auto clip = g.getClipBounds();
        for (int b = 0; b < owner_.bar_count(); ++b) {
            const auto br = bar_rect(b);
            if (br.getRight() < clip.getX() || br.getX() > clip.getRight()) {
                for (int c = 0; c < owner_.divisions(b); ++c) held = held || owner_.cell(b, c).has_value();
                continue;
            }
            auto head = br.reduced(3, 0).withHeight(kHeader);
            g.setColour(colours::text_dim);
            g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
            g.drawText("BAR " + juce::String(first + b), head.withTrimmedRight(36), juce::Justification::centredLeft);
            const auto dr = division_rect(b).toFloat().reduced(0.5f, 1.5f);
            g.setColour(colours::panel_raised);
            g.fillRoundedRectangle(dr, 3.0f);
            g.setColour(colours::outline);
            g.drawRoundedRectangle(dr, 3.0f, 1.0f);
            g.setColour(colours::text);
            g.setFont(juce::FontOptions(10.5f));
            g.drawText(juce::String::fromUTF8("\xc3\xb7") + juce::String(owner_.divisions(b)), dr, juce::Justification::centred);
            // The bar line.
            g.setColour(colours::outline.brighter(0.2f));
            g.drawVerticalLine(br.getX(), static_cast<float>(br.getY()), static_cast<float>(br.getBottom()));
            for (int c = 0; c < owner_.divisions(b); ++c) {
                const auto r = cell_rect(b, c).toFloat();
                const auto& ch = owner_.cell(b, c);
                const bool hover = hover_ == std::make_pair(b, c);
                g.setColour(hover ? colours::accent.withAlpha(0.25f) : ch ? colours::panel_raised : colours::panel);
                g.fillRoundedRectangle(r, 4.0f);
                g.setColour(hover ? colours::accent : ch ? colours::accent_warm.withAlpha(0.8f) : colours::outline);
                g.drawRoundedRectangle(r.reduced(0.5f), 4.0f, hover || ch ? 1.5f : 1.0f);
                auto inner = cell_rect(b, c).reduced(3, 3);
                if (ch) {
                    held = true;
                    const bool tiny = inner.getWidth() < 34;
                    g.setColour(colours::text);
                    g.setFont(juce::FontOptions(tiny ? 11.0f : 15.0f, juce::Font::bold));
                    g.drawFittedText(utf8(ch->numeral), inner.removeFromTop(inner.getHeight() / 3 + 4), juce::Justification::centredBottom, 1, 0.6f);
                    g.setColour(colours::accent_warm);
                    g.setFont(juce::FontOptions(tiny ? 10.0f : 12.5f));
                    g.drawFittedText(utf8(ch->chord.display_name()), inner.removeFromTop(16), juce::Justification::centred, 1, 0.6f);
                    // Its notes, as a little column (edited ones marked).
                    const auto notes = owner_.cell_notes(b, c);
                    if (!notes.empty() && inner.getHeight() > 12) {
                        const int lo = notes.front() - 2, hi = notes.back() + 2;
                        const float span = static_cast<float>(std::max(12, hi - lo));
                        g.setColour(ch->notes.empty() ? colours::accent.withAlpha(0.8f) : colours::accent_warm);
                        for (int n : notes) {
                            const float y = inner.getBottom() - (static_cast<float>(n - lo) / span) * inner.getHeight();
                            g.fillRoundedRectangle(juce::Rectangle<float>(static_cast<float>(inner.getX() + 2), y - 1.5f,
                                                                          static_cast<float>(inner.getWidth() - 4), 3.0f),
                                                   1.5f);
                        }
                    }
                } else {
                    const bool holds = held && std::make_pair(b, c) < last;
                    g.setColour(colours::text_dim.withAlpha(0.7f));
                    g.setFont(juce::FontOptions(10.5f));
                    if (inner.getWidth() > 28)
                        g.drawFittedText(holds ? juce::String::fromUTF8("\xe2\x86\x90 hold") : juce::String("+"), inner,
                                         juce::Justification::centred, 2);
                }
            }
        }
    }

private:
    ChordExplorer& owner_;
    std::pair<int, int> hover_{-1, -1};
};

// --- Cell editor --------------------------------------------------------------------------------

/// A cell's chord and its notes: type a chord symbol; move each note up or
/// down a semitone (or an octave), delete it, or click the keyboard to add
/// or remove a note. Changes are heard and land in the build at once.
class ChordExplorer::CellEditor final : public juce::Component, private juce::MidiKeyboardState::Listener {
public:
    CellEditor(ChordExplorer& owner, int bar, int cell) : owner_(owner), bar_(bar), cell_(cell)
    {
        setComponentID("chord_cell_editor");
        symbol_.setTitle("Chord symbol");
        symbol_.setComponentID("chord_cell_symbol");
        symbol_.setTextToShowWhenEmpty("Type a chord: Cm7, F#m7b5, Bb/D...", colours::text_dim);
        symbol_.setIndents(8, 6);
        symbol_.onReturnKey = [this] { apply_symbol(); };
        addAndMakeVisible(symbol_);
        set_.onClick = [this] { apply_symbol(); };
        play_.onClick = [this] { owner_.audition_notes(notes()); };
        auto_.setTooltip("Let the voicing (and voice leading) choose the notes again");
        auto_.onClick = [this] {
            owner_.set_cell_notes(bar_, cell_, {});
            refresh();
        };
        clear_.onClick = [this] {
            owner_.clear_cell(bar_, cell_);
            refresh();
        };
        for (auto* b : {&set_, &play_, &auto_, &clear_}) addAndMakeVisible(*b);
        keyboard_.setAvailableRange(36, 96);
        keyboard_.setOctaveForMiddleC(4);
        keyboard_.setScrollButtonsVisible(false);
        keyboard_.setColour(juce::MidiKeyboardComponent::keyDownOverlayColourId, colours::accent_warm.withAlpha(0.85f));
        keyboard_.setTitle("Click a key to add that note to the chord, or to take it out");
        addAndMakeVisible(keyboard_);
        state_.addListener(this);
        refresh();
        setSize(520, 330);
    }
    ~CellEditor() override { state_.removeListener(this); }

    std::vector<int> notes() const { return owner_.cell_notes(bar_, cell_); }

    void refresh()
    {
        const auto& ch = owner_.cell(bar_, cell_);
        title_ = "Bar " + juce::String(owner_.first_bar() + bar_) +
                 (owner_.divisions(bar_) > 1 ? ", chord " + juce::String(cell_ + 1) + " of " + juce::String(owner_.divisions(bar_))
                                             : juce::String()) +
                 (ch ? "  -  " + utf8(ch->numeral) + "  " + utf8(ch->chord.display_name()) : juce::String("  -  empty"));
        if (ch && !symbol_.hasKeyboardFocus(true)) symbol_.setText(utf8(ch->chord.name()), false);
        rows_.clear();
        const auto n = notes();
        for (int i = static_cast<int>(n.size()) - 1; i >= 0; --i) {
            const int note = n[static_cast<std::size_t>(i)];
            auto row = std::make_unique<Row>();
            row->name.setText(ch ? spelled(note, ch->chord) : dialogs::note_name(note), juce::dontSendNotification);
            row->name.setColour(juce::Label::textColourId, colours::text);
            row->up.onClick = [this, note] { move(note, 1); };
            row->down.onClick = [this, note] { move(note, -1); };
            row->oct_up.onClick = [this, note] { move(note, 12); };
            row->oct_down.onClick = [this, note] { move(note, -12); };
            row->del.onClick = [this, note] { toggle(note); };
            row->del.setTooltip("Delete this note");
            for (juce::Component* c : std::initializer_list<juce::Component*>{&row->name, &row->up, &row->down, &row->oct_up,
                                                                               &row->oct_down, &row->del})
                addAndMakeVisible(*c);
            rows_.push_back(std::move(row));
        }
        showing_ = true;
        state_.allNotesOff(1);
        for (int note : n) state_.noteOn(1, note, 0.8f);
        showing_ = false;
        for (auto* b : {&play_, &auto_, &clear_}) b->setEnabled(ch.has_value());
        resized();
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(colours::background);
        g.setColour(colours::text);
        g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
        g.drawText(title_, 12, 6, getWidth() - 24, 22, juce::Justification::centredLeft, true);
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(11.0f));
        g.drawText("NOTES  (top to bottom)", 12, 66, 240, 16, juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(12);
        r.removeFromTop(24);
        auto top = r.removeFromTop(30);
        set_.setBounds(top.removeFromRight(60));
        top.removeFromRight(6);
        symbol_.setBounds(top);
        r.removeFromTop(26);
        keyboard_.setBounds(r.removeFromBottom(70));
        keyboard_.setKeyWidth(static_cast<float>(keyboard_.getWidth()) / 36.0f);
        r.removeFromBottom(8);
        auto buttons = r.removeFromRight(110);
        for (auto* b : {&play_, &auto_, &clear_}) {
            b->setBounds(buttons.removeFromTop(28));
            buttons.removeFromTop(6);
        }
        r.removeFromRight(10);
        const int rh = rows_.empty() ? 22 : std::min(24, r.getHeight() / static_cast<int>(rows_.size()));
        for (auto& row : rows_) {
            auto line = r.removeFromTop(rh);
            row->name.setBounds(line.removeFromLeft(70));
            for (auto* b : {&row->oct_down, &row->down, &row->up, &row->oct_up, &row->del}) {
                b->setBounds(line.removeFromLeft(b == &row->del ? 34 : 44).reduced(1, 1));
                line.removeFromLeft(2);
            }
        }
    }

private:
    struct Row {
        juce::Label name;
        juce::TextButton down{juce::String::fromUTF8("\xe2\x96\xbc")}, up{juce::String::fromUTF8("\xe2\x96\xb2")},
            oct_down{"-8va"}, oct_up{"+8va"}, del{juce::String::fromUTF8("\xc3\x97")};
    };

    void apply_symbol()
    {
        const auto text = symbol_.getText().trim().toStdString();
        const auto chord = theory::parse_chord_symbol(text);
        if (!chord) {
            owner_.context_.status("\"" + symbol_.getText() + "\" is not a chord symbol (try Cm7, F#m7b5, Bb/D)");
            return;
        }
        const auto& old = owner_.cell(bar_, cell_);
        owner_.set_cell(bar_, cell_, *chord, old && old->chord.name() == chord->name() ? old->numeral : chord->name());
        refresh();
    }
    void move(int note, int by)
    {
        auto n = notes();
        for (auto& x : n)
            if (x == note) x = juce::jlimit(0, 127, note + by);
        owner_.set_cell_notes(bar_, cell_, n);
        owner_.audition_notes(owner_.cell_notes(bar_, cell_));
        refresh();
    }
    void toggle(int note)
    {
        auto n = notes();
        const auto it = std::find(n.begin(), n.end(), note);
        if (it != n.end()) {
            if (n.size() == 1) return;  // a chord keeps a note: Empty This Cell clears it
            n.erase(it);
        } else {
            n.push_back(note);
        }
        owner_.set_cell_notes(bar_, cell_, n);
        owner_.audition_notes(owner_.cell_notes(bar_, cell_));
        refresh();
    }

    void handleNoteOn(juce::MidiKeyboardState*, int, int note, float) override
    {
        if (showing_ || !owner_.cell(bar_, cell_)) return;
        juce::Component::SafePointer<CellEditor> self(this);
        juce::MessageManager::callAsync([self, note] {
            if (self != nullptr) self->toggle(note);
        });
    }
    void handleNoteOff(juce::MidiKeyboardState*, int, int, float) override {}

    ChordExplorer& owner_;
    int bar_, cell_;
    juce::String title_;
    juce::TextEditor symbol_;
    juce::TextButton set_{"Set"}, play_{"Hear"}, auto_{"Auto Voice"}, clear_{"Empty"};
    juce::MidiKeyboardState state_;
    juce::MidiKeyboardComponent keyboard_{state_, juce::MidiKeyboardComponent::horizontalKeyboard};
    std::vector<std::unique_ptr<Row>> rows_;
    bool showing_ = false;
};

// --- Voice picker ------------------------------------------------------------------------------

/// The chord's voices, bass first: click one to send it or not, or drag
/// across several to set them all alike.
class ChordExplorer::VoicePicker final : public juce::Component, public juce::SettableTooltipClient {
public:
    explicit VoicePicker(ChordExplorer& owner) : owner_(owner)
    {
        setComponentID("chord_voice_picker");
        setTitle("Voices to send");
        setTooltip("Which voices of each chord Send writes, counting up from the bass: click one, or drag across several. "
                   "Sent to a section, the lowest goes to its lowest instrument.");
    }
    juce::Rectangle<int> key(int v) const
    {
        const int w = (getWidth() - 58) / kMaxVoices;
        return {58 + v * w, 0, w - 2, getHeight()};
    }
    int voice_at(juce::Point<int> p) const
    {
        for (int v = 0; v < kMaxVoices; ++v)
            if (key(v).contains(p)) return v;
        return -1;
    }
    void paint(juce::Graphics& g) override
    {
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
        g.drawText("VOICES", 0, 0, 56, getHeight(), juce::Justification::centredLeft);
        for (int v = 0; v < kMaxVoices; ++v) {
            const auto r = key(v).toFloat();
            const bool on = owner_.voice_selected(v);
            g.setColour(on ? colours::accent.withAlpha(0.85f) : colours::panel_raised);
            g.fillRoundedRectangle(r, 3.0f);
            g.setColour(on ? colours::accent : colours::outline);
            g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, 1.0f);
            g.setColour(on ? juce::Colours::white : colours::text_dim);
            g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
            g.drawText(v == 0 ? juce::String("Bass") : juce::String(v + 1), r, juce::Justification::centred);
        }
    }
    void mouseDown(const juce::MouseEvent& e) override
    {
        const int v = voice_at(e.getPosition());
        if (v < 0) return;
        painting_ = !owner_.voice_selected(v);
        owner_.set_voice_selected(v, painting_);
    }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        const int v = voice_at(e.getPosition());
        if (v >= 0 && owner_.voice_selected(v) != painting_) owner_.set_voice_selected(v, painting_);
    }

private:
    ChordExplorer& owner_;
    bool painting_ = true;
};

// --- Explorer ---------------------------------------------------------------------------------

ChordExplorer::ChordExplorer(AppContext& c) : context_(c), target_(c, "New Harmony Track (piano)")
{
    dialogs::style_caption(key_caption_, "Key");
    dialogs::style_caption(length_caption_, "Length");
    dialogs::style_caption(start_caption_, "Start bar");
    dialogs::style_caption(voicing_caption_, "Voicing");
    dialogs::style_caption(rhythm_caption_, "Rhythm");
    dialogs::style_caption(target_caption_, "Target");
    for (auto* l : {&key_caption_, &length_caption_, &start_caption_, &voicing_caption_, &rhythm_caption_, &target_caption_})
        addAndMakeVisible(*l);

    dialogs::fill_key_combos(tonic_, mode_, dialogs::project_key(c), true);
    tonic_.setTitle("Key");
    tonic_.setTooltip("Preview key (the project key is not changed)");
    mode_.setTitle("Mode");
    mode_.setTooltip("Mode");
    for (auto* cb : {&tonic_, &mode_}) {
        addAndMakeVisible(*cb);
        cb->onChange = [this] {
            rebuild_matrix();
            rerender();
        };
    }

    length_.setTitle("Length");
    length_.addItem("4 bars", 4);
    length_.addItem("8 bars", 8);
    length_.setSelectedId(4, juce::dontSendNotification);
    addAndMakeVisible(length_);
    length_.onChange = [this] { rerender(); };

    start_.setTitle("Start bar");
    start_.setSliderStyle(juce::Slider::IncDecButtons);
    start_.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 44, 24);
    start_.setRange(1.0, 999.0, 1.0);
    start_.setValue(1.0, juce::dontSendNotification);
    start_.setIncDecButtonsMode(juce::Slider::incDecButtonsNotDraggable);
    start_.setTooltip("Bar the progression starts at when sent");
    addAndMakeVisible(start_);
    start_.onValueChange = [this] { rerender(); };

    voicing_.setTitle("Voicing");
    voicing_.addItem("Closed", 1);
    voicing_.addItem("Open", 2);
    voicing_.addItem("Drop 2", 3);
    voicing_.addItem("Drop 3", 4);
    voicing_.addItem("Spread", 5);
    voicing_.addItem("Two Hands (5 notes)", 6);
    voicing_.setSelectedId(6, juce::dontSendNotification);  // a full two-handed chord: 2 in the bass clef, 3 above
    rhythm_.setTitle("Rhythm");
    rhythm_.addItem("Block", 1);
    rhythm_.addItem("Pulse", 2);
    rhythm_.addItem("Arpeggio", 3);
    rhythm_.addItem("Strum", 4);
    rhythm_.setSelectedId(1, juce::dontSendNotification);
    for (auto* cb : {&voicing_, &rhythm_}) {
        addAndMakeVisible(*cb);
        cb->onChange = [this] { rerender(); };
    }
    addAndMakeVisible(voice_lead_);
    voice_lead_.setToggleState(true, juce::dontSendNotification);
    voice_lead_.setTooltip("Move each voice as little as possible from chord to chord");
    voice_lead_.onClick = [this] { rerender(); };

    addAndMakeVisible(search_);
    search_.setTitle("Search progressions");
    search_.setIndents(8, 7);
    search_.setTextToShowWhenEmpty("Search progressions (pop, jazz, minor...)", colours::text_dim);
    search_.onTextChange = [this] { filter(); };
    addAndMakeVisible(list_);
    list_.setModel(this);
    list_.setRowHeight(44);
    list_.setColour(juce::ListBox::backgroundColourId, colours::panel);
    list_.setColour(juce::ListBox::outlineColourId, colours::outline);
    list_.setOutlineThickness(1);

    matrix_caption_.setFont(juce::FontOptions(11.5f, juce::Font::bold));
    matrix_caption_.setColour(juce::Label::textColourId, colours::text_dim);
    addAndMakeVisible(matrix_caption_);
    int size = 3;
    for (auto* b : {&triads_, &sevenths_, &ninths_}) {
        addAndMakeVisible(*b);
        b->setClickingTogglesState(true);
        b->setRadioGroupId(4711);
        b->setToggleState(size == 3, juce::dontSendNotification);
        b->onClick = [this, b, size] {
            if (!b->getToggleState()) return;
            chord_size_ = size;
            rebuild_matrix();
        };
        ++size;
    }
    triads_.setConnectedEdges(juce::Button::ConnectedOnRight);
    sevenths_.setConnectedEdges(juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
    ninths_.setConnectedEdges(juce::Button::ConnectedOnLeft);
    for (int i = 0; i < 7; ++i) {
        matrix_.push_back(std::make_unique<MatrixButton>());
        auto* b = matrix_.back().get();
        addAndMakeVisible(*b);
        b->onClick = [this, b] {
            if (!b->was_dragged()) audition_chord(b->chord());
        };
        b->on_double_click = [this, b] { add_to_build(b->chord(), b->numeral()); };
    }
    borrowed_caption_.setFont(juce::FontOptions(11.5f, juce::Font::bold));
    borrowed_caption_.setColour(juce::Label::textColourId, colours::text_dim);
    addAndMakeVisible(borrowed_caption_);
    borrow_from_.setTitle("Borrow from");
    borrow_from_.setComponentID("chord_borrow_from");
    borrow_from_.setTooltip("Where the borrowed chords come from: the parallel key (with the Neapolitan), or another mode "
                            "on the same tonic");
    {
        const int saved = hw::ui_settings().getIntValue("chord_explorer_borrow_from", 0);
        const auto& sources = theory::borrow_sources();
        if (saved >= 0 && saved < static_cast<int>(sources.size())) borrow_source_ = sources[static_cast<std::size_t>(saved)];
    }
    borrow_from_.onChange = [this] {
        const int i = borrow_from_.getSelectedItemIndex();
        if (i < 0) return;
        borrow_source_ = theory::borrow_sources()[static_cast<std::size_t>(i)];
        hw::ui_settings().setValue("chord_explorer_borrow_from", i);
        rebuild_matrix();
    };
    addAndMakeVisible(borrow_from_);

    // The build strip and its keys.
    strip_ = std::make_unique<BuildStrip>(*this);
    strip_view_.setViewedComponent(strip_.get(), false);
    strip_view_.setScrollBarsShown(false, true);
    strip_view_.setScrollBarThickness(10);
    strip_view_.setComponentID("chord_build_view");
    addAndMakeVisible(strip_view_);
    add_bars_.setComponentID("chord_add_bars");
    remove_bar_.setComponentID("chord_remove_bar");
    add_bars_.setTooltip("One more bar at the end");
    remove_bar_.setTooltip("Take the last bar away");
    add_bars_.onClick = [this] { add_bars(1); };
    remove_bar_.onClick = [this] { remove_last_bar(); };
    addAndMakeVisible(add_bars_);
    addAndMakeVisible(remove_bar_);
    voice_picker_ = std::make_unique<VoicePicker>(*this);
    addAndMakeVisible(*voice_picker_);
    send_to_box_.setComponentID("chord_send_to");
    send_to_box_.setTitle("Send to");
    send_to_box_.addItem("The Target Track", 1);
    send_to_box_.addItem("Strings (spread)", 2);
    send_to_box_.addItem("Woodwinds (spread)", 3);
    send_to_box_.addItem("Brass (spread)", 4);
    send_to_box_.addItem("Choir (spread)", 5);
    send_to_box_.setSelectedId(1, juce::dontSendNotification);
    send_to_box_.setTooltip("Send to the Target track, or spread the chosen voices over a section: the lowest voice to its "
                            "lowest instrument (Basses), up to the highest (Violins I)");
    send_to_box_.onChange = [this] { set_send_to(static_cast<SendTo>(send_to_box_.getSelectedId() - 1)); };
    addAndMakeVisible(send_to_box_);
    for (auto* z : {&zoom_w_, &zoom_h_}) {
        z->setSliderStyle(juce::Slider::LinearHorizontal);
        z->setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        z->setDoubleClickReturnValue(true, 1.0);
        addAndMakeVisible(*z);
    }
    zoom_w_.setRange(0.5, 4.0, 0.01);
    zoom_h_.setRange(1.0, 2.6, 0.01);
    zoom_w_.setValue(1.0, juce::dontSendNotification);
    zoom_h_.setValue(1.0, juce::dontSendNotification);
    zoom_w_.setComponentID("chord_zoom_width");
    zoom_h_.setComponentID("chord_zoom_height");
    zoom_w_.setTitle("Bar width");
    zoom_h_.setTitle("Strip height");
    zoom_w_.setTooltip("Zoom the bars wider or narrower (double-click: back to normal)");
    zoom_h_.setTooltip("Make the progression taller to see the chords' notes (double-click: back to normal)");
    zoom_w_.onValueChange = [this] { set_build_zoom(zoom_w_.getValue(), zoom_height_); };
    zoom_h_.onValueChange = [this] { set_build_zoom(zoom_width_, zoom_h_.getValue()); };
    for (auto [label, text] : {std::pair{&zoom_w_caption_, "W"}, std::pair{&zoom_h_caption_, "H"}}) {
        label->setText(text, juce::dontSendNotification);
        label->setColour(juce::Label::textColourId, colours::text_dim);
        label->setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(*label);
    }
    for (auto* b : {&use_preset_, &use_build_key_}) {
        addAndMakeVisible(*b);
        b->setClickingTogglesState(true);
        b->setRadioGroupId(4712);
    }
    use_preset_.setConnectedEdges(juce::Button::ConnectedOnRight);
    use_build_key_.setConnectedEdges(juce::Button::ConnectedOnLeft);
    use_preset_.setToggleState(true, juce::dontSendNotification);
    use_preset_.setComponentID("chord_use_preset");
    use_build_key_.setComponentID("chord_use_build");
    use_preset_.setTooltip("Audition and Send the progression chosen on the left");
    use_build_key_.setTooltip("Audition and Send your progression from the build strip");
    use_preset_.onClick = [this] { use_build(false); };
    use_build_key_.onClick = [this] { use_build(true); };
    for (auto* b : {&fill_, &clear_}) addAndMakeVisible(*b);
    fill_.setTooltip("Copy the chosen progression's chords into the build, a bar each, to change them");
    clear_.setTooltip("Empty the build");
    fill_.onClick = [this] { fill_build_from_progression(); };
    clear_.onClick = [this] { clear_build(); };

    cards_ = std::make_unique<Cards>(c, *this);
    addAndMakeVisible(*cards_);

    chord_info_.setFont(juce::FontOptions(13.5f));
    chord_info_.setColour(juce::Label::textColourId, colours::text);
    addAndMakeVisible(chord_info_);
    keyboard_.setAvailableRange(36, 96);
    keyboard_.setOctaveForMiddleC(4);
    keyboard_.setInterceptsMouseClicks(false, false);
    keyboard_.setWantsKeyboardFocus(false);
    keyboard_.setScrollButtonsVisible(false);
    keyboard_.setColour(juce::MidiKeyboardComponent::keyDownOverlayColourId, colours::accent.withAlpha(0.85f));
    keyboard_.setColour(juce::MidiKeyboardComponent::shadowColourId, juce::Colours::transparentBlack);
    keyboard_.setColour(juce::MidiKeyboardComponent::keySeparatorLineColourId, colours::outline);
    addAndMakeVisible(keyboard_);

    addAndMakeVisible(audition_);
    addAndMakeVisible(stop_);
    addAndMakeVisible(target_);
    addAndMakeVisible(send_);
    audition_.setTooltip("Loop the progression (nothing is written)");
    send_.setTooltip("Write the progression to the target track (one undo step)");
    send_.setColour(juce::TextButton::buttonColourId, colours::accent);
    target_.setTooltip("Track that receives the progression");
    audition_.onClick = [this] { audition(); };
    stop_.onClick = [this] { context_.engine.stop_preview(); };
    send_.onClick = [this] { send(); };
    target_.onChange = [this] { rerender(); };

    rebuild_matrix();
    filter();
    setSize(1280, 960);
}

ChordExplorer::~ChordExplorer() { list_.setModel(nullptr); }

void ChordExplorer::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
    g.setColour(colours::panel);
    g.fillRect(0, 0, getWidth(), kTopBar);
    g.fillRect(0, getHeight() - kBottomBar, getWidth(), kBottomBar);
    g.setColour(colours::outline);
    g.drawHorizontalLine(kTopBar - 1, 0.0f, static_cast<float>(getWidth()));
    g.drawHorizontalLine(getHeight() - kBottomBar, 0.0f, static_cast<float>(getWidth()));
    g.drawVerticalLine(kLeftWidth, static_cast<float>(kTopBar), static_cast<float>(getHeight() - kBottomBar - strip_height()));
    // The build strip's band, above the bottom bar.
    const int strip_top = getHeight() - kBottomBar - strip_height();
    g.setColour(colours::panel.darker(0.15f));
    g.fillRect(0, strip_top, getWidth(), strip_height());
    g.setColour(colours::outline);
    g.drawHorizontalLine(strip_top, 0.0f, static_cast<float>(getWidth()));
    g.setColour(colours::text_dim);
    g.setFont(juce::FontOptions(11.5f, juce::Font::bold));
    g.drawText("YOUR PROGRESSION  -  drag chords into the cells (or double-click one); click a cell to edit its chord and notes; "
               "click a bar's " + juce::String::fromUTF8("\xc3\xb7") + " to divide it",
               16, strip_top + 6, getWidth() - 260, 20, juce::Justification::centredLeft);
}

void ChordExplorer::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop(kTopBar).reduced(12, 13);
    auto place = [&top](juce::Label& caption, int caption_w, juce::Component& c, int w) {
        caption.setBounds(top.removeFromLeft(caption_w));
        top.removeFromLeft(6);
        c.setBounds(top.removeFromLeft(w));
        top.removeFromLeft(14);
    };
    key_caption_.setBounds(top.removeFromLeft(28));
    top.removeFromLeft(6);
    tonic_.setBounds(top.removeFromLeft(64));
    top.removeFromLeft(4);
    mode_.setBounds(top.removeFromLeft(136));
    top.removeFromLeft(14);
    place(length_caption_, 46, length_, 86);
    place(start_caption_, 58, start_, 96);
    place(voicing_caption_, 52, voicing_, 92);
    place(rhythm_caption_, 48, rhythm_, 96);
    voice_lead_.setBounds(top.removeFromLeft(120));

    auto bottom = r.removeFromBottom(kBottomBar).reduced(12, 12);
    audition_.setBounds(bottom.removeFromLeft(100));
    bottom.removeFromLeft(6);
    stop_.setBounds(bottom.removeFromLeft(70));
    bottom.removeFromLeft(18);
    use_preset_.setBounds(bottom.removeFromLeft(80));
    use_build_key_.setBounds(bottom.removeFromLeft(90));
    send_.setBounds(bottom.removeFromRight(130));
    bottom.removeFromRight(8);
    target_.setBounds(bottom.removeFromRight(220));
    bottom.removeFromRight(6);
    send_to_box_.setBounds(bottom.removeFromRight(170));
    bottom.removeFromRight(6);
    target_caption_.setBounds(bottom.removeFromRight(50));

    {
        auto band = r.removeFromBottom(strip_height()).reduced(16, 6);
        auto head = band.removeFromTop(22);
        clear_.setBounds(head.removeFromRight(70));
        head.removeFromRight(6);
        fill_.setBounds(head.removeFromRight(130));
        band.removeFromTop(6);
        // Under the bars: more bars, and the zoom bottom right.
        auto foot = band.removeFromBottom(22);
        add_bars_.setBounds(foot.removeFromLeft(60));
        foot.removeFromLeft(6);
        remove_bar_.setBounds(foot.removeFromLeft(60));
        foot.removeFromLeft(24);
        voice_picker_->setBounds(foot.removeFromLeft(330));
        zoom_h_.setBounds(foot.removeFromRight(110));
        zoom_h_caption_.setBounds(foot.removeFromRight(24));
        foot.removeFromRight(10);
        zoom_w_.setBounds(foot.removeFromRight(110));
        zoom_w_caption_.setBounds(foot.removeFromRight(24));
        band.removeFromBottom(4);
        strip_view_.setBounds(band);
        strip_->update_size(band.getHeight() - strip_view_.getScrollBarThickness());
    }
    auto left = r.removeFromLeft(kLeftWidth).reduced(12);
    search_.setBounds(left.removeFromTop(28));
    left.removeFromTop(8);
    list_.setBounds(left);

    auto right = r.withTrimmedLeft(1).reduced(16, 12);
    auto caption = right.removeFromTop(24);
    ninths_.setBounds(caption.removeFromRight(52));
    sevenths_.setBounds(caption.removeFromRight(52));
    triads_.setBounds(caption.removeFromRight(64));
    matrix_caption_.setBounds(caption);
    right.removeFromTop(6);
    auto row = right.removeFromTop(54);
    const int gap = 6;
    const int bw = (row.getWidth() - gap * 6) / 7;
    for (auto& b : matrix_) {
        b->setBounds(row.removeFromLeft(bw));
        row.removeFromLeft(gap);
    }
    right.removeFromTop(8);
    // Borrowed chords (modal mixture), under the key's.
    {
        auto cap = right.removeFromTop(24);
        borrowed_caption_.setBounds(cap.removeFromLeft(330));
        borrow_from_.setBounds(cap.removeFromLeft(270).reduced(0, 1));
    }
    auto brow = right.removeFromTop(50);
    const int bn = std::max(1, static_cast<int>(borrowed_.size()));
    const int bbw = std::min(bw, (brow.getWidth() - gap * (bn - 1)) / bn);
    for (auto& b : borrowed_) {
        b->setBounds(brow.removeFromLeft(bbw));
        brow.removeFromLeft(gap);
    }
    right.removeFromTop(12);
    auto kb = right.removeFromBottom(84);
    chord_info_.setBounds(right.removeFromBottom(26));
    keyboard_.setKeyWidth(static_cast<float>(kb.getWidth()) / 36.0f);
    keyboard_.setBounds(kb);
    right.removeFromBottom(10);
    cards_->setBounds(right);
}

int ChordExplorer::getNumRows() { return static_cast<int>(shown_.size()); }

void ChordExplorer::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (row < 0 || row >= getNumRows()) return;
    const auto* p = shown_[static_cast<std::size_t>(row)];
    g.fillAll(selected ? colours::accent.withAlpha(0.28f) : (row % 2 ? colours::panel : colours::panel.brighter(0.025f)));
    g.setColour(colours::text);
    g.setFont(juce::FontOptions(13.5f));
    g.drawText(utf8(p->name), 10, 3, w - 60, h / 2, juce::Justification::bottomLeft, true);
    g.setColour(colours::text_dim);
    g.setFont(juce::FontOptions(11.0f));
    g.drawText(juce::String(p->bars) + " bars", w - 56, 3, 48, h / 2, juce::Justification::bottomRight);
    g.setColour(selected ? colours::text : colours::accent.withAlpha(0.9f));
    g.setFont(juce::FontOptions(12.0f));
    g.drawText(utf8(p->roman_text()), 10, h / 2 + 2, w - 16, h / 2 - 4, juce::Justification::topLeft, true);
    g.setColour(colours::outline.withAlpha(0.5f));
    g.drawHorizontalLine(h - 1, 0.0f, static_cast<float>(w));
}

void ChordExplorer::selectedRowsChanged(int) { progression_changed(); }

void ChordExplorer::filter()
{
    const auto* previous = chosen();
    const auto query = search_.getText().trim().toStdString();
    shown_.clear();
    const auto& catalog = context_.session.catalogs().progressions;
    if (query.empty())
        for (const auto& p : catalog.all()) shown_.push_back(&p);
    else
        shown_ = catalog.search(query);
    list_.updateContent();
    auto it = std::find(shown_.begin(), shown_.end(), previous);
    if (it != shown_.end()) list_.selectRow(static_cast<int>(it - shown_.begin()));
    else if (!shown_.empty()) list_.selectRow(0);
    else list_.deselectAllRows();
    list_.repaint();
    progression_changed();
}

bool ChordExplorer::select_progression(const std::string& name)
{
    for (std::size_t i = 0; i < shown_.size(); ++i)
        if (shown_[i]->name == name) {
            list_.selectRow(static_cast<int>(i));
            list_.scrollToEnsureRowIsOnscreen(static_cast<int>(i));
            return true;
        }
    return false;
}

const theory::Progression* ChordExplorer::chosen() const
{
    const int row = list_.getSelectedRow();
    return row >= 0 && row < static_cast<int>(shown_.size()) ? shown_[static_cast<std::size_t>(row)] : nullptr;
}

theory::Key ChordExplorer::key() const
{
    if (auto k = dialogs::key_from_combos(tonic_, mode_)) return *k;
    return theory::Key{theory::SpelledPitch{0, 0}, theory::Mode::Major};
}

theory::VoicingOptions ChordExplorer::voicing_options() const
{
    theory::VoicingOptions v;
    static const theory::VoicingStyle styles[] = {theory::VoicingStyle::Closed, theory::VoicingStyle::Open,
                                                  theory::VoicingStyle::Drop2, theory::VoicingStyle::Drop3,
                                                  theory::VoicingStyle::Spread, theory::VoicingStyle::TwoHands};
    v.style = styles[juce::jlimit(1, 6, voicing_.getSelectedId()) - 1];
    v.voice_lead = voice_lead_.getToggleState();
    const auto id = target_.track_id();
    if (v.style == theory::VoicingStyle::TwoHands || send_to_ != SendTo::Track) {
        // Both hands (or a section): the whole keyboard; a section's parts are fitted to each instrument.
        v.low = 21;
        v.high = 108;
        v.center = 66;
    } else if (const auto* t = id.empty() ? nullptr : context_.session.data().find_track(id)) {
        const auto [low, high] = context_.session.range_for(*t);
        v.low = std::max(low, 28);
        v.high = std::min(high, 100);
        if (v.high - v.low < 12) {
            v.low = low;
            v.high = high;
        }
        v.center = v.high - v.low >= 24 ? juce::jlimit(v.low + 10, v.high - 10, 60) : (v.low + v.high) / 2;
    }
    return v;
}

void ChordExplorer::progression_changed()
{
    const auto* p = chosen();
    const int current = length_.getSelectedId();
    length_.clear(juce::dontSendNotification);
    length_.addItem("4 bars", 4);
    length_.addItem("8 bars", 8);
    if (p && (p->fixed_length || p->bars == 12 ||
              std::find(p->allowed_bar_lengths.begin(), p->allowed_bar_lengths.end(), 12) != p->allowed_bar_lengths.end())) {
        length_.addItem("12 bars", 12);
        length_.addItem("24 bars", 24);
    }
    length_.setSelectedId(length_.indexOfItemId(current) >= 0 ? current : 4, juce::dontSendNotification);
    rerender();
}

void ChordExplorer::rebuild_matrix()
{
    const auto k = key();
    const auto chords = theory::diatonic_chords(k, chord_size_);
    matrix_caption_.setText("CHORDS IN " + utf8(k.tonic.display_name()).toUpperCase() + " " +
                                juce::String(theory::display_name(k.mode)).toUpperCase() + "  -  click to audition",
                            juce::dontSendNotification);
    for (std::size_t i = 0; i < matrix_.size(); ++i) {
        const bool has = i < chords.size();
        matrix_[i]->setVisible(has);
        if (has) matrix_[i]->set(chords[i], utf8(theory::roman_label(static_cast<int>(i) + 1, chords[i].quality)));
    }
    // Borrowed from the parallel key (and the Neapolitan): triads, or sevenths
    // for 7ths and 9ths.
    const auto ref = theory::key_reference(k);
    const bool minor_home = theory::is_minor_family(ref.key.mode);
    {
        borrow_from_.clear(juce::dontSendNotification);
        int n = 0;
        for (auto src : theory::borrow_sources())
            borrow_from_.addItem("From " + juce::String(theory::borrow_source_name(src, minor_home)) +
                                     (src == theory::BorrowSource::Parallel ? " + Neapolitan" : ""),
                                 ++n);
        const auto& sources = theory::borrow_sources();
        const auto it = std::find(sources.begin(), sources.end(), borrow_source_);
        borrow_from_.setSelectedItemIndex(it == sources.end() ? 0 : static_cast<int>(it - sources.begin()),
                                          juce::dontSendNotification);
    }
    const auto borrowed = theory::borrowed_chords(ref, borrow_source_, chord_size_ > 3);
    for (auto& b : borrowed_) removeChildComponent(b.get());
    borrowed_.clear();
    for (const auto& bc : borrowed) {
        auto b = std::make_unique<MatrixButton>();
        b->borrowed = true;
        b->set(bc.chord, utf8(bc.roman));
        b->setTooltip("Borrowed from " + utf8(bc.source) + (bc.name.empty() ? juce::String() : " (" + utf8(bc.name) + ")") +
                      ": click to hear, drag into the build");
        auto* raw = b.get();
        raw->onClick = [this, raw] {
            if (!raw->was_dragged()) audition_chord(raw->chord());
        };
        raw->on_double_click = [this, raw] { add_to_build(raw->chord(), raw->numeral()); };
        addAndMakeVisible(*raw);
        borrowed_.push_back(std::move(b));
    }
    borrowed_caption_.setText(borrowed.empty() ? juce::String("BORROWED CHORDS  -  this mode adds none to the key")
                                               : juce::String("BORROWED CHORDS  -  drag into the build"),
                              juce::dontSendNotification);
    resized();
}

void ChordExplorer::rerender()
{
    render_.reset();
    error_.clear();
    cards_->selected = -1;
    const auto* p = chosen();
    if (p) {
        const auto o = render_options();
        std::string error;
        render_ = theory::render_progression(*p, context_.session.data().meter, o, &error);
        if (!render_) {
            error_ = utf8(error.empty() ? std::string("This progression cannot be rendered with these settings.") : error);
            if (p->fixed_length) error_ << "\nChoose " << juce::String(p->bars) << " or " << juce::String(p->bars * 2)
                                        << " bars to play whole choruses.";
        }
    }
    build_render_ = render_build();
    audition_.setEnabled(active_render() != nullptr);
    send_.setEnabled(active_render() != nullptr);
    if (strip_) strip_->repaint();
    if (render_ && !render_->chords.empty()) {
        const auto& first = render_->chords.front();
        show_on_keyboard(first.voicing.notes, describe(first.numeral, first.chord, first.voicing.notes));
    } else {
        show_on_keyboard({}, {});
    }
    cards_->repaint();
}

juce::String ChordExplorer::describe(const std::string& numeral, const theory::Chord& chord, const std::vector<int>& notes)
{
    juce::String s;
    if (!numeral.empty()) s << utf8(numeral) << "  =  ";
    s << utf8(chord.display_name()) << "      voiced  " << voicing_text(notes, chord);
    return s;
}

void ChordExplorer::show_on_keyboard(const std::vector<int>& notes, const juce::String& caption)
{
    chord_info_.setText(caption, juce::dontSendNotification);
    keyboard_state_.reset();
    for (int n : notes) keyboard_state_.noteOn(1, n, 0.8f);
    keyboard_.repaint();
}

void ChordExplorer::audition_chord(const theory::Chord& chord)
{
    const auto v = theory::voice_chord(chord, voicing_options());
    std::vector<NoteEvent> notes;
    for (int n : v.notes) notes.push_back({0, kTicksPerQuarter * 3, n, 84, 0, {}});
    const auto [family, program] = dialogs::preview_voice(context_, target_.track_id());
    context_.engine.preview(notes, family, program, dialogs::project_bpm(context_), false);
    std::string numeral;
    for (std::size_t i = 0; i < matrix_.size(); ++i)
        if (&matrix_[i]->chord() == &chord) numeral = theory::roman_label(static_cast<int>(i) + 1, chord.quality);
    show_on_keyboard(v.notes, describe(numeral, chord, v.notes));
    context_.status("Auditioning " + utf8(chord.display_name()) + ": " + voicing_text(v.notes, chord));
}

void ChordExplorer::audition_card(int index)
{
    if (!render_ || index < 0 || index >= static_cast<int>(render_->chords.size())) return;
    const auto& rc = render_->chords[static_cast<std::size_t>(index)];
    std::vector<NoteEvent> notes;
    const Ticks length = std::min<Ticks>(rc.duration, kTicksPerQuarter * 4);
    for (int n : rc.voicing.notes) notes.push_back({0, length, n, 84, 0, {}});
    const auto [family, program] = dialogs::preview_voice(context_, target_.track_id());
    context_.engine.preview(notes, family, program, dialogs::project_bpm(context_), false);
    show_on_keyboard(rc.voicing.notes, describe(rc.numeral, rc.chord, rc.voicing.notes));
    context_.status(utf8(rc.numeral) + " = " + utf8(rc.chord.display_name()) + ": " + voicing_text(rc.voicing.notes, rc.chord));
}

void ChordExplorer::audition()
{
    const auto* r = active_render();
    if (!r) return;
    const auto [family, program] = dialogs::preview_voice(context_, target_.track_id());
    context_.engine.preview(dialogs::shifted(r->notes, r->start), family, program, dialogs::project_bpm(context_), true,
                            r->end - r->start);
    if (use_build_) context_.status("Looping your progression - press Stop to end the audition");
    else if (const auto* p = chosen()) context_.status("Looping " + utf8(p->name) + " - press Stop to end the audition");
}

void ChordExplorer::send()
{
    // The window can outlive a project: re-read the tracks so a target from
    // another project is not used (it falls back to the selected track, or a new one).
    if (const auto id = target_.track_id(); !id.empty() && !context_.session.data().find_track(id)) {
        target_.setSelectedId(1, juce::dontSendNotification);
        target_.refresh();
        rerender();
    }
    if (use_build_) {
        std::string track = target_.track_id();
        if (track.empty()) {
            track = context_.session.add_track("Harmony", "keys.piano");
            if (track.empty()) return;
            target_.refresh();
            target_.select(track);
            rerender();
        }
        if (!build_render_) return;
        if (send_to_ != SendTo::Track) {
            send_to_section(*build_render_, "Your progression");
            return;
        }
        int bars = 0;
        for (int b = 0; b < bar_count(); ++b)
            for (const auto& c : bars_[static_cast<std::size_t>(b)].cells)
                if (c) bars = b + 1;
        const juce::String what = "Your progression (" + juce::String(bars) + " bars from bar " +
                                  juce::String(static_cast<int>(start_.getValue())) + ")";
        send_with(track, chosen_voices(*build_render_), "built", what, project::CollisionPolicy::Ask);
        return;
    }
    const auto* p = chosen();
    if (!p) return;
    std::string track = target_.track_id();
    if (track.empty()) {
        track = context_.session.add_track("Harmony", "keys.piano");
        if (track.empty()) {
            context_.status("Could not create a harmony track");
            return;
        }
        target_.refresh();
        target_.select(track);
        rerender();  // the new track's range
    }
    if (!render_) return;
    const juce::String what = utf8(p->name) + " (" + juce::String(length_.getSelectedId()) + " bars from bar " +
                              juce::String(static_cast<int>(start_.getValue())) + ")";
    if (send_to_ != SendTo::Track) {
        send_to_section(*render_, what);
        return;
    }
    send_with(track, chosen_voices(*render_), p->id, what, project::CollisionPolicy::Ask);
}

void ChordExplorer::send_with(const std::string& track_id, const theory::ProgressionRender& render,
                              const std::string& prog_id, const juce::String& what, project::CollisionPolicy policy)
{
    const auto result = context_.session.send_progression(track_id, render, prog_id, policy);
    if (result.needs_decision) {
        juce::Component::SafePointer<ChordExplorer> self(this);
        dialogs::ask_collision(what, [self, track_id, render, prog_id, what](project::CollisionPolicy decided) {
            if (self != nullptr) self->send_with(track_id, render, prog_id, what, decided);
        });
        return;
    }
    dialogs::report_send(context_, result, what);
}

// --- The build ---------------------------------------------------------------------------------

theory::ProgressionRenderOptions ChordExplorer::render_options() const
{
    theory::ProgressionRenderOptions o;
    o.key = key();
    o.start_bar = static_cast<int>(start_.getValue());
    o.length_bars = std::max(1, length_.getSelectedId());
    o.expand = theory::ExpandMode::Stretch;
    o.voicing = voicing_options();
    static const theory::ChordRhythm rhythms[] = {theory::ChordRhythm::Block, theory::ChordRhythm::BeatPulse,
                                                  theory::ChordRhythm::Arpeggio, theory::ChordRhythm::Strum};
    o.rhythm = rhythms[juce::jlimit(1, 4, rhythm_.getSelectedId()) - 1];
    return o;
}

const theory::ProgressionRender* ChordExplorer::active_render() const
{
    if (use_build_) return build_render_ ? &*build_render_ : nullptr;
    return render_ ? &*render_ : nullptr;
}

int ChordExplorer::first_bar() const { return static_cast<int>(start_.getValue()); }

int ChordExplorer::strip_height() const { return juce::roundToInt(178.0 * zoom_height_); }

std::optional<theory::ProgressionRender> ChordExplorer::render_build() const
{
    const auto& meter = context_.session.data().meter;
    const int start = first_bar();
    std::vector<theory::Chord> chords;
    std::vector<Ticks> starts, durations;
    std::vector<std::string> numerals;
    std::vector<std::vector<int>> edited;
    // Bar by bar, cell by cell; an empty cell holds the chord before it (up to the last chord).
    for (int b = 0; b < bar_count(); ++b) {
        const auto& bar = bars_[static_cast<std::size_t>(b)];
        const Ticks from = meter.bar_start(start + b), to = meter.bar_start(start + b + 1);
        for (int c = 0; c < bar.divisions; ++c) {
            const Ticks cs = from + (to - from) * c / bar.divisions, ce = from + (to - from) * (c + 1) / bar.divisions;
            const auto& cell = bar.cells[static_cast<std::size_t>(c)];
            if (!cell) continue;
            if (!durations.empty()) durations.back() = cs - starts.back();  // the one before rings up to here
            chords.push_back(cell->chord);
            starts.push_back(cs);
            durations.push_back(ce - cs);
            numerals.push_back(cell->numeral);
            edited.push_back(cell->notes);
        }
    }
    if (chords.empty()) return std::nullopt;
    const auto options = render_options();
    // Voiced together (voice leading), then the edited chords play their own notes.
    auto voicings = theory::voice_progression(chords, options.voicing);
    for (std::size_t i = 0; i < chords.size() && i < voicings.size(); ++i)
        if (!edited[i].empty()) {
            auto n = edited[i];
            std::sort(n.begin(), n.end());
            voicings[i].notes = n;
        }
    return theory::realize_voiced(chords, voicings, starts, durations, numerals, meter, options);
}

void ChordExplorer::build_changed()
{
    // Which cell each rendered chord came from (the editor shows its voiced notes).
    build_cells_.clear();
    for (int b = 0; b < bar_count(); ++b)
        for (int c = 0; c < divisions(b); ++c)
            if (cell(b, c)) build_cells_.push_back({b, c});
    rerender();
    if (strip_) strip_->repaint();
}

std::vector<int> ChordExplorer::cell_notes(int bar, int c) const
{
    const auto& ch = cell(bar, c);
    if (!ch) return {};
    if (!ch->notes.empty()) {
        auto n = ch->notes;
        std::sort(n.begin(), n.end());
        return n;
    }
    if (build_render_)
        for (std::size_t i = 0; i < build_cells_.size() && i < build_render_->chords.size(); ++i)
            if (build_cells_[i] == std::make_pair(bar, c)) return build_render_->chords[i].voicing.notes;
    return theory::voice_chord(ch->chord, voicing_options()).notes;
}

void ChordExplorer::set_cell(int bar, int c, const theory::Chord& chord, const std::string& numeral)
{
    if (bar < 0 || bar >= bar_count() || c < 0 || c >= divisions(bar)) return;
    bars_[static_cast<std::size_t>(bar)].cells[static_cast<std::size_t>(c)] = BuildChord{chord, numeral, {}};
    use_build(true);  // building: Audition and Send use the build
    audition_notes(cell_notes(bar, c));
    context_.status("Bar " + juce::String(first_bar() + bar) +
                    (divisions(bar) > 1 ? ", chord " + juce::String(c + 1) : juce::String()) + ": " + utf8(chord.display_name()));
}

void ChordExplorer::clear_cell(int bar, int c)
{
    if (bar < 0 || bar >= bar_count() || c < 0 || c >= divisions(bar)) return;
    bars_[static_cast<std::size_t>(bar)].cells[static_cast<std::size_t>(c)].reset();
    build_changed();
}

void ChordExplorer::set_cell_notes(int bar, int c, std::vector<int> notes)
{
    if (bar < 0 || bar >= bar_count() || c < 0 || c >= divisions(bar)) return;
    auto& ch = bars_[static_cast<std::size_t>(bar)].cells[static_cast<std::size_t>(c)];
    if (!ch) return;
    std::sort(notes.begin(), notes.end());
    notes.erase(std::unique(notes.begin(), notes.end()), notes.end());
    ch->notes = std::move(notes);
    build_changed();
}

void ChordExplorer::set_divisions(int bar, int n)
{
    if (bar < 0 || bar >= bar_count()) return;
    n = juce::jlimit(1, kMaxDivisions, n);
    auto& b = bars_[static_cast<std::size_t>(bar)];
    if (b.divisions == n) return;
    // Each chord goes to the new cell that starts where it started (or the one it falls in).
    std::vector<std::optional<BuildChord>> cells(static_cast<std::size_t>(n));
    for (int c = 0; c < b.divisions; ++c)
        if (const auto& ch = b.cells[static_cast<std::size_t>(c)]) {
            const int to = c * n / b.divisions;
            if (!cells[static_cast<std::size_t>(to)]) cells[static_cast<std::size_t>(to)] = ch;
        }
    b.divisions = n;
    b.cells = std::move(cells);
    build_changed();
    context_.status("Bar " + juce::String(first_bar() + bar) + " holds " + juce::String(n) + (n == 1 ? " chord" : " chords"));
}

void ChordExplorer::add_bars(int n)
{
    for (int i = 0; i < n; ++i) bars_.emplace_back();
    resized();
    build_changed();
    strip_view_.setViewPosition(std::max(0, strip_->getWidth() - strip_view_.getWidth()), 0);
}

void ChordExplorer::remove_last_bar()
{
    if (bar_count() <= 1) return;
    bars_.pop_back();
    resized();
    build_changed();
}

void ChordExplorer::set_build_zoom(double width, double height)
{
    zoom_width_ = juce::jlimit(0.5, 4.0, width);
    zoom_height_ = juce::jlimit(1.0, 2.6, height);
    zoom_w_.setValue(zoom_width_, juce::dontSendNotification);
    zoom_h_.setValue(zoom_height_, juce::dontSendNotification);
    resized();
    repaint();
}

void ChordExplorer::audition_notes(const std::vector<int>& notes)
{
    if (notes.empty()) return;
    std::vector<NoteEvent> events;
    for (int n : notes) events.push_back({0, kTicksPerQuarter * 3, n, 84, 0, {}});
    const auto [family, program] = dialogs::preview_voice(context_, target_.track_id());
    context_.engine.preview(events, family, program, dialogs::project_bpm(context_), false);
    show_on_keyboard(notes, {});
}

void ChordExplorer::edit_cell(int bar, int c)
{
    if (bar < 0 || bar >= bar_count() || c < 0 || c >= divisions(bar) || !strip_) return;
    auto editor = std::make_unique<CellEditor>(*this, bar, c);
    const auto area = strip_->localAreaToGlobal(strip_->cell_rect(bar, c));
    juce::CallOutBox::launchAsynchronously(std::move(editor), area, nullptr);
}

bool ChordExplorer::add_to_build(const theory::Chord& chord, const std::string& numeral)
{
    for (int b = 0; b < bar_count(); ++b)
        for (int c = 0; c < divisions(b); ++c)
            if (!cell(b, c)) {
                set_cell(b, c, chord, numeral);
                return true;
            }
    bars_.emplace_back();
    resized();
    set_cell(bar_count() - 1, 0, chord, numeral);
    return true;
}

void ChordExplorer::clear_build_chord(int slot)
{
    if (slot < 0 || slot >= bar_count()) return;
    for (auto& c : bars_[static_cast<std::size_t>(slot)].cells) c.reset();
    build_changed();
}

void ChordExplorer::clear_build()
{
    for (auto& b : bars_) b = BuildBar{};
    build_changed();
}

void ChordExplorer::fill_build_from_progression()
{
    if (!render_) return;
    const auto& meter = context_.session.data().meter;
    const int start_bar = meter.bar_at(render_->start);
    const int last_bar = meter.bar_at(render_->end - 1);
    bars_.assign(static_cast<std::size_t>(std::max(kBuildBars, last_bar - start_bar + 1)), BuildBar{});
    // Each bar divided as finely as its chords fall (whole, halves, thirds, quarters...).
    for (int bar = start_bar; bar <= last_bar; ++bar) {
        const Ticks from = meter.bar_start(bar), len = meter.bar_start(bar + 1) - from;
        std::vector<const theory::RealizedChord*> in;
        for (const auto& rc : render_->chords)
            if (meter.bar_at(rc.start) == bar) in.push_back(&rc);
        if (in.empty()) continue;
        int n = 1;
        for (int d : {1, 2, 3, 4, 6, 8, 12, 16}) {
            n = d;
            if (std::all_of(in.begin(), in.end(), [&](const auto* rc) { return ((rc->start - from) * d) % len == 0; })) break;
        }
        auto& b = bars_[static_cast<std::size_t>(bar - start_bar)];
        b.divisions = n;
        b.cells.assign(static_cast<std::size_t>(n), std::nullopt);
        for (const auto* rc : in) {
            const int c = static_cast<int>((rc->start - from) * n / len);
            if (!b.cells[static_cast<std::size_t>(c)]) b.cells[static_cast<std::size_t>(c)] = BuildChord{rc->chord, rc->numeral, {}};
        }
    }
    resized();
    use_build(true);
    context_.status("The progression is in the build: drag chords onto its cells, or click one to change it");
}

void ChordExplorer::use_build(bool build)
{
    use_build_ = build;
    use_preset_.setToggleState(!build, juce::dontSendNotification);
    use_build_key_.setToggleState(build, juce::dontSendNotification);
    build_changed();
}

const theory::Chord& ChordExplorer::borrowed_chord(int i) const { return borrowed_[static_cast<std::size_t>(i)]->chord(); }
const theory::Chord& ChordExplorer::key_chord(int i) const { return matrix_[static_cast<std::size_t>(i)]->chord(); }

// --- Voices and sections -------------------------------------------------------------------------

void ChordExplorer::set_voice_selected(int voice, bool on)
{
    if (voice < 0 || voice >= kMaxVoices) return;
    voices_[static_cast<std::size_t>(voice)] = on;
    if (voice_picker_) voice_picker_->repaint();
}

bool ChordExplorer::voice_selected(int voice) const
{
    return voice >= 0 && voice < kMaxVoices && voices_[static_cast<std::size_t>(voice)];
}

void ChordExplorer::set_send_to(SendTo to)
{
    send_to_ = to;
    send_to_box_.setSelectedId(static_cast<int>(to) + 1, juce::dontSendNotification);
    target_.setEnabled(to == SendTo::Track);
    rerender();
}

theory::ProgressionRender ChordExplorer::chosen_voices(const theory::ProgressionRender& render) const
{
    theory::ProgressionRender out = render;
    out.notes.clear();
    for (auto& rc : out.chords) {
        auto sorted = rc.voicing.notes;
        std::sort(sorted.begin(), sorted.end());
        std::vector<int> keep;
        for (std::size_t v = 0; v < sorted.size(); ++v)
            // Voices past the sixth go with the top one.
            if (voice_selected(static_cast<int>(std::min<std::size_t>(v, kMaxVoices - 1)))) keep.push_back(sorted[v]);
        for (const auto& n : render.notes)
            if (n.start >= rc.start && n.start < rc.start + rc.duration &&
                std::find(keep.begin(), keep.end(), n.pitch) != keep.end())
                out.notes.push_back(n);
        rc.voicing.notes = keep;
    }
    return out;
}

void ChordExplorer::send_to_section(const theory::ProgressionRender& render, const juce::String& what)
{
    using S = session::Session::Section;
    const S section = send_to_ == SendTo::Strings ? S::Strings : send_to_ == SendTo::Woodwinds ? S::Woodwinds
                                                    : send_to_ == SendTo::Brass         ? S::Brass
                                                                                        : S::Choir;
    auto& session = context_.session;
    // The section's instruments that are in the project, lowest first.
    std::vector<std::string> tracks;
    for (const auto& id : session.section_tracks(section))
        if (!id.empty()) tracks.insert(tracks.begin(), id);
    if (tracks.empty()) {
        context_.status("No " + send_to_box_.getText().upToFirstOccurrenceOf(" (", false, false).toLowerCase() +
                        " in this project to spread the chords over");
        return;
    }
    const auto picked = chosen_voices(render);
    // Voice v of each chord (counting the chosen ones up from the bass) to
    // track t: one each when the numbers match; otherwise spread evenly.
    const int m = static_cast<int>(tracks.size());
    std::vector<std::pair<std::string, theory::ProgressionRender>> parts;
    for (int t = 0; t < m; ++t) {
        theory::ProgressionRender part = picked;
        part.notes.clear();
        const auto* track = session.data().find_track(tracks[static_cast<std::size_t>(t)]);
        const auto [lo, hi] = track ? session.range_for(*track) : std::pair<int, int>{0, 127};
        for (auto& rc : part.chords) {
            const auto& notes = rc.voicing.notes;  // the chosen voices, ascending
            const int k = static_cast<int>(notes.size());
            std::vector<int> mine;
            if (k > 0) {
                if (k >= m) {
                    for (int v = 0; v < k; ++v)
                        if (v * m / k == t) mine.push_back(notes[static_cast<std::size_t>(v)]);
                } else {
                    mine.push_back(notes[static_cast<std::size_t>(m == 1 ? 0 : juce::roundToInt(double(t) * (k - 1) / (m - 1)))]);
                }
            }
            // Into the instrument's range, by octaves.
            std::vector<int> fitted;
            for (int n : mine) {
                int x = n;
                while (x < lo && x + 12 <= 127) x += 12;
                while (x > hi && x - 12 >= 0) x -= 12;
                fitted.push_back(x);
            }
            for (const auto& n : picked.notes)
                if (n.start >= rc.start && n.start < rc.start + rc.duration)
                    for (std::size_t i = 0; i < mine.size(); ++i)
                        if (n.pitch == mine[i]) {
                            auto e = n;
                            e.pitch = fitted[i];
                            part.notes.push_back(e);
                        }
            rc.voicing.notes = fitted;
        }
        parts.push_back({tracks[static_cast<std::size_t>(t)], std::move(part)});
    }
    const int written = session.send_progression_parts(parts, use_build_ ? "built" : "chord_explorer",
                                                        "Spread " + what.toStdString() + " over the " +
                                                            send_to_box_.getText().upToFirstOccurrenceOf(" (", false, false).toStdString());
    context_.status(what + " spread over " + juce::String(written) + " instruments, the lowest voice to the lowest (Undo takes it back)");
}

}  // namespace vcc::desktop
