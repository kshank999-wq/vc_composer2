#include <filesystem>

#include "vcc/core/Files.h"
#include "vcc/project/Project.h"
#include "vcc_test.h"

using namespace vcc;
using namespace vcc::project;

static std::vector<NoteEvent> three_notes(Ticks at)
{
    return {{at, kTicksPerQuarter, 60, 80, 0, "staccato"},
            {at + kTicksPerQuarter, kTicksPerQuarter, 62, 90, 0, "staccato"},
            {at + 2 * kTicksPerQuarter, kTicksPerQuarter * 2, 64, 100, 0, {}}};
}

TEST_CASE("edits are single undo steps and ids are never reused")
{
    Project p;
    std::string a = p.add_track("Violins I", TrackKind::Instrument, "strings.violins_1");
    CHECK_EQ(p.data().tracks.size(), 1u);
    CHECK(p.undo());
    CHECK(p.data().tracks.empty());
    CHECK(p.redo());
    CHECK_EQ(p.data().tracks[0].id, a);
    p.undo();
    std::string b = p.add_track("Violins I");
    CHECK(a != b);
    CHECK(!p.can_redo());
}

TEST_CASE("a failed edit leaves the document unchanged and records nothing")
{
    Project p;
    p.add_track("Horn");
    std::size_t steps = p.history_size();
    CHECK(!p.edit("noop", [](ProjectData& d) { d.tracks.clear(); return false; }));
    CHECK(!p.edit("throws", [](ProjectData& d) -> bool { d.tracks.clear(); throw 1; }));
    CHECK_EQ(p.data().tracks.size(), 1u);
    CHECK_EQ(p.history_size(), steps);
}

TEST_CASE("duplicate names get friendly suffixes")
{
    Project p;
    p.add_track("Horn");
    p.add_track("Horn");
    p.add_track("Horn");
    CHECK_EQ(p.data().tracks[1].name, std::string("Horn (2)"));
    CHECK_EQ(p.data().tracks[2].name, std::string("Horn (3)"));
}

TEST_CASE("T08 send to occupied range respects the collision policy")
{
    Project p;
    std::string t = p.add_track("Celli");
    Ticks bar = kTicksPerWhole;
    REQUIRE(p.insert_notes(t, 0, bar, three_notes(0), CollisionPolicy::Ask).ok);
    auto ask = p.insert_notes(t, 0, bar, three_notes(0), CollisionPolicy::Ask);
    CHECK(!ask.ok);
    CHECK(ask.needs_decision);
    CHECK_EQ(p.data().tracks[0].active_version()->regions.size(), 1u);

    auto lane = p.insert_notes(t, 0, bar, three_notes(0), CollisionPolicy::NewLane);
    REQUIRE(lane.ok);
    CHECK_EQ(p.data().tracks[0].active_version()->regions.back().lane, 1);

    auto merge = p.insert_notes(t, 0, bar, {{kTicksPerQuarter * 3, kTicksPerQuarter, 67, 80, 0, {}}}, CollisionPolicy::Merge);
    REQUIRE(merge.ok);
    std::size_t lane0 = 0;
    for (const auto& r : p.data().tracks[0].active_version()->regions) lane0 += r.lane == 0 ? r.notes.size() : 0;
    CHECK_EQ(lane0, 4u);

    auto replace = p.insert_notes(t, 0, bar, {{0, bar, 48, 70, 0, {}}}, CollisionPolicy::Replace);
    REQUIRE(replace.ok);
    lane0 = 0;
    for (const auto& r : p.data().tracks[0].active_version()->regions) lane0 += r.lane == 0 ? r.notes.size() : 0;
    CHECK_EQ(lane0, 1u);
    // The other lane is untouched by a lane-0 replace.
    CHECK(p.undo());
    CHECK(p.undo());
    CHECK_EQ(p.data().tracks[0].active_version()->regions.size(), 2u);
}

