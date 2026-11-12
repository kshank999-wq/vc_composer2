#include "vcc/core/Time.h"
#include "vcc_test.h"

using namespace vcc;

TEST_CASE("time signatures parse and give bar lengths")
{
    auto s = TimeSignature::parse("7/8");
    REQUIRE(s);
    CHECK_EQ(s->ticks_per_bar(), 7 * kTicksPerQuarter / 2);
    CHECK(!TimeSignature::parse("4/3"));
    CHECK(!TimeSignature::parse("x/4"));
    CHECK(TimeSignature::parse("12/8")->is_compound());
    auto g = TimeSignature{7, 8}.default_grouping();
    CHECK_EQ(g.size(), 3u);
    CHECK_EQ(g[2], 3);
}

TEST_CASE("meter map does bar arithmetic across changes")
{
    MeterMap m(TimeSignature{4, 4});
    m.set(3, TimeSignature{3, 4});
    m.set(5, TimeSignature{7, 8});
    CHECK_EQ(m.bar_start(1), 0);
    CHECK_EQ(m.bar_start(3), 2 * kTicksPerWhole);
    CHECK_EQ(m.bar_start(5), 2 * kTicksPerWhole + 2 * 3 * kTicksPerQuarter);
    CHECK_EQ(m.bar_at(m.bar_start(5)), 5);
    CHECK_EQ(m.bar_at(m.bar_start(5) - 1), 4);
    CHECK_EQ(m.bar_at(m.bar_start(9) + 10), 9);
    CHECK_EQ(m.span(5, 16), 16 * 7 * kTicksPerQuarter / 2);
    CHECK(m.at_bar(4) == (TimeSignature{3, 4}));
}

TEST_CASE("tempo map converts ticks and seconds both ways")
{
    TempoMap t(120.0);
    CHECK_NEAR(t.seconds_at(kTicksPerQuarter), 0.5, 1e-12);
    t.set(4 * kTicksPerQuarter, 60.0);
    CHECK_NEAR(t.seconds_at(5 * kTicksPerQuarter), 3.0, 1e-12);
    CHECK_EQ(t.tick_at_seconds(3.0), 5 * kTicksPerQuarter);
    CHECK_EQ(t.tick_at_seconds(1.0), 2 * kTicksPerQuarter);
}

TEST_CASE("tempo changes do not move bar positions")
{
    MeterMap m;
    TempoMap a(90.0), b(140.0);
    // Bars are a pure function of the meter map.
    CHECK_EQ(m.bar_start(9), 8 * kTicksPerWhole);
    CHECK(a.seconds_at(m.bar_start(9)) != b.seconds_at(m.bar_start(9)));
}

TEST_CASE("non-drop timecode round trips")
{
    for (FrameRate r : {FrameRate::Fps24, FrameRate::Fps23_976, FrameRate::Fps25, FrameRate::Fps30}) {
        for (std::int64_t f : {0LL, 1LL, 23LL, 86399LL, 216000LL, 1234567LL}) {
            auto tc = frames_to_timecode(f, r);
            auto back = timecode_to_frames(tc, r);
            REQUIRE(back);
            CHECK_EQ(*back, f);
        }
    }
    CHECK_EQ(frames_to_timecode(24 * 3600, FrameRate::Fps24).to_string(FrameRate::Fps24),
             std::string("01:00:00:00"));
}

TEST_CASE("29.97 drop frame skips labels and round trips")
{
    const FrameRate df = FrameRate::Fps29_97_Drop;
    // The frame after 00:00:59;29 is 00:01:00;02.
    auto tc = frames_to_timecode(1800, df);
    CHECK_EQ(tc.to_string(df), std::string("00:01:00;02"));
    // Tenth minutes keep 00 and 01.
    CHECK_EQ(frames_to_timecode(17982, df).to_string(df), std::string("00:10:00;00"));
    // One hour of drop frame is 107892 frames.
    CHECK_EQ(*timecode_to_frames(*Timecode::parse("01:00:00;00"), df), 107892);
    CHECK(!timecode_to_frames(*Timecode::parse("00:01:00;00"), df));
    for (std::int64_t f = 0; f < 40000; f += 7) {
        auto back = timecode_to_frames(frames_to_timecode(f, df), df);
        REQUIRE(back);
        CHECK_EQ(*back, f);
    }
    // Drop frame keeps timecode within a frame of wall-clock over an hour.
    double wall = frames_to_seconds(107892, df);
    CHECK_NEAR(wall, 3600.0, 1.0 / 29.97 * 4);
}

TEST_CASE("59.94 drop frame drops four labels")
{
    const FrameRate df = FrameRate::Fps59_94_Drop;
    CHECK_EQ(frames_to_timecode(3600, df).to_string(df), std::string("00:01:00;04"));
    CHECK_EQ(*timecode_to_frames(*Timecode::parse("01:00:00;00"), df), 215784);
}

TEST_CASE("frame rates parse by name")
{
    CHECK(frame_rate_from_string("23.976") == FrameRate::Fps23_976);
    CHECK(frame_rate_from_string("29.97df") == FrameRate::Fps29_97_Drop);
    CHECK(!frame_rate_from_string("31"));
    CHECK_EQ(nominal_fps(FrameRate::Fps23_976), 24);
    CHECK_EQ(seconds_to_frames(frames_to_seconds(1000, FrameRate::Fps23_976), FrameRate::Fps23_976), 1000);
}
