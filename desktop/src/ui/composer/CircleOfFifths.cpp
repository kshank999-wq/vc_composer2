#include "ui/composer/CircleOfFifths.h"

#include <cmath>

#include "AppResources.h"
#include "ui/composer/ChordPalette.h"
#include "ui/Dialogs.h"
#include "ui/hw/HwTheme.h"
#include "vcc/theory/Progression.h"
#include "vcc/theory/Voicing.h"

namespace vcc::desktop::composer_ui {

namespace {

constexpr float kPi = juce::MathConstants<float>::pi;

juce::String disp(const theory::SpelledPitch& p) { return juce::String::fromUTF8(p.display_name().c_str()); }

/// "A C E": a chord's notes, spelled.
juce::String chord_notes(const theory::Chord& c)
{
    juce::String out;
    for (const auto& t : c.tones()) out << (out.isEmpty() ? "" : " ") << disp(t);
    return out;
}

juce::String chord_label(const theory::Chord& c) { return juce::String::fromUTF8(c.display_name().c_str()); }

juce::String key_title(const theory::Key& k)
{
    return disp(k.tonic) + (theory::is_minor_family(k.mode) ? " minor" : " major");
}

juce::Component::SafePointer<dialogs::ToolWindow>& window_slot()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> slot;
    return slot;
}

CircleOfFifthsPanel*& panel_slot()
{
    static CircleOfFifthsPanel* panel = nullptr;
    return panel;
}

}  // namespace

struct CircleOfFifthsPanel::Player final : juce::Timer {
    /// The render's notes as timed on/offs (ms from the start), and when each chord begins.
    struct Event {
        double ms;
        bool on;
        int note;
    };
    Player(CircleOfFifthsPanel& p, const theory::ProgressionRender& render, const TempoMap& tempo) : panel(p)
    {
        const double t0 = tempo.seconds_at(render.start);
        auto ms_at = [&](Ticks t) { return (tempo.seconds_at(t) - t0) * 1000.0; };
        for (const auto& n : render.notes) {
            events.push_back({ms_at(n.start), true, n.pitch});
            events.push_back({ms_at(n.start + n.duration), false, n.pitch});
        }
        std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
            return a.ms < b.ms || (!(b.ms < a.ms) && !a.on && b.on);  // offs before ons at the same moment
        });
        for (const auto& rc : render.chords) chord_ms.push_back(ms_at(rc.start));
        end_ms = ms_at(render.end);
        started = juce::Time::getMillisecondCounterHiRes();
        timerCallback();
        startTimer(10);
    }
    void release()
    {
        for (int n : sounding) panel.context_.engine.send_live(juce::MidiMessage::noteOff(1, n));
        sounding.clear();
    }
    void timerCallback() override
    {
        const double now = juce::Time::getMillisecondCounterHiRes() - started;
        while (next < events.size() && events[next].ms <= now) {
            const auto& e = events[next++];
            if (e.on) {
                panel.context_.engine.send_live(juce::MidiMessage::noteOn(1, e.note, static_cast<juce::uint8>(85)));
                sounding.push_back(e.note);
            } else {
                panel.context_.engine.send_live(juce::MidiMessage::noteOff(1, e.note));
                const auto it = std::find(sounding.begin(), sounding.end(), e.note);
                if (it != sounding.end()) sounding.erase(it);
            }
        }
        int step = -1;
        for (std::size_t i = 0; i < chord_ms.size(); ++i)
            if (chord_ms[i] <= now) step = static_cast<int>(i);
        if (step != panel.playing_step_) {
            panel.playing_step_ = step;
            panel.repaint();
        }
        if (now >= end_ms && next >= events.size()) {
            stopTimer();
            release();
            juce::Component::SafePointer<CircleOfFifthsPanel> safe(&panel);
            juce::MessageManager::callAsync([safe] {
                if (safe) safe->stop_progression(false);
            });
        }
    }
    CircleOfFifthsPanel& panel;
    std::vector<Event> events;
    std::vector<double> chord_ms;
    std::vector<int> sounding;
    std::size_t next = 0;
    double end_ms = 0.0, started = 0.0;
};

