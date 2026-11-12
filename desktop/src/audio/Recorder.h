// One recording pass (Main Spec §11): a 24-bit WAV per record-armed track,
// written through juce::AudioFormatWriter::ThreadedWriter so the audio
// thread only copies into a lock-free FIFO; a background TimeSliceThread
// does the disk writes.
//
// Life of a pass: opened on the message thread (files created), handed to
// the audio thread, which writes the armed inputs while the transport is
// inside the record range, then handed back (stop, punch out, locate) to the
// message thread, which closes the files and places the clips.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>

namespace vcc::desktop {

struct RecordTake {
    std::string track_id;
    int input = 0;     ///< First device input channel.
    int channels = 1;  ///< 1 or 2.
    juce::File file;
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
    // Audio thread while the pass is active; message thread after hand-back.
    std::int64_t start_sample = -1;  ///< Timeline sample of the first recorded sample.
    std::int64_t samples = 0;        ///< Written.
    std::int64_t dropped = 0;        ///< Lost: the disk writer could not keep up.
};

struct RecordPass {
    std::uint64_t id = 0;
    std::vector<RecordTake> takes;
    double sample_rate = 48000.0;
    bool punch = false;
    std::int64_t punch_in = 0, punch_out = 0;  ///< Timeline samples.
    int latency = 0;                           ///< Device input + output latency, samples.
    juce::AudioBuffer<float> silence;          ///< For inputs the device does not have.
    std::int64_t recorded = 0;                 ///< Audio thread.

    /// Audio thread: writes `count` samples of the inputs (already offset to
    /// the first sample to write) for timeline sample `at`.
    void write(const float* const* inputs, int num_inputs, int input_offset, int count, std::int64_t at) noexcept;
};

/// "<name>_NN.wav" in `folder`, the first that does not exist yet.
juce::File unique_take_file(const juce::File& folder, const std::string& track_name);

/// Opens the WAV writer for a take. Returns an empty string or why not.
juce::String open_take(RecordTake& take, double sample_rate, juce::TimeSliceThread& thread, int fifo_samples);

}  // namespace vcc::desktop
