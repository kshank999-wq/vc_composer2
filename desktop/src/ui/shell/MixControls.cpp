#include "ui/shell/MixControls.h"

#include "ui/console/MixActions.h"

namespace vcc::desktop::shell {

InsertSlots::InsertSlots(AppContext& context, bool compact) : context_(context), compact_(compact)
{
    setTooltip(compact ? "Inserts A-E: click a light to bypass or activate; right-click for the slot; double-click to open it"
                       : "Click a light to bypass or activate; click a name to open it (or add one); right-click for more");
}

void InsertSlots::set_owner(const std::string& owner_id)
{
    owner_ = owner_id;
    repaint();
}

juce::Rectangle<int> InsertSlots::slot_rect(int i) const
{
    if (compact_) {
        const int w = getWidth() / kSlots;
        return {i * w, 0, w, getHeight()};
    }
    const int h = getHeight() / kSlots;
    return {0, i * h, getWidth(), h};
}

juce::Rectangle<int> InsertSlots::led_rect(int i) const
{
    const auto r = slot_rect(i);
    return compact_ ? r : r.withWidth(30);
}

int InsertSlots::slot_at(juce::Point<int> p) const
{
    for (int i = 0; i < kSlots; ++i)
        if (slot_rect(i).contains(p)) return i;
    return -1;
}

void InsertSlots::paint(juce::Graphics& g)
{
    const auto* list = console::inserts_of(context_, owner_);
    for (int i = 0; i < kSlots; ++i) {
        const auto r = slot_rect(i);
        const bool used = list && i < static_cast<int>(list->size());
        const auto* slot = used ? &(*list)[static_cast<std::size_t>(i)] : nullptr;
        const bool active = slot && !slot->bypass && !slot->suggestion;
        const juce::Colour colour = slot && (slot->bypass || slot->suggestion) ? hw::tok::meter_amber : hw::Theme::get().led();
        const auto letter = juce::String::charToString(static_cast<juce::juce_wchar>('A' + i));
        if (compact_) {
            hw::paint::led(g, r.toFloat().getCentre().translated(0.0f, -3.0f), active || (slot && slot->bypass), colour, 5.0f);
            hw::paint::silk(g, letter, r.toFloat().withTrimmedTop(static_cast<float>(r.getHeight()) - 9.0f), 7.5f,
                            juce::Justification::centred);
            continue;
        }
        const auto row = r.toFloat().reduced(0.0f, 1.0f);
        hw::paint::lcd_window(g, row.withTrimmedLeft(30.0f));
        hw::paint::led(g, {row.getX() + 7.0f, row.getCentreY()}, active || (slot && slot->bypass), colour, 6.0f);
        hw::paint::silk(g, letter, row.withX(row.getX() + 14.0f).withWidth(12.0f), 9.5f, juce::Justification::centred);
        juce::String name = slot ? console::insert_name(*slot).toUpperCase() : juce::String("-");
        if (slot && slot->suggestion) name = "(" + name + ")";
        if (slot && slot->bypass) name << "  BYP";
        hw::paint::lcd_text(g, name, row.withTrimmedLeft(36.0f), 10.5f, juce::Justification::centredLeft, slot ? 1.0f : 0.4f);
    }
}

void InsertSlots::mouseUp(const juce::MouseEvent& e)
{
    if (owner_.empty() || e.mouseWasDraggedSinceMouseDown()) return;
    const int i = slot_at(e.getPosition());
    if (i < 0) return;
    const auto* list = console::inserts_of(context_, owner_);
    const bool used = list && i < static_cast<int>(list->size());
    if (e.mods.isPopupMenu()) {
        console::show_insert_menu(context_, owner_, used ? i : static_cast<int>(list ? list->size() : 0), *this);
        return;
    }
    if (used && led_rect(i).contains(e.getPosition())) {
        console::toggle_insert_bypass(context_, owner_, i);
        return;
    }
    if (compact_) return;  // the deck's slots open on a double-click
    if (used && context_.show_insert_window) context_.show_insert_window(owner_, i);
    else console::show_insert_menu(context_, owner_, list ? static_cast<int>(list->size()) : 0, *this);
}

void InsertSlots::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (!compact_ || owner_.empty()) return;
    const int i = slot_at(e.getPosition());
    const auto* list = console::inserts_of(context_, owner_);
    if (i >= 0 && list && i < static_cast<int>(list->size()) && context_.show_insert_window) context_.show_insert_window(owner_, i);
}

}  // namespace vcc::desktop::shell
