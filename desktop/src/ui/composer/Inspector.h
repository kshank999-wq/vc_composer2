// The Composer view's inspector (docs/spec/ui/SPEC.md, "Layout" 3c):
// articulation and dynamics keys that mark the selected notes and light when
// every selected note carries the mark, the Velocity and Humanize ring
// knobs that write performance overrides (the score is never changed by
// them), the voice keys and transpose.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "ui/AppContext.h"
#include "ui/hw/HwControls.h"
#include "vcc/project/RenderCache.h"

namespace vcc::desktop::composer_ui {

class Inspector final : public hw::Panel {
public:
    explicit Inspector(AppContext& context);
    ~Inspector() override;

    /// Runs a notation command on the selected notes (the Composer view's editor).
    std::function<void(composer::Command)> on_command;

    /// The editor's voice (0-3), for the lit voice key.
    void set_voice(int voice);
    void refresh();
    /// Re-reads the articulation keys' legends from the active shortcut profile.
    void refresh_legends();
    /// Laid out in columns, for when the inspector sits under the edit window.
    void set_horizontal(bool horizontal);

    void resized() override;
    void paintOverChildren(juce::Graphics& g) override;

    hw::Keycap& articulation_key(int i) { return *artic_[static_cast<std::size_t>(i)]; }
    hw::Keycap& dynamic_key(int i) { return *dyn_[static_cast<std::size_t>(i)]; }
    hw::Keycap& voice_key(int i) { return *voice_[static_cast<std::size_t>(i)]; }
    hw::Keycap& transpose_key(int i) { return *transpose_[static_cast<std::size_t>(i)]; }
    hw::RingKnob& velocity() { return velocity_; }
    hw::RingKnob& humanize() { return humanize_; }
    /// The selected notes' performed velocity (overrides included), -1 when none or mixed.
    int performed_velocity() const noexcept { return shown_velocity_; }
    /// Sets every selected note's performed velocity (as the knob does). One undo step.
    void set_velocity(int velocity);
    /// Humanizes the selected notes by `percent` (as releasing the knob does). One undo step.
    void apply_humanize(double percent);

private:
    std::vector<notation::EventId> selected() const;
    /// Performed velocity per selected note, and each one's current override.
    std::map<notation::EventId, std::pair<int, int>> velocities() const;

    AppContext& context_;
    int listener_ = 0;
    mutable project::RenderCache render_;
    std::vector<std::unique_ptr<hw::Keycap>> artic_, dyn_, voice_, transpose_;
    std::vector<composer::Command> artic_cmd_, dyn_cmd_;
    hw::RingKnob velocity_, humanize_;
    hw::Lcd vel_lcd_{"-", 12.0f}, hum_lcd_{"0%", 12.0f};
    int shown_velocity_ = -1;
    int gesture_ = 0;
    bool horizontal_ = false;
    std::uint64_t seed_ = 1;
};

}  // namespace vcc::desktop::composer_ui
