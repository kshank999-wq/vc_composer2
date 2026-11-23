#include "ui/composer/StaffTools.h"

#include "AppResources.h"
#include "ui/hw/HwTheme.h"

namespace vcc::desktop::composer_ui {

using C = composer::Command;

namespace {
constexpr int kKey = 28;       // touch-sized keys
constexpr int kGap = 3;
constexpr int kGroupGap = 10;
constexpr int kHeader = 16;
}  // namespace

juce::Path IconKey::path(Icon icon, juce::Rectangle<float> a)
{
    juce::Path p;
    if (icon == Icon::Arrow) {
        // The pointer: a tall arrowhead with its tail, leaning left.
        p.startNewSubPath(0.0f, 0.0f);
        p.lineTo(0.0f, 15.0f);
        p.lineTo(4.0f, 11.4f);
        p.lineTo(6.6f, 17.4f);
        p.lineTo(9.2f, 16.2f);
        p.lineTo(6.6f, 10.4f);
        p.lineTo(11.6f, 10.4f);
        p.closeSubPath();
    } else {
        // An old quill: a curved feather, its vane notched where the barbs
        // part, and the cut nib at the bottom left.
        p.startNewSubPath(1.0f, 19.0f);                      // the nib's point
        p.quadraticTo(3.0f, 15.0f, 5.0f, 13.0f);             // the bare shaft
        p.cubicTo(5.5f, 8.0f, 11.0f, 2.0f, 19.0f, 0.0f);     // the vane's outer edge, up to the tip
        p.cubicTo(17.5f, 3.0f, 15.0f, 5.0f, 13.2f, 6.4f);    // down the inner edge
        p.lineTo(14.6f, 7.4f);                               // a notch where the barbs part
        p.cubicTo(12.5f, 10.0f, 9.5f, 12.4f, 6.4f, 14.0f);
        p.quadraticTo(4.0f, 16.0f, 2.4f, 19.4f);             // back down the shaft
        p.closeSubPath();
    }
    p.applyTransform(p.getTransformToScaleToFit(a, true));
    return p;
}

const juce::MouseCursor& IconKey::quill_cursor()
{
    static const juce::MouseCursor cursor = [] {
        // Drawn at twice the size for sharp screens; the nib (bottom left) writes.
        constexpr int size = 64;
        juce::Image img(juce::Image::ARGB, size, size, true);
        juce::Graphics g(img);
        const auto area = juce::Rectangle<float>(2.0f, 2.0f, size - 4.0f, size - 4.0f);
        const auto p = path(Icon::Quill, area);
        g.setColour(juce::Colours::black.withAlpha(0.85f));
        g.strokePath(p, juce::PathStrokeType(3.0f));
        g.setColour(juce::Colour(0xfff4efe2));
        g.fillPath(p);
        const auto b = p.getBounds();
        juce::Path rib;
        rib.startNewSubPath(b.getX() + b.getWidth() * 0.12f, b.getBottom() - b.getHeight() * 0.06f);
        rib.quadraticTo(b.getX() + b.getWidth() * 0.45f, b.getY() + b.getHeight() * 0.45f, b.getRight() - b.getWidth() * 0.05f,
                        b.getY() + b.getHeight() * 0.03f);
        g.setColour(juce::Colour(0xff5a4a32));
        g.strokePath(rib, juce::PathStrokeType(1.6f));
        // The nib's point: where a note goes.
        const auto nib = p.getBounds().getBottomLeft();
        return juce::MouseCursor(juce::ScaledImage(img, 2.0), juce::Point<int>(juce::roundToInt(nib.x / 2.0f), juce::roundToInt(nib.y / 2.0f - 1.0f)));
    }();
    return cursor;
}

void IconKey::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
    hw::Keycap::paintButton(g, highlighted, down);
    const auto area = getLocalBounds().toFloat().reduced(getWidth() * 0.17f, getHeight() * 0.15f);
    const auto colour = getToggleState() ? hw::Theme::get().led() : juce::Colour(0xffd9d4c8);
    g.setColour(colour);
    const auto p = path(icon_, area);
    g.fillPath(p);
    if (icon_ == Icon::Quill) {
        // The rachis: the shaft running up the middle of the vane.
        const auto b = p.getBounds();
        juce::Path rib;
        rib.startNewSubPath(b.getX() + b.getWidth() * 0.12f, b.getBottom() - b.getHeight() * 0.06f);
        rib.quadraticTo(b.getX() + b.getWidth() * 0.45f, b.getY() + b.getHeight() * 0.45f, b.getRight() - b.getWidth() * 0.05f,
                        b.getY() + b.getHeight() * 0.03f);
        g.setColour(hw::tok::panel_bottom.withAlpha(0.85f));
        g.strokePath(rib, juce::PathStrokeType(0.9f));
    }
}

