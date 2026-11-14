// External MIDI out (docs/spec/ui, "Track types": a MIDI track's regions
// sent to external hardware).
//
// The audio thread never touches a MIDI device: it stamps each message with
// the wall-clock time it should sound (block start + its offset + the output
// latency, so the hardware lines up with what is heard) and pushes it onto a
// lock-free queue; a sender thread waits for each time and sends it.
// Destinations are "sinks": a real MIDI output port, or a virtual one a test
// registers by name.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

#include <juce_audio_devices/juce_audio_devices.h>

#include "vcc/audio/SpscRingBuffer.h"

namespace vcc::desktop {

class MidiSink {
public:
    virtual ~MidiSink() = default;
    virtual void send(const juce::MidiMessage& m) = 0;
};

class MidiOutRouter final : private juce::Thread {
public:
    static constexpr int kMaxSinks = 16;
    MidiOutRouter();
    ~MidiOutRouter() override;

    /// A virtual destination by name, for every router in the process (the checks).
    static void register_virtual(const juce::String& name, std::shared_ptr<MidiSink> sink);
    static void unregister_virtual(const juce::String& name);
    /// The MIDI outputs to offer: virtual ones, then the system's ports.
    static juce::StringArray device_names();

    /// Message thread: the sink index for a device name, opening it when
    /// needed; -1 when there is no such device (or `hardware` is false and
    /// it is not virtual: offline renders never play real hardware).
    int sink_for(const juce::String& name, bool hardware);

    /// Audio thread: send `m` when Time::getMillisecondCounterHiRes() reaches `due_ms`.
    void push(int sink, const juce::MidiMessage& m, double due_ms) noexcept;
    /// Messages lost because the queue was full (should stay 0).
    int dropped() const noexcept { return dropped_.load(); }

private:
    void run() override;
    struct Event {
        int sink = -1;
        std::uint8_t bytes[3] = {0, 0, 0};
        int size = 0;
        double due = 0.0;
    };
    audio::SpscQueue<Event> queue_{8192};
    std::array<std::shared_ptr<MidiSink>, kMaxSinks> sinks_{};
    std::array<juce::String, kMaxSinks> names_{};
    int count_ = 0;  // message thread
    std::atomic<int> dropped_{0};
};

}  // namespace vcc::desktop