CircleOfFifthsPanel::CircleOfFifthsPanel(AppContext& context) : context_(context)
{
    setComponentID("circle_of_fifths");
    setWantsKeyboardFocus(true);
    setTitle("Circle of Fifths");
    setDescription("Key reference: arrow keys move around the circle, Up and Down switch major and minor");
    auto& settings = hw::ui_settings();
    follow_ = settings.getBoolValue("circle_follow", true);
    manual_ = theory::Key::parse(settings.getValue("circle_key", "C major").toStdString()).value_or(theory::Key{});
    sevenths_ = settings.getBoolValue("circle_sevenths", false);
    step_length_ = std::clamp(settings.getIntValue("circle_step_length", 1), 0, 2);
    borrowed_ = settings.getBoolValue("circle_borrowed", true);
    borrow_source_ = static_cast<theory::BorrowSource>(
        std::clamp(settings.getIntValue("circle_borrow_from", 0), 0, static_cast<int>(theory::borrow_sources().size()) - 1));
    form_value_ = static_cast<theory::MinorForm>(std::clamp(settings.getIntValue("circle_form", 0), 0, 3));

    follow_key_.set_led_dot(true);
    manual_key_.set_led_dot(true);
    follow_key_.setComponentID("circle_follow");
    manual_key_.setComponentID("circle_manual");
    follow_key_.setTooltip("Show the score's key where the selection (or the playhead) is");
    manual_key_.setTooltip("Show the key clicked on the circle; the score does not change");
    follow_key_.onClick = [this] { set_following(true); };
    manual_key_.onClick = [this] { set_following(false); };
    sevenths_key_.set_led_dot(true);
    sevenths_key_.setTooltip("Diatonic seventh chords instead of triads");
    sevenths_key_.onClick = [this] { set_sevenths(sevenths_key_.getToggleState()); };
    form_.setTitle("Minor form");
    form_.addItem("Natural minor", 1);
    form_.addItem("Harmonic minor", 2);
    form_.addItem("Melodic minor (ascending)", 3);
    form_.addItem("Melodic minor (descending)", 4);
    form_.setComponentID("circle_form");
    form_.setTooltip("Which minor scale to show (the score's key signature is not changed)");
    form_.onChange = [this] { set_minor_form(static_cast<theory::MinorForm>(std::max(0, form_.getSelectedItemIndex()))); };
    apply_.setComponentID("circle_apply");
    apply_.setTooltip("Set the project's key to this one (asks first; one undo step)");
    apply_.onClick = [this] { apply_key_to_score(); };
    send_.setComponentID("circle_send");
    send_.setTooltip("Fill the Chord Mode palette with the selected chord (nothing is inserted)");
    send_.onClick = [this] { send_to_chord_mode(); };
    audition_.setComponentID("circle_audition");
    audition_.setTooltip("Play the progression in the order the chords were picked (or the selected chord); again to stop");
    audition_.onClick = [this] {
        if (player_) stop_progression();
        else if (!progression_.empty()) play_progression();
        else if (const auto* c = selected_chord_ptr()) this->audition(*c);
    };
    put_.setComponentID("circle_put");
    put_.setTooltip("Write the progression on the selected track at the playhead (on the beat), chord symbols too; one undo step");
    put_.onClick = [this] { put_on_track(); };
    clear_.setComponentID("circle_clear");
    clear_.setTooltip("Start a new progression");
    clear_.onClick = [this] { clear_progression(); };
    length_box_.setTitle("Chord length");
    length_box_.addItem("2 beats each", 1);
    length_box_.addItem("1 bar each", 2);
    length_box_.addItem("2 bars each", 3);
    length_box_.setComponentID("circle_step_length");
    length_box_.onChange = [this] { set_step_length(std::max(0, length_box_.getSelectedItemIndex())); };
    output_box_.setTitle("Put as");
    output_box_.addItem("Put as notation", 1);
    output_box_.addItem("Put as MIDI", 2);
    output_box_.setComponentID("circle_put_as");
    output_box_.setTooltip("Notation writes the chords in the score, spelled as named; MIDI writes a region");
    output_box_.onChange = [this] { set_put_as_notation(output_box_.getSelectedId() != 2); };
    voicing_box_.setTitle("Voicing");
    voicing_box_.addItem("Root bass, smooth upper", 1);
    voicing_box_.addItem("Smooth, with inversions", 2);
    voicing_box_.addItem("Root position", 3);
    voicing_box_.setComponentID("circle_voicing");
    voicing_box_.setTooltip("Root bass, smooth upper: the root in the bass, the other voices moving as little as they can "
                            "above it; with inversions the bass may move to an inversion too; root position stacks each "
                            "chord straight up from its root");
    voicing_box_.onChange = [this] { set_voicing(std::max(0, voicing_box_.getSelectedItemIndex())); };
    rhythm_box_.setTitle("Rhythm");
    rhythm_box_.addItem("Block chords", 1);
    rhythm_box_.addItem("Pulse on the beat", 2);
    rhythm_box_.addItem("Arpeggio", 3);
    rhythm_box_.addItem("Strum", 4);
    rhythm_box_.setComponentID("circle_rhythm");
    rhythm_box_.onChange = [this] {
        static const theory::ChordRhythm kRhythms[] = {theory::ChordRhythm::Block, theory::ChordRhythm::BeatPulse,
                                                       theory::ChordRhythm::Arpeggio, theory::ChordRhythm::Strum};
        set_rhythm(kRhythms[std::clamp(rhythm_box_.getSelectedItemIndex(), 0, 3)]);
    };
    save_.setComponentID("circle_save");
    save_.setTooltip("Keep this progression in My progressions (with its key, lengths, voicing and rhythm)");
    save_.onClick = [this] {
        auto* w = new juce::AlertWindow("Save progression", "A name for it:", juce::MessageBoxIconType::NoIcon);
        w->addTextEditor("name", progression_text().upToFirstOccurrenceOf("  -  3", false, false), "");
        w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<CircleOfFifthsPanel> self(this);
        w->enterModalState(true, juce::ModalCallbackFunction::create([self, w](int r) {
            if (self && r == 1) self->save_progression(w->getTextEditorContents("name"));
        }), true);
    };
    saved_box_.setTitle("Saved progressions");
    saved_box_.setComponentID("circle_saved");
    saved_box_.onChange = [this] {
        const auto name = saved_box_.getText();
        if (name.isNotEmpty() && saved_box_.getSelectedId() > 0) load_progression(name);
        saved_box_.setSelectedId(0, juce::dontSendNotification);
    };
    {
        auto& st = hw::ui_settings();
        as_notation_ = st.getBoolValue("circle_put_notation", true);
        voicing_ = std::clamp(st.getIntValue("circle_voicing", 0), 0, 2);
        rhythm_ = theory::chord_rhythm_from_string(st.getValue("circle_rhythm", "block").toStdString()).value_or(theory::ChordRhythm::Block);
    }
    refresh_saved();
    for (auto* c : std::initializer_list<juce::Component*>{&put_, &clear_, &length_box_, &output_box_, &voicing_box_,
                                                          &rhythm_box_, &save_, &saved_box_})
        addAndMakeVisible(*c);
    for (int i = 0; i < 7; ++i) {
        chord_keys_.push_back(std::make_unique<hw::Keycap>("", true));
        auto& k = *chord_keys_.back();
        k.setComponentID("circle_chord_" + juce::String(i));
        k.set_keep_case(true);
        k.onClick = [this, i] { toggle_step(false, i, juce::ModifierKeys::currentModifiers.isShiftDown()); };
        addAndMakeVisible(k);
    }
    borrowed_key_.set_led_dot(true);
    borrowed_key_.setComponentID("circle_borrowed");
    borrowed_key_.setTooltip("Chords borrowed from outside the key (modal mixture): the parallel key's chords and the "
                             "Neapolitan. Never labelled diatonic.");
    borrowed_key_.onClick = [this] { set_borrowed(borrowed_key_.getToggleState()); };
    addAndMakeVisible(borrowed_key_);
    borrow_from_.setTitle("Borrow from");
    borrow_from_.setComponentID("circle_borrow_from");
    borrow_from_.setTooltip("Where the borrowed chords come from: the parallel key (with the Neapolitan), or another "
                            "scale on the same tonic");
    borrow_from_.onChange = [this] {
        const int i = borrow_from_.getSelectedItemIndex();
        if (i >= 0) set_borrow_source(theory::borrow_sources()[static_cast<std::size_t>(i)]);
    };
    addAndMakeVisible(borrow_from_);
    for (int i = 0; i < 8; ++i) {
        borrowed_keys_.push_back(std::make_unique<hw::Keycap>("", true));
        auto& k = *borrowed_keys_.back();
        k.setComponentID("circle_borrowed_" + juce::String(i));
        k.set_keep_case(true);
        k.set_led_colour(juce::Colour(0xffe0a040));
        k.onClick = [this, i] { toggle_step(true, i, juce::ModifierKeys::currentModifiers.isShiftDown()); };
        addChildComponent(k);
    }
    static const char* relations[] = {"Relative", "Parallel", "Dominant", "Subdominant"};
    for (int i = 0; i < 4; ++i) {
        relation_keys_.push_back(std::make_unique<hw::Keycap>("", false));
        auto& k = *relation_keys_.back();
        k.set_sublegend(relations[i]);
        k.onClick = [this, i] {
            const theory::Key* keys[] = {&ref_.relative, &ref_.parallel, &ref_.dominant, &ref_.subdominant};
            set_reference(*keys[i]);
        };
        addAndMakeVisible(k);
    }
    for (auto* c : std::initializer_list<juce::Component*>{&follow_key_, &manual_key_, &sevenths_key_, &form_, &apply_, &send_, &audition_})
        addAndMakeVisible(*c);
    panel_slot() = this;
    rebuild();
    startTimer(300);
}

CircleOfFifthsPanel::~CircleOfFifthsPanel()
{
    stopTimer();
    if (player_) player_->stopTimer();  // no note-offs here: the engine may be gone at shutdown
    if (panel_slot() == this) panel_slot() = nullptr;
}

void CircleOfFifthsPanel::remember() const
{
    auto& settings = hw::ui_settings();
    settings.setValue("circle_follow", follow_);
    settings.setValue("circle_key", juce::String(manual_.name()));
    settings.setValue("circle_sevenths", sevenths_);
    settings.setValue("circle_borrowed", borrowed_);
    settings.setValue("circle_borrow_from", static_cast<int>(borrow_source_));
    settings.setValue("circle_form", static_cast<int>(form_value_));
}

theory::Key CircleOfFifthsPanel::score_key(juce::String& anchor) const
{
    const auto& s = context_.session;
    const auto& d = s.data();
    const auto& sel = s.selection();
    const Ticks at = sel.has_range() ? sel.from : std::max<Ticks>(0, context_.engine.position());
    const project::Track* t = d.find_track(s.selected_track());
    if (!t || !t->active_version()) {
        const auto tracks = s.music_tracks();
        t = tracks.empty() ? nullptr : tracks.front();
    }
    const int bar = d.meter.bar_at(at);
    if (t && t->active_version()) {
        const auto ks = t->active_version()->score.key_signature_at(at);
        anchor = "From the score at bar " + juce::String(bar) + (sel.has_range() ? " (selection)" : " (playhead)");
        // The project names the mode the signature cannot (a minor key's).
        const auto project = theory::Key::parse(d.key);
        const bool minor = ks.is_minor || (project && project->fifths() == ks.fifths && theory::is_minor_family(project->mode));
        return theory::key_for_signature(ks.fifths, minor);
    }
    if (const auto k = theory::Key::parse(d.key)) {
        anchor = "The project's key";
        return *k;
    }
    anchor = "Reference default (no key set)";
    return theory::Key{};
}