StaffTools::StaffTools(bool detached) : detached_(detached)
{
    setComponentID(detached ? "staff_tools_window" : "staff_tools");
    setTitle("Staff tools");
    // Select or Write: the arrow edits what is there, the quill writes notes.
    select_.setComponentID("staff_tools_select");
    select_.setTooltip("Select: click notes to select them, drag to move them; clicks never write");
    select_.setTitle("Select (edit notes)");
    write_.setComponentID("staff_tools_write");
    write_.setTooltip("Write: click the staff to write notes (the quill)");
    write_.setTitle("Write (enter notes)");
    select_.onClick = [this] {
        if (on_mode) on_mode(false);
    };
    write_.onClick = [this] {
        if (on_mode) on_mode(true);
    };
    for (auto* k : {&select_, &write_}) {
        k->setWantsKeyboardFocus(false);
        k->setClickingTogglesState(false);
        addAndMakeVisible(*k);
    }
    write_.setToggleState(true, juce::dontSendNotification);
    // Accent strength: how hard the track's accents play.
    accent_.setComponentID("staff_tools_accent");
    accent_.setTitle("Accent strength");
    accent_.setTooltip("Accent strength, 1-100: how hard this track's accents play (50 as written; double-click for 50)");
    accent_.setRange(1.0, 100.0, 1.0);
    accent_.setValue(50.0, juce::dontSendNotification);
    accent_.setDoubleClickReturnValue(true, 50.0);
    accent_.setWantsKeyboardFocus(false);
    accent_.onValueChange = [this] {
        accent_label_.setText("Accent\n" + juce::String(juce::roundToInt(accent_.getValue())), juce::dontSendNotification);
        if (on_accent) on_accent(juce::roundToInt(accent_.getValue()));
    };
    addAndMakeVisible(accent_);
    // The quiet level of a new crescendo / diminuendo, under its key: drag
    // or double-click and type (expression 0-100; 20 to start, so the first note is heard).
    for (auto* lv : {&cresc_level_, &dim_level_}) {
        lv->setSliderStyle(juce::Slider::LinearBar);
        lv->setRange(0.0, 100.0, 1.0);
        lv->setValue(20.0, juce::dontSendNotification);
        lv->setTextBoxIsEditable(true);
        lv->setWantsKeyboardFocus(false);
        lv->setColour(juce::Slider::trackColourId, hw::Theme::get().led().withAlpha(0.35f));
        lv->setColour(juce::Slider::textBoxTextColourId, juce::Colour(0xffd9d4c8));
        lv->setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        addAndMakeVisible(*lv);
    }
    cresc_level_.setComponentID("staff_tools_cresc_level");
    dim_level_.setComponentID("staff_tools_dim_level");
    cresc_level_.setTitle("Crescendo start level");
    dim_level_.setTitle("Diminuendo end level");
    cresc_level_.setTooltip("Where a crescendo starts: its quiet level (expression 0-100)");
    dim_level_.setTooltip("Where a diminuendo ends: its quiet level (expression 0-100)");
    cresc_level_.onValueChange = [this] {
        if (on_hairpin_level) on_hairpin_level(1, juce::roundToInt(cresc_level_.getValue()));
    };
    dim_level_.onValueChange = [this] {
        if (on_hairpin_level) on_hairpin_level(2, juce::roundToInt(dim_level_.getValue()));
    };
    accent_label_.setText("Accent\n50", juce::dontSendNotification);
    accent_label_.setFont(juce::FontOptions(9.5f, juce::Font::bold));
    accent_label_.setColour(juce::Label::textColourId, juce::Colour(0xffd9d4c8));
    accent_label_.setJustificationType(juce::Justification::centredLeft);
    accent_label_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(accent_label_);
    using W = Key::When;
    // Note values, dot, triplet, rest: always.
    add(C::SelectDurationWhole, "", "\xee\xb2\xa2", "Whole note", W::Always, true);
    add(C::SelectDurationHalf, "", "\xee\xb2\xa3", "Half note", W::Always);
    add(C::SelectDurationQuarter, "", "\xee\xb2\xa5", "Quarter note", W::Always);
    add(C::SelectDurationEighth, "", "\xee\xb2\xa7", "Eighth note", W::Always);
    add(C::SelectDurationSixteenth, "", "\xee\xb2\xa9", "Sixteenth note", W::Always);
    add(C::SelectDurationThirtySecond, "", "\xee\xb2\xab", "Thirty-second note", W::Always);
    add(C::ToggleDot, "", "\xee\xb2\xb7", "Dot", W::Always);
    add(C::ToggleTuplet, "3", nullptr, "Triplet", W::Always);
    add(C::ToggleRest, "", "\xee\x93\xa5", "Rest", W::Always);
    // Accidentals on a pitched staff.
    add(C::AccidentalFlat, "", "\xee\x89\xa0", "Flat", W::Pitched, true);
    add(C::AccidentalNatural, "", "\xee\x89\xa1", "Natural", W::Pitched);
    add(C::AccidentalSharp, "", "\xee\x89\xa2", "Sharp", W::Pitched);
    // Ties and slurs.
    add(C::ToggleTie, "Tie", nullptr, "Tie the note to the next", W::Always, true);
    add(C::StartSlur, "Slur", nullptr, "Slur the selected notes", W::Selection);
    // Articulations and dynamics: on what is selected.
    add(C::ArticulationAccent, "", "\xee\x92\xa0", "Accent", W::Selection, true);
    add(C::ArticulationStaccato, "", "\xee\x92\xa2", "Staccato", W::Selection);
    add(C::ArticulationTenuto, "", "\xee\x92\xa4", "Tenuto", W::Selection);
    add(C::ArticulationMarcato, "", "\xee\x92\xac", "Marcato", W::Selection);
    add(C::DynamicPiano, "p", nullptr, "Piano", W::Selection, true);
    add(C::DynamicMezzoPiano, "mp", nullptr, "Mezzo piano", W::Selection);
    add(C::DynamicMezzoForte, "mf", nullptr, "Mezzo forte", W::Selection);
    add(C::DynamicForte, "f", nullptr, "Forte", W::Selection);
    // Hairpins: across the selected notes, or drawn - press on the staff
    // where it starts and drag to where it ends.
    add(C::StartCrescendo, "<", nullptr, "Crescendo: drag it out on the staff (or across the selected notes)", W::Always, true);
    add(C::StartDiminuendo, ">", nullptr, "Diminuendo: drag it out on the staff (or across the selected notes)", W::Always);
    // Voices.
    for (int v = 0; v < 4; ++v)
        add(static_cast<C>(static_cast<int>(C::SelectVoice1) + v), juce::String(v + 1), nullptr, "Voice " + juce::String(v + 1),
            W::Always, v == 0);
    // Clef, text; erase and select.
    add(C::EditClef, "Clef", nullptr, "Clef of this staff", W::Entry, true);
    add(C::AddText, "Text", nullptr, "Expression text at the cursor", W::Entry);
    add(C::DeleteSelection, "Del", nullptr, "Erase the selected notes", W::Selection, true);
    add(C::SelectAll, "All", nullptr, "Select everything on this staff", W::Always);
    for (auto& k : keys_) {
        auto& cap = *k.cap;
        cap.setWantsKeyboardFocus(false);
        if (k.command >= C::DynamicPianississimo && k.command <= C::DynamicFortississimo) cap.set_dynamic_legend(true);
        if (k.command >= C::SelectVoice1 && k.command <= C::SelectVoice4) cap.set_led_dot(true);
        const auto command = k.command;
        cap.onClick = [this, command] {
            if (on_command) on_command(command);
        };
        addAndMakeVisible(cap);
    }
    chord_.set_led_dot(true);
    chord_.setComponentID("staff_tools_chord");
    chord_.setTooltip("Chord Mode: enter whole chords with a click");
    chord_.onClick = [this] {
        if (on_chord_mode) on_chord_mode();
    };
    circle_.setComponentID("staff_tools_circle");
    circle_.setTooltip("Circle of Fifths and key reference");
    circle_.onClick = [this] {
        if (on_circle) on_circle();
    };
    detach_.setButtonText(detached ? "Dock" : "Detach");
    detach_.setComponentID("staff_tools_detach");
    detach_.setTooltip(detached ? "Put the tools back beside the staff" : "Float the tools in their own window, anywhere");
    detach_.onClick = [this] {
        if (on_detach) on_detach();
    };
    collapse_.setComponentID("staff_tools_collapse");
    collapse_.setTooltip("Fold the tools away (Cmd/Ctrl+Alt+T brings them back)");
    collapse_.onClick = [this] {
        if (on_collapse) on_collapse();
    };
    for (auto* k : {&chord_, &circle_, &detach_, &collapse_}) {
        k->setWantsKeyboardFocus(false);
        k->set_legend_size(9.5f);
        addAndMakeVisible(*k);
    }
    collapse_.setVisible(!detached);
}

