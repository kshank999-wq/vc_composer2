// A minimal VST3 effect used only by the desktop check to prove plug-in
// delay compensation and effect-parameter automation: it delays its input by
// a configurable number of samples, reports exactly that latency to the host
// (and keeps it while bypassed), and applies a gain.
//
// Parameters: 0 "Latency" (0..8192 samples), 1 "Gain" (0..1, linear).
// Changing the latency at run time calls setLatencySamples from the audio
// callback, the way a plug-in switching to a look-ahead mode would.
#include <juce_audio_processors/juce_audio_processors.h>

#include <csignal>

namespace {

class TestEffect final : public juce::AudioProcessor {
public:
    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;
    static constexpr int kMaxLatency = 8192;

    TestEffect()
        : juce::AudioProcessor(BusesProperties()
                                   .withInput("Input", juce::AudioChannelSet::stereo(), true)
                                   .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    {
        addParameter(latency_ = new juce::AudioParameterInt(juce::ParameterID{"latency", 1}, "Latency", 0, kMaxLatency, 0));
        addParameter(gain_ = new juce::AudioParameterFloat(juce::ParameterID{"gain", 1}, "Gain", 0.0f, 1.0f, 1.0f));
    }

    const juce::String getName() const override { return "VCC Test Effect"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo() &&
               layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }

    void prepareToPlay(double, int max_block) override
    {
        int size = 1;
        while (size < kMaxLatency + max_block + 1) size <<= 1;
        ring_.setSize(2, size);
        ring_.clear();
        mask_ = size - 1;
        write_ = 0;
        current_ = latency_->get();
        setLatencySamples(current_);
    }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
#if defined(VCC_SCAN_TROUBLE) && VCC_SCAN_TROUBLE == 3
        // The hosting check's crasher: turning the gain all the way down
        // crashes it while it plays, the way a buggy plug-in would.
        if (gain_->get() < 0.01f) std::raise(SIGSEGV);
#endif
        run(buffer, gain_->get());
    }
    // Bypassed: no gain, but the same delay, so compensation stays aligned.
    void processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override { run(buffer, 1.0f); }

    void getStateInformation(juce::MemoryBlock& out) override
    {
        juce::MemoryOutputStream s(out, false);
        s.writeInt(latency_->get());
        s.writeFloat(gain_->get());
    }

    void setStateInformation(const void* data, int size) override
    {
        if (size >= 8) {
            juce::MemoryInputStream s(data, static_cast<size_t>(size), false);
            *latency_ = s.readInt();
            *gain_ = s.readFloat();
        }
    }

private:
    void run(juce::AudioBuffer<float>& buffer, float gain)
    {
        if (ring_.getNumSamples() == 0) return;
        if (latency_->get() != current_) {
            current_ = latency_->get();
            setLatencySamples(current_);  // the host is told and re-aligns
        }
        const int n = buffer.getNumSamples();
        for (int ch = 0; ch < std::min(2, buffer.getNumChannels()); ++ch) {
            float* d = buffer.getWritePointer(ch);
            float* ring = ring_.getWritePointer(ch);
            int w = write_;
            for (int i = 0; i < n; ++i) {
                ring[w] = d[i];
                d[i] = ring[(w - current_) & mask_] * gain;
                w = (w + 1) & mask_;
            }
        }
        write_ = (write_ + n) & mask_;
    }

    juce::AudioParameterInt* latency_ = nullptr;
    juce::AudioParameterFloat* gain_ = nullptr;
    juce::AudioBuffer<float> ring_;
    int mask_ = 0, write_ = 0, current_ = 0;
};

}  // namespace

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TestEffect(); }

#if defined(VCC_SCAN_TROUBLE)
// Scanning-check builds of this plug-in misbehave on load (see ScanTrouble.cpp).
#include "ScanTrouble.cpp"
#endif