void CircleOfFifthsPanel::rebuild()
{
    theory::Key key = manual_;
    if (follow_) key = score_key(anchor_);
    else anchor_ = "Manual reference (the score is unchanged)";
    ref_ = theory::key_reference(key, form_value_);
    // A new key: the progression's numerals belonged to the old one.
    if (progression_key_ != ref_.key.name()) {
        if (!progression_.empty()) {
            stop_progression();
            progression_.clear();
        }
        progression_key_ = ref_.key.name();
    }
    last_key_ = key.name() + (follow_ ? "/f" : "/m") + std::to_string(static_cast<int>(form_value_)) + (sevenths_ ? "7" : "3");
    const auto& chords = sevenths_ ? ref_.sevenths : ref_.triads;
    for (std::size_t i = 0; i < chord_keys_.size(); ++i) {
        auto& k = *chord_keys_[i];
        if (i < chords.size()) {
            const auto& c = chords[i];
            k.setButtonText(juce::String::fromUTF8(c.roman.c_str()));
            k.set_sublegend({});
            k.setTooltip(juce::String::fromUTF8(c.chord.display_name().c_str()) +
                         (c.uses_altered ? " - uses an altered degree of the scale shown, outside the key signature" : ""));
            k.setTitle(juce::String::fromUTF8(c.roman.c_str()) + " " + juce::String::fromUTF8(c.chord.display_name().c_str()));
        }
        k.setToggleState(!places(false, static_cast<int>(i)).empty(), juce::dontSendNotification);
    }
    lent_ = theory::borrowed_chords(ref_, borrow_source_, sevenths_);
    if (selected_borrowed_ >= static_cast<int>(lent_.size())) selected_borrowed_ = -1;
    {
        // "Borrow from": named for this key ("Parallel minor" in a major key).
        const bool minor_home = theory::is_minor_family(ref_.key.mode);
        borrow_from_.clear(juce::dontSendNotification);
        int n = 0;
        for (auto src : theory::borrow_sources())
            borrow_from_.addItem("From " + juce::String(theory::borrow_source_name(src, minor_home)) +
                                     (src == theory::BorrowSource::Parallel ? " + Neapolitan" : ""),
                                 ++n);
        borrow_from_.setSelectedItemIndex(static_cast<int>(borrow_source_), juce::dontSendNotification);
        borrow_from_.setVisible(borrowed_);
    }
    const auto& lent = borrowed();
    for (std::size_t i = 0; i < borrowed_keys_.size(); ++i) {
        auto& k = *borrowed_keys_[i];
        const bool shown = borrowed_ && i < lent.size();
        k.setVisible(shown);
        if (!shown) continue;
        const auto& b = lent[i];
        k.setButtonText(juce::String::fromUTF8(b.roman.c_str()));
        k.setTooltip(chord_label(b.chord) + " (" + chord_notes(b.chord) + "), borrowed from " + juce::String(b.source) +
                     (b.name.empty() ? juce::String() : " - the " + juce::String(b.name)));
        k.setTitle("Borrowed " + juce::String::fromUTF8(b.roman.c_str()) + " " + chord_label(b.chord));
        k.setToggleState(!places(true, static_cast<int>(i)).empty(), juce::dontSendNotification);
    }
    borrowed_key_.setToggleState(borrowed_, juce::dontSendNotification);
    const theory::Key* keys[] = {&ref_.relative, &ref_.parallel, &ref_.dominant, &ref_.subdominant};
    for (std::size_t i = 0; i < relation_keys_.size(); ++i) relation_keys_[i]->setButtonText(key_title(*keys[i]));
    follow_key_.setToggleState(follow_, juce::dontSendNotification);
    manual_key_.setToggleState(!follow_, juce::dontSendNotification);
    sevenths_key_.setToggleState(sevenths_, juce::dontSendNotification);
    form_.setSelectedItemIndex(static_cast<int>(form_value_), juce::dontSendNotification);
    form_.setEnabled(theory::is_minor_family(ref_.key.mode));
    send_.setEnabled(selected_chord_ptr() != nullptr);
    audition_.setEnabled(selected_chord_ptr() != nullptr || !progression_.empty());
    audition_.setButtonText(player_ ? "Stop" : progression_.empty() ? "Audition" : "Audition progression");
    put_.setEnabled(!progression_.empty());
    clear_.setEnabled(!progression_.empty());
    length_box_.setSelectedItemIndex(step_length_, juce::dontSendNotification);
    output_box_.setSelectedId(as_notation_ ? 1 : 2, juce::dontSendNotification);
    voicing_box_.setSelectedItemIndex(voicing_, juce::dontSendNotification);
    rhythm_box_.setSelectedItemIndex(rhythm_ == theory::ChordRhythm::Block       ? 0
                                     : rhythm_ == theory::ChordRhythm::BeatPulse ? 1
                                     : rhythm_ == theory::ChordRhythm::Arpeggio  ? 2
                                                                                 : 3,
                                     juce::dontSendNotification);
    save_.setEnabled(!progression_.empty());
    repaint();
}

void CircleOfFifthsPanel::refresh()
{
    if (!follow_) return;
    juce::String anchor;
    const auto key = score_key(anchor);
    const std::string id = key.name() + "/f" + std::to_string(static_cast<int>(form_value_)) + (sevenths_ ? "7" : "3");
    if (id != last_key_ || anchor != anchor_) {
        if (id != last_key_) selected_ = selected_borrowed_ = -1;
        rebuild();
    }
}

void CircleOfFifthsPanel::set_following(bool on)
{
    follow_ = on;
    if (!on) manual_ = ref_.key;  // manual starts from what was shown
    selected_ = selected_borrowed_ = -1;
    remember();
    rebuild();
}

void CircleOfFifthsPanel::set_reference(const theory::Key& key)
{
    follow_ = false;
    manual_ = key;
    selected_ = selected_borrowed_ = -1;
    remember();
    rebuild();
    context_.status("Reference: " + key_title(key) + " (" + juce::String(theory::signature_text(ref_.fifths)) +
                    "). The score is unchanged.");
}

void CircleOfFifthsPanel::choose_position(int index, bool minor)
{
    const auto& c = theory::circle_of_fifths();
    const auto& p = c[static_cast<std::size_t>(((index % 12) + 12) % 12)];
    set_reference(theory::Key{minor ? p.minor : p.major, minor ? theory::Mode::Minor : theory::Mode::Major});
}

void CircleOfFifthsPanel::set_minor_form(theory::MinorForm form)
{
    form_value_ = form;
    selected_ = selected_borrowed_ = -1;
    remember();
    rebuild();
}

void CircleOfFifthsPanel::set_borrowed(bool on)
{
    borrowed_ = on;
    if (!on) selected_borrowed_ = -1;
    remember();
    rebuild();
    resized();
}

void CircleOfFifthsPanel::set_borrow_source(theory::BorrowSource source)
{
    borrow_source_ = source;
    selected_borrowed_ = -1;
    remember();
    rebuild();
}

void CircleOfFifthsPanel::set_sevenths(bool on)
{
    sevenths_ = on;
    remember();
    rebuild();
}

void CircleOfFifthsPanel::select_chord(int index, bool audition)
{
    const auto& chords = sevenths_ ? ref_.sevenths : ref_.triads;
    if (index < 0 || index >= static_cast<int>(chords.size())) return;
    selected_ = index;
    selected_borrowed_ = -1;
    rebuild();
    if (audition) this->audition(chords[static_cast<std::size_t>(index)].chord);
}

