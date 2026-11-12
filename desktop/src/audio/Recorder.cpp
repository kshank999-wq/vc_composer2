#include "audio/Recorder.h"

namespace vcc::desktop {

void RecordPass::write(const float* const* inputs, int num_inputs, int input_offset, int count, std::int64_t at) noexcept
{
    if (count <= 0 || count > silence.getNumSamples()) return;
    const float* channels[2] = {nullptr, nullptr};
    for (auto& take : takes) {
        if (!take.writer) continue;
        for (int c = 0; c < take.channels; ++c) {
            const int in = take.input + c;
            channels[c] = (inputs != nullptr && in >= 0 && in < num_inputs && inputs[in] != nullptr)
                              ? inputs[in] + input_offset
                              : silence.getReadPointer(0);
        }
        if (take.writer->write(channels, count)) {
            if (take.start_sample < 0) take.start_sample = at;
            take.samples += count;
        } else {
            take.dropped += count;
        }
    }
    recorded += count;
}

juce::File unique_take_file(const juce::File& folder, const std::string& track_name)
{
    juce::String base = juce::File::createLegalFileName(juce::String(track_name)).trim();
    if (base.isEmpty()) base = "Audio";
    for (int n = 1;; ++n) {
        const auto f = folder.getChildFile(base + "_" + juce::String(n).paddedLeft('0', 2) + ".wav");
        if (!f.exists()) return f;
    }
}

juce::String open_take(RecordTake& take, double sample_rate, juce::TimeSliceThread& thread, int fifo_samples)
{
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(take.file);
    if (!static_cast<juce::FileOutputStream*>(stream.get())->openedOk())
        return "cannot create " + take.file.getFullPathName();
    juce::WavAudioFormat wav;
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(sample_rate)
                             .withNumChannels(take.channels)
                             .withBitsPerSample(24);
    auto writer = wav.createWriterFor(stream, options);
    if (!writer) {
        stream.reset();
        take.file.deleteFile();
        return "cannot write a WAV file to " + take.file.getParentDirectory().getFullPathName();
    }
    take.writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(writer.release(), thread, fifo_samples);
    return {};
}

}  // namespace vcc::desktop
