// A track (or a preview) played by the built-in sound pack (audio/SoundPack):
// the Salamander grand for pianos, MuseScore General's General MIDI program
// for everything else, its drum kit on the drum channel.
//
// Each instance plays its own copy of the bank's player (the samples are
// shared, read-only); its voices and MIDI channels are allocated in prepare(),
// so process() never allocates, locks or blocks.
#pragma once

#include <memory>

#include "audio/Instrument.h"
#include "audio/SoundPack.h"

namespace vcc::desktop {

/// Which bank and program play an instrument: the piano for GM programs 0-1
/// (acoustic pianos), MuseScore General for the rest, its standard kit for
/// unpitched instruments. Null bank: the pack is not loaded.
struct PackVoice {
    std::shared_ptr<const sounds::Bank> bank;
    int bank_number = 0;  ///< 128: drums
    int program = 0;
    juce::String name;    ///< "Salamander Grand Piano", "MuseScore General: String Ensemble 1"
    bool valid() const noexcept { return bank != nullptr; }
};
PackVoice pack_voice_for(int gm_program, bool unpitched);

class SoundFontInstrument final : public Instrument {
public:
    explicit SoundFontInstrument(PackVoice voice, int polyphony = 48);
    ~SoundFontInstrument() override;

    void prepare(double sample_rate, int max_block_size) override;
    void process(juce::AudioBuffer<float>& out, juce::MidiBuffer& midi, int num_samples) noexcept override;
    void all_notes_off() noexcept override;
    juce::String display_name() const override { return voice_.name; }

    const PackVoice& voice() const noexcept { return voice_; }
    /// The General MIDI program playing now (an articulation's switch changes it).
    int current_program() const;

private:
    void render(juce::AudioBuffer<float>& out, int start, int count) noexcept;
    void handle(const juce::MidiMessage& m) noexcept;

    PackVoice voice_;
    int polyphony_;
    tsf* player_ = nullptr;
    int preset_ = -1;
    float gain_ = 1.0f;
    std::vector<float> scratch_;  ///< left then right, unweaved
};

}  // namespace vcc::desktop
