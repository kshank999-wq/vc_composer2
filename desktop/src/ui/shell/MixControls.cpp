#include "ui/shell/MixControls.h"

#include "ui/console/MixActions.h"
#include "ui/console/RackGear.h"

namespace vcc::desktop::shell {

InsertSlots::InsertSlots(AppContext& context, bool compact) : context_(context), compact_(compact)
{
    setTooltip("Inserts, as rack gear: click a unit to open the rack and turn its knobs; click its lamp to bypass; "
               "click an empty bay to add gear; right-click for more");
}

void InsertSlots::set_owner(const std::string& owner_id)
{
    owner_ = owner_id;
    repaint();
}

juce::Rectangle<int> InsertSlots::slot_rect(int i) const
{
    // Stacked, as in a rack (the deck's compact strips too).
    const int h = getHeight() / kSlots;
    return {0, i * h, getWidth(), h};
}

juce::Rectangle<int> InsertSlots::led_rect(int i) const
{
    // The ear and the power lamp.
    const auto r = slot_rect(i);
    return r.withWidth(std::min(r.getWidth() / 3, std::max(14, r.getHeight() + 8)));
}

int InsertSlots::slot_at(juce::Point<int> p) const
{
    for (int i = 0; i < kSlots; ++i)
        if (slot_rect(i).contains(p)) return i;
    return -1;
}

void InsertSlots::paint(juce::Graphics& g)
{
    // Each slot is a little rack unit: the gear's faceplate, its power lamp
    // at the left (click it to bypass), its name; an empty slot is an empty bay.
    const auto* list = console::inserts_of(context_, owner_);
    for (int i = 0; i < kSlots; ++i) {
        const auto r = slot_rect(i).toFloat().reduced(0.0f, 0.5f);
        const bool used = list && i < static_cast<int>(list->size());
        console::paint_mini_unit(g, r, used ? &(*list)[static_cast<std::size_t>(i)] : nullptr);
        if (!used && !compact_ && list && i == static_cast<int>(list->size()))
            hw::paint::silk(g, "+", r, 10.0f, juce::Justification::centred);
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
    // A unit opens the strip's rack; an empty bay offers gear to put there.
    if (used && context_.show_insert_window) context_.show_insert_window(owner_, i);
    else console::show_insert_menu(context_, owner_, list ? static_cast<int>(list->size()) : 0, *this);
}

void InsertSlots::mouseDoubleClick(const juce::MouseEvent&) {}

}  // namespace vcc::desktop::shell
