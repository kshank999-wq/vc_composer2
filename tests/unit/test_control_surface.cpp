#include "vcc/project/ControlSurface.h"
#include "vcc_test.h"

using namespace vcc;
using namespace vcc::project::surface;

namespace {

const Profile& generic() { return builtin_profiles()[1]; }
const Profile& mackie() { return builtin_profiles()[0]; }

struct Fixture {
    TempoMap tempo{120.0};  // a beat (quarter) is 0.5 s, a 4/4 bar 2 s
    MeterMap meter{TimeSignature{4, 4}};
    Timeline t;
    Fixture()
    {
        t.tempo = &tempo;
        t.meter = &meter;
        t.frames_per_second = 24.0;
        t.end_seconds = 60.0;
        t.markers = {4.0, 10.0, 22.5};
    }
};

}  // namespace

TEST_CASE("control surface: relative encodings and the shuttle decode as they are sent")
{
    // Generic: jog CC 16 two's complement, shuttle CC 17 absolute (64 = rest).
    auto e = decode(generic(), 0xB0, 16, 1);
    REQUIRE(e);
    CHECK(e->action == Action::Jog);
    CHECK_EQ(e->steps, 1);
    CHECK_EQ(decode(generic(), 0xB0, 16, 127)->steps, -1);
    CHECK_EQ(decode(generic(), 0xB0, 16, 125)->steps, -3);
    CHECK(!decode(generic(), 0xB0, 16, 0));  // at rest: nothing
    // Mackie: CC 60 sign-magnitude; 0x41 is one step back.
    CHECK_EQ(decode(mackie(), 0xB0, 60, 0x01)->steps, 1);
    CHECK_EQ(decode(mackie(), 0xB0, 60, 0x41)->steps, -1);
    CHECK_EQ(decode(mackie(), 0xB3, 60, 0x45)->steps, -5);  // any channel
    // Binary offset.
    Profile p{"t", {{Action::Jog, {Source::Kind::Controller, 2, 10}, Encoding::BinaryOffset}}};
    CHECK_EQ(decode(p, 0xB1, 10, 66)->steps, 2);
    CHECK_EQ(decode(p, 0xB1, 10, 61)->steps, -3);
    CHECK(!decode(p, 0xB0, 10, 66));  // channel 2 only
    // Shuttle: centre, full forward, full back.
    CHECK_NEAR(decode(generic(), 0xB0, 17, 64)->value, 0.0, 1e-9);
    CHECK_NEAR(decode(generic(), 0xB0, 17, 127)->value, 1.0, 1e-9);
    CHECK_NEAR(decode(generic(), 0xB0, 17, 0)->value, -1.0, 1e-9);
    Profile bend{"b", {{Action::Shuttle, {Source::Kind::PitchBend, 0, 0}, Encoding::Absolute}}};
    CHECK_NEAR(decode(bend, 0xE0, 0, 64)->value, 0.0, 1e-9);
    CHECK_NEAR(decode(bend, 0xE0, 127, 127)->value, 1.0, 1e-9);
}

TEST_CASE("control surface: buttons act on press only, and a shared device keeps its other messages")
{
    auto play = decode(mackie(), 0x90, 94, 127);
    REQUIRE(play);
    CHECK(play->action == Action::Play);
    CHECK(play->pressed);
    CHECK(!decode(mackie(), 0x90, 94, 0));  // note-on velocity 0 is a release
    CHECK(!decode(mackie(), 0x80, 94, 64));
    CHECK(decode(generic(), 0xB0, 18, 127)->action == Action::PlayPause);
    CHECK(!decode(generic(), 0xB0, 18, 0));
    // A keyboard's notes are not the surface's: they are left to play.
    CHECK(claims(mackie(), 0x90, 94, 127));
    CHECK(claims(mackie(), 0x80, 94, 0));  // its releases too
    CHECK(!claims(mackie(), 0x90, 60, 100));
    CHECK(!claims(generic(), 0xB0, 1, 40));  // the mod wheel
    CHECK(!decode(generic(), 0xB0, 1, 40));
    // Learning: what a message is.
    auto s = source_of(0xB2, 33, 1);
    REQUIRE(s);
    CHECK(s->kind == Source::Kind::Controller);
    CHECK_EQ(s->channel, 3);
    CHECK_EQ(s->number, 33);
    CHECK_EQ(describe(*s), std::string("CC 33 ch 3"));
    CHECK(!source_of(0x90, 60, 0));
    for (auto a : all_actions()) CHECK(action_from_string(to_string(a)) == a);
}

