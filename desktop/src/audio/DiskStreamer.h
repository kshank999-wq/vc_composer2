// Audio clips streamed from disk (Main Spec §11).
//
// Every clip that plays has its own small read-ahead buffer (a lock-free
// single-producer/single-consumer ring) that a background disk thread keeps
// filled from the file, converting the file's sample rate to the engine's on
// the fly. The audio thread only pops samples: it never opens, reads or
// seeks a file, never waits and never allocates. Memory is bounded by the
// read-ahead, not by the length of the project (the earlier design held
// every file in memory, about 11 MB per stereo minute).
//
// Seeking: the audio thread asks a stream for clip position k. When the
// buffer does not start at k (after a locate, or a jump), it raises a seek
// request and plays silence until the disk thread has refilled from k. To
// keep that from being heard, the engine "cues" streams while the transport
// is stopped, so they are already positioned when play is pressed; and every
// stream starts out filled from the start of its clip, so clips reached
// during playback are ready before they begin. A block that has to play
// silence because the disk could not keep up is counted (take_underruns()).
//
// Offline rendering (bounces, stems, the checks) runs synchronous: the
// reading thread fills the buffer itself before it reads, so a print never
// drops audio and is identical however fast the disk is.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include <juce_audio_formats/juce_audio_formats.h>

namespace vcc::desktop {

class DiskStreamer;

/// One clip's view of one file, at the engine's sample rate.
class ClipStream {
public:
    ~ClipStream();
    ClipStream(const ClipStream&) = delete;
    ClipStream& operator=(const ClipStream&) = delete;

    /// Audio thread: adds `n` samples of the clip from clip position `k`
    /// (0 = the clip's first sample) times `gains[i]` into l/r. Returns false
    /// when it had to play silence (the buffer was not there yet).
    bool read(std::int64_t k, int n, const float* gains, float* l, float* r, bool mono_sum) noexcept;
    /// Audio thread, while stopped: positions the stream so a play from clip
    /// position `k` (clamped to the clip) finds its audio ready.
    void cue(std::int64_t k) noexcept;

    std::int64_t length() const noexcept { return length_; }
    int channels() const noexcept { return channels_; }

private:
    friend class DiskStreamer;
    ClipStream() = default;
    /// Disk thread (or the reading thread, when synchronous): serves a seek
    /// and tops the buffer up. Returns true if it did any work.
    bool service();
    bool fill_some(int max_frames);
    void reset_reader(std::int64_t k);

    DiskStreamer* owner_ = nullptr;
    std::unique_ptr<juce::AudioFormatReader> reader_;
    std::int64_t offset_ = 0;  ///< Clip start in the file, engine samples.
    std::int64_t length_ = 0;  ///< Engine samples.
    std::int64_t file_frames_ = 0;
    double ratio_ = 1.0;       ///< File samples per engine sample.
    int channels_ = 1;

    // The ring: frames in [read, write) are clip positions [start_k_, ...).
    juce::AbstractFifo fifo_{1};
    juce::AudioBuffer<float> ring_;
    // Seek protocol: the reader raises `requested_`; the filler, having
    // refilled from `seek_k_`, publishes `ready_ = requested_`.
    std::atomic<std::uint32_t> requested_{0};
    std::atomic<std::uint32_t> ready_{0};
    std::atomic<std::int64_t> seek_k_{0};
    std::int64_t next_k_ = 0;  ///< Reader: clip position at the front of the ring.

    // Filler state (disk thread only).
    std::uint32_t serving_ = 0;
    std::int64_t fill_k_ = 0;       ///< Clip position of the next frame written.
    double source_pos_ = 0.0;       ///< File frame the interpolator reads next.
    juce::AudioBuffer<float> staging_;  ///< File-rate frames for one fill step.
    juce::AudioBuffer<float> converted_;
    juce::LagrangeInterpolator interp_[2];
};

class DiskStreamer : private juce::TimeSliceClient {
public:
    DiskStreamer();
    ~DiskStreamer() override;

    /// Opens a clip on a file for an engine running at `sample_rate`.
    /// `offset` and `length` are in engine samples. Null with `error` set
    /// ("Missing audio file: ...", "Unreadable audio file: ...").
    std::shared_ptr<ClipStream> open(const std::string& path, double sample_rate, std::int64_t offset,
                                     std::int64_t length, juce::String& error);

    /// Offline mode: readers fill their own buffers and no thread runs.
    void set_synchronous(bool on);
    bool synchronous() const noexcept { return synchronous_.load(); }

    /// Blocks where a stream that was in place ran dry (the disk fell
    /// behind) since the last call. The short silence right after a jump
    /// while playing is not counted.
    int take_underruns() noexcept { return underruns_.exchange(0); }
    /// Seconds of read-ahead per clip.
    static constexpr double kReadAheadSeconds = 0.75;

private:
    friend class ClipStream;
    int useTimeSlice() override;
    void detach(ClipStream* stream);

    juce::AudioFormatManager formats_;
    juce::TimeSliceThread thread_{"VCC disk streaming"};
    std::mutex streams_lock_;  ///< Message thread vs disk thread only; never the audio thread.
    std::set<ClipStream*> streams_;
    std::atomic<bool> synchronous_{false};
    std::atomic<int> underruns_{0};
};

}  // namespace vcc::desktop
