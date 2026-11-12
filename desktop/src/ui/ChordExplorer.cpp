#include "ui/ChordExplorer.h"

#include <algorithm>
#include <cmath>

#include "ui/Theme.h"

namespace vcc::desktop {

namespace {

constexpr int kLeftWidth = 300;
constexpr int kTopBar = 52;
constexpr int kBottomBar = 54;

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

    void paintButton(juce::Graphics& g, bool highlighted, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced(1.0f);
        g.setColour(down ? colours::accent.withAlpha(0.45f) : highlighted ? colours::panel_raised.brighter(0.08f) : colours::panel_raised);
        g.fillRoundedRectangle(r, 5.0f);
        g.setColour(highlighted ? colours::accent : colours::outline);
        g.drawRoundedRectangle(r.reduced(0.5f), 5.0f, 1.0f);
        auto area = getLocalBounds().reduced(4, 5);
        g.setColour(colours::text);
        g.setFont(juce::FontOptions(16.0f, juce::Font::bold));
        g.drawText(numeral_, area.removeFromTop(area.getHeight() / 2 + 2), juce::Justification::centredBottom, true);
        g.setColour(colours::accent);
        g.setFont(juce::FontOptions(13.0f));
        g.drawText(utf8(chord_.display_name()), area, juce::Justification::centredTop, true);
    }

private:
    theory::Chord chord_;
    juce::String numeral_;
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

        // What Send to Track will write: the rendered notes, rhythm included.
        if (!rects_.empty()) {
            auto rest = r.withTop(rects_.back().getBottom() + 14);
            if (rest.getHeight() >= 70) {
                g.setColour(colours::text_dim);
                g.setFont(juce::FontOptions(11.5f, juce::Font::bold));
                g.drawText("NOTES TO SEND  -  " + juce::String(static_cast<int>(owner_.render_->notes.size())) + " notes, bars " +
                               juce::String(meter.bar_at(owner_.render_->start)) + juce::String::fromUTF8("–") +
                               juce::String(meter.bar_at(owner_.render_->end - 1)),
                           rest.removeFromTop(18), juce::Justification::centredLeft);
                rest.removeFromTop(4);
                dialogs::draw_mini_roll(g, rest.withHeight(std::min(rest.getHeight(), 170)).toFloat(), owner_.render_->notes,
                                        owner_.render_->start, owner_.render_->end, meter, colours::accent);
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
    tonic_.setTooltip("Preview key (the project key is not changed)");
    mode_.setTooltip("Mode");
    for (auto* cb : {&tonic_, &mode_}) {
        addAndMakeVisible(*cb);
        cb->onChange = [this] {
            rebuild_matrix();
            rerender();
        };
    }

    length_.addItem("4 bars", 4);
    length_.addItem("8 bars", 8);
    length_.setSelectedId(4, juce::dontSendNotification);
    addAndMakeVisible(length_);
    length_.onChange = [this] { rerender(); };

    start_.setSliderStyle(juce::Slider::IncDecButtons);
    start_.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 44, 24);
    start_.setRange(1.0, 999.0, 1.0);
    start_.setValue(1.0, juce::dontSendNotification);
    start_.setIncDecButtonsMode(juce::Slider::incDecButtonsNotDraggable);
    start_.setTooltip("Bar the progression starts at when sent");
    addAndMakeVisible(start_);
    start_.onValueChange = [this] { rerender(); };

    voicing_.addItem("Closed", 1);
    voicing_.addItem("Open", 2);
    voicing_.addItem("Drop 2", 3);
    voicing_.addItem("Drop 3", 4);
    voicing_.addItem("Spread", 5);
    voicing_.setSelectedId(1, juce::dontSendNotification);
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
        b->onClick = [this, b] { audition_chord(b->chord()); };
    }

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
    setSize(1060, 680);
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
    g.drawVerticalLine(kLeftWidth, static_cast<float>(kTopBar), static_cast<float>(getHeight() - kBottomBar));
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
    send_.setBounds(bottom.removeFromRight(130));
    bottom.removeFromRight(8);
    target_.setBounds(bottom.removeFromRight(240));
    bottom.removeFromRight(6);
    target_caption_.setBounds(bottom.removeFromRight(50));

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
    right.removeFromTop(14);
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
                                                  theory::VoicingStyle::Spread};
    v.style = styles[juce::jlimit(1, 5, voicing_.getSelectedId()) - 1];
    v.voice_lead = voice_lead_.getToggleState();
    const auto id = target_.track_id();
    if (const auto* t = id.empty() ? nullptr : context_.session.data().find_track(id)) {
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
}

void ChordExplorer::rerender()
{
    render_.reset();
    error_.clear();
    cards_->selected = -1;
    const auto* p = chosen();
    if (p) {
        theory::ProgressionRenderOptions o;
        o.key = key();
        o.start_bar = static_cast<int>(start_.getValue());
        o.length_bars = std::max(1, length_.getSelectedId());
        o.expand = theory::ExpandMode::Stretch;
        o.voicing = voicing_options();
        static const theory::ChordRhythm rhythms[] = {theory::ChordRhythm::Block, theory::ChordRhythm::BeatPulse,
                                                      theory::ChordRhythm::Arpeggio, theory::ChordRhythm::Strum};
        o.rhythm = rhythms[juce::jlimit(1, 4, rhythm_.getSelectedId()) - 1];
        std::string error;
        render_ = theory::render_progression(*p, context_.session.data().meter, o, &error);
        if (!render_) {
            error_ = utf8(error.empty() ? std::string("This progression cannot be rendered with these settings.") : error);
            if (p->fixed_length) error_ << "\nChoose " << juce::String(p->bars) << " or " << juce::String(p->bars * 2)
                                        << " bars to play whole choruses.";
        }
    }
    audition_.setEnabled(render_.has_value());
    send_.setEnabled(render_.has_value());
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
    if (!render_) return;
    const auto [family, program] = dialogs::preview_voice(context_, target_.track_id());
    context_.engine.preview(dialogs::shifted(render_->notes, render_->start), family, program, dialogs::project_bpm(context_),
                            true, render_->end - render_->start);
    if (const auto* p = chosen()) context_.status("Looping " + utf8(p->name) + " - press Stop to end the audition");
}

void ChordExplorer::send()
{
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
    send_with(track, *render_, p->id, what, project::CollisionPolicy::Ask);
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

}  // namespace vcc::desktop