// --- Progression ---------------------------------------------------------------------------

std::vector<int> CircleOfFifthsPanel::voiced(const theory::Chord& chord) const
{
    std::vector<int> out;
    for (const auto& v : theory::voice_for_entry(chord, 4, theory::VoicingStyle::Closed)) out.push_back(v.midi);
    return out;
}

std::vector<int> CircleOfFifthsPanel::places(bool borrowed_chord, int index) const
{
    std::vector<int> out;
    const theory::Chord* c = nullptr;
    const auto& chords = sevenths_ ? ref_.sevenths : ref_.triads;
    if (!borrowed_chord && index >= 0 && index < static_cast<int>(chords.size())) c = &chords[static_cast<std::size_t>(index)].chord;
    if (borrowed_chord && index >= 0 && index < static_cast<int>(lent_.size())) c = &lent_[static_cast<std::size_t>(index)].chord;
    if (!c) return out;
    for (std::size_t i = 0; i < progression_.size(); ++i)
        if (progression_[i].borrowed == borrowed_chord && progression_[i].chord.name() == c->name()) out.push_back(static_cast<int>(i) + 1);
    return out;
}

void CircleOfFifthsPanel::toggle_step(bool borrowed_chord, int index, bool repeat)
{
    const auto& chords = sevenths_ ? ref_.sevenths : ref_.triads;
    Step step;
    step.borrowed = borrowed_chord;
    if (!borrowed_chord) {
        if (index < 0 || index >= static_cast<int>(chords.size())) return;
        step.chord = chords[static_cast<std::size_t>(index)].chord;
        step.roman = chords[static_cast<std::size_t>(index)].roman;
    } else {
        if (index < 0 || index >= static_cast<int>(lent_.size())) return;
        step.chord = lent_[static_cast<std::size_t>(index)].chord;
        step.roman = lent_[static_cast<std::size_t>(index)].roman;
        step.source = lent_[static_cast<std::size_t>(index)].source;
    }
    if (!repeat && !places(borrowed_chord, index).empty()) {
        // Picked again: out of the progression (every time it was in), the rest renumbered.
        stop_progression();
        progression_.erase(std::remove_if(progression_.begin(), progression_.end(),
                                          [&](const Step& s) { return s.borrowed == borrowed_chord && s.chord.name() == step.chord.name(); }),
                           progression_.end());
        if ((borrowed_chord ? selected_borrowed_ : selected_) == index) selected_ = selected_borrowed_ = -1;
        rebuild();
        return;
    }
    progression_.push_back(step);
    selected_ = borrowed_chord ? -1 : index;
    selected_borrowed_ = borrowed_chord ? index : -1;
    rebuild();
    audition(step.chord);
}

void CircleOfFifthsPanel::clear_progression()
{
    stop_progression();
    progression_.clear();
    rebuild();
}

void CircleOfFifthsPanel::set_step_length(int which)
{
    step_length_ = std::clamp(which, 0, 2);
    hw::ui_settings().setValue("circle_step_length", step_length_);
    rebuild();
}

Ticks CircleOfFifthsPanel::step_ticks(Ticks at) const
{
    const auto& d = context_.session.data();
    const auto sig = d.meter.at_tick(at);
    if (step_length_ == 0) return 2 * sig.beat_ticks();
    return sig.ticks_per_bar() * (step_length_ == 2 ? 2 : 1);
}

void CircleOfFifthsPanel::play_progression()
{
    stop_progression();
    if (progression_.empty()) return;
    const auto& d = context_.session.data();
    const Ticks at = std::max<Ticks>(0, context_.engine.position());
    const auto render = render_from(at);
    player_ = std::make_unique<Player>(*this, render, d.tempo);
    rebuild();
}

void CircleOfFifthsPanel::stop_progression(bool send_note_offs)
{
    if (!player_) return;
    player_->stopTimer();
    if (send_note_offs) player_->release();
    player_.reset();
    playing_step_ = -1;
    if (send_note_offs) rebuild();
    else {
        audition_.setButtonText(progression_.empty() ? "Audition" : "Audition progression");
        repaint();
    }
}

juce::String CircleOfFifthsPanel::progression_text() const
{
    juce::StringArray parts;
    for (std::size_t i = 0; i < progression_.size(); ++i)
        parts.add(juce::String(static_cast<int>(i) + 1) + " " + juce::String::fromUTF8(progression_[i].roman.c_str()) + " " +
                  chord_label(progression_[i].chord));
    return parts.joinIntoString("  -  ");
}

theory::ProgressionRender CircleOfFifthsPanel::render_from(Ticks start) const
{
    const auto& d = context_.session.data();
    std::vector<theory::Chord> chords;
    std::vector<Ticks> starts, lengths;
    std::vector<std::string> numerals;
    Ticks at = start;
    for (const auto& s : progression_) {
        const Ticks len = step_ticks(at);
        chords.push_back(s.chord);
        starts.push_back(at);
        lengths.push_back(len);
        numerals.push_back(s.roman);
        at += len;
    }
    theory::ProgressionRenderOptions o;
    o.key = ref_.key;
    o.rhythm = rhythm_;
    o.velocity = 80;
    o.legato = 0.95;
    o.voicing.style = theory::VoicingStyle::Closed;
    o.voicing.voice_lead = voicing_ != 2;
    o.voicing.free_inversions = voicing_ == 1;
    o.voicing.center = 60;
    // Within the selected track's range when it has one.
    if (const auto* t = d.find_track(context_.session.selected_track())) {
        const auto [low, high] = context_.session.range_for(*t);
        o.voicing.low = std::max(low, 24);
        o.voicing.high = std::min(high, 96);
        o.voicing.center = std::clamp(60, o.voicing.low + 7, std::max(o.voicing.low + 7, o.voicing.high - 7));
    }
    if (voicing_ == 0)  // root in the bass, the upper voices led smoothly
        return theory::realize_voiced(chords, theory::voice_root_bass(chords, o.voicing), starts, lengths, numerals, d.meter, o);
    return theory::realize_chords(chords, starts, lengths, numerals, d.meter, o);
}

