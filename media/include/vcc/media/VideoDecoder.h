// Frame-accurate picture decoding for scoring to picture (Main Spec §13).
//
// Built on FFmpeg when it is available at build time (VCC_WITH_FFMPEG);
// otherwise a stub that opens nothing and says why. Shipping builds must link
// an LGPL-configured FFmpeg dynamically (docs/01, licence gates).
//
// Access is optimised for scoring: playback asks for consecutive frames, so
// the decoder reads forward and only seeks for a real jump (backwards, or
// more than two seconds ahead). Every answer is the frame whose display
// interval contains the requested time - never "a frame near it".
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vcc::media {

/// True when this build can decode video.
bool available() noexcept;

struct VideoInfo {
    int width = 0;
    int height = 0;
    double fps = 0.0;                 ///< Real frames per second (23.976... = 24000/1001).
    int fps_num = 0, fps_den = 1;
    std::int64_t frame_count = 0;
    double duration_seconds = 0.0;
    std::string timecode;             ///< Source timecode of the first frame, if the file carries one.
    std::string codec;
    bool has_audio = false;
};

struct Frame {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;   ///< width*height*4 bytes, row-major, R G B A.
    std::int64_t index = -1;          ///< Frame number from the first frame.
};

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool open(const std::string& path, std::string* error = nullptr);
    void close();
    bool is_open() const noexcept;
    const VideoInfo& info() const noexcept;

    /// The frame shown at `seconds` from the first frame, scaled to fit
    /// `max_width` (0 = native size). False before the first or after the
    /// last frame, or on a decode error.
    bool frame_at(double seconds, Frame& out, int max_width = 0);

    /// The frame with this index (0-based).
    bool frame(std::int64_t index, Frame& out, int max_width = 0);

    /// A 64-bit average hash of a frame, for comparing cuts.
    std::optional<std::uint64_t> signature(std::int64_t index);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vcc::media
