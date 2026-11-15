#include "ui/composer/CircleOfFifths.h"

#include <cmath>

#include "AppResources.h"
#include "ui/Dialogs.h"
#include "ui/hw/HwTheme.h"
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
    borrowed_ = settings.getBoolValue("circle_borrowed", true);
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
    audition_.setTooltip("Hear the selected chord, or the scale");
    audition_.onClick = [this] {
        if (const auto* c = selected_chord_ptr()) this->audition(*c);
    };
    for (int i = 0; i < 7; ++i) {
        chord_keys_.push_back(std::make_unique<hw::Keycap>("", true));
        auto& k = *chord_keys_.back();
        k.setComponentID("circle_chord_" + juce::String(i));
        k.set_keep_case(true);
        k.onClick = [this, i] { select_chord(i, true); };
        addAndMakeVisible(k);
    }
    borrowed_key_.set_led_dot(true);
    borrowed_key_.setComponentID("circle_borrowed");
    borrowed_key_.setTooltip("Chords borrowed from outside the key (modal mixture): the parallel key's chords and the "
                             "Neapolitan. Never labelled diatonic.");
    borrowed_key_.onClick = [this] { set_borrowed(borrowed_key_.getToggleState()); };
    addAndMakeVisible(borrowed_key_);
    for (int i = 0; i < 8; ++i) {
        borrowed_keys_.push_back(std::make_unique<hw::Keycap>("", true));
        auto& k = *borrowed_keys_.back();
        k.setComponentID("circle_borrowed_" + juce::String(i));
        k.set_keep_case(true);
        k.set_led_colour(juce::Colour(0xffe0a040));
        k.onClick = [this, i] { select_borrowed(i, true); };
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
    if (panel_slot() == this) panel_slot() = nullptr;
}

void CircleOfFifthsPanel::remember() const
{
    auto& settings = hw::ui_settings();
    settings.setValue("circle_follow", follow_);
    settings.setValue("circle_key", juce::String(manual_.name()));
    settings.setValue("circle_sevenths", sevenths_);
    settings.setValue("circle_borrowed", borrowed_);
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
        k.setToggleState(static_cast<int>(i) == selected_, juce::dontSendNotification);
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
        k.setToggleState(static_cast<int>(i) == selected_borrowed_, juce::dontSendNotification);
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
    audition_.setEnabled(selected_chord_ptr() != nullptr);
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
    send_.setBounds(row.removeFromLeft(170));
    row.removeFromLeft(6);
    audition_.setBounds(row.removeFromLeft(100));
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
    else if (key.getKeyCode() >= '1' && key.getKeyCode() <= '7') select_chord(key.getKeyCode() - '1', true);
    else return false;
    return true;
}

void show_circle_of_fifths(AppContext& context)
{
    dialogs::show_tool(window_slot(), "Circle of Fifths", 1040, 700, [&context] { return new CircleOfFifthsPanel(context); });
}

CircleOfFifthsPanel* circle_of_fifths_panel() { return panel_slot(); }

}  // namespace vcc::desktop::composer_ui