bool CircleOfFifthsPanel::put_on_track(project::CollisionPolicy policy)
{
    auto& session = context_.session;
    const auto& d = session.data();
    const auto* t = d.find_track(session.selected_track());
    if (progression_.empty()) return false;
    if (!t || (t->kind != project::TrackKind::Instrument && t->kind != project::TrackKind::MidiOnly)) {
        context_.status("Select an instrument track to put the progression on");
        return false;
    }
    // From the playhead, on the beat it is in.
    const Ticks here = std::max<Ticks>(0, context_.engine.position());
    const int bar = d.meter.bar_at(here);
    const Ticks bar_start = d.meter.bar_start(bar);
    const Ticks beat = std::max<Ticks>(1, d.meter.at_bar(bar).beat_ticks());
    const Ticks at = bar_start + (here - bar_start) / beat * beat;
    const std::string track = t->id;
    const juce::String what = "Progression " + progression_text() + " at bar " + juce::String(bar);
    project::InsertResult result;
    if (!as_notation_) {
        result = session.send_progression(track, render_from(at), "circle_of_fifths", policy);
    } else {
        // Notation: a strum is written as the chord (the roll is playing, not writing).
        const auto keep = rhythm_;
        if (rhythm_ == theory::ChordRhythm::Strum) rhythm_ = theory::ChordRhythm::Block;
        const auto render = render_from(at);
        rhythm_ = keep;
        const auto* v = t->active_version();
        if (!v || v->score.staff_count() == 0) {
            context_.status("The track has no staff to write on");
            return false;
        }
        const auto& staff = v->score.staff(0);
        // The notes sounding together are one chord on the page, spelled as
        // their chord is (Eb in a borrowed bVI, never D#), held until the next.
        std::map<Ticks, std::vector<int>> groups;
        for (const auto& n : render.notes) groups[n.start].push_back(n.pitch);
        std::vector<session::Session::WrittenEvent> events;
        for (auto it = groups.begin(); it != groups.end(); ++it) {
            const theory::RealizedChord* rc = nullptr;
            for (const auto& c : render.chords)
                if (it->first >= c.start && it->first < c.start + c.duration) rc = &c;
            if (!rc) continue;
            const auto next = std::next(it);
            const Ticks chord_end = rc->start + rc->duration;
            const Ticks end = next != groups.end() && next->first < chord_end ? next->first : chord_end;
            std::vector<theory::VoicedTone> voiced_tones;
            auto pitches = it->second;
            std::sort(pitches.begin(), pitches.end());
            for (int midi : pitches)
                for (const auto& tone : rc->chord.tones())
                    if (tone.pitch_class() == ((midi % 12) + 12) % 12) {
                        voiced_tones.push_back({tone, midi});
                        break;
                    }
            if (voiced_tones.empty()) continue;
            events.push_back({it->first, end - it->first, written_chord(voiced_tones, staff, t->effective_transpose())});
        }
        std::vector<project::ChordMark> symbols;
        for (const auto& rc : render.chords) symbols.push_back({rc.start, rc.chord.name()});
        result = session.write_events(track, events, symbols, policy, "Write progression on " + t->name);
    }
    if (result.needs_decision) {
        juce::Component::SafePointer<CircleOfFifthsPanel> self(this);
        dialogs::ask_collision(what, [self](project::CollisionPolicy decided) {
            if (self) self->put_on_track(decided);
        });
        return false;
    }
    dialogs::report_send(context_, result, what);
    return result.ok;
}

void CircleOfFifthsPanel::set_put_as_notation(bool on)
{
    as_notation_ = on;
    hw::ui_settings().setValue("circle_put_notation", on);
    rebuild();
}

void CircleOfFifthsPanel::set_voicing(int which)
{
    voicing_ = std::clamp(which, 0, 2);
    hw::ui_settings().setValue("circle_voicing", voicing_);
    rebuild();
}

void CircleOfFifthsPanel::set_rhythm(theory::ChordRhythm rhythm)
{
    rhythm_ = rhythm;
    hw::ui_settings().setValue("circle_rhythm", juce::String(theory::to_string(rhythm)));
    rebuild();
}

namespace {
juce::var saved_list()
{
    auto v = juce::JSON::parse(hw::ui_settings().getValue("circle_saved", "[]"));
    return v.isArray() ? v : juce::var(juce::Array<juce::var>());
}
void store_list(const juce::var& list) { hw::ui_settings().setValue("circle_saved", juce::JSON::toString(list, true)); }
}  // namespace

juce::StringArray CircleOfFifthsPanel::saved_progressions() const
{
    juce::StringArray names;
    const auto list_var = saved_list();
    for (const auto& p : *list_var.getArray()) names.add(p["name"].toString());
    return names;
}

bool CircleOfFifthsPanel::save_progression(const juce::String& name)
{
    const auto clean = name.trim();
    if (clean.isEmpty() || progression_.empty()) return false;
    auto* entry = new juce::DynamicObject();
    entry->setProperty("name", clean);
    entry->setProperty("key", juce::String(ref_.key.name()));
    entry->setProperty("length", step_length_);
    entry->setProperty("voicing", voicing_);
    entry->setProperty("rhythm", juce::String(theory::to_string(rhythm_)));
    juce::Array<juce::var> steps;
    for (const auto& st : progression_) {
        auto* o = new juce::DynamicObject();
        o->setProperty("chord", juce::String(st.chord.name()));
        o->setProperty("roman", juce::String::fromUTF8(st.roman.c_str()));
        o->setProperty("borrowed", st.borrowed);
        o->setProperty("source", juce::String(st.source));
        steps.add(juce::var(o));
    }
    entry->setProperty("steps", steps);
    juce::Array<juce::var> list;
    const auto list_var = saved_list();
    for (const auto& p : *list_var.getArray())
        if (p["name"].toString() != clean) list.add(p);  // saving under a name replaces it
    list.add(juce::var(entry));
    store_list(list);
    refresh_saved();
    context_.status("Saved \"" + clean + "\" to My progressions");
    return true;
}

bool CircleOfFifthsPanel::load_progression(const juce::String& name)
{
    const auto list_var = saved_list();
    for (const auto& p : *list_var.getArray()) {
        if (p["name"].toString() != name) continue;
        const auto key = theory::Key::parse(p["key"].toString().toStdString());
        if (!key) return false;
        std::vector<Step> steps;
        if (const auto* arr = p["steps"].getArray())
            for (const auto& s : *arr) {
                const auto chord = theory::parse_chord_symbol(s["chord"].toString().toStdString());
                if (!chord) continue;
                steps.push_back({*chord, s["roman"].toString().toStdString(), static_cast<bool>(s["borrowed"]),
                                 s["source"].toString().toStdString()});
            }
        if (steps.empty()) return false;
        stop_progression();
        set_reference(*key);  // its numerals are in this key
        progression_ = std::move(steps);
        progression_key_ = ref_.key.name();
        step_length_ = std::clamp(static_cast<int>(p["length"]), 0, 2);
        voicing_ = std::clamp(static_cast<int>(p["voicing"]), 0, 2);
        rhythm_ = theory::chord_rhythm_from_string(p["rhythm"].toString().toStdString()).value_or(theory::ChordRhythm::Block);
        selected_ = selected_borrowed_ = -1;
        rebuild();
        context_.status("Loaded \"" + name + "\" (" + key_title(ref_.key) + ")");
        return true;
    }
    return false;
}

bool CircleOfFifthsPanel::delete_progression(const juce::String& name)
{
    juce::Array<juce::var> list;
    bool found = false;
    const auto list_var = saved_list();
    for (const auto& p : *list_var.getArray()) {
        if (p["name"].toString() == name) found = true;
        else list.add(p);
    }
    if (!found) return false;
    store_list(list);
    refresh_saved();
    return true;
}

void CircleOfFifthsPanel::refresh_saved()
{
    saved_box_.clear(juce::dontSendNotification);
    const auto names = saved_progressions();
    for (int i = 0; i < names.size(); ++i) saved_box_.addItem(names[i], i + 1);
    saved_box_.setTextWhenNothingSelected(names.isEmpty() ? "My progressions (none saved)" : "My progressions...");
    saved_box_.setTextWhenNoChoicesAvailable("My progressions (none saved)");
}

void CircleOfFifthsPanel::select_borrowed(int index, bool audition)
{
    const auto& lent = borrowed();
    if (index < 0 || index >= static_cast<int>(lent.size())) return;
    selected_borrowed_ = index;
    selected_ = -1;
    rebuild();
    if (audition) this->audition(lent[static_cast<std::size_t>(index)].chord);
}

const theory::Chord* CircleOfFifthsPanel::selected_chord_ptr() const
{
    const auto& chords = sevenths_ ? ref_.sevenths : ref_.triads;
    if (selected_ >= 0 && selected_ < static_cast<int>(chords.size())) return &chords[static_cast<std::size_t>(selected_)].chord;
    const auto& lent = borrowed();
    if (selected_borrowed_ >= 0 && selected_borrowed_ < static_cast<int>(lent.size()))
        return &lent[static_cast<std::size_t>(selected_borrowed_)].chord;
    return nullptr;
}

