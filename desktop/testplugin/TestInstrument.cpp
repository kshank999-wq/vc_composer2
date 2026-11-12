// A minimal VST3 instrument used only by the desktop check to prove plug-in
// hosting end to end (Main Spec §20: "a VST3 plugin loads, saves and restores
// state"): a sine voice per MIDI note and one saved parameter.
#include <juce_audio_processors/juce_audio_processors.h>

namespace {

class TestInstrument final : public juce::AudioProcessor {
public:
    using juce::AudioProcessor::processBlock;
    TestInstrument()
        : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
    {
        addParameter(level_ = new juce::AudioParameterFloat(juce::ParameterID{"level", 1}, "Level", 0.0f, 1.0f, 0.5f));
    }

    const juce::String getName() const override { return "VCC Test Instrument"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }

    void prepareToPlay(double sample_rate, int) override { sample_rate_ = sample_rate; }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        buffer.clear();
        int position = 0;
        for (const auto metadata : midi) {
            render(buffer, position, metadata.samplePosition);
            position = metadata.samplePosition;
            const auto m = metadata.getMessage();
            if (m.isNoteOn()) { note_ = m.getNoteNumber(); phase_ = 0.0; }
            else if ((m.isNoteOff() && m.getNoteNumber() == note_) || m.isAllNotesOff()) note_ = -1;
        }
        render(buffer, position, buffer.getNumSamples());
    }

    void getStateInformation(juce::MemoryBlock& out) override
    {
        juce::MemoryOutputStream s(out, false);
        s.writeFloat(level_->get());
    }

    void setStateInformation(const void* data, int size) override
    {
        if (size >= 4) {
            juce::MemoryInputStream s(data, static_cast<size_t>(size), false);
            *level_ = s.readFloat();
        }
    }

private:
    void render(juce::AudioBuffer<float>& b, int from, int to)
    {
        if (note_ < 0) return;
        const double f = juce::MidiMessage::getMidiNoteInHertz(note_);
        for (int i = from; i < to; ++i) {
            const float v = 0.2f * level_->get() * static_cast<float>(std::sin(phase_));
            phase_ += 2.0 * juce::MathConstants<double>::pi * f / sample_rate_;
            for (int ch = 0; ch < b.getNumChannels(); ++ch) b.setSample(ch, i, v);
        }
    }

    juce::AudioParameterFloat* level_ = nullptr;
    double sample_rate_ = 48000.0, phase_ = 0.0;
    int note_ = -1;
};

}  // namespace

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TestInstrument(); }
