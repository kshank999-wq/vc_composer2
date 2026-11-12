// Waveform overviews for audio clips (Main Spec §11): one juce::AudioThumbnail
// per audio file, keyed by its resolved path. Thumbnails are generated on the
// AudioThumbnailCache's background thread, as JUCE does; whoever draws them is
// told to repaint as the data comes in.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>

namespace vcc::desktop {

class WaveformCache final : private juce::ChangeListener {
public:
    WaveformCache();
    ~WaveformCache() override;

    /// The thumbnail of a file on disk, or nullptr when the file is missing or
    /// unreadable. Re-reads a file that changed since it was first asked for.
    juce::AudioThumbnail* get(const std::string& path);
    /// True when the file exists (checked at most once a second per path).
    bool exists(const std::string& path);

    /// Called on the message thread when more of a waveform is ready.
    std::function<void()> on_changed;

    juce::AudioFormatManager& formats() noexcept { return formats_; }

    /// What an import needs to know about a file. False if no format reads it.
    struct FileInfo {
        int channels = 0;
        double sample_rate = 0.0;
        double length_seconds = 0.0;
    };
    static bool read_info(juce::AudioFormatManager& formats, const juce::File& file, FileInfo& info);

private:
    struct Entry {
        std::unique_ptr<juce::AudioThumbnail> thumb;
        juce::Time modified;
        bool exists = false;
        juce::uint32 checked = 0;
    };
    Entry& refresh(const std::string& path);
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    juce::AudioFormatManager formats_;
    juce::AudioThumbnailCache cache_{64};
    std::map<std::string, Entry> entries_;
};

}  // namespace vcc::desktop
