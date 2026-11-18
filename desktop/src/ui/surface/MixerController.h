// A hardware mixer controller in Mackie Control mode (Studio > Mixer
// Controller...): SSL UF8, Avid S1, Icon, Behringer X-Touch, PreSonus
// FaderPort, the Mackie MCU and others. Its eight strips follow the mixer's
// tracks, eight at a time:
//
//   - faders set each track's level (motorised faders follow the mix, and
//     stay where a hand holds them); the master fader sets the master bus;
//   - V-Pots pan (a push centres), Rec arms, Solo and Mute, Select selects;
//   - the scribble strips show each track's name and level; the meters move;
//   - Bank and Channel move the eight strips across the tracks;
//   - Play, Stop, Record, Rewind, Fast Forward, Cycle (loop), Click, Undo
//     and Save do what they say; the jog wheel moves the playhead.
//
// Its MIDI input is taken off the instrument path whole (SurfaceInput), so
// nothing it sends plays or records. Settings are remembered and the
// controller is found again when it is plugged back in.
#pragma once

#include <array>
#include <functional>
#include <memory>

#include "ui/AppContext.h"
#include "vcc/project/Mackie.h"

namespace vcc::desktop {

class MixerController final : private juce::Timer {
public:
    explicit MixerController(AppContext& context);
    ~MixerController() override;

    /// The controller's MIDI ports, by device name (empty: none). Remembered.
    void connect(const juce::String& input_name, const juce::String& output_name);
    void disconnect();
    juce::String input_name() const { return input_name_; }
    juce::String output_name() const { return output_name_; }
    bool connected() const { return input_name_.isNotEmpty(); }
    /// "Connected: X-Touch - tracks 1-8 of 24", or why not.
    juce::String status() const;

    /// The first track on strip 1 (0-based), moved by Bank and Channel.
    int bank_offset() const noexcept { return offset_; }
    void set_bank_offset(int offset);
    /// The track ids on the eight strips now (empty where none).
    std::array<std::string, project::mackie::kStrips> strip_tracks() const;

    /// What the surface sent, decoded (the checks feed this directly).
    void handle(const project::mackie::Event& e);
    /// Everything sent to the surface goes through here (the checks capture it).
    std::function<void(const project::mackie::Bytes&)> sender;
    /// Sends every fader, light, ring and name again (after connecting, a bank move).
    void refresh_all();
    /// One pass of following the mix (the timer runs it ~30 times a second).
    void tick();

private:
    void timerCallback() override { tick(); }
    void send(const project::mackie::Bytes& b);
    void open_ports();
    std::vector<std::string> mixer_tracks() const;

    AppContext& context_;
    juce::String input_name_, output_name_, input_id_;
    std::unique_ptr<juce::MidiOutput> out_;
    int offset_ = 0;
    std::array<bool, project::mackie::kStrips + 1> touched_{};
    struct Sent {
        double fader = -1.0, pan = -9.0;
        int mute = -1, solo = -1, rec = -1, select = -1;
        std::string name, value;
    };
    std::array<Sent, project::mackie::kStrips + 1> sent_{};
    std::array<int, 128> lights_{};
    int ticks_ = 0;
};

/// Studio > Mixer Controller...: the ports, the state, and what it does.
void show_mixer_controller_window(AppContext& context, MixerController& controller);

}  // namespace vcc::desktop