TEST_CASE("replace trims a note that straddles the start")
{
    Project p;
    std::string t = p.add_track("Bass");
    REQUIRE(p.insert_notes(t, 0, 2 * kTicksPerWhole, {{0, 2 * kTicksPerWhole, 36, 90, 0, {}}}, CollisionPolicy::Ask).ok);
    REQUIRE(p.insert_notes(t, kTicksPerWhole, 2 * kTicksPerWhole, {{kTicksPerWhole, kTicksPerQuarter, 40, 90, 0, {}}},
                           CollisionPolicy::Replace).ok);
    const auto& regions = p.data().tracks[0].active_version()->regions;
    CHECK_EQ(regions[0].notes[0].duration, kTicksPerWhole);
}

TEST_CASE("inserts are refused for bad notes and non-MIDI tracks")
{
    Project p;
    std::string t = p.add_track("Viola");
    CHECK(!p.insert_notes(t, 0, 100, {{0, 200, 60, 80, 0, {}}}, CollisionPolicy::Ask).ok);
    std::string ref = p.add_track("Temp Score", TrackKind::Reference);
    CHECK(!p.insert_notes(ref, 0, 100, {{0, 50, 60, 80, 0, {}}}, CollisionPolicy::Ask).ok);
    CHECK(!p.insert_notes("nope", 0, 100, {}, CollisionPolicy::Ask).ok);
}

TEST_CASE("acceptance: versions 1 and 2 alternate without overwriting each other")
{
    Project p;
    std::string horn = p.add_track("French Horns", TrackKind::Instrument, "brass.horn_section");
    REQUIRE(p.insert_notes(horn, 0, kTicksPerWhole, three_notes(0), CollisionPolicy::Ask).ok);
    std::string v1 = p.data().tracks[0].active_version_id;
    auto v2 = p.duplicate_version(horn);
    REQUIRE(v2);
    CHECK_EQ(p.data().tracks[0].active_version()->name, std::string("Version 2"));
    REQUIRE(p.insert_notes(horn, 0, kTicksPerWhole, {{0, kTicksPerWhole, 55, 90, 0, {}}}, CollisionPolicy::Replace).ok);
    for (int i = 0; i < 10; ++i) {
        REQUIRE(p.set_active_version(horn, v1));
        CHECK_EQ(p.data().tracks[0].active_version()->regions[0].notes.size(), 3u);
        REQUIRE(p.set_active_version(horn, *v2));
        CHECK_EQ(p.data().tracks[0].active_version()->regions.back().notes[0].pitch, 55);
    }
    CHECK(p.rename_version(horn, *v2, "Darker voicing"));
}

TEST_CASE("snapshots recall coherent combinations of versions")
{
    Project p;
    std::string a = p.add_track("Strings");
    std::string b = p.add_track("Brass");
    std::string a1 = p.data().tracks[0].active_version_id, b1 = p.data().tracks[1].active_version_id;
    std::string s1 = p.create_snapshot("Director A");
    auto a2 = p.duplicate_version(a);
    auto b2 = p.duplicate_version(b);
    std::string s2 = p.create_snapshot("Director B");
    REQUIRE(p.recall_snapshot(s1));
    CHECK_EQ(p.data().find_track(a)->active_version_id, a1);
    CHECK_EQ(p.data().find_track(b)->active_version_id, b1);
    REQUIRE(p.recall_snapshot(s2));
    CHECK_EQ(p.data().find_track(a)->active_version_id, *a2);
    CHECK_EQ(p.data().find_track(b)->active_version_id, *b2);
}