juce::String CircleOfFifthsPanel::selection_text() const
{
    const auto* c = selected_chord_ptr();
    if (!c) return {};
    juce::String roman, from;
    if (selected_ >= 0) {
        const auto& chords = sevenths_ ? ref_.sevenths : ref_.triads;
        roman = juce::String::fromUTF8(chords[static_cast<std::size_t>(selected_)].roman.c_str());
        from = "diatonic in " + key_title(ref_.key);
    } else {
        const auto& b = borrowed()[static_cast<std::size_t>(selected_borrowed_)];
        roman = juce::String::fromUTF8(b.roman.c_str());
        from = "borrowed from " + juce::String(b.source) + (b.name.empty() ? juce::String() : " (" + juce::String(b.name) + ")");
    }
    // The notes the key does not have.
    juce::StringArray outside;
    for (const auto& t : c->tones()) {
        bool in = false;
        for (const auto& d : ref_.scale) in = in || d.note.pitch_class() == t.pitch_class();
        if (!in) outside.add(disp(t));
    }
    juce::String text = roman + " = " + chord_label(*c) + " (" + chord_notes(*c) + "), " + from;
    if (!outside.isEmpty())
        text << "  -  " << outside.joinIntoString(" and ") << (outside.size() == 1 ? " is" : " are") << " outside "
             << key_title(ref_.key);
    return text;
}

void CircleOfFifthsPanel::audition(const theory::Chord& chord)
{
    const auto voiced = theory::voice_for_entry(chord, 4, theory::VoicingStyle::Closed);
    std::vector<int> notes;
    for (const auto& v : voiced) {
        context_.engine.send_live(juce::MidiMessage::noteOn(1, v.midi, static_cast<juce::uint8>(85)));
        notes.push_back(v.midi);
    }
    juce::Component::SafePointer<CircleOfFifthsPanel> self(this);
    juce::Timer::callAfterDelay(800, [self, notes] {
        if (!self) return;
        for (int n : notes) self->context_.engine.send_live(juce::MidiMessage::noteOff(1, n));
    });
}

bool CircleOfFifthsPanel::send_to_chord_mode()
{
    const auto* chord = selected_chord_ptr();
    if (!chord || !context_.send_to_chord_mode) return false;
    context_.send_to_chord_mode(chord->name());
    context_.status("Chord Mode is set to " + chord_label(*chord) + ": click a staff (or Insert) to enter it");
    return true;
}

void CircleOfFifthsPanel::apply_key_to_score()
{
    const auto key = ref_.key;
    auto* w = new juce::AlertWindow("Apply key to score",
                                    "Set the project's key to " + key_title(key) + " (" + juce::String(theory::signature_text(ref_.fifths)) +
                                        ")? Every part's key signature changes; transposing instruments keep their written keys. "
                                        "One undo step.",
                                    juce::MessageBoxIconType::QuestionIcon);
    w->setComponentID("circle_apply_question");
    w->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<CircleOfFifthsPanel> self(this);
    w->enterModalState(true, juce::ModalCallbackFunction::create([self, key](int r) {
        if (!self || r != 1) return;
        if (self->context_.session.set_key(key.name())) self->context_.status("The score is now in " + key_title(key));
    }), true);
}

juce::Rectangle<float> CircleOfFifthsPanel::circle_area() const
{
    auto r = getLocalBounds().withTrimmedTop(44).reduced(10).toFloat();
    const float size = std::min(r.getHeight(), r.getWidth() * 0.5f);
    return r.removeFromLeft(size).withSizeKeepingCentre(size, size);
}

juce::Rectangle<int> CircleOfFifthsPanel::info_area() const
{
    const auto c = circle_area();
    return getLocalBounds().withTrimmedTop(44).withTrimmedLeft(juce::roundToInt(c.getRight()) + 16).reduced(0, 10).withTrimmedRight(12);
}

juce::Rectangle<int> CircleOfFifthsPanel::chords_area() const
{
    return info_area().withTrimmedTop(30 + 18 + 6 + 64 + 6 + 58 + 18);
}

juce::Point<float> CircleOfFifthsPanel::segment_centre(int index, bool minor) const
{
    const auto c = circle_area();
    const float radius = c.getWidth() / 2.0f;
    const float r = minor ? radius * 0.48f : radius * 0.78f;
    const float angle = 2.0f * kPi * static_cast<float>(index) / 12.0f;
    return {c.getCentreX() + r * std::sin(angle), c.getCentreY() - r * std::cos(angle)};
}

std::pair<int, bool> CircleOfFifthsPanel::position_at(juce::Point<float> p) const
{
    const auto c = circle_area();
    const float radius = c.getWidth() / 2.0f;
    const auto d = p - c.getCentre();
    const float dist = std::hypot(d.x, d.y);
    if (dist < radius * 0.28f || dist > radius) return {-1, false};
    float angle = std::atan2(d.x, -d.y);
    if (angle < 0) angle += 2.0f * kPi;
    const int index = static_cast<int>(std::floor(angle / (2.0f * kPi) * 12.0f + 0.5f)) % 12;
    return {index, dist < radius * 0.62f};
}