hw::Keycap& StaffTools::add(C c, const juce::String& legend, const char* glyph, const juce::String& tip, Key::When when,
                            bool group_start)
{
    Key k;
    k.cap = std::make_unique<hw::Keycap>(legend, false);
    k.command = c;
    k.when = when;
    k.group_start = group_start;
    k.cap->setTooltip(tip);
    k.cap->setTitle(tip);
    k.cap->setComponentID("staff_tool_" + juce::String(static_cast<int>(c)));
    k.cap->set_legend_size(10.5f);
    const auto& mf = music_font();
    if (glyph && mf.loaded) k.cap->set_music_symbol(juce::String::fromUTF8(glyph), mf.font.font_for_staff_space(4.2));
    keys_.push_back(std::move(k));
    return *keys_.back().cap;
}

hw::Keycap* StaffTools::key_for(C command)
{
    for (auto& k : keys_)
        if (k.command == command) return k.cap->isVisible() ? k.cap.get() : nullptr;
    return nullptr;
}

int StaffTools::visible_keys() const
{
    int n = 0;
    for (const auto& k : keys_) n += k.cap->isVisible() ? 1 : 0;
    return n;
}

void StaffTools::set_state(const StaffToolsState& s)
{
    state_ = s;
    for (auto& k : keys_) {
        bool show = true;
        switch (k.when) {
            case Key::When::Always: show = true; break;
            case Key::When::Entry: show = !s.has_selection; break;
            case Key::When::Selection: show = s.has_selection; break;
            case Key::When::Pitched: show = s.pitched; break;
        }
        k.cap->setVisible(show);
        k.cap->setEnabled(s.editable);
        bool lit = false;
        const int c = static_cast<int>(k.command);
        if (c >= static_cast<int>(C::SelectDurationWhole) && c <= static_cast<int>(C::SelectDurationThirtySecond))
            lit = s.value == c - static_cast<int>(C::SelectDurationWhole);
        else if (k.command == C::ToggleDot) lit = s.dot;
        else if (k.command == C::ToggleTuplet) lit = s.triplet;
        else if (k.command == C::ToggleRest) lit = s.rest;
        else if (k.command == C::StartCrescendo) lit = s.hairpin == 1;
        else if (k.command == C::StartDiminuendo) lit = s.hairpin == 2;
        else if (c >= static_cast<int>(C::SelectVoice1) && c <= static_cast<int>(C::SelectVoice4))
            lit = s.voice == c - static_cast<int>(C::SelectVoice1);
        k.cap->setToggleState(lit, juce::dontSendNotification);
    }
    chord_.setToggleState(s.chord_mode, juce::dontSendNotification);
    chord_.setEnabled(s.editable && s.pitched);
    select_.setToggleState(!s.writing, juce::dontSendNotification);
    write_.setToggleState(s.writing, juce::dontSendNotification);
    select_.setEnabled(s.editable);
    write_.setEnabled(s.editable);
    accent_.setEnabled(s.editable);
    if (juce::roundToInt(accent_.getValue()) != s.accent) {
        accent_.setValue(s.accent, juce::dontSendNotification);
        accent_label_.setText("Accent\n" + juce::String(s.accent), juce::dontSendNotification);
    cresc_level_.setValue(s.cresc_level, juce::dontSendNotification);
    dim_level_.setValue(s.dim_level, juce::dontSendNotification);
    }
    resized();
    repaint();
}

