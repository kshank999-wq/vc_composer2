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

StaffTools::StaffTools(bool detached) : detached_(detached)
{
    setComponentID(detached ? "staff_tools_window" : "staff_tools");
    setTitle("Staff tools");
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
    add(C::StartCrescendo, "<", nullptr, "Crescendo", W::Selection);
    add(C::StartDiminuendo, ">", nullptr, "Diminuendo", W::Selection);
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
        else if (c >= static_cast<int>(C::SelectVoice1) && c <= static_cast<int>(C::SelectVoice4))
            lit = s.voice == c - static_cast<int>(C::SelectVoice1);
        k.cap->setToggleState(lit, juce::dontSendNotification);
    }
    chord_.setToggleState(s.chord_mode, juce::dontSendNotification);
    chord_.setEnabled(s.editable && s.pitched);
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
    for (const auto& k : keys_) {
        if (!k.cap->isVisible()) continue;
        const bool wide = k.cap->getButtonText().length() > 2;
        place(*k.cap, wide ? 38 : kKey, k.group_start);
    }
    place(const_cast<hw::Keycap&>(chord_), 52, true);
    place(const_cast<hw::Keycap&>(circle_), 52, false);
    place(const_cast<hw::Keycap&>(detach_), 56, true);
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
    hw::paint::silk(g, "Staff tools" + (state_.track.isNotEmpty() ? "  -  " + state_.track : juce::String()),
                    juce::Rectangle<float>(8.0f, 1.0f, r.getWidth() - 40.0f, static_cast<float>(kHeader)), 9.5f);
}

}  // namespace vcc::desktop::composer_ui
