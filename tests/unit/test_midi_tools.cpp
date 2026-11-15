#include <filesystem>

#include <cstdio>

#include "vcc/music/MidiFile.h"
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

namespace {
music::SmfEvent note(std::int64_t tick, bool on, int channel, int pitch, int velocity)
{
    music::SmfEvent e;
    e.tick = tick;
    e.event.type = on ? music::MidiEventType::NoteOn : music::MidiEventType::NoteOff;
    e.event.channel = static_cast<std::uint8_t>(channel);
    e.event.data1 = static_cast<std::uint8_t>(pitch);
    e.event.data2 = static_cast<std::uint8_t>(velocity);
    return e;
}
}  // namespace

TEST_CASE("importing a MIDI file: parts become tracks with regions, tempo and meter taken, one undo step")
{
    // A 480-ppq file: a tempo/meter track, a piano part with a pedal, and a drum part.
    music::SmfFile f;
    f.ticks_per_quarter = 480;
    music::SmfTrack conductor;
    conductor.meta.push_back(music::smf_tempo(0, 100.0));
    conductor.meta.push_back(music::smf_time_signature(0, music::TimeSignature{3, 4}));
    music::SmfTrack piano;
    piano.name = "Piano";
    music::SmfEvent prog;
    prog.event.type = music::MidiEventType::ProgramChange;
    prog.event.data1 = 0;
    piano.events.push_back(prog);
    piano.events.push_back(note(0, true, 0, 60, 90));
    piano.events.push_back(note(480, false, 0, 60, 0));
    music::SmfEvent pedal;
    pedal.tick = 0;
    pedal.event.type = music::MidiEventType::ControlChange;
    pedal.event.data1 = 64;
    pedal.event.data2 = 127;
    piano.events.push_back(pedal);
    piano.events.push_back(note(960, true, 0, 64, 80));
    piano.events.push_back(note(1200, false, 0, 64, 0));
    music::SmfTrack drums;
    drums.name = "Drums";
    drums.events.push_back(note(0, true, 9, 36, 110));
    drums.events.push_back(note(120, false, 9, 36, 0));
    f.tracks = {conductor, piano, drums};
    const auto path = (std::filesystem::temp_directory_path() / "vcc_import_test.mid").string();
    REQUIRE(music::write_smf_file(f, path).ok);

    session::Session s(catalogs());
    s.new_empty();
    const auto tracks_before = s.data().tracks.size();
    const auto r = s.import_midi_file(path, {}, 0);
    REQUIRE(r.error.empty());
    CHECK(r.parts == 2);
    CHECK(r.notes == 3);
    CHECK(r.new_tracks);
    CHECK(r.took_tempo);
    CHECK(s.undo_label() == "Import vcc_import_test");
    CHECK(s.data().tracks.size() == tracks_before + 2);
    CHECK(std::abs(s.data().tempo.bpm_at(0) - 100.0) < 0.01);
    CHECK(s.data().meter.at_bar(1).numerator == 3);
    const auto* pt = s.data().find_track(r.track_ids[0]);
    REQUIRE(pt != nullptr);
    CHECK(pt->name == "Piano");
    const auto& reg = pt->active_version()->regions.at(0);
    REQUIRE(reg.notes.size() == 2);
    CHECK(reg.notes[0].start == 0);
    CHECK(reg.notes[0].duration == Q);  // 480 file ticks = a quarter
    CHECK(reg.notes[1].start == 2 * Q);
    CHECK(reg.end == 3 * Q);  // one bar of 3/4
    REQUIRE(reg.controllers.size() == 1);
    CHECK(reg.controllers[0].controller == 64);
    const auto* dt = s.data().find_track(r.track_ids[1]);
    REQUIRE(dt != nullptr);
    CHECK(dt->midi_channel == 9);  // percussion
    s.undo();
    CHECK(s.data().tracks.size() == tracks_before);

    // A one-part file onto a chosen track, from bar 3: earlier regions there move to a take lane.
    music::SmfFile one;
    one.ticks_per_quarter = 960;
    music::SmfTrack line;
    line.events.push_back(note(0, true, 0, 67, 70));
    line.events.push_back(note(960, false, 0, 67, 0));
    one.tracks = {line};
    REQUIRE(music::write_smf_file(one, path).ok);
    const auto keys = s.add_track("Keys", "keys.piano");
    const auto r2 = s.import_midi_file(path, keys, s.data().meter.bar_start(3) + 100);
    REQUIRE(r2.parts == 1);
    CHECK(!r2.new_tracks);
    CHECK(r2.track_ids[0] == keys);
    CHECK(s.data().find_track(keys)->active_version()->regions[0].notes[0].start == s.data().meter.bar_start(3));
    const auto r3 = s.import_midi_file(path, keys, s.data().meter.bar_start(3));
    REQUIRE(r3.parts == 1);
    int lane0 = 0;
    for (const auto& x : s.data().find_track(keys)->active_version()->regions) lane0 += x.lane == 0 ? 1 : 0;
    CHECK(lane0 == 1);

    // Not a MIDI file.
    {
        std::FILE* bad = std::fopen(path.c_str(), "wb");
        std::fputs("not midi", bad);
        std::fclose(bad);
    }
    CHECK(!s.import_midi_file(path, {}, 0).error.empty());
    std::filesystem::remove(path);
}