void CircleOfFifthsPanel::paint(juce::Graphics& g)
{
    const auto& theme = hw::Theme::get();
    g.fillAll(hw::tok::panel_bottom);
    const auto c = circle_area();
    const float radius = c.getWidth() / 2.0f;
    const auto centre = c.getCentre();
    const int active = theory::circle_index(ref_.key);
    const bool minor = theory::is_minor_family(ref_.key.mode);
    const int dominant = (active + 1) % 12, subdominant = (active + 11) % 12;
    auto wedge = [&](int i, float inner, float outer) {
        juce::Path p;
        const float a0 = 2.0f * kPi * (static_cast<float>(i) - 0.5f) / 12.0f, a1 = 2.0f * kPi * (static_cast<float>(i) + 0.5f) / 12.0f;
        p.addPieSegment(c.withSizeKeepingCentre(outer * 2, outer * 2), a0, a1, inner / outer);
        return p;
    };
    const auto& circle = theory::circle_of_fifths();
    for (int i = 0; i < 12; ++i) {
        for (bool inner : {false, true}) {
            const auto path = wedge(i, inner ? radius * 0.3f : radius * 0.62f, inner ? radius * 0.62f : radius);
            const bool on = i == active && inner == minor;
            const bool related = i == active && inner != minor;          // relative key
            const bool near = (i == dominant || i == subdominant) && inner == minor;
            juce::Colour fill = inner ? juce::Colour(0xff23272c) : juce::Colour(0xff2c3036);
            if (near) fill = fill.interpolatedWith(theme.lcd(), 0.18f);
            if (related) fill = fill.interpolatedWith(theme.lcd(), 0.32f);
            if (on) fill = theme.lcd().withAlpha(0.85f);
            g.setColour(fill);
            g.fillPath(path);
            g.setColour(hw::tok::edge);
            g.strokePath(path, juce::PathStrokeType(1.0f));
            const auto& p = circle[static_cast<std::size_t>(i)];
            juce::String label = inner ? disp(p.minor) + "m" : disp(p.major);
            if (!inner && p.major_enharmonic) label << "/" << disp(*p.major_enharmonic);
            if (inner && p.minor_enharmonic) label << "/" << disp(*p.minor_enharmonic) << "m";
            const auto at = segment_centre(i, inner);
            g.setColour(on ? juce::Colour(0xff15181b) : hw::tok::text_bright);
            g.setFont(hw::fonts::ui(inner ? 11.5f : 13.5f, true));
            g.drawText(label, juce::Rectangle<float>(at.x - 40, at.y - (inner ? 8 : 12), 80, 16), juce::Justification::centred, false);
            if (!inner) {
                // The key signature, small, under the name.
                const int f = p.fifths;
                juce::String sig = f == 0 ? juce::String("0") : juce::String(std::abs(f)) + juce::String::fromUTF8(f > 0 ? "\xE2\x99\xAF" : "\xE2\x99\xAD");
                g.setFont(hw::fonts::ui(10.0f, false));
                g.setColour((on ? juce::Colour(0xff15181b) : hw::tok::text).withAlpha(0.8f));
                g.drawText(sig, juce::Rectangle<float>(at.x - 30, at.y + 4, 60, 12), juce::Justification::centred, false);
            }
        }
    }
    g.setColour(hw::tok::panel_bottom);
    g.fillEllipse(c.withSizeKeepingCentre(radius * 0.6f, radius * 0.6f));
    g.setColour(hw::tok::text);
    g.setFont(hw::fonts::silk(9.5f, 0.08f, true));
    g.drawText("MAJOR OUTSIDE", juce::Rectangle<float>(centre.x - 60, centre.y - 14, 120, 12), juce::Justification::centred, false);
    g.drawText("MINOR INSIDE", juce::Rectangle<float>(centre.x - 60, centre.y + 2, 120, 12), juce::Justification::centred, false);

    // --- The key: name, signature on a staff, scale, chords -----------------------------
    auto r = info_area();
    g.setColour(hw::tok::text_bright);
    g.setFont(hw::fonts::ui(22.0f, true));
    g.drawText(key_title(ref_.key), r.removeFromTop(30).toFloat(), juce::Justification::centredLeft, false);
    g.setFont(hw::fonts::ui(12.0f, false));
    g.setColour(hw::tok::text);
    juce::String sig = juce::String(theory::signature_text(ref_.fifths));
    if (!ref_.signature.empty()) {
        sig << ": ";
        for (std::size_t i = 0; i < ref_.signature.size(); ++i) sig << (i ? " " : "") << disp(ref_.signature[i]);
    }
    g.drawText(sig + "   -   " + anchor_, r.removeFromTop(18).toFloat(), juce::Justification::centredLeft, true);

    // The signature on a small treble staff.
    r.removeFromTop(6);
    auto staff = r.removeFromTop(64).withWidth(std::min(r.getWidth(), 220)).toFloat().reduced(0, 16);
    const float space = staff.getHeight() / 4.0f;
    g.setColour(hw::tok::text.withAlpha(0.7f));
    for (int l = 0; l < 5; ++l) g.drawHorizontalLine(juce::roundToInt(staff.getY() + space * static_cast<float>(l)), staff.getX(), staff.getRight());
    const auto& mf = music_font();
    if (mf.loaded) {
        const auto font = mf.font.font_for_staff_space(space);
        g.setFont(font);
        g.setColour(hw::tok::text_bright);
        // Treble clef on the G line; positions in half-spaces above the bottom line.
        // SMuFL glyphs sit on their baseline: the clef's on the G line.
        g.drawSingleLineText(juce::String::fromUTF8("\xee\x81\x90"), juce::roundToInt(staff.getX() + 4),
                             juce::roundToInt(staff.getBottom() - space));
        static const int kSharpPos[7] = {8, 5, 9, 6, 3, 7, 4};  // F5 C5 G5 D5 A4 E5 B4
        static const int kFlatPos[7] = {4, 7, 3, 6, 2, 5, 1};   // B4 E5 A4 D5 G4 C5 F4
        for (std::size_t i = 0; i < ref_.signature.size(); ++i) {
            const int pos = ref_.fifths > 0 ? kSharpPos[i] : kFlatPos[i];
            const float y = staff.getBottom() - static_cast<float>(pos) * space / 2.0f;
            const float x = staff.getX() + 34 + static_cast<float>(i) * space * 1.1f;
            g.drawSingleLineText(juce::String::fromUTF8(ref_.fifths > 0 ? "\xee\x89\xa2" : "\xee\x89\xa0"),
                                 juce::roundToInt(x), juce::roundToInt(y));
        }
    }

    // The scale: degree, note, function; altered degrees marked.
    r.removeFromTop(6);
    auto scale = r.removeFromTop(58);
    std::vector<int> members;
    const auto& chords = sevenths_ ? ref_.sevenths : ref_.triads;
    if (const auto* sel = selected_chord_ptr())
        for (const auto& t : sel->tones()) members.push_back(t.pitch_class());
    const int cw = scale.getWidth() / 7;
    for (const auto& d : ref_.scale) {
        auto cell = scale.removeFromLeft(cw).reduced(2).toFloat();
        const bool lit = std::find(members.begin(), members.end(), d.note.pitch_class()) != members.end();
        g.setColour(lit ? theme.lcd().withAlpha(0.35f) : juce::Colour(0xff23272c));
        g.fillRoundedRectangle(cell, 4.0f);
        g.setColour(d.altered ? juce::Colour(0xffe0a040) : hw::tok::edge);
        g.drawRoundedRectangle(cell, 4.0f, d.altered ? 1.6f : 1.0f);
        g.setColour(hw::tok::text);
        g.setFont(hw::fonts::ui(10.0f, false));
        juce::String top = juce::String(d.degree);
        if (d.degree - 1 < static_cast<int>(chords.size()))
            top << "  " << juce::String::fromUTF8(chords[static_cast<std::size_t>(d.degree - 1)].roman.c_str());
        g.drawText(top, cell.withHeight(14).translated(0, 2), juce::Justification::centred, false);
        g.setColour(hw::tok::text_bright);
        g.setFont(hw::fonts::ui(16.0f, true));
        g.drawText(disp(d.note) + (d.altered ? "*" : ""), cell.withTrimmedTop(14).withHeight(20), juce::Justification::centred, false);
        g.setColour(hw::tok::text);
        g.setFont(hw::fonts::ui(9.0f, false));
        g.drawText(d.function, cell.withTrimmedTop(36), juce::Justification::centred, true);
    }
    if (!ref_.altered_note.empty()) {
        g.setColour(juce::Colour(0xffe0a040));
        g.setFont(hw::fonts::ui(11.0f, false));
        g.drawText("* " + juce::String(ref_.altered_note) + ": outside the key signature", r.removeFromTop(16).toFloat(),
                   juce::Justification::centredLeft, true);
    }

    // Under each chord key: its name and its notes; the borrowed ones also say where from.
    auto under = [&](hw::Keycap& k, const theory::Chord& chord, const juce::String& extra, bool lent) {
        const auto b = k.getBounds().toFloat();
        auto area = juce::Rectangle<float>(b.getX(), b.getBottom() + 2, b.getWidth(), 46);
        g.setColour(lent ? juce::Colour(0xffe0a040) : hw::tok::text_bright);
        g.setFont(hw::fonts::ui(12.5f, true));
        g.drawText(chord_label(chord), area.removeFromTop(16), juce::Justification::centred, true);
        g.setColour(hw::tok::text);
        g.setFont(hw::fonts::ui(10.5f, false));
        g.drawText(chord_notes(chord), area.removeFromTop(14), juce::Justification::centred, true);
        if (extra.isNotEmpty()) {
            g.setFont(hw::fonts::ui(9.0f, false));
            g.setColour(hw::tok::text.withAlpha(0.75f));
            g.drawText(extra, area.removeFromTop(13), juce::Justification::centred, true);
        }
    };
    for (std::size_t i = 0; i < chord_keys_.size() && i < chords.size(); ++i)
        under(*chord_keys_[i], chords[i].chord, chords[i].uses_altered ? juce::String("altered degree") : juce::String(), false);
    g.setColour(hw::tok::text);
    g.setFont(hw::fonts::silk(9.5f, 0.08f, true));
    const auto area = chords_area();
    g.drawText("DIATONIC CHORDS", area.withHeight(14).toFloat(), juce::Justification::centredLeft, false);
    if (borrowed_ && borrowed().empty()) {
        g.setColour(hw::tok::text);
        g.setFont(hw::fonts::ui(12.0f, false));
        const auto b = borrowed_key_.getBounds();
        g.drawText(juce::String(theory::borrow_source_name(borrow_source_, theory::is_minor_family(ref_.key.mode))) +
                       " adds nothing beyond the chords of " + key_title(ref_.key),
                   juce::Rectangle<float>(static_cast<float>(b.getX()), static_cast<float>(b.getBottom() + 12),
                                          static_cast<float>(info_area().getWidth()), 18.0f),
                   juce::Justification::centredLeft, true);
    }
    if (borrowed_) {
        const auto& lent = borrowed();
        for (std::size_t i = 0; i < borrowed_keys_.size() && i < lent.size(); ++i) {
            juce::String from = lent[i].name.empty() ? juce::String(lent[i].source) : juce::String(lent[i].name);
            under(*borrowed_keys_[i], lent[i].chord, from, true);
        }
    }
    // What the selected chord is, and the notes it brings in from outside.
    if (const auto text = selection_text(); text.isNotEmpty()) {
        g.setColour(selected_borrowed_ >= 0 ? juce::Colour(0xffe0a040) : hw::Theme::get().lcd());
        g.setFont(hw::fonts::ui(12.0f, true));
        const auto at = send_.getBounds().translated(0, -24).withWidth(info_area().getWidth()).withHeight(18).toFloat();
        g.drawText(text, at, juce::Justification::centredLeft, true);
    }
}

