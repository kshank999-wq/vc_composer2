// Director review (A/B auditions, version comparison, approval) and project
// packaging (Main Spec §15, §16).
#include <filesystem>

#include "vcc/core/Files.h"
#include "vcc/editing/ScoreEditor.h"
#include "vcc/session/Package.h"
#include "vcc/session/Session.h"
#include "vcc_test.h"

using namespace vcc;
using session::Session;
namespace fs = std::filesystem;

namespace {
const session::Catalogs& catalogs()
{
    static auto c = session::Catalogs::load_from_directory(VCC_CONTENT_DIR);
    return *c;
}
std::string tmp(const std::string& name) { return std::string(VCC_TEST_TMP_DIR) + "/" + name; }

/// A horn line, one quarter note per beat for `bars` bars, starting at `pitch`.
std::vector<NoteEvent> line(int bars, int pitch)
{
    std::vector<NoteEvent> out;
    for (int i = 0; i < bars * 4; ++i) out.push_back({i * kTicksPerQuarter, kTicksPerQuarter, pitch + (i % 4), 80, 0, {}});
    return out;
}

void write_bytes(const std::string& path, std::size_t n, char fill)
{
    fs::create_directories(fs::u8path(path).parent_path());
    REQUIRE(files::write_file_atomic(path, std::string(n, fill)));
}
}  // namespace

TEST_CASE("review: versions compare by bar, as played")
{
    Session s(catalogs());
    s.new_empty();
    const auto horn = s.add_track("Horn", "brass.horn_f");
    REQUIRE(s.insert_notes(horn, 0, 8 * kTicksPerWhole, line(8, 60), project::CollisionPolicy::Replace, "line").ok);
    const auto v1 = s.data().find_track(horn)->active_version_id;
    const auto v2 = *s.duplicate_version(horn);
    CHECK(s.compare_versions(horn, v1, v2).same());
    CHECK_EQ(s.compare_versions(horn, v1, v2).describe(), std::string("Same music"));
    // Bars 3 and 4 and bar 7 rewritten in Version 2.
    std::vector<NoteEvent> alt;
    for (int i = 0; i < 8; ++i) alt.push_back({2 * kTicksPerWhole + i * kTicksPerQuarter, kTicksPerQuarter, 67, 90, 0, {}});
    REQUIRE(s.insert_notes(horn, 2 * kTicksPerWhole, 4 * kTicksPerWhole, alt, project::CollisionPolicy::Replace, "alt").ok);
    REQUIRE(s.insert_notes(horn, 6 * kTicksPerWhole, 7 * kTicksPerWhole,
                           {{6 * kTicksPerWhole, kTicksPerWhole, 55, 70, 0, {}}}, project::CollisionPolicy::Replace, "alt")
                .ok);
    const auto diff = s.compare_versions(horn, v1, v2);
    REQUIRE(diff.bars.size() == 2u);
    CHECK_EQ(diff.bars[0].first, 3);
    CHECK_EQ(diff.bars[0].second, 4);
    CHECK_EQ(diff.bars[1].first, 7);
    CHECK_EQ(diff.bars[1].second, 7);
    CHECK_EQ(diff.describe(), std::string("bars 3-4, 7"));
    CHECK_EQ(diff.notes_a, 32u);
    CHECK_EQ(diff.notes_b, 32u - 4u + 1u);
}

TEST_CASE("review: an audition switches versions with no undo step; choosing is one step")
{
    Session s(catalogs());
    s.new_empty();
    const auto horn = s.add_track("Horn", "brass.horn_f");
    const auto strings = s.add_track("Strings", "strings.violins_1");
    const auto h1 = s.data().find_track(horn)->active_version_id;
    const auto s1 = s.data().find_track(strings)->active_version_id;
    const auto a = s.create_snapshot("A: sparse");
    const auto h2 = *s.duplicate_version(horn);
    const auto s2 = *s.duplicate_version(strings);
    const auto b = s.create_snapshot("B: full");
    REQUIRE(s.data().snapshots.size() == 2u);
    CHECK_EQ(s.data().snapshots[1].track_versions.at(horn), h2);

    const auto start = s.active_versions();
    const auto history = s.project().history_size();
    CHECK(s.audition_versions(s.data().snapshots[0].track_versions));
    CHECK_EQ(s.data().find_track(horn)->active_version_id, h1);
    CHECK_EQ(s.data().find_track(strings)->active_version_id, s1);
    CHECK(s.audition_versions(s.data().snapshots[1].track_versions));
    CHECK(s.audition_versions({{horn, h1}}));  // one track's A against the rest of B
    CHECK_EQ(s.data().find_track(strings)->active_version_id, s2);
    CHECK_EQ(s.project().history_size(), history);  // listening is not editing
    CHECK(!s.audition_versions({{horn, "no-such-version"}}));

    // Keep A: one step, and undo goes back to where the audition began.
    CHECK(s.choose_versions("Choose A", start, s.data().snapshots[0].track_versions));
    CHECK_EQ(s.project().history_size(), history + 1);
    CHECK_EQ(s.project().undo_label(), std::string("Choose A"));
    CHECK_EQ(s.data().find_track(horn)->active_version_id, h1);
    CHECK(s.undo());
    CHECK_EQ(s.data().find_track(horn)->active_version_id, h2);
    CHECK_EQ(s.data().find_track(strings)->active_version_id, s2);
    // Choosing what was there anyway records nothing.
    const auto before = s.project().history_size();
    s.audition_versions(s.data().snapshots[0].track_versions);
    CHECK(!s.choose_versions("Choose B", start, start));
    CHECK_EQ(s.project().history_size(), before);
    CHECK_EQ(s.data().find_track(horn)->active_version_id, h2);
    (void)a;
    (void)b;
}

