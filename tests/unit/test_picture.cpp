#include <bitset>

#include "vcc/picture/Picture.h"
#include "vcc_test.h"

using namespace vcc;
using namespace vcc::picture;

static project::ProjectData data(FrameRate rate = FrameRate::Fps24, const char* start = "01:00:00:00")
{
    project::ProjectData d;
    d.frame_rate = rate;
    d.start_timecode = start;
    d.tempo = TempoMap(120.0);
    return d;
}

TEST_CASE("timecode maps to project time from the project's start timecode")
{
    auto d = data();
    CHECK_NEAR(*project_seconds_for_timecode(d, "01:00:00:00"), 0.0, 1e-9);
    CHECK_NEAR(*project_seconds_for_timecode(d, "01:00:10:12"), 10.5, 1e-9);
    CHECK_NEAR(*project_seconds_for_timecode(d, "00:59:58:00"), -2.0, 1e-9);
    CHECK_EQ(project_timecode(d, 10.5), std::string("01:00:10:12"));
    CHECK(!project_seconds_for_timecode(d, "nonsense"));
}

TEST_CASE("23.976 picture with a two-second leader lines up frame for frame")
{
    auto d = data(FrameRate::Fps23_976);
    project::PictureVersion p;
    p.rate = FrameRate::Fps23_976;
    p.start_timecode = "00:59:58:00";  // leader before the first frame of action
    CHECK_NEAR(project_seconds_for_frame(d, p, 0), -2.0 * 1001.0 / 1000.0, 1e-9);
    // Frame 48 is 01:00:00:00, project time zero.
    CHECK_NEAR(project_seconds_for_frame(d, p, 48), 0.0, 1e-9);
    CHECK_EQ(frame_at(d, p, 0.0), 48);
    for (std::int64_t f : {0LL, 1LL, 47LL, 48LL, 1000LL, 86399LL})
        CHECK_EQ(frame_at(d, p, project_seconds_for_frame(d, p, f)), f);
    CHECK(!rate_mismatch(d, p));
    p.offset_seconds = 1.0;
    CHECK_NEAR(project_seconds_for_frame(d, p, 48), 1.0, 1e-9);
    project::PictureVersion q;
    q.rate = FrameRate::Fps25;
    CHECK(rate_mismatch(d, q));
}

TEST_CASE("a picture event is located in the music, with its offset from the nearest beat")
{
    auto d = data();
    // 120 bpm 4/4: beats every 0.5 s. A hit at 2.26 s is in bar 2, nearest
    // to beat 2 (2.5 s), 240 ms early.
    auto m = locate_in_music(d, 2.26);
    CHECK_EQ(m.bar, 2);
    CHECK_EQ(m.nearest_beat_tick, kTicksPerWhole + kTicksPerQuarter);
    CHECK_NEAR(m.offset_ms, -240.0, 1e-6);
    auto n = locate_in_music(d, 2.01);
    CHECK_NEAR(n.offset_ms, 10.0, 1e-6);
    CHECK_NEAR(n.offset_frames, 0.24, 1e-6);
}

TEST_CASE("tempo fit lands a chosen beat exactly on a hit and leaves the rest alone")
{
    TempoMap tempo(120.0);
    tempo.set(8 * kTicksPerWhole, 90.0);  // a later change that must survive
    // Put bar 5 beat 1 (tick 4 whole notes) on a hit at 8.4 s, fitting from bar 2.
    std::string err;
    auto fitted = fit_tempo_to_hit(tempo, kTicksPerWhole, 4 * kTicksPerWhole, 8.4, &err);
    REQUIRE(fitted);
    CHECK_NEAR(fitted->seconds_at(4 * kTicksPerWhole), 8.4, 1e-9);
    CHECK_NEAR(fitted->seconds_at(kTicksPerWhole), 2.0, 1e-9);   // untouched before
    CHECK_NEAR(fitted->bpm_at(5 * kTicksPerWhole), 120.0, 1e-9);  // original tempo carries on
    CHECK_NEAR(fitted->bpm_at(8 * kTicksPerWhole), 90.0, 1e-9);   // later change kept
    CHECK(!fit_tempo_to_hit(tempo, kTicksPerWhole, 4 * kTicksPerWhole, 2.1, &err));  // would need > 400 bpm
    CHECK(err.find("bpm") != std::string::npos);
    CHECK(!fit_tempo_to_hit(tempo, 4 * kTicksPerWhole, kTicksPerWhole, 8.0, &err));
}

TEST_CASE("streamers sweep into the hit and the punch follows it")
{
    project::Marker m;
    m.seconds = 10.0;
    m.streamer_seconds = 2.0;
    CHECK(!streamer_progress(m, 7.9));
    CHECK_NEAR(*streamer_progress(m, 8.0), 0.0, 1e-9);
    CHECK_NEAR(*streamer_progress(m, 9.5), 0.75, 1e-9);
    CHECK(!streamer_progress(m, 10.0));
    CHECK(punch_visible(m, 10.0));
    CHECK(punch_visible(m, 10.05));
    CHECK(!punch_visible(m, 10.2));
}

TEST_CASE("changed regions between cuts are found and merged")
{
    std::vector<std::uint64_t> a(20, 0x0F0F0F0F0F0F0F0FULL), b = a;
    for (int i = 5; i < 8; ++i) b[i] = ~a[i];
    b[9] = ~a[9];                 // one step apart: merged with the first
    b.push_back(0);               // the new cut is longer
    b[3] ^= 0x7;                  // tiny difference: not a change
    auto r = changed_regions(a, b, 1.0, 100.0);
    REQUIRE(r.size() == 2u);
    CHECK_NEAR(r[0].start_seconds, 105.0, 1e-9);
    CHECK_NEAR(r[0].end_seconds, 110.0, 1e-9);
    CHECK_NEAR(r[1].start_seconds, 120.0, 1e-9);
    CHECK_EQ(r[1].note, std::string("length differs"));
    CHECK(changed_regions(a, a, 1.0, 0.0).empty());
}