void CircleOfFifthsPanel::resized()
{
    auto top = getLocalBounds().removeFromTop(40).reduced(10, 6);
    follow_key_.setBounds(top.removeFromLeft(136));
    top.removeFromLeft(4);
    manual_key_.setBounds(top.removeFromLeft(82));
    top.removeFromLeft(10);
    form_.setBounds(top.removeFromLeft(196).reduced(0, 2));
    top.removeFromLeft(6);
    sevenths_key_.setBounds(top.removeFromLeft(56));
    apply_.setBounds(top.removeFromRight(150));
    // Under the scale: the chords (name and notes under each), the
    // borrowed chords, the related keys, and the actions.
    auto r = chords_area();
    r.removeFromTop(16);
    auto row = r.removeFromTop(34);
    const int w = row.getWidth() / 7;
    for (auto& k : chord_keys_) k->setBounds(row.removeFromLeft(w).reduced(2, 0));
    r.removeFromTop(48);
    row = r.removeFromTop(26);
    borrowed_key_.setBounds(row.removeFromLeft(170));
    row.removeFromLeft(8);
    borrow_from_.setBounds(row.removeFromLeft(260));
    if (borrowed_) {
        r.removeFromTop(4);
        row = r.removeFromTop(34);
        const int w8 = row.getWidth() / 8;
        for (auto& k : borrowed_keys_) k->setBounds(row.removeFromLeft(w8).reduced(2, 0));
        r.removeFromTop(48);
    }
    r.removeFromTop(6);
    row = r.removeFromTop(36);
    const int w4 = row.getWidth() / 4;
    for (auto& k : relation_keys_) k->setBounds(row.removeFromLeft(w4).reduced(2, 0));
    r.removeFromTop(30);
    row = r.removeFromTop(30);
    send_.setBounds(row.removeFromLeft(160));
    row.removeFromLeft(6);
    audition_.setBounds(row.removeFromLeft(180));
    row.removeFromLeft(6);
    put_.setBounds(row.removeFromLeft(130));
    // The progression's own row: its length and Clear.
    r.removeFromTop(6);
    row = r.removeFromTop(28);
    length_box_.setBounds(row.removeFromLeft(130).reduced(0, 2));
    row.removeFromLeft(4);
    rhythm_box_.setBounds(row.removeFromLeft(150).reduced(0, 2));
    row.removeFromLeft(4);
    voicing_box_.setBounds(row.removeFromLeft(170).reduced(0, 2));
    r.removeFromTop(6);
    row = r.removeFromTop(28);
    output_box_.setBounds(row.removeFromLeft(150).reduced(0, 2));
    row.removeFromLeft(4);
    clear_.setBounds(row.removeFromLeft(70));
    row.removeFromLeft(4);
    save_.setBounds(row.removeFromLeft(80));
    row.removeFromLeft(4);
    saved_box_.setBounds(row.removeFromLeft(180).reduced(0, 2));
}

void CircleOfFifthsPanel::paintOverChildren(juce::Graphics& g)
{
    // Each picked chord's place in the progression, on its key; the one
    // sounding now ringed.
    auto badge = [&](hw::Keycap& k, const std::vector<int>& at, bool lent) {
        if (at.empty() || !k.isVisible()) return;
        juce::StringArray nums;
        for (int n : at) nums.add(juce::String(n));
        const juce::String text = nums.joinIntoString(",");
        g.setFont(hw::fonts::ui(11.0f, true));
        const float w = std::max(16.0f, juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), text) + 8.0f);
        const auto b = k.getBounds().toFloat();
        const juce::Rectangle<float> r(b.getRight() - w + 3.0f, b.getY() - 6.0f, w, 16.0f);
        g.setColour(lent ? juce::Colour(0xffe0a040) : hw::Theme::get().lcd());
        g.fillRoundedRectangle(r, 8.0f);
        g.setColour(juce::Colour(0xff15181b));
        g.drawText(text, r, juce::Justification::centred, false);
        if (playing_step_ >= 0 && std::find(at.begin(), at.end(), playing_step_ + 1) != at.end()) {
            g.setColour(juce::Colours::white);
            g.drawRoundedRectangle(b.expanded(2.0f), 6.0f, 2.0f);
        }
    };
    for (std::size_t i = 0; i < chord_keys_.size(); ++i) badge(*chord_keys_[i], places(false, static_cast<int>(i)), false);
    for (std::size_t i = 0; i < borrowed_keys_.size(); ++i) badge(*borrowed_keys_[i], places(true, static_cast<int>(i)), true);
    // The progression, in order, under the keys.
    if (!progression_.empty()) {
        const auto row = clear_.getBounds();
        g.setColour(hw::tok::text_bright);
        g.setFont(hw::fonts::ui(12.0f, true));
        g.drawText("Progression: " + progression_text(),
                   juce::Rectangle<float>(static_cast<float>(send_.getX()), static_cast<float>(row.getBottom() + 8),
                                          static_cast<float>(info_area().getWidth()), 18.0f),
                   juce::Justification::centredLeft, true);
    }
}

void CircleOfFifthsPanel::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    const auto [index, minor] = position_at(e.position);
    if (index >= 0) choose_position(index, minor);
}

bool CircleOfFifthsPanel::keyPressed(const juce::KeyPress& key)
{
    const int at = theory::circle_index(ref_.key);
    const bool minor = theory::is_minor_family(ref_.key.mode);
    if (key.getKeyCode() == juce::KeyPress::rightKey) choose_position(at + 1, minor);
    else if (key.getKeyCode() == juce::KeyPress::leftKey) choose_position(at + 11, minor);
    else if (key.getKeyCode() == juce::KeyPress::upKey || key.getKeyCode() == juce::KeyPress::downKey) choose_position(at, !minor);
    else if (key.getKeyCode() >= '1' && key.getKeyCode() <= '7') toggle_step(false, key.getKeyCode() - '1', key.getModifiers().isShiftDown());
    else return false;
    return true;
}

void show_circle_of_fifths(AppContext& context)
{
    dialogs::show_tool(window_slot(), "Circle of Fifths", 1060, 760, [&context] { return new CircleOfFifthsPanel(context); },
                       [] {
                           // Closing mid-progression: let go of what is sounding.
                           if (auto* p = panel_slot()) p->stop_progression();
                       });
}

CircleOfFifthsPanel* circle_of_fifths_panel() { return panel_slot(); }

}  // namespace vcc::desktop::composer_ui