TEST_CASE("review: an approved version's music is locked; its copy, its name and other versions are not")
{
    Session s(catalogs());
    s.new_empty();
    const auto horn = s.add_track("Horn", "brass.horn_f");
    REQUIRE(s.insert_notes(horn, 0, 2 * kTicksPerWhole, line(2, 60), project::CollisionPolicy::Replace, "line").ok);
    const auto v1 = s.data().find_track(horn)->active_version_id;
    CHECK(s.set_version_approved(horn, v1, true));
    CHECK(s.data().find_track(horn)->active_version()->approved);
    // Locked: notes, the score, a version deletion.
    CHECK(!s.insert_notes(horn, 0, kTicksPerWhole, {{0, kTicksPerQuarter, 72, 80, 0, {}}},
                          project::CollisionPolicy::Replace, "x")
               .ok);
    CHECK(s.last_refusal().find("approved") != std::string::npos);
    CHECK(s.last_refusal().find("Horn") != std::string::npos);
    // A note written in the score (the line above is a MIDI region).
    auto score = s.data().find_track(horn)->active_version()->score;
    {
        editing::ScoreEditor editor(score);
        editor.set_cursor(editing::EditCursor{});
        editor.set_duration(notation::NoteValue::Quarter);
        notation::Pitch p;
        p.step = notation::Step::G;
        editor.insert_pitch(p);
    }
    REQUIRE(score.total_note_count() == 1u);
    CHECK(!s.commit_score(horn, score, "Write a note"));
    CHECK_EQ(s.data().find_track(horn)->active_version()->score.total_note_count(), 0u);
    // Free: renaming it, the tempo, the mix, a key change (it reaches every score).
    CHECK(s.rename_version(horn, v1, "Version 1 (director)"));
    CHECK(s.set_tempo(96.0));
    CHECK(s.set_mute(horn, true));
    CHECK(s.set_key("D major"));
    CHECK(s.last_refusal().empty());
    // Duplicate: the copy is unapproved and editable; the approved one is kept.
    const auto v2 = *s.duplicate_version(horn);
    CHECK(!s.data().find_track(horn)->active_version()->approved);
    CHECK(s.insert_notes(horn, 0, kTicksPerWhole, {{0, kTicksPerQuarter, 72, 80, 0, {}}},
                         project::CollisionPolicy::Replace, "x")
              .ok);
    CHECK(!s.compare_versions(horn, v1, v2).same());
    // Released, it can be edited again; approval survives save and reopen.
    CHECK(s.set_version_approved(horn, v1, false));
    CHECK(s.set_active_version(horn, v1));
    CHECK(s.insert_notes(horn, 0, kTicksPerWhole, {{0, kTicksPerQuarter, 74, 80, 0, {}}},
                         project::CollisionPolicy::Replace, "x")
              .ok);
    CHECK(s.set_version_approved(horn, v2, true));
    const auto snap = s.create_snapshot("Final");
    CHECK(s.set_snapshot_approved(snap, true));
    const auto path = tmp("approved.vcc2");
    REQUIRE(s.save_as(path));
    Session r(catalogs());
    REQUIRE(r.open(path));
    const auto* t = r.data().find_track(horn);
    REQUIRE(t);
    CHECK(!t->versions[0].approved);
    CHECK(t->versions[1].approved);
    CHECK(r.data().snapshots.back().approved);
}

