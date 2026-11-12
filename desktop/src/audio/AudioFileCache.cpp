#include "audio/AudioFileCache.h"

#include <cmath>

namespace vcc::desktop {

AudioFileCache::AudioFileCache() { formats_.registerBasicFormats(); }

std::shared_ptr<const AudioData> AudioFileCache::get(const std::string& path, double sample_rate, juce::String& error)
{
    if (path.empty() || !juce::File::isAbsolutePath(juce::String(path))) {
        error = "Missing audio file: " + (path.empty() ? juce::String("(none)") : juce::String(path));
        return nullptr;
    }
    const juce::File file{juce::String(path)};
    if (!file.existsAsFile()) {
        error = "Missing audio file: " + file.getFileName();
        return nullptr;
    }
    const auto key = std::make_pair(path, sample_rate);
    const juce::Time modified = file.getLastModificationTime();
    const std::int64_t size = file.getSize();
    if (auto it = entries_.find(key); it != entries_.end()) {
        if (it->second.modified == modified && it->second.size == size) return it->second.data;
        entries_.erase(it);
    }

    std::unique_ptr<juce::AudioFormatReader> reader(formats_.createReaderFor(file));
    if (!reader || reader->lengthInSamples <= 0 || reader->numChannels == 0 || reader->sampleRate <= 0.0) {
        error = "Unreadable audio file: " + file.getFileName();
        return nullptr;
    }
    if (reader->lengthInSamples > std::numeric_limits<int>::max() / 2) {
        error = "Audio file too long to play: " + file.getFileName();
        return nullptr;
    }
    const int channels = static_cast<int>(std::min<unsigned int>(2, reader->numChannels));
    const int length = static_cast<int>(reader->lengthInSamples);
    juce::AudioBuffer<float> raw(channels, length);
    reader->read(&raw, 0, length, 0, true, channels > 1);

    auto data = std::make_shared<AudioData>();
    data->sample_rate = sample_rate;
    data->file_sample_rate = reader->sampleRate;
    if (juce::approximatelyEqual(reader->sampleRate, sample_rate)) {
        data->samples = std::move(raw);
    } else {
        // Convert once, here, so playback is a plain read.
        const double ratio = reader->sampleRate / sample_rate;  // input samples per output sample
        const int out_length = static_cast<int>(std::ceil(length / ratio));
        data->samples.setSize(channels, out_length);
        juce::AudioBuffer<float> padded(channels, length + 64);
        padded.clear();
        for (int ch = 0; ch < channels; ++ch) padded.copyFrom(ch, 0, raw, ch, 0, length);
        for (int ch = 0; ch < channels; ++ch) {
            juce::LagrangeInterpolator interpolator;
            interpolator.process(ratio, padded.getReadPointer(ch), data->samples.getWritePointer(ch), out_length);
        }
    }
    entries_[key] = {data, modified, size};
    return data;
}

void AudioFileCache::forget(const std::string& path)
{
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->first.first == path) it = entries_.erase(it);
        else ++it;
    }
}

void AudioFileCache::keep_only_rate(double sample_rate)
{
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (!juce::approximatelyEqual(it->first.second, sample_rate)) it = entries_.erase(it);
        else ++it;
    }
}

}  // namespace vcc::desktop
