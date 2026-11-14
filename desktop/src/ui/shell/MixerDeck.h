// The Producer view's bottom deck as a mixer (docs/spec/ui, "Mix and
// master"): one strip per track and bus - insert lights, pan, a 120 px
// fader beside its stereo meter, a dB readout, S and M, and the coloured
// name plate - then a wider master strip outlined in the LED colour. It
// scrolls sideways when the strips do not fit. A key switches the deck back
// to the keyboard.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ui/AppContext.h"
#include "ui/console/MixActions.h"
#include "ui/hw/HwControls.h"
#include "ui/shell/MixControls.h"

namespace vcc::desktop::shell {

class MixerDeck final : public hw::Panel, private juce::Timer {
public:
    static constexpr int kHeight = 262;
    static constexpr int kStrip = 78, kMasterStrip = 112;

    explicit MixerDeck(AppContext& context);
    ~MixerDeck() override;

    /// The Keyboard key: back to the keyboard deck.
    std::function<void()> on_keyboard;

    class Strip;
    int strip_count() const noexcept { return static_cast<int>(strips_.size()); }
    Strip& strip(int i) { return *strips_[static_cast<std::size_t>(i)]; }
    /// The strip for a track or bus, or null.
    Strip* strip_for(const std::string& owner_id);
    hw::Keycap& keyboard_key() { return keyboard_; }

    void resized() override;
    void paintOverChildren(juce::Graphics& g) override;

private:
    void timerCallback() override;
    void rebuild();
    AppContext& context_;
    int listener_ = 0;
    juce::Viewport viewport_;
    std::unique_ptr<Strip> master_;  ///< Pinned at the right, outside the scroller.
    juce::Component holder_;
    std::vector<std::unique_ptr<Strip>> strips_;
    std::vector<std::string> ids_;
    hw::Keycap keyboard_{"Keyboard"}, console_{"Console"};
};

class MixerDeck::Strip final : public juce::Component {
public:
    Strip(AppContext& context, std::string owner_id, bool master);
    void refresh();
    void follow_automation();
    void push_meter();
    const std::string& owner() const noexcept { return owner_; }
    bool master() const noexcept { return master_; }
    hw::Fader& fader() { return fader_; }
    hw::SmallKnob& pan() { return pan_; }
    hw::Keycap& solo() { return solo_; }
    hw::Keycap& mute() { return mute_; }
    InsertSlots& inserts() { return slots_; }
    const hw::Lcd& db_lcd() const noexcept { return db_; }

    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    void resized() override;
    void mouseUp(const juce::MouseEvent& e) override;

private:
    AppContext& context_;
    std::string owner_;
    bool master_;
    console::MixWriter writer_;
    InsertSlots slots_;
    hw::Fader fader_;
    hw::SmallKnob pan_;
    hw::Meter meter_l_{hw::Meter::Orientation::Vertical, 12}, meter_r_{hw::Meter::Orientation::Vertical, 12};
    hw::Lcd db_{{}, 10.5f};
    hw::Keycap solo_{"S", true}, mute_{"M", true};
    juce::String name_;
    juce::Colour colour_;
    bool selected_ = false;
};

}  // namespace vcc::desktop::shell