TEST_CASE("package: media are collected, paths made relative, dependencies listed, missing files reported")
{
    const std::string root = tmp("pkg-src");
    fs::remove_all(fs::u8path(root));
    fs::remove_all(fs::u8path(tmp("pkg-out")));
    write_bytes(root + "/Project/Audio Files/Vln_01.wav", 1000, 'a');
    write_bytes(root + "/Elsewhere/Vln_01.wav", 500, 'b');  // same name, another file
    write_bytes(root + "/Elsewhere/Choir.wav", 300, 'c');
    write_bytes(root + "/Movies/Cut3.mov", 2000, 'm');

    Session s(catalogs());
    s.new_empty();
    const auto vox = s.add_audio_track("Vox", true);
    project::AudioClip c1;
    c1.file = "Audio Files/Vln_01.wav";  // relative: beside the project
    c1.length = 2.0;
    project::AudioClip c2;
    c2.file = root + "/Elsewhere/Vln_01.wav";
    c2.start_seconds = 4.0;
    c2.length = 1.0;
    project::AudioClip c3;
    c3.file = root + "/Elsewhere/Choir.wav";
    c3.start_seconds = 6.0;
    c3.length = 1.0;
    c3.lane = 1;  // a take: collected too
    project::AudioClip gone;
    gone.file = root + "/Nowhere/Lost.wav";
    gone.start_seconds = 8.0;
    gone.length = 1.0;
    REQUIRE(s.add_audio_clips({{vox, c1}, {vox, c2}, {vox, c3}, {vox, gone}}, "Import", false).size() == 4u);
    // The same file used twice is one asset.
    project::AudioClip again = c1;
    again.start_seconds = 10.0;
    s.add_audio_clips({{vox, again}}, "Import", false);
    project::PictureVersion pic;
    pic.name = "Cut 3";
    pic.path = root + "/Movies/Cut3.mov";
    s.import_picture(pic);
    const auto horn = s.add_track("Horn", "brass.horn_f");
    project::PluginRef kontakt;
    kontakt.format = "VST3";
    kontakt.identifier = "VST3-Kontakt-1234";
    kontakt.name = "Kontakt 8";
    kontakt.vendor = "Native Instruments";
    kontakt.state_base64 = "c3RhdGU=";
    REQUIRE(s.set_instrument_plugin(horn, kontakt));
    s.rename_project("Cue 1M3: Arrival");
    REQUIRE(s.save_as(root + "/Project/Cue.vcc2"));

    auto inv = session::package::inventory(s.data(), s.path());
    CHECK_EQ(inv.assets.size(), 5u);  // two Vln_01, Choir, Lost, the picture
    CHECK_EQ(inv.missing(), 1);
    CHECK_EQ(inv.total_bytes(), static_cast<std::uintmax_t>(1000 + 500 + 300 + 2000));
    REQUIRE(inv.dependencies.size() == 1u);
    CHECK_EQ(inv.dependencies[0].name, std::string("Kontakt 8"));

    int calls = 0;
    const auto out = tmp("pkg-out") + "/Cue 1M3 Package";
    auto res = session::package::collect(s.project(), s.path(), out, {}, [&](double) {
        ++calls;
        return true;
    });
    REQUIRE(res.ok);
    CHECK(calls >= 4);
    CHECK_EQ(res.copied, 4);
    CHECK(fs::exists(fs::u8path(out + "/Audio Files/Vln_01.wav")));
    CHECK(fs::exists(fs::u8path(out + "/Audio Files/Vln_01 2.wav")));
    CHECK(fs::exists(fs::u8path(out + "/Audio Files/Choir.wav")));
    CHECK(fs::exists(fs::u8path(out + "/Picture/Cut3.mov")));
    CHECK_EQ(fs::file_size(fs::u8path(out + "/Audio Files/Vln_01 2.wav")), static_cast<std::uintmax_t>(500));
    CHECK_EQ(res.project_file, out + "/Cue 1M3_ Arrival.vcc2");
    const auto report = files::read_file(out + "/Package Report.txt");
    REQUIRE(report);
    CHECK(report->find("MISSING (1)") != std::string::npos);
    CHECK(report->find("Lost.wav") != std::string::npos);
    CHECK(report->find("Kontakt 8 (Native Instruments) [VST3] - Horn") != std::string::npos);
    CHECK(fs::exists(fs::u8path(out + "/package.json")));

    // Moved anywhere, the package opens with every collected file found.
    const auto moved = tmp("pkg-out") + "/Moved";
    fs::rename(fs::u8path(out), fs::u8path(moved));
    Session r(catalogs());
    REQUIRE(r.open(moved + "/Cue 1M3_ Arrival.vcc2"));
    int found = 0, lost = 0;
    for (const auto& c : r.data().find_track(vox)->active_version()->audio_clips)
        (fs::exists(fs::u8path(r.resolve_audio_path(c.file))) ? found : lost) += 1;
    CHECK_EQ(found, 4);
    CHECK_EQ(lost, 1);
    CHECK_EQ(r.data().pictures.front().path, std::string("Picture/Cut3.mov"));
    CHECK_EQ(session::package::resolve(r.data().pictures.front().path, r.path()), moved + "/Picture/Cut3.mov");
    CHECK_EQ(r.data().find_track(horn)->instrument_plugin.state_base64, std::string("c3RhdGU="));
    // The original project is untouched.
    CHECK_EQ(s.data().find_track(vox)->active_version()->audio_clips.front().file, std::string("Audio Files/Vln_01.wav"));

    // Without the picture; into a folder that is not empty: refused.
    auto res2 = session::package::collect(s.project(), s.path(), tmp("pkg-out") + "/NoPic", {false});
    REQUIRE(res2.ok);
    CHECK(!fs::exists(fs::u8path(tmp("pkg-out") + "/NoPic/Picture/Cut3.mov")));
    auto res3 = session::package::collect(s.project(), s.path(), tmp("pkg-out") + "/NoPic");
    CHECK(!res3.ok);
    CHECK(res3.error.find("not empty") != std::string::npos);
    // Cancelled: nothing left behind.
    auto res4 = session::package::collect(s.project(), s.path(), tmp("pkg-out") + "/Cancelled", {},
                                          [](double) { return false; });
    CHECK(res4.cancelled);
    CHECK(!fs::exists(fs::u8path(tmp("pkg-out") + "/Cancelled")));
    CHECK_EQ(session::package::safe_name("a/b:c?"), std::string("a_b_c_"));
}

