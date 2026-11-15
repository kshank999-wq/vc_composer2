// Director review (A/B auditions, version comparison, approval) and project
// packaging (Main Spec §15, §16).
#include <filesystem>
#include <set>

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

#include "vcc/theory/Progression.h"

TEST_CASE("progressions: any chord list is voice-led and played in a rhythm")
{
    using namespace vcc::theory;
    MeterMap meter(TimeSignature{4, 4});
    const Key key = *Key::parse("C major");
    std::vector<Chord> chords;
    for (const char* s : {"C", "Am", "Fm", "G"}) chords.push_back(*parse_chord_symbol(s));
    std::vector<Ticks> starts, lengths;
    for (int i = 0; i < 4; ++i) {
        starts.push_back(i * kTicksPerWhole);
        lengths.push_back(kTicksPerWhole);
    }
    ProgressionRenderOptions o;
    o.key = key;
    o.voicing.voice_lead = true;
    auto led = realize_chords(chords, starts, lengths, {"I", "vi", "iv", "V"}, meter, o);
    REQUIRE(led.chords.size() == 4u);
    CHECK_EQ(led.start, 0);
    CHECK_EQ(led.end, 4 * kTicksPerWhole);
    CHECK_EQ(led.chords[2].numeral, std::string("iv"));
    // Every voicing holds its chord's notes.
    for (const auto& rc : led.chords) {
        std::set<int> want, got;
        for (const auto& t : rc.chord.tones()) want.insert(t.pitch_class());
        for (int n : rc.voicing.notes) got.insert(((n % 12) + 12) % 12);
        CHECK(want == got);
    }
    // Voice-led moves less than root position each time.
    o.voicing.voice_lead = false;
    auto blocky = realize_chords(chords, starts, lengths, {}, meter, o);
    int led_moves = 0, block_moves = 0;
    for (std::size_t i = 1; i < 4; ++i) {
        led_moves += voice_movement(led.chords[i - 1].voicing, led.chords[i].voicing);
        block_moves += voice_movement(blocky.chords[i - 1].voicing, blocky.chords[i].voicing);
    }
    CHECK(led_moves < block_moves);
    // Rhythms: block = one note per voice per chord; arpeggio = eighths.
    o.voicing.voice_lead = true;
    o.rhythm = ChordRhythm::Block;
    std::size_t voices = 0;
    for (const auto& rc : led.chords) voices += rc.voicing.notes.size();
    CHECK_EQ(realize_chords(chords, starts, lengths, {}, meter, o).notes.size(), voices);
    o.rhythm = ChordRhythm::Arpeggio;
    CHECK_EQ(realize_chords(chords, starts, lengths, {}, meter, o).notes.size(), 32u);
    o.rhythm = ChordRhythm::BeatPulse;
    CHECK_EQ(realize_chords(chords, starts, lengths, {}, meter, o).notes.size(), 4u * voices);
}

