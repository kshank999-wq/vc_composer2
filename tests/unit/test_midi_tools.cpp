#include <filesystem>

#include "vcc/project/MidiTools.h"
#include "vcc/project/Render.h"
#include "vcc/session/Session.h"
#include "vcc_test.h"

using namespace vcc;

namespace {
const session::Catalogs& catalogs()
{
    static auto c = session::Catalogs::load_from_directory(VCC_CONTENT_DIR);
    return *c;
}
constexpr Ticks Q = kTicksPerQuarter;
constexpr Ticks S = Q / 4;  // a sixteenth
}  // namespace

TEST_CASE("quantize snaps starts to the grid, keeps lengths, with strength and swing")
{
    project::QuantizeSettings q;
    q.grid = S;
    std::vector<NoteEvent> notes{{S + 40, S, 60, 90, 0, {}}, {2 * S - 30, S, 62, 90, 0, {}}};
    CHECK(project::quantize_notes(notes, q) == 2);
    CHECK(notes[0].start == S);
    CHECK(notes[0].duration == S);  // length kept
    CHECK(notes[1].start == 2 * S);

    // Half strength: half way.
    std::vector<NoteEvent> half{{S + 40, S, 60, 90, 0, {}}};
    q.strength = 0.5;
    project::quantize_notes(half, q);
    CHECK(half[0].start == S + 20);

    // Swing: every second sixteenth is late by a third of a step.
    q.strength = 1.0;
    q.swing = 1.0 / 3.0;
    CHECK(project::quantize_tick(S + 10, q) == S + S / 3);
    CHECK(project::quantize_tick(2 * S + 10, q) == 2 * S);

    // Ends too: never shorter than half a step.
    q.swing = 0.0;
    q.ends = true;
    std::vector<NoteEvent> e{{0, S + 50, 64, 90, 0, {}}, {4 * S, 10, 65, 90, 0, {}}};
    project::quantize_notes(e, q);
    CHECK(e[0].duration == S);
    CHECK(e[1].duration == S / 2);
}

TEST_CASE("quantize never leaves overlapping notes of one pitch")
{
    project::QuantizeSettings q;
    q.grid = Q;
    // Both land on beat 2: the duplicate goes.
    std::vector<NoteEvent> notes{{Q - 20, Q, 60, 90, 0, {}}, {Q + 30, Q, 60, 80, 0, {}}};
    project::quantize_notes(notes, q);
    CHECK(notes.size() == 1);
    CHECK(validate_notes(notes, 0, 10 * Q).empty());
}

TEST_CASE("velocity: scale, offset, set and compress, kept in 1..127")
{
    std::vector<NoteEvent> notes{{0, Q, 60, 40, 0, {}}, {Q, Q, 62, 100, 0, {}}};
    std::vector<NoteEvent*> p{&notes[0], &notes[1]};
    project::VelocityChange louder;
    louder.scale = 1.5;
    CHECK(project::change_velocities(p, louder) == 2);
    CHECK(notes[0].velocity == 60);
    CHECK(notes[1].velocity == 127);  // clamped
    project::VelocityChange flat;
    flat.compress = 1.0;
    project::change_velocities(p, flat);
    CHECK(notes[0].velocity == notes[1].velocity);
    project::VelocityChange set;
    set.set = 64;
    project::change_velocities(p, set);
    CHECK(notes[0].velocity == 64);
    project::VelocityChange down;
    down.offset = -100;
    project::change_velocities(p, down);
    CHECK(notes[0].velocity == 1);
}

TEST_CASE("session: quantize, velocity, CC and input quantize on regions, undoable and saved")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto t = s.add_track("Keys", "keys.piano");
    project::MidiRegion r;
    r.start = 0;
    r.end = 4 * Q;
    r.notes = {{S + 40, S, 60, 90, 0, {}}, {4 * Q - 30, S, 67, 50, 0, {}}};
    const auto ids = s.add_recorded_regions({{t, r}}, "Record MIDI");
    REQUIRE(ids.size() == 1);
    auto region = [&] { return s.data().find_track(t)->active_version()->regions.front(); };

    // Everything (no selection): the second note moves past the region's end; the region grows a bar.
    project::QuantizeSettings q;
    q.grid = S;
    CHECK(s.quantize_regions(t, {}, q) == 2);
    CHECK(s.undo_label() == "Quantize");
    CHECK(region().notes[0].start == S);
    CHECK(region().notes[1].start == 4 * Q);
    CHECK(region().end == 8 * Q);
    CHECK(s.quantize_regions(t, {}, q) == 0);  // already there: no step

    // Velocity on one chosen note.
    project::VelocityChange v;
    v.offset = 10;
    CHECK(s.change_region_velocities(t, {{ids[0], 4 * Q, 67}}, v) == 1);
    CHECK(s.undo_label() == "Velocity");
    CHECK(region().notes[1].velocity == 60);
    CHECK(region().notes[0].velocity == 90);

    // Draw sustain over beats 1-2: the region holds it, it renders, and it saves.
    CHECK(s.draw_region_controller(t, ids[0], 64, 0, 2 * Q, {{0, 0, 0, 127}, {Q, 0, 0, 0}}));
    CHECK(s.undo_label() == "Draw CC");
    REQUIRE(region().controllers.size() == 2);
    CHECK(region().controllers[0].controller == 64);
    bool rendered = false;
    for (const auto& c : project::render_track(*s.data().find_track(t)).controllers)
        rendered = rendered || (c.controller == 64 && c.value == 127 && c.tick == 0);
    CHECK(rendered);
    // Redrawn: the old points in the range go.
    CHECK(s.draw_region_controller(t, ids[0], 64, 0, 2 * Q, {{Q / 2, 0, 0, 100}}));
    CHECK(region().controllers.size() == 1);

    CHECK(s.set_input_quantize(t, S));
    CHECK(s.data().find_track(t)->input_quantize == S);

    // Saved and read back.
    const auto file = std::filesystem::temp_directory_path() / "vcc_midi_tools_test.vcc2";
    std::string error;
    REQUIRE(s.save_as(file.string(), &error));
    session::Session back(catalogs());
    REQUIRE(back.open(file.string(), &error));
    const auto* bt = back.data().find_track(t);
    REQUIRE(bt != nullptr);
    CHECK(bt->input_quantize == S);
    CHECK(bt->active_version()->regions.front().controllers.size() == 1);
    CHECK(bt->active_version()->regions.front().controllers[0].value == 100);
    std::filesystem::remove(file);

    s.undo();
    s.undo();
    CHECK(region().controllers.size() == 2);
}