void StaffTools::layout(int width, bool apply, int* height) const
{
    int x = 6, y = kHeader + 2, row = 0;
    auto place = [&](juce::Component& c, int w, bool gap_before) {
        if (gap_before && x > 6) x += kGroupGap - kGap;
        if (x + w > width - 6 && x > 6) {
            x = 6;
            y += kKey + kGap;
            ++row;
        }
        if (apply) c.setBounds(x, y, w, kKey);
        x += w + kGap;
    };
    place(const_cast<IconKey&>(select_), kKey, false);
    place(const_cast<IconKey&>(write_), kKey, false);
    for (const auto& k : keys_) {
        if (!k.cap->isVisible()) continue;
        const bool wide = k.cap->getButtonText().length() > 2;
        const bool hairpin = k.command == composer::Command::StartCrescendo || k.command == composer::Command::StartDiminuendo;
        place(*k.cap, hairpin ? 34 : wide ? 38 : kKey, k.group_start);
        if (hairpin && apply) {
            // The key on top, its level number under it.
            auto& cap = *k.cap;
            const auto b = cap.getBounds();
            cap.setBounds(b.withHeight(17));
            auto& lv = const_cast<juce::Slider&>(k.command == composer::Command::StartCrescendo ? cresc_level_ : dim_level_);
            lv.setBounds(b.getX(), b.getY() + 18, b.getWidth(), b.getHeight() - 18);
            lv.setTextBoxStyle(juce::Slider::TextBoxLeft, false, b.getWidth(), b.getHeight() - 18);
        }
    }
    place(const_cast<hw::Keycap&>(chord_), 52, true);
    place(const_cast<hw::Keycap&>(circle_), 52, false);
    place(const_cast<hw::Keycap&>(detach_), 56, true);
    // The accent dial at the right end: the knob, its name and value.
    {
        if (x > 6) x += kGroupGap - kGap;
        const int w = kKey + 44;
        if (x + w > width - 6 && x > 6) {
            x = 6;
            y += kKey + kGap;
        }
        if (apply) {
            const_cast<hw::SmallKnob&>(accent_).setBounds(x, y, kKey, kKey);
            const_cast<juce::Label&>(accent_label_).setBounds(x + kKey + 2, y, 42, kKey);
        }
        x += w + kGap;
    }
    if (height) *height = y + kKey + 5;
}

