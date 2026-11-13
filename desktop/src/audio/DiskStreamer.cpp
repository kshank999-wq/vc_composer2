#include "audio/DiskStreamer.h"

#include <algorithm>
#include <cmath>

namespace vcc::desktop {

namespace {
/// Engine frames produced per fill step: small enough that a seek is served
/// quickly, large enough that a step is an efficient read.
constexpr int kFillStep = 4096;
/// A seek counts as served once this much is buffered (or the clip ends).
constexpr int kReadyFrames = 2048;
}  // namespace

// --- ClipStream ---------------------------------------------------------------------------

ClipStream::~ClipStream()
{
    if (owner_) owner_->detach(this);
}

void ClipStream::reset_reader(std::int64_t k)
{
    fill_k_ = std::clamp<std::int64_t>(k, 0, length_);
    source_pos_ = std::floor(static_cast<double>(offset_ + fill_k_) * ratio_);
    for (auto& i : interp_) i.reset();
}

bool ClipStream::fill_some(int max_frames)
{
    const int space = fifo_.getFreeSpace();
    const int frames = static_cast<int>(std::min<std::int64_t>({space, max_frames, length_ - fill_k_}));
    if (frames <= 0) return false;

    // Produce `frames` engine-rate frames into converted_.
    const bool same_rate = std::abs(ratio_ - 1.0) < 1e-9;
    const int needed = same_rate ? frames : static_cast<int>(std::ceil(frames * ratio_)) + 8;
    const auto start = static_cast<std::int64_t>(source_pos_);
    staging_.clear(0, needed);
    const std::int64_t in_file = std::min<std::int64_t>(needed, std::max<std::int64_t>(0, file_frames_ - start));
    if (in_file > 0) reader_->read(&staging_, 0, static_cast<int>(in_file), start, true, channels_ > 1);
    if (channels_ == 1) staging_.copyFrom(1, 0, staging_, 0, 0, needed);
    if (same_rate) {
        for (int ch = 0; ch < 2; ++ch) converted_.copyFrom(ch, 0, staging_, ch, 0, frames);
        source_pos_ += frames;
    } else {
        int used = 0;
        for (int ch = 0; ch < 2; ++ch)
            used = interp_[ch].process(ratio_, staging_.getReadPointer(ch), converted_.getWritePointer(ch), frames);
        source_pos_ += used;
    }

    int start1, size1, start2, size2;
    fifo_.prepareToWrite(frames, start1, size1, start2, size2);
    for (int ch = 0; ch < 2; ++ch) {
        if (size1 > 0) ring_.copyFrom(ch, start1, converted_, ch, 0, size1);
        if (size2 > 0) ring_.copyFrom(ch, start2, converted_, ch, size1, size2);
    }
    fifo_.finishedWrite(size1 + size2);
    fill_k_ += size1 + size2;
    return true;
}

bool ClipStream::service()
{
    bool worked = false;
    const std::uint32_t req = requested_.load(std::memory_order_acquire);
    if (req != serving_) {
        // The reader is not reading while a seek is outstanding, so the ring
        // can be emptied from this side.
        serving_ = req;
        fifo_.reset();
        reset_reader(seek_k_.load(std::memory_order_relaxed));
        worked = true;
    }
    worked = fill_some(kFillStep) || worked;
    if (ready_.load(std::memory_order_relaxed) != serving_ &&
        (fifo_.getNumReady() >= kReadyFrames || fifo_.getFreeSpace() == 0 || fill_k_ >= length_))
        ready_.store(serving_, std::memory_order_release);
    return worked;
}

void ClipStream::cue(std::int64_t k) noexcept
{
    k = std::clamp<std::int64_t>(k, 0, length_);
    if (k == next_k_) return;
    next_k_ = k;
    seek_k_.store(k, std::memory_order_relaxed);
    requested_.fetch_add(1, std::memory_order_release);
}

bool ClipStream::read(std::int64_t k, int n, const float* gains, float* l, float* r, bool mono_sum) noexcept
{
    const int want = static_cast<int>(std::min<std::int64_t>(n, length_ - k));
    if (want <= 0 || k < 0) return true;
    if (k != next_k_) cue(k);
    if (owner_->synchronous()) {
        // Offline: fill here, as far as this read needs.
        while (ready_.load(std::memory_order_acquire) != requested_.load(std::memory_order_relaxed) ||
               fifo_.getNumReady() < want)
            if (!service() && fifo_.getNumReady() < want && fill_k_ >= length_) break;
    }
    if (ready_.load(std::memory_order_acquire) != requested_.load(std::memory_order_relaxed) ||
        fifo_.getNumReady() < want) {
        // Not there yet: play silence and move the stream on to where the
        // next block will read. Right after a jump that is expected (a seek
        // is being served); a stream that was in place but ran dry means the
        // disk fell behind, and that is what underruns() reports.
        const bool starved = ready_.load(std::memory_order_acquire) == requested_.load(std::memory_order_relaxed);
        next_k_ = k + want;
        seek_k_.store(k + want, std::memory_order_relaxed);
        requested_.fetch_add(1, std::memory_order_release);
        if (starved) owner_->underruns_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    int start1, size1, start2, size2;
    fifo_.prepareToRead(want, start1, size1, start2, size2);
    const float* rl = ring_.getReadPointer(0);
    const float* rr = ring_.getReadPointer(1);
    auto mix = [&](int from, int count, int out) {
        for (int i = 0; i < count; ++i) {
            float vl = rl[from + i], vr = rr[from + i];
            if (mono_sum) vl = vr = 0.5f * (vl + vr);
            const float g = gains[out + i];
            l[out + i] += vl * g;
            r[out + i] += vr * g;
        }
    };
    mix(start1, size1, 0);
    mix(start2, size2, size1);
    fifo_.finishedRead(size1 + size2);
    next_k_ = k + want;
    return true;
}

// --- DiskStreamer --------------------------------------------------------------------------

DiskStreamer::DiskStreamer()
{
    formats_.registerBasicFormats();
    thread_.addTimeSliceClient(this);
    thread_.startThread(juce::Thread::Priority::high);
}

DiskStreamer::~DiskStreamer()
{
    thread_.removeTimeSliceClient(this);
    thread_.stopThread(2000);
    std::lock_guard<std::mutex> lock(streams_lock_);
    for (auto* s : streams_) s->owner_ = nullptr;
    streams_.clear();
}

void DiskStreamer::set_synchronous(bool on) { synchronous_.store(on); }

std::shared_ptr<ClipStream> DiskStreamer::open(const std::string& path, double sample_rate, std::int64_t offset,
                                               std::int64_t length, juce::String& error)
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
    std::unique_ptr<juce::AudioFormatReader> reader(formats_.createReaderFor(file));
    if (!reader || reader->lengthInSamples <= 0 || reader->numChannels == 0 || reader->sampleRate <= 0.0) {
        error = "Unreadable audio file: " + file.getFileName();
        return nullptr;
    }
    std::shared_ptr<ClipStream> s(new ClipStream());
    s->owner_ = this;
    s->channels_ = reader->numChannels > 1 ? 2 : 1;
    s->ratio_ = reader->sampleRate / sample_rate;
    s->file_frames_ = reader->lengthInSamples;
    s->offset_ = std::max<std::int64_t>(0, offset);
    s->length_ = std::max<std::int64_t>(0, length);
    s->reader_ = std::move(reader);
    const int capacity = std::max(8192, static_cast<int>(kReadAheadSeconds * sample_rate));
    s->fifo_.setTotalSize(capacity + 1);
    s->ring_.setSize(2, capacity + 1);
    s->staging_.setSize(2, static_cast<int>(std::ceil(kFillStep * std::max(1.0, s->ratio_))) + 16);
    s->converted_.setSize(2, kFillStep);
    // Start filled from the clip's beginning, so a clip reached in playback
    // is ready before it starts.
    s->seek_k_ = 0;
    s->reset_reader(0);
    while (s->fifo_.getFreeSpace() > 0 && s->fill_k_ < s->length_ && s->fill_some(kFillStep)) {}
    {
        std::lock_guard<std::mutex> lock(streams_lock_);
        streams_.insert(s.get());
    }
    return s;
}

void DiskStreamer::detach(ClipStream* stream)
{
    // Waits for the disk thread to be outside useTimeSlice, which holds the lock.
    std::lock_guard<std::mutex> lock(streams_lock_);
    streams_.erase(stream);
}

int DiskStreamer::useTimeSlice()
{
    if (synchronous_.load()) return 20;
    bool worked = false;
    {
        std::lock_guard<std::mutex> lock(streams_lock_);
        for (auto* s : streams_) worked = s->service() || worked;
    }
    // Busy: come straight back. Idle: a short nap, well inside the read-ahead.
    return worked ? 0 : 5;
}

}  // namespace vcc::desktop
