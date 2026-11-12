// What plays a track: the built-in placeholder synth or a hosted plug-in.
//
// Instruments are created, prepared and destroyed on the message thread and
// only ever processed on the audio thread. The engine guarantees an
// instrument outlives every playback snapshot that refers to it.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace vcc::desktop {

class Instrument {
public:
    virtual ~Instrument() = default;

    /// Message thread, while the instrument is not being processed.
    virtual void prepare(double sample_rate, int max_block_size) = 0;

    /// Audio thread. Adds (not replaces) stereo output into `out` channels
    /// 0 and 1 over [0, num_samples). Must not allocate, lock or block.
    virtual void process(juce::AudioBuffer<float>& out, juce::MidiBuffer& midi, int num_samples) noexcept = 0;

    /// Audio thread: silence every sounding note now (stop, locate).
    virtual void all_notes_off() noexcept = 0;

    /// Whether keyswitch notes should be sent. Placeholders do not want them:
    /// a keyswitch is an instruction to a sample player, never music.
    virtual bool wants_keyswitches() const noexcept { return false; }

    virtual juce::String display_name() const = 0;
};

}  // namespace vcc::desktop