TEST_CASE("MIDI comping: a range of a take replaces the main lane's notes there; promote swaps")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto t = s.add_track("Keys", "keys.piano");
    const Ticks bar = s.data().meter.bar_start(2);
    project::MidiRegion take1, take2;
    take1.start = take2.start = 0;
    take1.end = take2.end = 2 * bar;
    for (int i = 0; i < 8; ++i) {
        take1.notes.push_back({i * Q, Q / 2, 60, 80, 0, {}});
        take2.notes.push_back({i * Q, Q / 2, 72, 90, 0, {}});
    }
    take2.controllers.push_back({Q * 5, 0, 64, 127});
    s.add_recorded_regions({{t, take1}}, "Record MIDI");
    s.add_recorded_regions({{t, take2}}, "Record MIDI");  // take 2 main, take 1 on lane 1
    auto regions = [&] { return s.data().find_track(t)->active_version()->regions; };
    std::string take1_id;
    for (const auto& r : regions())
        if (r.lane == 1) take1_id = r.id;
    REQUIRE(!take1_id.empty());

    // Beats 3-5 (ticks 2Q..5Q) from take 1: its 60s there, take 2's 72s elsewhere.
    CHECK(s.comp_region_take(t, take1_id, 2 * Q, 5 * Q));
    CHECK(s.undo_label() == "Comp take");
    const project::MidiRegion* main = nullptr;
    for (const auto& r : regions())
        if (r.lane == 0) main = &r;
    REQUIRE(main != nullptr);
    std::vector<NoteEvent> notes = main->notes;
    std::sort(notes.begin(), notes.end());
    REQUIRE(notes.size() == 8);
    for (const auto& n : notes) CHECK(n.pitch == (n.start >= 2 * Q && n.start < 5 * Q ? 60 : 72));
    CHECK(main->controllers.size() == 1);  // the pedal at 5Q is outside the range: kept

    // A range over take 1 where the main lane has nothing: a new main region.
    s.undo();
    project::MidiRegion late;
    late.start = 4 * bar;
    late.end = 5 * bar;
    late.notes = {{4 * bar, Q, 50, 70, 0, {}}};
    late.lane = 0;
    s.add_recorded_regions({{t, late}}, "Record MIDI");  // lands on lane 0, nothing overlaps
    // Promote take 1: take 2 (main, overlapping) goes to take 1's lane.
    CHECK(s.promote_region_take(t, take1_id));
    int main_60 = 0;
    for (const auto& r : regions())
        if (r.lane == 0)
            for (const auto& n : r.notes) main_60 += n.pitch == 60 ? 1 : 0;
    CHECK(main_60 == 8);
    CHECK(!s.promote_region_take(t, take1_id));  // already on the main lane
}

TEST_CASE("splitting a MIDI region: notes stay where they start, controllers by tick, one undo step")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto t = s.add_track("Keys", "keys.piano");
    project::MidiRegion r;
    r.start = 0;
    r.end = 8 * Q;
    r.notes = {{0, Q, 60, 80, 0, {}}, {3 * Q, 2 * Q, 62, 80, 0, {}}, {5 * Q, Q, 64, 80, 0, {}}};
    r.controllers = {{Q, 0, 64, 127}, {6 * Q, 0, 64, 0}};
    s.add_recorded_regions({{t, r}}, "Record MIDI");
    const auto right = s.split_regions({t}, 4 * Q);
    REQUIRE(right.size() == 1);
    CHECK(s.undo_label() == "Split region");
    const auto& regions = s.data().find_track(t)->active_version()->regions;
    REQUIRE(regions.size() == 2);
    const auto& left = regions[0].id == right[0] ? regions[1] : regions[0];
    const auto& rr = regions[0].id == right[0] ? regions[0] : regions[1];
    CHECK(left.end == 4 * Q);
    CHECK(rr.start == 4 * Q);
    CHECK(rr.end == 8 * Q);
    CHECK(left.notes.size() == 2);  // the note across the cut keeps its length, on the left
    CHECK(left.notes[1].duration == 2 * Q);
    CHECK(rr.notes.size() == 1);
    CHECK(left.controllers.size() == 1);
    CHECK(rr.controllers.size() == 1);
    CHECK(s.split_regions({t}, 4 * Q).empty());  // on a boundary: nothing
    s.undo();
    CHECK(s.data().find_track(t)->active_version()->regions.size() == 1);
}
