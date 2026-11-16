// Picture + score export: a movie written by VideoEncoder decodes back with
// its size, fractional rate, frame count, source timecode, audio, and each
// frame's picture (brightness encodes the frame number).
#include <cmath>
#include <cstdio>
#include <vector>

#include "vcc/media/VideoDecoder.h"
#include "vcc/media/VideoEncoder.h"
#include "vcc_test.h"

using namespace vcc;

namespace {

int level(std::int64_t n) { return static_cast<int>(30 + 4 * n); }

media::Frame grey(int w, int h, int v)
{
    media::Frame f;
    f.width = w;
    f.height = h;
    f.rgba.assign(static_cast<std::size_t>(w * h * 4), static_cast<std::uint8_t>(v));
    for (std::size_t i = 3; i < f.rgba.size(); i += 4) f.rgba[i] = 255;
    return f;
}

bool write_movie(const std::string& path, std::string& codec)
{
    media::VideoEncoder enc;
    media::EncodeSettings s;
    s.width = 129;  // rounded down to even
    s.height = 96;
    s.fps_num = 24000;
    s.fps_den = 1001;
    s.timecode = "01:00:00:00";
    std::string error;
    if (!enc.open(path, s, &error)) {
        std::fprintf(stderr, "    open: %s\n", error.c_str());
        return false;
    }
    codec = enc.video_codec();
    const int per_frame = 48000 * 1001 / 24000;  // ~2002 samples
    std::vector<float> audio(static_cast<std::size_t>(per_frame * 2));
    std::int64_t t = 0;
    for (std::int64_t n = 0; n < 48; ++n) {
        if (!enc.add_frame(grey(128, 96, level(n)))) return false;
        for (int i = 0; i < per_frame; ++i, ++t) {
            const float v = 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 1000.0 * static_cast<double>(t) / 48000.0));
            audio[static_cast<std::size_t>(i * 2)] = v;
            audio[static_cast<std::size_t>(i * 2 + 1)] = v;
        }
        if (!enc.add_audio(audio.data(), per_frame)) return false;
    }
    if (enc.frames_written() != 48) return false;
    if (!enc.finish(&error)) {
        std::fprintf(stderr, "    finish: %s\n", error.c_str());
        return false;
    }
    return true;
}

}  // namespace

TEST_CASE("an exported movie decodes back frame for frame, with timecode and sound")
{
    for (const char* ext : {".mov", ".mp4"}) {
        const std::string path = std::string(VCC_TEST_TMP_DIR) + "/export" + ext;
        std::remove(path.c_str());
        std::string codec;
        REQUIRE(write_movie(path, codec));
        // Licence gate: never a GPL encoder.
        CHECK(codec.find("x264") == std::string::npos && codec.find("x265") == std::string::npos);
        media::VideoDecoder d;
        std::string err;
        REQUIRE(d.open(path, &err));
        const auto& i = d.info();
        CHECK_EQ(i.width, 128);
        CHECK_EQ(i.height, 96);
        CHECK_EQ(i.fps_num, 24000);
        CHECK_EQ(i.fps_den, 1001);
        CHECK_EQ(i.frame_count, 48);
        CHECK_EQ(i.timecode, std::string("01:00:00:00"));
        CHECK(i.has_audio);
        for (const std::int64_t n : {0, 1, 17, 30, 47}) {
            media::Frame f;
            REQUIRE(d.frame(n, f));
            const int got = f.rgba[(f.rgba.size() / 8) * 4];
            CHECK(std::abs(got - level(n)) <= 6);
            if (std::abs(got - level(n)) > 6) std::fprintf(stderr, "    %s frame %lld: %d, wanted %d\n", ext, static_cast<long long>(n), got, level(n));
        }
    }
}
