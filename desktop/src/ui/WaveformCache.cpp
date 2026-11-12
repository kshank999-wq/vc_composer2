#include "ui/WaveformCache.h"

namespace vcc::desktop {

WaveformCache::WaveformCache() { formats_.registerBasicFormats(); }

WaveformCache::~WaveformCache()
{
    for (auto& [path, e] : entries_)
        if (e.thumb) e.thumb->removeChangeListener(this);
}

WaveformCache::Entry& WaveformCache::refresh(const std::string& path)
{
    Entry& e = entries_[path];
    const juce::uint32 now = juce::Time::getMillisecondCounter();
    if (e.checked != 0 && now - e.checked < 1000) return e;
    e.checked = now == 0 ? 1 : now;
    const juce::File file(juce::String::fromUTF8(path.c_str()));
    e.exists = file.existsAsFile();
    if (!e.exists) return e;
    const juce::Time modified = file.getLastModificationTime();
    if (e.thumb && modified == e.modified) return e;
    e.modified = modified;
    if (!e.thumb) {
        // 256 source samples per thumbnail sample: fine enough for close zoom.
        e.thumb = std::make_unique<juce::AudioThumbnail>(256, formats_, cache_);
        e.thumb->addChangeListener(this);
    }
    // The source is read on the cache's background thread.
    e.thumb->setSource(new juce::FileInputSource(file));
    return e;
}

juce::AudioThumbnail* WaveformCache::get(const std::string& path)
{
    if (path.empty()) return nullptr;
    Entry& e = refresh(path);
    if (!e.exists || !e.thumb) return nullptr;
    return e.thumb.get();
}

bool WaveformCache::exists(const std::string& path) { return !path.empty() && refresh(path).exists; }

void WaveformCache::changeListenerCallback(juce::ChangeBroadcaster*)
{
    if (on_changed) on_changed();
}

bool WaveformCache::read_info(juce::AudioFormatManager& formats, const juce::File& file, FileInfo& info)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (!reader || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0) return false;
    info.channels = static_cast<int>(reader->numChannels);
    info.sample_rate = reader->sampleRate;
    info.length_seconds = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
    return true;
}

}  // namespace vcc::desktop