TEST_CASE("control surface: the jog steps onto the grid, in beats, bars, sixteenths and frames")
{
    Fixture f;
    JogShuttle j;
    j.settings.acceleration = false;
    j.settings.step = Step::Beat;
    CHECK_NEAR(j.jog(0.0, 1, 0.0, f.t), 0.5, 1e-9);
    CHECK_NEAR(j.jog(0.5, 3, 1.0, f.t), 2.0, 1e-9);
    // Between beats: one step lands on the next beat, not a beat further.
    CHECK_NEAR(j.jog(0.7, 1, 2.0, f.t), 1.0, 1e-9);
    CHECK_NEAR(j.jog(0.7, -1, 3.0, f.t), 0.5, 1e-9);
    CHECK_NEAR(j.jog(1.0, -1, 4.0, f.t), 0.5, 1e-9);
    CHECK_NEAR(j.jog(0.2, -5, 5.0, f.t), 0.0, 1e-9);  // never before the start
    j.settings.step = Step::Bar;
    CHECK_NEAR(j.jog(0.0, 1, 6.0, f.t), 2.0, 1e-9);
    CHECK_NEAR(j.jog(3.0, -1, 7.0, f.t), 2.0, 1e-9);
    CHECK_NEAR(j.jog(2.0, -1, 8.0, f.t), 0.0, 1e-9);
    j.settings.step = Step::Sixteenth;
    CHECK_NEAR(j.jog(0.0, 2, 9.0, f.t), 0.25, 1e-9);
    j.settings.step = Step::Frame;
    CHECK_NEAR(j.jog(0.0, 1, 10.0, f.t), 1.0 / 24.0, 1e-9);
    CHECK_NEAR(j.jog(1.0, -24, 11.0, f.t), 0.0, 1e-9);
    CHECK_NEAR(j.jog(0.51, 1, 12.0, f.t), 13.0 / 24.0, 1e-9);  // onto the frame grid
    // Across a meter change: 3/4 from bar 3 (at 4 s), beats stay quarters,
    // bars are three beats long.
    f.meter.set(3, TimeSignature{3, 4});
    j.settings.step = Step::Bar;
    CHECK_NEAR(j.jog(4.0, 1, 13.0, f.t), 5.5, 1e-9);
    CHECK_NEAR(j.jog(5.5, -2, 14.0, f.t), 2.0, 1e-9);
    // 6/8 counts eighths.
    f.meter.set(3, TimeSignature{6, 8});
    j.settings.step = Step::Beat;
    CHECK_NEAR(j.jog(4.0, 1, 15.0, f.t), 4.25, 1e-9);
}

TEST_CASE("control surface: sensitivity, reverse and acceleration")
{
    Fixture f;
    JogShuttle j;
    j.settings.acceleration = false;
    j.settings.detents_per_step = 3;
    double at = 0.0;
    at = j.jog(at, 1, 0.0, f.t);
    at = j.jog(at, 1, 0.5, f.t);
    CHECK_NEAR(at, 0.0, 1e-9);
    CHECK_EQ(j.pending_detents(), 2);
    at = j.jog(at, 1, 1.0, f.t);
    CHECK_NEAR(at, 0.5, 1e-9);
    CHECK_EQ(j.pending_detents(), 0);
    j.jog(at, 2, 1.5, f.t);
    CHECK_EQ(j.jog(at, -1, 2.0, f.t), at);  // a turn the other way starts afresh
    CHECK_EQ(j.pending_detents(), -1);
    JogShuttle r;
    r.settings.acceleration = false;
    r.settings.reverse = true;
    CHECK_NEAR(r.jog(1.0, 1, 0.0, f.t), 0.5, 1e-9);
    // A fast spin moves further than the same detents turned slowly.
    JogShuttle slow, fast;
    double s = 0.0, q = 0.0;
    for (int i = 0; i < 10; ++i) {
        s = slow.jog(s, 1, i * 0.5, f.t);
        q = fast.jog(q, 1, i * 0.01, f.t);
    }
    CHECK_NEAR(s, 5.0, 1e-9);
    CHECK(q > s * 2.0);
    CHECK(q <= 20.0 + 1e-9);  // at most 4x
}

