// Picture + score export (Main Spec §13 "video export option: picture +
// stereo score draft for director review"): writes a movie from RGBA frames
// and interleaved stereo audio.
//
// Licence gate (docs/01): only encoders that ship in an LGPL FFmpeg are
// used, never libx264/libx265. In order of preference: the platform's own
// H.264 (VideoToolbox on macOS, Media Foundation on Windows), then FFmpeg's
// native MPEG-4 Part 2 at a high quality. Audio is FFmpeg's native AAC. The
// movie carries a timecode track (.mov and .mp4).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "vcc/media/VideoDecoder.h"

namespace vcc::media {

/// True when this build can write movies.
bool encoding_available() noexcept;

struct EncodeSettings {
    int width = 1280;   ///< Rounded down to even.
    int height = 720;   ///< Rounded down to even.
    int fps_num = 24, fps_den = 1;
    int sample_rate = 48000;
    int channels = 2;
    std::string timecode;  ///< Of the first frame ("01:00:00:00" or "...;00"); empty for none.
    std::int64_t video_bitrate = 8'000'000;
};

class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    /// The container follows the extension (.mov or .mp4).
    bool open(const std::string& path, const EncodeSettings& settings, std::string* error = nullptr);
    /// The next frame: RGBA, `settings.width` x `settings.height` (any other
    /// size is scaled).
    bool add_frame(const Frame& frame);
    /// `frames` sample frames of interleaved audio (`settings.channels` per frame).
    bool add_audio(const float* interleaved, int frames);
    /// Flushes and closes the file. False (reason in `error`) when anything failed.
    bool finish(std::string* error = nullptr);
    /// "h264_videotoolbox", "mpeg4"...; empty before open.
    const std::string& video_codec() const noexcept;
    std::int64_t frames_written() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vcc::media