int StaffTools::height_for(int width) const
{
    int h = 0;
    layout(width, false, &h);
    return h;
}

void StaffTools::resized()
{
    layout(getWidth(), true, nullptr);
    collapse_.setBounds(getWidth() - 22, 1, 18, 14);
}

void StaffTools::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour(hw::tok::panel_bottom.withAlpha(detached_ ? 1.0f : 0.96f));
    g.fillRoundedRectangle(r, 5.0f);
    g.setColour(hw::Theme::get().led().withAlpha(0.6f));
    g.drawRoundedRectangle(r.reduced(0.5f), 5.0f, 1.0f);
    hw::paint::silk(g, "Staff tools" + (state_.track.isNotEmpty() ? "  -  " + state_.track : juce::String()) +
                           (detached_ ? juce::String() : juce::String("   (drag here to float it - another screen too)")),
                    juce::Rectangle<float>(8.0f, 1.0f, r.getWidth() - 40.0f, static_cast<float>(kHeader)), 9.5f);
    if (dragging_out_) {
        g.setColour(hw::Theme::get().led().withAlpha(0.25f));
        g.fillRoundedRectangle(r, 5.0f);
    }
}

void StaffTools::mouseMove(const juce::MouseEvent& e)
{
    setMouseCursor(!detached_ && e.y < kHeader ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
}

void StaffTools::mouseDown(const juce::MouseEvent& e)
{
    dragging_out_ = false;
    drag_from_ = e.getPosition();
}

void StaffTools::mouseDrag(const juce::MouseEvent& e)
{
    // Pulled by its title strip: it comes out of the edit window.
    if (detached_ || drag_from_.y >= kHeader) return;
    if (!dragging_out_ && e.getDistanceFromDragStart() > 12) {
        dragging_out_ = true;
        repaint();
    }
}

void StaffTools::mouseUp(const juce::MouseEvent& e)
{
    if (!dragging_out_) return;
    dragging_out_ = false;
    repaint();
    if (on_drag_out) on_drag_out(e.getScreenPosition() - drag_from_);
}

}  // namespace vcc::desktop::composer_ui
