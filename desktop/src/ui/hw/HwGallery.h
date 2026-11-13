// Every hardware control in one place, laid out like the Edit window's top
// deck and toolbar (docs/spec/ui/mockups): the step-1 build of the UI spec,
// for comparing with the mockups side by side. Opened from View › Control
// Gallery; the headless check photographs it in each theme.
#pragma once

#include <memory>
#include <vector>

#include "ui/hw/HwControls.h"

namespace vcc::desktop::hw {

class Gallery final : public juce::Component, private juce::ChangeListener {
public:
    Gallery();
    ~Gallery() override;
    void resized() override;
    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;

    /// For the check: the controls, by role.
    Keycap& step_key() { return *entry_[2]; }
    Keycap& record_arm() { return *rsm_[0]; }
    RingKnob& velocity() { return velocity_; }
    Meter& master_left() { return master_l_; }
    Fader& fader() { return fader_; }
    Wheel& pitch_wheel() { return pitch_; }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    Keycap& key(std::vector<std::unique_ptr<Keycap>>& into, const juce::String& legend, bool toggles, bool on = false);
    Lcd& lcd(const juce::String& text, float size, const juce::String& caption = {});
    Panel& panel(const juce::String& title, bool screws = false);

    std::vector<std::unique_ptr<Panel>> panels_;
    std::vector<std::unique_ptr<Lcd>> lcds_;
    std::vector<std::unique_ptr<Keycap>> view_switch_, transport_, session_, entry_, views_, zoom_, rsm_, artic_,
        dynamics_, voices_, theme_keys_, octave_;
    std::vector<std::unique_ptr<SmallKnob>> small_knobs_;
    std::vector<std::unique_ptr<Led>> leds_;
    std::vector<std::unique_ptr<Meter>> track_meters_;
    Meter master_l_{Meter::Orientation::Horizontal, 30}, master_r_{Meter::Orientation::Horizontal, 30};
    Meter fader_l_{Meter::Orientation::Vertical, 20}, fader_r_{Meter::Orientation::Vertical, 20};
    RingKnob velocity_, humanize_;
    Fader fader_;
    Wheel pitch_{true}, mod_{false};

    // Panels by role (owned in panels_).
    Panel *nameplate_ = nullptr, *transport_panel_ = nullptr, *counter_ = nullptr, *session_panel_ = nullptr,
          *master_ = nullptr, *toolbar_ = nullptr, *header_ = nullptr, *performance_ = nullptr, *mix_ = nullptr,
          *wheels_ = nullptr, *inspector_ = nullptr, *theme_panel_ = nullptr;
    Lcd *main_counter_ = nullptr, *sub_counter_ = nullptr, *range_ = nullptr, *tempo_ = nullptr, *meter_lcd_ = nullptr,
        *key_lcd_ = nullptr, *grid_ = nullptr, *track_no_ = nullptr, *scribble_ = nullptr, *vol_lcd_ = nullptr,
        *pan_lcd_ = nullptr, *vel_lcd_ = nullptr, *hum_lcd_ = nullptr, *fader_lcd_ = nullptr, *octave_lcd_ = nullptr;
};

}  // namespace vcc::desktop::hw
