#include <algorithm>
#include <filesystem>

#include "vcc/core/Files.h"
#include "vcc/project/Merge.h"
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

TEST_CASE("no track-count ceiling: 1,024 instrument and 2,048 audio tracks save, load and edit, with undo memory bounded")
{
    // Memory Management spec §3/§10: no artificial cap; a data-structure stress test.
    Project p;
    const std::string first = p.add_track("Template", TrackKind::Instrument, "strings.violins_1");
    const Track model = *p.data().find_track(first);
    REQUIRE(p.edit("Add 3,071 tracks", [&](ProjectData& d) {
        for (int i = 1; i < 3072; ++i) {
            Track t = model;
            t.id = p.ids().next("trk");
            t.name = (i < 1024 ? "Strings " : "Audio ") + std::to_string(i);
            t.kind = i < 1024 ? TrackKind::Instrument : TrackKind::Audio;
            for (auto& v : t.versions) v.id = p.ids().next("ver");
            t.active_version_id = t.versions.front().id;
            d.tracks.push_back(std::move(t));
        }
        return true;
    }));
    CHECK_EQ(p.data().tracks.size(), 3072u);
    const auto dir = std::filesystem::temp_directory_path() / "vcc_stress_project";
    std::filesystem::create_directories(dir);
    const auto file = (dir / "big.vcc2").string();
    std::string error;
    REQUIRE(p.save(file, &error));
    auto back = Project::load(file, &error);
    REQUIRE(back.has_value());
    CHECK_EQ(back->data().tracks.size(), 3072u);
    std::size_t audio = 0;
    for (const auto& t : back->data().tracks) audio += t.kind == TrackKind::Audio;
    CHECK_EQ(audio, 2048u);
    // A long run of small edits: undo keeps its budget, and at least 20 steps.
    for (int i = 0; i < 120; ++i)
        p.edit("Gain", [&](ProjectData& d) {
            d.tracks[static_cast<std::size_t>(i)].gain_db = -1.0;
            return true;
        });
    CHECK(p.undo_depth() >= 20u);
    CHECK(p.undo_bytes() <= 384u * 1024u * 1024u);
    CHECK(p.undo());
    CHECK(p.data().tracks[119].gain_db == 0.0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("collaboration: two copies of a shared project never hand out the same id")
{
    Project base;
    const std::string violins = base.add_track("Violins I", TrackKind::Instrument, "strings.violins_1");
    auto add_note = [](Project& p, const std::string& track, Ticks at) {
        notation::ScoreNote n;
        n.start_tick = at;
        n.duration = notation::quarter();
        n.pitch = notation::spell_midi_note(67, {});
        notation::EventId id = 0;
        p.edit("Write", [&](ProjectData& d) {
            auto* t = d.find_track(track);
            id = t->active_version()->score.insert_note(0, n);
            return id != notation::kInvalidEventId;
        });
        return id;
    };
    const auto first = add_note(base, violins, 0);
    CHECK(first < (notation::EventId{1} << notation::Score::kSiteShift));  // unshared: ids as always

    // Two collaborators, each with a copy and a site of their own.
    const auto doc = base.to_json();
    auto ann = Project::from_json(doc);
    auto ben = Project::from_json(doc);
    REQUIRE(ann && ben);
    ann->set_site(0x1a2b);
    ben->set_site(0x3c4d);
    const auto a1 = add_note(*ann, violins, kTicksPerQuarter);
    const auto a2 = add_note(*ann, violins, 2 * kTicksPerQuarter);
    const auto b1 = add_note(*ben, violins, kTicksPerQuarter);
    CHECK(a1 != b1);
    CHECK(a1 != a2);
    CHECK_EQ(a1 >> notation::Score::kSiteShift, 0x1a2bu);
    CHECK_EQ(b1 >> notation::Score::kSiteShift, 0x3c4du);
    CHECK_EQ(a1 & notation::Score::kSiteCounterMask, 1u);  // each site counts on its own
    const auto at = ann->add_track("Horns", TrackKind::Instrument, "brass.horn_section");
    const auto bt = ben->add_track("Horns", TrackKind::Instrument, "brass.horn_section");
    CHECK(at != bt);
    CHECK(at.find("-1a2b-") != std::string::npos);

    // Saved and reopened on the same machine, the site's counter continues
    // past what it had handed out: nothing is reused.
    auto again = Project::from_json(ann->to_json());
    REQUIRE(again);
    again->set_site(0x1a2b);
    const auto a3 = add_note(*again, violins, 3 * kTicksPerQuarter);
    CHECK_EQ(a3 & notation::Score::kSiteCounterMask, 3u);
    CHECK(a3 != a1 && a3 != a2);
}

// --- Three-way merge (collaboration, Main Spec §15) ---------------------------------

namespace {
struct Pair {
    json::Value base;
    std::optional<Project> ann, ben;
    std::string violins;
};
notation::EventId write(Project& p, const std::string& track, Ticks at, int midi)
{
    notation::ScoreNote n;
    n.start_tick = at;
    n.duration = notation::quarter();
    n.pitch = notation::spell_midi_note(midi, {});
    notation::EventId id = 0;
    p.edit("Write", [&](ProjectData& d) {
        id = d.find_track(track)->active_version()->score.insert_note(0, n);
        return id != 0;
    });
    return id;
}
Pair shared_pair()
{
    Pair x;
    Project p;
    x.violins = p.add_track("Violins I", TrackKind::Instrument, "strings.violins_1");
    write(p, x.violins, 0, 67);
    x.base = p.to_json();
    x.ann = Project::from_json(x.base);
    x.ben = Project::from_json(x.base);
    x.ann->set_site(0xa);
    x.ben->set_site(0xb);
    return x;
}
const notation::ScoreNote* note(const Project& p, const std::string& track, notation::EventId id)
{
    return p.data().find_track(track)->active_version()->score.find_note(id);
}
}  // namespace

TEST_CASE("merge: separate changes from two people combine with no questions")
{
    auto x = shared_pair();
    const auto a1 = write(*x.ann, x.violins, kTicksPerQuarter, 69);         // Ann writes a note
    const auto b1 = write(*x.ben, x.violins, 2 * kTicksPerQuarter, 71);     // Ben writes another in the same bar
    x.ben->edit("Mix", [&](ProjectData& d) { d.find_track(x.violins)->gain_db = -6.0; return true; });
    const auto horns = x.ben->add_track("Horns", TrackKind::Instrument, "brass.horn_section");
    const auto r = merge_projects(x.base, x.ann->to_json(), x.ben->to_json());
    CHECK(r.clean());
    CHECK(r.from_ours > 0);
    CHECK(r.from_theirs > 0);
    auto merged = Project::from_json(r.merged);
    REQUIRE(merged);
    CHECK(note(*merged, x.violins, a1) != nullptr);
    CHECK(note(*merged, x.violins, b1) != nullptr);
    CHECK_EQ(merged->data().find_track(x.violins)->active_version()->score.total_note_count(), 3u);
    CHECK_EQ(merged->data().find_track(x.violins)->gain_db, -6.0);
    CHECK(merged->data().find_track(horns) != nullptr);
    // The same change on both sides is taken once.
    auto y = shared_pair();
    y.ann->edit("Mix", [&](ProjectData& d) { d.find_track(y.violins)->pan = 0.5; return true; });
    y.ben->edit("Mix", [&](ProjectData& d) { d.find_track(y.violins)->pan = 0.5; return true; });
    CHECK(merge_projects(y.base, y.ann->to_json(), y.ben->to_json()).clean());
}

TEST_CASE("merge: the same note changed differently is a conflict, described and resolvable")
{
    auto x = shared_pair();
    const auto first = x.ann->data().find_track(x.violins)->active_version()->score.notes(0).front().id;
    auto repitch = [&](Project& p, int midi) {
        p.edit("Pitch", [&](ProjectData& d) {
            auto& sc = d.find_track(x.violins)->active_version()->score;
            auto n = *sc.find_note(first);
            n.pitch = notation::spell_midi_note(midi, {});
            return sc.modify_note(first, n);
        });
    };
    repitch(*x.ann, 72);  // C5
    repitch(*x.ben, 64);  // E4
    x.ben->edit("Mix", [&](ProjectData& d) { d.find_track(x.violins)->mute = true; return true; });  // not in conflict
    const auto r = merge_projects(x.base, x.ann->to_json(), x.ben->to_json());
    REQUIRE(r.conflicts.size() == 1u);  // the note, once - not its step and octave apart
    for (const auto& c : r.conflicts) {
        CHECK(c.kind == MergeConflict::Kind::Changed);
        CHECK(c.description.find("Violins I") != std::string::npos);
        CHECK(c.description.find("note") != std::string::npos);
    }
    // Ours by default; the rest of Ben's work is in.
    auto mine = Project::from_json(r.merged);
    REQUIRE(mine);
    CHECK_EQ(notation::midi_note_number(note(*mine, x.violins, first)->pitch), 72);
    CHECK(mine->data().find_track(x.violins)->mute);
    // Taking theirs at every conflict gives Ben's pitch.
    auto theirs = Project::from_json(r.resolve(std::vector<bool>(r.conflicts.size(), true)));
    REQUIRE(theirs);
    CHECK_EQ(notation::midi_note_number(note(*theirs, x.violins, first)->pitch), 64);
}

TEST_CASE("merge: a note deleted on one side and changed on the other is asked about")
{
    auto x = shared_pair();
    const auto first = x.ann->data().find_track(x.violins)->active_version()->score.notes(0).front().id;
    x.ann->edit("Delete", [&](ProjectData& d) { return d.find_track(x.violins)->active_version()->score.remove_note(first); });
    x.ben->edit("Pitch", [&](ProjectData& d) {
        auto& sc = d.find_track(x.violins)->active_version()->score;
        auto n = *sc.find_note(first);
        n.pitch = notation::spell_midi_note(60, {});
        return sc.modify_note(first, n);
    });
    const auto r = merge_projects(x.base, x.ann->to_json(), x.ben->to_json());
    REQUIRE(r.conflicts.size() == 1u);
    CHECK(r.conflicts[0].kind == MergeConflict::Kind::ChangedAndDeleted);
    CHECK(!r.conflicts[0].ours.has_value());
    auto deleted = Project::from_json(r.merged);  // ours: deleted
    REQUIRE(deleted);
    CHECK(note(*deleted, x.violins, first) == nullptr);
    auto kept = Project::from_json(r.resolve({true}));  // theirs: kept, changed
    REQUIRE(kept);
    REQUIRE(note(*kept, x.violins, first) != nullptr);
    CHECK_EQ(notation::midi_note_number(note(*kept, x.violins, first)->pitch), 60);
}

TEST_CASE("track locks: a part somebody else is writing cannot be edited here; the mix stays mine")
{
    auto x = shared_pair();
    Project& p = *x.ann;
    const auto first = p.data().find_track(x.violins)->active_version()->score.notes(0).front().id;
    p.set_track_locks({{x.violins, "Ben"}});
    auto pitch = [&](int midi) {
        return p.edit("Pitch", [&](ProjectData& d) {
            auto& sc = d.find_track(x.violins)->active_version()->score;
            auto n = *sc.find_note(first);
            n.pitch = notation::spell_midi_note(midi, {});
            return sc.modify_note(first, n);
        });
    };
    const auto refused = p.refusal_count();
    CHECK(!pitch(71));
    CHECK(p.refusal_count() == refused + 1);
    CHECK(p.last_refusal().find("Ben is writing") == 0);
    CHECK(!p.edit("Rename", [&](ProjectData& d) { d.find_track(x.violins)->name = "Vln"; return true; }));
    CHECK(!p.edit("Delete", [&](ProjectData& d) {
        auto& t = d.tracks;
        t.erase(std::remove_if(t.begin(), t.end(), [&](const Track& k) { return k.id == x.violins; }), t.end());
        return true;
    }));
    CHECK(p.edit("Mute", [&](ProjectData& d) { d.find_track(x.violins)->mute = true; return true; }));
    // A sync brings in what the holder wrote, whatever the lock.
    ProjectData theirs = p.data();
    theirs.find_track(x.violins)->name = "Violins (Ben)";
    const auto site = p.data().site;
    p.replace_document("Sync", theirs);
    CHECK(p.data().find_track(x.violins)->name == "Violins (Ben)");
    CHECK(p.data().site == site);
    CHECK(p.undo_label() == "Sync");
    // Unlocked, it edits again.
    p.set_track_locks({});
    CHECK(pitch(71));
}
