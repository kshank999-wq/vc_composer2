// Frame-accurate decoding, checked against a generated movie whose
// brightness encodes its frame number (frame N has luma (7N mod 256)), so a
// neighbouring frame cannot pass for the right one.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>

#include "vcc/media/VideoDecoder.h"
#include "vcc/picture/Picture.h"
#include "vcc_test.h"

using namespace vcc;

namespace {

std::string movie()
{
    static std::string path = [] {
        std::string p = std::string(VCC_TEST_TMP_DIR) + "/frames.mov";
        std::string cmd = std::string("\"") + VCC_FFMPEG_TOOL +
                          "\" -hide_banner -loglevel error -y -f lavfi -i "
                          "\"nullsrc=s=64x48:r=24000/1001:d=3,geq=lum='mod(N*7\\,256)':cb=128:cr=128\" "
                          "-c:v png -pix_fmt rgb24 -timecode 00:59:58:00 \"" + p + "\"";
        if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "    could not generate the test movie\n");
        return p;
    }();
    return path;
}

int luma(const media::Frame& f) { return f.rgba.empty() ? -1 : f.rgba[(f.rgba.size() / 8) * 4]; }
/// The RGB level of frame `index`: luma (7N mod 256) generated as limited-range
/// video, expanded to full range on decode ((Y - 16) * 255 / 219, clipped).
int expected(std::int64_t index)
{
    const double y = static_cast<double>((index * 7) % 256);
    return static_cast<int>(std::lround(std::clamp((y - 16.0) * 255.0 / 219.0, 0.0, 255.0)));
}

/// Brightness identifies a frame only where it is not clipped.
bool matches(const media::Frame& f, std::int64_t index)
{
    const int e = expected(index);
    return e <= 0 || e >= 255 || std::abs(luma(f) - e) <= 2;
}

}  // namespace

TEST_CASE("probe reads size, fractional rate, frame count and source timecode")
{
    media::VideoDecoder d;
    std::string err;
    REQUIRE(d.open(movie(), &err));
    const auto& i = d.info();
    CHECK_EQ(i.width, 64);
    CHECK_EQ(i.height, 48);
    CHECK_EQ(i.fps_num, 24000);
    CHECK_EQ(i.fps_den, 1001);
    CHECK_EQ(i.frame_count, 72);
    CHECK_EQ(i.timecode, std::string("00:59:58:00"));
    CHECK(!d.open(std::string(VCC_TEST_TMP_DIR) + "/missing.mov", &err));
    CHECK(!err.empty());
}

TEST_CASE("every frame decoded in order is the right frame")
{
    media::VideoDecoder d;
    REQUIRE(d.open(movie()));
    media::Frame f;
    for (std::int64_t n = 0; n < 72; ++n) {
        REQUIRE(d.frame(n, f));
        CHECK_EQ(f.index, n);
        CHECK(matches(f, n));
    }
    CHECK(!d.frame(72, f));
}

TEST_CASE("time lookups land on the frame whose interval contains the time, across jumps")
{
    media::VideoDecoder d;
    REQUIRE(d.open(movie()));
    const double fps = d.info().fps;
    media::Frame f;
    // Forwards, backwards, long jumps, and times just inside a frame's interval.
    for (std::int64_t n : {10LL, 11LL, 5LL, 70LL, 0LL, 69LL, 33LL, 34LL, 2LL}) {
        const double t = static_cast<double>(n) / fps + 0.25 / fps;
        REQUIRE(d.frame_at(t, f));
        CHECK_EQ(f.index, n);
        CHECK(matches(f, n));
    }
    // Just before a boundary is still the earlier frame.
    REQUIRE(d.frame_at(20.0 / fps - 1e-4, f));
    CHECK_EQ(f.index, 19);
    CHECK(!d.frame_at(-0.01, f));
    CHECK(!d.frame_at(10.0, f));
}

TEST_CASE("frames scale to a display width and keep their aspect")
{
    media::VideoDecoder d;
    REQUIRE(d.open(movie()));
    media::Frame f;
    REQUIRE(d.frame(3, f, 32));
    CHECK_EQ(f.width, 32);
    CHECK_EQ(f.height, 24);
    CHECK_EQ(f.rgba.size(), 32u * 24u * 4u);
}

TEST_CASE("signatures tell different frames apart and a cut compared with itself is unchanged")
{
    media::VideoDecoder d;
    REQUIRE(d.open(movie()));
    auto a = d.signature(1), b = d.signature(20);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(*a != *b);
    std::vector<std::uint64_t> sigs;
    for (std::int64_t n = 0; n < 72; n += 24) sigs.push_back(*d.signature(n));
    CHECK(picture::changed_regions(sigs, sigs, 1.0, 0.0).empty());
}

TEST_CASE("the movie's own timecode places its frames on the project timeline")
{
    media::VideoDecoder d;
    REQUIRE(d.open(movie()));
    project::ProjectData data;
    data.frame_rate = FrameRate::Fps23_976;
    data.start_timecode = "01:00:00:00";
    project::PictureVersion p;
    p.rate = FrameRate::Fps23_976;
    p.start_timecode = d.info().timecode;
    // Two seconds of leader: frame 48 is the first frame of the programme.
    CHECK_NEAR(picture::project_seconds_for_frame(data, p, 48), 0.0, 1e-9);
    media::Frame f;
    REQUIRE(d.frame_at(picture::picture_seconds_at(data, p, 0.0) + 1e-6, f));
    CHECK_EQ(f.index, 48);
}