TEST_CASE("control surface: the shuttle has a dead zone, a curve, a top speed and bounds")
{
    Fixture f;
    JogShuttle j;
    j.settings.dead_zone = 0.1;
    j.settings.max_speed = 8.0;
    j.set_shuttle(0.05);
    CHECK_EQ(j.shuttle_speed(), 0.0);
    CHECK(!j.shuttling());
    CHECK_EQ(j.advance(3.0, 0.1, f.t), 3.0);
    j.set_shuttle(1.0);
    CHECK_NEAR(j.shuttle_speed(), 8.0, 1e-9);
    j.set_shuttle(-1.0);
    CHECK_NEAR(j.shuttle_speed(), -8.0, 1e-9);
    // The curve: half a turn is slower than half speed, and speed rises
    // with the turn.
    j.set_shuttle(0.55);
    const double half = j.shuttle_speed();
    CHECK(half > 0.1);
    CHECK(half < 4.0);
    double prev = 0.0;
    for (int i = 2; i <= 20; ++i) {
        j.set_shuttle(i / 20.0);
        CHECK(j.shuttle_speed() >= prev);
        prev = j.shuttle_speed();
    }
    j.settings.curve = Curve::Linear;
    j.set_shuttle(0.55);
    CHECK_NEAR(j.shuttle_speed(), 0.1 + 7.9 * 0.5, 1e-9);
    // Travel: speed times time, held within the project; a stalled clock
    // (a long gap between calls) cannot make it leap.
    j.set_shuttle(1.0);
    CHECK_NEAR(j.advance(10.0, 0.1, f.t), 10.8, 1e-9);
    CHECK_NEAR(j.advance(10.0, 5.0, f.t), 12.0, 1e-9);
    CHECK_NEAR(j.advance(59.9, 0.1, f.t), 60.0, 1e-9);
    j.set_shuttle(-1.0);
    CHECK_NEAR(j.advance(0.3, 0.1, f.t), 0.0, 1e-9);
    // Stable at several speeds: many small steps cover what one would.
    for (double raw : {0.3, 0.6, 1.0}) {
        j.set_shuttle(raw);
        double at = 20.0;
        for (int i = 0; i < 100; ++i) at = j.advance(at, 0.01, f.t);
        CHECK_NEAR(at, 20.0 + j.shuttle_speed() * 1.0, 1e-6);
    }
    j.settings.reverse = true;
    j.set_shuttle(1.0);
    CHECK(j.shuttle_speed() < 0.0);
    j.reset();
    CHECK(!j.shuttling());
}

TEST_CASE("control surface: markers and cue starts are the locate points; the timeline covers the project")
{
    Fixture f;
    CHECK_NEAR(*next_marker(f.t, 0.0), 4.0, 1e-9);
    CHECK_NEAR(*next_marker(f.t, 4.0), 10.0, 1e-9);  // sitting on one: the next
    CHECK(!next_marker(f.t, 22.5));
    CHECK_NEAR(*previous_marker(f.t, 10.0), 4.0, 1e-9);
    CHECK_NEAR(*previous_marker(f.t, 12.0), 10.0, 1e-9);
    CHECK(!previous_marker(f.t, 4.0));

    project::ProjectData d;
    d.tempo = TempoMap(120.0);
    project::Marker m;
    m.seconds = 7.25;
    d.markers.push_back(m);
    project::Cue c;
    c.start = 8 * kTicksPerQuarter;  // 4 s
    c.end = 16 * kTicksPerQuarter;
    d.cues.push_back(c);
    d.frame_rate = FrameRate::Fps25;
    const auto t = timeline_of(d);
    REQUIRE(t.markers.size() == 2u);
    CHECK_NEAR(t.markers[0], 4.0, 1e-9);
    CHECK_NEAR(t.markers[1], 7.25, 1e-9);
    CHECK_NEAR(t.frames_per_second, 25.0, 1e-9);
    CHECK(t.end_seconds >= 64.0);  // at least the 32 empty bars, and a bar past them
}