TEST_CASE("save/load round trips through ten edit-save-reopen cycles")
{
    std::string path = std::string(VCC_TEST_TMP_DIR) + "/cycle.vcc2.json";
    Project p;
    p.edit("setup", [](ProjectData& d) {
        d.name = "Cue 1M1";
        d.key = "D minor";
        d.meter.set(5, TimeSignature{7, 8});
        d.tempo.set(kTicksPerWhole * 4, 92.5);
        d.frame_rate = FrameRate::Fps23_976;
        project::Cue cue;
        cue.id = "cue-1";
        cue.code = "1M1";
        cue.title = "Opening";
        cue.end = kTicksPerWhole * 16;
        d.cues.push_back(cue);
        return true;
    });
    std::string t = p.add_track("Celli", TrackKind::Instrument, "strings.celli");
    p.add_track("Temp Score", TrackKind::Reference);
    for (int cycle = 0; cycle < 10; ++cycle) {
        Ticks at = kTicksPerWhole * cycle;
        REQUIRE(p.insert_notes(t, at, at + kTicksPerWhole, three_notes(at), CollisionPolicy::Ask).ok);
        std::string err;
        REQUIRE(p.save(path, &err));
        auto loaded = Project::load(path, &err);
        REQUIRE(loaded);
        CHECK(json::write(loaded->to_json()) == json::write(p.to_json()));
        p = std::move(*loaded);
    }
    const Track* celli = p.data().find_track(t);
    REQUIRE(celli);
    std::size_t notes = 0;
    for (const auto& r : celli->active_version()->regions) {
        notes += r.notes.size();
        for (const auto& n : r.notes)
            if (n.pitch != 64) CHECK_EQ(n.articulation, std::string("staccato"));
    }
    CHECK_EQ(notes, 30u);
    CHECK(p.data().meter.at_bar(6) == (TimeSignature{7, 8}));
    CHECK(p.data().frame_rate == FrameRate::Fps23_976);
    // The id counter survives, so new ids never collide with saved ones.
    std::string fresh = p.add_track("New");
    CHECK(p.data().tracks.size() == 3u && fresh != t);
}

TEST_CASE("loader refuses newer schemas, unknown formats and corrupt references")
{
    Project p;
    p.add_track("Oboe");
    json::Value doc = p.to_json();
    std::string err;
    json::Value newer = doc;
    newer.object().set("schema_version", kProjectSchemaVersion + 1);
    CHECK(!Project::from_json(newer, &err));
    CHECK(err.find("newer") != std::string::npos);
    json::Value wrong = doc;
    wrong.object().set("format", "something-else");
    CHECK(!Project::from_json(wrong, &err));
    json::Value dangling = doc;
    dangling.object()["tracks"].array()[0].object().set("output_bus_id", "bus-missing");
    CHECK(!Project::from_json(dangling, &err));
    json::Value dup = doc;
    auto copy = dup.object()["tracks"].array()[0];
    dup.object()["tracks"].array().push_back(copy);
    CHECK(!Project::from_json(dup, &err));
    CHECK(err.find("duplicate") != std::string::npos);
    json::Value badnote = doc;
    auto& version = badnote.object()["tracks"].array()[0].object()["versions"].array()[0].object();
    version["regions"].array().push_back(json::Object{{"id", "rgn-x"}, {"start", 0}, {"end", 10},
        {"notes", json::Array{json::Array{0, 5, 200, 80, 0}}}});
    CHECK(!Project::from_json(badnote, &err));
}

TEST_CASE("migration table reaches every older schema version")
{
    // Every schema from 1 up to the current one must have a path forward.
    // Today schema 1 is current, so this holds trivially; it starts biting
    // the day the format changes without a migration.
    for (int v = 1; v < kProjectSchemaVersion; ++v) {
        Project p;
        json::Value doc = p.to_json();
        doc.object().set("schema_version", v);
        CHECK(Project::from_json(doc).has_value());
    }
    CHECK(kProjectSchemaVersion >= 1);
}

TEST_CASE("reference tracks are excluded from deliverables unless opted in")
{
    Project p;
    p.add_track("Strings");
    std::string temp = p.add_track("Temp Score", TrackKind::Reference);
    auto ex = p.export_tracks();
    CHECK_EQ(ex.size(), 1u);
    p.edit("opt in", [&](ProjectData& d) { d.find_track(temp)->include_in_export = true; return true; });
    CHECK_EQ(p.export_tracks().size(), 2u);
}