TEST_CASE("progressions: chords written into the score, split at barlines, tied, with symbols")
{
    Session s(catalogs());
    s.new_empty();
    const auto piano = s.add_track("Piano", "keys.piano");
    auto pitch = [](notation::Step st, int oct, int alt = 0) {
        notation::Pitch p;
        p.step = st;
        p.octave = static_cast<std::int8_t>(oct);
        p.alteration = static_cast<std::int8_t>(alt);
        return p;
    };
    using St = notation::Step;
    const std::vector<notation::Pitch> c_major{pitch(St::C, 4), pitch(St::E, 4), pitch(St::G, 4)};
    const std::vector<notation::Pitch> e_flat{pitch(St::E, 4, -1), pitch(St::G, 4), pitch(St::B, 4, -1)};
    // C for a bar and a half (a whole tied to a half), then Eb for two beats.
    std::vector<Session::WrittenEvent> ev{{0, 6 * kTicksPerQuarter, c_major}, {6 * kTicksPerQuarter, 2 * kTicksPerQuarter, e_flat}};
    const auto steps = s.project().history_size();
    auto r = s.write_events(piano, ev, {{0, "C"}, {6 * kTicksPerQuarter, "Eb"}}, project::CollisionPolicy::Ask, "Put progression");
    REQUIRE(r.ok);
    CHECK_EQ(s.project().history_size(), steps + 1);
    const auto& score = s.data().find_track(piano)->active_version()->score;
    std::vector<const notation::ScoreNote*> notes;
    for (const auto& n : score.notes(0))
        if (!n.is_rest) notes.push_back(&n);
    REQUIRE(notes.size() == 9u);
    int tied = 0, whole = 0, half = 0, flats = 0;
    for (const auto* n : notes) {
        tied += n->tied_to_next ? 1 : 0;
        whole += n->duration.value == notation::NoteValue::Whole && n->start_tick == 0 ? 1 : 0;
        half += n->duration.value == notation::NoteValue::Half ? 1 : 0;
        flats += n->pitch.alteration == -1 ? 1 : 0;
    }
    CHECK_EQ(whole, 3);
    CHECK_EQ(tied, 3);       // the whole notes tie into bar 2
    CHECK_EQ(half, 6);       // C's half in bar 2, and the Eb chord
    CHECK_EQ(flats, 2);      // Eb and Bb spelled as given, not D# and A#
    REQUIRE(s.data().chords.size() == 2u);
    CHECK_EQ(s.data().chords[1].text, std::string("Eb"));
    // Over it again: asked; Merge writes voice 2; Replace replaces.
    auto again = s.write_events(piano, ev, {}, project::CollisionPolicy::Ask, "Put progression");
    CHECK(again.needs_decision);
    CHECK(s.write_events(piano, ev, {}, project::CollisionPolicy::Merge, "Put progression").ok);
    std::size_t in_voice2 = 0;
    for (const auto& n : s.data().find_track(piano)->active_version()->score.notes(0))
        if (n.voice == 1 && !n.is_rest) ++in_voice2;
    CHECK_EQ(in_voice2, 9u);
    CHECK(s.undo());
    CHECK(s.undo());
    std::size_t left = 0;
    for (const auto& n : s.data().find_track(piano)->active_version()->score.notes(0)) left += n.is_rest ? 0 : 1;
    CHECK_EQ(left, 0u);
    CHECK(s.data().chords.empty());
    // A piano's grand staff: below middle C goes on the bass staff.
    if (s.data().find_track(piano)->active_version()->score.staff_count() >= 2) {
        const std::vector<notation::Pitch> with_bass{pitch(St::C, 3), pitch(St::E, 4), pitch(St::G, 4)};
        REQUIRE(s.write_events(piano, {{0, 4 * kTicksPerQuarter, with_bass}}, {}, project::CollisionPolicy::Replace, "x").ok);
        const auto& sc = s.data().find_track(piano)->active_version()->score;
        std::size_t treble = 0, bass = 0;
        for (const auto& n : sc.notes(0)) treble += n.is_rest ? 0 : 1;
        for (const auto& n : sc.notes(1)) bass += n.is_rest ? 0 : 1;
        CHECK_EQ(treble, 2u);
        CHECK_EQ(bass, 1u);
        CHECK(s.undo());
    }
    // 3/4: a bar is a dotted half.
    s.set_meter(TimeSignature{3, 4});
    REQUIRE(s.write_events(piano, {{0, 3 * kTicksPerQuarter, c_major}}, {}, project::CollisionPolicy::Replace, "x").ok);
    const auto& n0 = s.data().find_track(piano)->active_version()->score.notes(0);
    bool dotted_half = false;
    for (const auto& n : n0) dotted_half = dotted_half || (n.duration.value == notation::NoteValue::Half && n.duration.dots == 1);
    CHECK(dotted_half);
}

#include "vcc/session/Relink.h"