TEST_CASE("review: timecode comments are pinned in time, threaded, resolvable, saved and packaged")
{
    Session s(catalogs());
    s.new_empty();
    const auto horn = s.add_track("Horn", "brass.horn_f");
    const auto late = s.add_comment(12.5, "Horns too loud here", "Director", horn, "2026-11-15T10:00:00Z");
    const auto early = s.add_comment(3.0, "Love this opening", "Director");
    REQUIRE(!late.empty());
    REQUIRE(s.data().comments.size() == 2u);
    CHECK_EQ(s.data().comments[0].id, early);  // kept in time order
    CHECK_EQ(s.data().comments[1].track_id, horn);
    CHECK_EQ(s.data().comments[1].created, std::string("2026-11-15T10:00:00Z"));
    CHECK(s.data().comments[0].created.size() == 20u);  // now, UTC
    CHECK(s.add_comment(1.0, "", "x").empty());           // nothing to say: nothing added
    CHECK(s.add_comment(1.0, "On a gone track", "x", "no-track") != "");
    CHECK(s.data().comments[0].track_id.empty());          // an unknown track: the whole project
    CHECK(s.remove_comment(s.data().comments[0].id));

    // A tempo change does not move them (they are in picture time).
    CHECK(s.set_tempo(90.0));
    CHECK(std::abs(s.find_comment(late)->seconds - 12.5) < 1e-9);
    CHECK(s.reply_to_comment(late, "Composer", "Pulled them back 3 dB"));
    CHECK(s.set_comment_resolved(late, true));
    CHECK(!s.set_comment_resolved(late, true));  // unchanged: no step
    CHECK(s.edit_comment(early, "Love this opening - keep it"));
    CHECK(s.move_comment(early, 2.0));
    CHECK_EQ(s.project().undo_label(), std::string("Move comment"));
    CHECK(s.undo());
    CHECK(std::abs(s.find_comment(early)->seconds - 3.0) < 1e-9);
    // Approval does not lock comments: they are about the music, not in it.
    const auto v = s.data().find_track(horn)->active_version_id;
    CHECK(s.set_version_approved(horn, v, true));
    CHECK(s.reply_to_comment(late, "Director", "Approved"));

    const auto path = tmp("comments.vcc2");
    REQUIRE(s.save_as(path));
    Session r(catalogs());
    REQUIRE(r.open(path));
    const auto* c = r.find_comment(late);
    REQUIRE(c);
    CHECK(c->resolved);
    CHECK_EQ(c->author, std::string("Director"));
    REQUIRE(c->replies.size() == 2u);
    CHECK_EQ(c->replies[0].text, std::string("Pulled them back 3 dB"));
    CHECK_EQ(r.find_comment(early)->text, std::string("Love this opening - keep it"));
    // The package report counts them.
    session::package::Options o;
    const auto report = session::package::report_text(r.data(), session::package::inventory(r.data(), r.path()), o);
    CHECK(report.find("Review comments: 1 open, 1 resolved") != std::string::npos);
}
