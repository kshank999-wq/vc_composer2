// External jog/shuttle wheels and transport controllers (Add-On Features
// spec §7, Feature E). The controller listens to one MIDI input through the
// engine's SurfaceInput (so the surface works whichever window has focus,
// and its messages never play or record), decodes them with the project's
// surface profiles, and moves the transport:
//
//   - the jog steps the playhead by frame, sixteenth, beat or bar, onto the
//     grid, with sensitivity, reverse and acceleration;
//   - the shuttle moves it continuously at a speed set by how far the ring
//     is turned (dead zone, curve, top speed). Scrubbing is a stepped seek:
//     at most one locate per refresh, playback stopped first, the playhead
//     held within the project - no audio dropouts and no runaway;
//   - keys play, pause, stop, locate to the next or previous marker or cue,
//     return to the start and toggle the loop.
//
// Plugging and unplugging are followed (the device is found again by name);
// an unplugged shuttle stops at once. Devices that type keys (most HID
// shuttles with their vendor's software) drive the Jog Forward / Jog Back
// commands instead. Settings live in Tools > Control Surfaces.
#pragma once

#include <functional>
#include <memory>

#include "ui/AppContext.h"
#include "vcc/project/ControlSurface.h"

namespace vcc::desktop::surface_ui {

class ControlSurface final : private juce::Timer {
public:
    explicit ControlSurface(AppContext& context);
    ~ControlSurface() override;

    // --- Device ---------------------------------------------------------------------------------
    /// Listens to a MIDI input (identifier and name; empty for none).
    void set_device(const juce::String& identifier, const juce::String& name);
    const juce::String& device_id() const noexcept { return device_id_; }
    const juce::String& device_name() const noexcept { return device_name_; }
    /// Whether the device is plugged in now.
    bool connected() const noexcept { return connected_; }
    /// Re-reads the MIDI inputs (the timer does this every second).
    void scan_devices();

    // --- Mapping and settings -------------------------------------------------------------------
    const project::surface::Profile& profile() const noexcept { return profile_; }
    /// A built-in profile by name (Custom keeps the learned bindings).
    void use_profile(const juce::String& name);
    /// Binds `action` to `source` (the profile becomes Custom).
    void bind(project::surface::Action action, const project::surface::Source& source,
              project::surface::Encoding encoding);
    void unbind(project::surface::Action action);
    /// The next control moved on the device is bound to `action`.
    void learn(project::surface::Action action);
    void cancel_learn();
    bool learning() const noexcept { return learning_; }
    project::surface::Action learning_action() const noexcept { return learn_action_; }

    project::surface::Settings& settings() noexcept { return jog_.settings; }
    /// Saves the settings (the window calls it after a change).
    void save();

    // --- Transport ------------------------------------------------------------------------------
    /// One jog detent forward or back (the Jog commands, for key-typing devices).
    void jog(int detents);
    const project::surface::JogShuttle& state() const noexcept { return jog_; }
    /// Feeds a message as if it came from the device (checks, and the window's test keys).
    void inject(const juce::MidiMessage& m);
    /// Handles what has arrived and moves the shuttle, at `now` seconds of
    /// any monotonic clock (the timer passes the real time).
    void process(double now);
    /// Runs process() on a timer (on by default; the checks drive it by hand).
    void set_polling(bool on)
    {
        if (on) startTimerHz(60);
        else stopTimer();
    }
    /// The last thing done ("Jog: bar 5", "Learned CC 16 for Jog wheel").
    const juce::String& last_action() const noexcept { return last_action_; }
    int messages() const noexcept { return messages_; }

    /// Called after the surface moved the playhead (the views follow);
    /// `on_grid` when it landed on a beat, bar or sixteenth (the score's
    /// entry cursor follows those, not frames or the shuttle).
    std::function<void(Ticks, bool on_grid)> on_moved;
    /// Called when the device, profile or learning state changes.
    std::function<void()> on_changed;

private:
    void timerCallback() override;
    void configure_input();
    void handle(const project::surface::Event& e, double now);
    void move_to(double seconds, bool on_grid = false);
    void say(const juce::String& text);
    void load();

    AppContext& context_;
    project::surface::JogShuttle jog_;
    project::surface::Profile profile_;
    juce::String device_id_, device_name_;
    bool connected_ = false;
    bool learning_ = false;
    project::surface::Action learn_action_ = project::surface::Action::Jog;
    double scrub_ = 0.0;       ///< The surface's playhead while it is moving it.
    double active_until_ = -1.0;
    double last_now_ = -1.0;
    double last_scan_ = -10.0;
    bool pending_move_ = false;
    bool pending_on_grid_ = false;
    bool varispeed_mode_ = false;  ///< The shuttle is playing the mix at its speed.
    double last_grain_ = -1.0;
    /// Whether scrubbing may be heard now (see Settings::audible).
    bool audible_ok() const;
    void end_varispeed();
    juce::String last_action_;
    int messages_ = 0;
};

/// Tools > Control Surfaces: device, profile, bindings with Learn, and the
/// jog and shuttle settings, with a live view of what the device sends.
void show_control_surfaces(AppContext& context, ControlSurface& surface);
/// The open window's content, or null (for checks).
juce::Component* control_surfaces_panel();

}  // namespace vcc::desktop::surface_ui
