// Mix controls shared by the channel strip and the mixer deck.
#pragma once

#include <string>

#include "ui/AppContext.h"
#include "ui/hw/HwControls.h"

namespace vcc::desktop::shell {

/// Five insert slots, A-E (docs/spec/ui, "Channel strip"). Each has a bypass
/// LED: click the LED to bypass or activate, click the name to open the
/// processor's window (or to add one to an empty slot), right-click for the
/// slot's menu. Compact: the LEDs alone in a row (the mixer deck).
class InsertSlots final : public juce::Component, public juce::SettableTooltipClient {
public:
    static constexpr int kSlots = 5;
    InsertSlots(AppContext& context, bool compact);
    void set_owner(const std::string& owner_id);
    const std::string& owner() const noexcept { return owner_; }
    void refresh() { repaint(); }
    void set_compact(bool compact)
    {
        compact_ = compact;
        repaint();
    }
    juce::Rectangle<int> slot_rect(int i) const;
    juce::Rectangle<int> led_rect(int i) const;

    void paint(juce::Graphics& g) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;

private:
    int slot_at(juce::Point<int> p) const;
    AppContext& context_;
    bool compact_;
    std::string owner_;
};

}  // namespace vcc::desktop::shell
