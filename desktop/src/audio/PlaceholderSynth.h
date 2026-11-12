// The built-in placeholder sound (Supplement §8: "functional MIDI
// placeholders"; Main Spec §20: an absent library yields a playable,
// clearly labelled stand-in, never a claim of the real sound).
//
// It is a small subtractive synth whose voice is chosen from the instrument
// family and General MIDI program: bowed strings swell, brass is bright,
// plucked and struck instruments decay, the drum channel plays synthesised
// kit pieces. It is meant to make a template audible on a machine with no
// libraries, not to be mistaken for an orchestra.
#pragma once

#include <memory>

#include "audio/Instrument.h"

namespace vcc::desktop {

enum class PlaceholderVoice {
    Bowed,    ///< strings, slow swell, saw through a low-pass
    Brass,    ///< bright saw, quicker attack
    Reed,     ///< woodwinds, square/triangle mix
    Flute,    ///< soft triangle with breath
    Keys,     ///< struck, decaying (piano, celesta, harpsichord)
    Plucked,  ///< harp, guitar, lute and plucked traditional instruments
    Mallet,   ///< tuned percussion, bells
    Organ,    ///< sustained, no decay
    Pad,      ///< synth pads and drones, slow and wide
    Lead,     ///< synth leads and basses, punchy saw
    Voice,    ///< choirs and solo voice, vowel-ish triangle
    Drums,    ///< General MIDI drum map on channel 10
};

/// Chooses a voice for an instrument family and GM program.
PlaceholderVoice placeholder_voice_for(const std::string& family, int gm_program, bool unpitched);
const char* to_string(PlaceholderVoice voice) noexcept;

class PlaceholderSynth final : public Instrument {
public:
    explicit PlaceholderSynth(PlaceholderVoice voice, int polyphony = 24);
    ~PlaceholderSynth() override;

    void prepare(double sample_rate, int max_block_size) override;
    void process(juce::AudioBuffer<float>& out, juce::MidiBuffer& midi, int num_samples) noexcept override;
    void all_notes_off() noexcept override;
    juce::String display_name() const override;

    PlaceholderVoice voice() const noexcept { return voice_; }

private:
    PlaceholderVoice voice_;
    juce::Synthesiser synth_;
    juce::AudioBuffer<float> scratch_;
};

}  // namespace vcc::desktop
