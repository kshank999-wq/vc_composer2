// Audio files for clip playback (Main Spec §11).
//
// Files are decoded on the message thread, converted to the engine's sample
// rate, and kept in memory; playback snapshots share the decoded data, so the
// audio thread only ever reads from RAM. Limitation: every file a project
// plays is memory-resident (about 11 MB per stereo minute at 48 kHz) and is
// decoded synchronously the first time a snapshot needs it; streaming from
// disk is the follow-up for very long projects.
#pragma once

#include <map>
#include <memory>
#include <string>

#include <juce_audio_formats/juce_audio_formats.h>

namespace vcc::desktop {

struct AudioData {
    juce::AudioBuffer<float> samples;  ///< At `sample_rate` (the engine's rate).
    double sample_rate = 0.0;
    double file_sample_rate = 0.0;
    int channels() const noexcept { return samples.getNumChannels(); }
    std::int64_t length() const noexcept { return samples.getNumSamples(); }
};

class AudioFileCache {
public:
    AudioFileCache();

    /// The file's audio at `sample_rate`, or nullptr with `error` set
    /// ("missing", "unreadable"). A missing file is retried on the next call.
    std::shared_ptr<const AudioData> get(const std::string& path, double sample_rate, juce::String& error);
    /// Drops a file (it was rewritten).
    void forget(const std::string& path);
    /// Drops everything not used at `sample_rate` (after a device change).
    void keep_only_rate(double sample_rate);

private:
    struct Entry {
        std::shared_ptr<const AudioData> data;
        juce::Time modified;
        std::int64_t size = 0;
    };
    juce::AudioFormatManager formats_;
    std::map<std::pair<std::string, double>, Entry> entries_;
};

}  // namespace vcc::desktop