TEST_CASE("relink: missing media are found by name in searched folders and relinked in one undo step")
{
    const std::string root = tmp("relink");
    fs::remove_all(fs::u8path(root));
    // Recorded on one machine...
    write_bytes(root + "/Old/Project/Audio Files/Vln_01.wav", 100, 'a');
    write_bytes(root + "/Old/Media/Choir.wav", 100, 'c');
    write_bytes(root + "/Old/Media/Horns.wav", 100, 'h');
    write_bytes(root + "/Old/Movies/Cut3.mov", 100, 'm');

    Session s(catalogs());
    s.new_empty();
    const auto vox = s.add_audio_track("Vox", true);
    auto clip = [](const std::string& file, double at, int lane = 0) {
        project::AudioClip c;
        c.file = file;
        c.start_seconds = at;
        c.length = 1.0;
        c.lane = lane;
        return c;
    };
    REQUIRE(s.add_audio_clips({{vox, clip(root + "/Old/Project/Audio Files/Vln_01.wav", 0.0)},
                               {vox, clip(root + "/Old/Media/Choir.wav", 2.0)},
                               {vox, clip(root + "/Old/Media/Choir.wav", 4.0, 1)},  // a take, same file
                               {vox, clip(root + "/Old/Media/Horns.wav", 6.0)}},
                              "Import", false)
                .size() == 4u);
    project::PictureVersion pic;
    pic.name = "Cut 3";
    pic.path = root + "/Old/Movies/Cut3.mov";
    s.import_picture(pic);
    fs::create_directories(fs::u8path(root + "/New/Project"));
    REQUIRE(s.save_as(root + "/New/Project/Cue.vcc2"));
    CHECK(session::relink::missing(s.data(), s.path()).empty());

    // ...and opened on another, where everything sits somewhere else.
    fs::create_directories(fs::u8path(root + "/New/Project/Audio Files"));
    fs::rename(fs::u8path(root + "/Old/Project/Audio Files/Vln_01.wav"), fs::u8path(root + "/New/Project/Audio Files/vln_01.WAV"));
    fs::create_directories(fs::u8path(root + "/Drive/Sessions/Media"));
    fs::rename(fs::u8path(root + "/Old/Media/Choir.wav"), fs::u8path(root + "/Drive/Sessions/Media/Choir.wav"));
    write_bytes(root + "/Drive/Backup/Choir.wav", 50, 'x');            // same name, other folder: ranks lower
    write_bytes(root + "/Drive/.Trash/Horns.wav", 100, 'h');           // hidden folders are not searched
    write_bytes(root + "/Drive/Bounces/Horns.aif", 100, 'h');          // converted: offered, never chosen
    fs::create_directories(fs::u8path(root + "/Drive/Picture A"));
    fs::create_directories(fs::u8path(root + "/Drive/Picture B"));
    fs::rename(fs::u8path(root + "/Old/Movies/Cut3.mov"), fs::u8path(root + "/Drive/Picture A/Cut3.mov"));
    write_bytes(root + "/Drive/Picture B/Cut3.mov", 100, 'n');         // two equally good: the user picks
    fs::remove_all(fs::u8path(root + "/Old"));

    const auto gone = session::relink::missing(s.data(), s.path());
    REQUIRE(gone.size() == 4u);  // Vln_01, Choir (used twice: one asset), Horns, the picture
    int calls = 0;
    const auto res = session::relink::search(gone, {root + "/New", root + "/Drive", root + "/New/Project"}, [&](int) {
        ++calls;
        return true;
    });
    CHECK(!res.cancelled);
    CHECK(calls >= 1);
    CHECK(res.files_scanned >= 6);
    CHECK_EQ(res.found(), 2);
    CHECK_EQ(res.ambiguous(), 2);
    std::map<std::string, std::string> pick;
    for (const auto& m : res.matches) {
        const std::string name = fs::u8path(m.stored).filename().u8string();
        if (name == "Vln_01.wav") {
            CHECK_EQ(m.candidates.size(), 1u);  // found once though two searched folders hold it
            CHECK_EQ(m.chosen, root + "/New/Project/Audio Files/vln_01.WAV");
        } else if (name == "Choir.wav") {
            REQUIRE(m.candidates.size() == 2u);
            CHECK_EQ(m.chosen, root + "/Drive/Sessions/Media/Choir.wav");  // its folder matches "Media"
        } else if (name == "Horns.wav") {
            REQUIRE(m.candidates.size() == 1u);
            CHECK(!m.candidates[0].same_name);
            CHECK(m.chosen.empty());
            pick[m.stored] = m.candidates[0].path;  // the user accepts the .aif
        } else {
            CHECK(m.kind == session::package::Asset::Kind::Picture);
            CHECK_EQ(m.candidates.size(), 2u);
            CHECK(m.chosen.empty());
            pick[m.stored] = root + "/Drive/Picture A/Cut3.mov";
        }
        if (!m.chosen.empty()) pick[m.stored] = m.chosen;
    }

    const auto undo_before = s.undo_label();
    CHECK_EQ(s.relink_media(pick), 5);  // four clips (two share Choir) and the picture
    CHECK_EQ(s.undo_label(), std::string("Relink media"));
    CHECK(session::relink::missing(s.data(), s.path()).empty());
    const auto& clips = s.data().find_track(vox)->active_version()->audio_clips;
    // Inside the project's folder: stored relative, so the project stays portable.
    CHECK_EQ(clips[0].file, std::string("Audio Files/vln_01.WAV"));
    CHECK_EQ(clips[1].file, root + "/Drive/Sessions/Media/Choir.wav");
    CHECK_EQ(clips[2].file, clips[1].file);
    CHECK_EQ(clips[3].file, root + "/Drive/Bounces/Horns.aif");
    CHECK_EQ(s.data().pictures.front().path, root + "/Drive/Picture A/Cut3.mov");
    CHECK_EQ(s.relink_media(pick), 0);  // nothing left to do: no empty undo step
    s.undo();
    CHECK_EQ(session::relink::missing(s.data(), s.path()).size(), 4u);
    CHECK_EQ(s.undo_label(), undo_before);
    s.redo();

    // An approved version's music is locked, but where its files are kept is not.
    const auto vid = s.data().find_track(vox)->active_version_id;
    REQUIRE(s.set_version_approved(vox, vid, true));
    std::map<std::string, std::string> back{{root + "/Drive/Bounces/Horns.aif", root + "/Drive/.Trash/Horns.wav"}};
    CHECK_EQ(s.relink_media(back), 1);

    CHECK_EQ(session::relink::stored_path(root + "/elsewhere/x.wav", ""), root + "/elsewhere/x.wav");
}
