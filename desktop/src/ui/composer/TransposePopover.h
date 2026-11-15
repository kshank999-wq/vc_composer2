// Track Trans (Add-On Features spec §10): the popover a track's Trans key
// opens, anchored to the track. +1/-1 and octave keys, a semitone readout
// with the interval's name, a target key, Reset, the A/B bypass, notation +
// playback or playback only, and Apply to notes. It stays open while the
// music plays; every change is heard at once and undoable.
#pragma once

#include <string>

#include "ui/AppContext.h"
#include "ui/hw/HwControls.h"

namespace vcc::desktop::composer_ui {

class TransposePopover final : public juce::Component, private juce::Timer {
public:
    TransposePopover(AppContext& context, std::string track_id);
    ~TransposePopover() override;

    /// Opens it in a call-out pointing at `anchor` (screen area).
    static TransposePopover* show(AppContext& context, const std::string& track_id, juce::Rectangle<int> anchor);
    /// The open one (for checks), or null.
    static TransposePopover* current();

    void nudge(int semitones);  ///< +1, -1, +12, -12 from the current shift
    void set(int semitones);
    void reset();
    void toggle_bypass();
    void set_playback_only(bool on);
    /// Asks, then writes the shift into the notes (one undo step).
    void apply_to_notes();
    /// Sets the shift that takes the project's key to `tonic` (nearest way, up or down).
    void transpose_to_key(int pitch_class);

    void paint(juce::Graphics& g) override;
    void resized() override;

    // For the checks.
    hw::Keycap& up_key() { return up_; }
    hw::Keycap& down_key() { return down_; }
    hw::Keycap& bypass_key() { return bypass_; }
    const juce::String& readout() const noexcept { return readout_; }
    const juce::String& warning() const noexcept { return warning_; }

private:
    void timerCallback() override { refresh(); }
    void refresh();

    AppContext& context_;
    std::string track_;
    juce::String readout_, interval_, warning_;
    hw::Keycap down_{"-1"}, up_{"+1"}, oct_down_{"Oct -"}, oct_up_{"Oct +"}, reset_{"Reset"};
    hw::Keycap bypass_{"A/B bypass", false}, notation_{"Notation + playback", false}, playback_{"Playback only", false};
    hw::Keycap apply_{"Apply to notes..."};
    juce::ComboBox target_;
};

}  // namespace vcc::desktop::composer_ui
