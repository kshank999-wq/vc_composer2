// The desktop app's document controller, exercised without a window.
#include <filesystem>

#include "vcc/core/Files.h"
#include "vcc/editing/ScoreEditor.h"
#include "vcc/session/Session.h"
#include "vcc_test.h"

using namespace vcc;
using session::Change;
using session::Session;
using session::ProjectSetup;

namespace {
const session::Catalogs& catalogs()
{
    static auto c = session::Catalogs::load_from_directory(VCC_CONTENT_DIR);
    return *c;
}
std::string tmp(const std::string& name) { return std::string(VCC_TEST_TMP_DIR) + "/" + name; }
}  // namespace

TEST_CASE("catalogs load from disk and from in-memory documents identically")
{
    CHECK_EQ(catalogs().error_count(), 0);
    CHECK(catalogs().templates->all().size() >= 200);
    std::map<std::string, std::string> docs;
    for (const char* f : {"instruments/core.json", "instruments/traditional.json", "instruments/historical.json",
                          "articulations.json", "progressions.json", "ostinato_presets.json",
                          "templates/cinematic.json", "templates/modern.json"})
        docs[f] = *files::read_file(std::string(VCC_CONTENT_DIR) + "/" + f);
    auto mem = session::Catalogs::load_from_documents(docs);
    CHECK_EQ(mem->error_count(), 0);
    CHECK(mem->templates->get("cinematic.horror.psychological") != nullptr);
    auto bad = session::Catalogs::load_from_documents({{"progressions.json", "{ nope"}});
    CHECK(bad->error_count() >= 1);
}

TEST_CASE("listeners hear document, selection and file changes")
{
    Session s(catalogs());
    int doc = 0, sel = 0, file = 0;
    int id = s.add_listener([&](Change c) {
        if (c == Change::Document) ++doc;
        if (c == Change::Selection) ++sel;
        if (c == Change::File) ++file;
    });
    auto r = s.new_from_template("cinematic.suspense.tension");
    REQUIRE(r.ok);
    CHECK(doc >= 1);
    CHECK(!s.selected_track().empty());
    CHECK(s.dirty());
    CHECK(s.title().find('*') != std::string::npos);
    int before = doc;
    s.set_mute(s.selected_track(), true);
    CHECK_EQ(doc, before + 1);
    s.remove_listener(id);
    s.set_mute(s.selected_track(), false);
    CHECK_EQ(doc, before + 1);
}

TEST_CASE("fader moves coalesce into one undo step; mute/solo decide audibility")
{
    Session s(catalogs());
    REQUIRE(s.new_from_template("cinematic.romance.love").ok);
    auto tracks = s.music_tracks();
    REQUIRE(tracks.size() >= 2);
    std::string a = tracks[0]->id, b = tracks[1]->id;
    const double gain_before = tracks[0]->gain_db;  // (pointers into the document do not outlive an edit)
    for (int i = 0; i < 50; ++i) s.set_gain(a, -0.2 * i);
    CHECK_NEAR(s.data().find_track(a)->gain_db, -9.8, 1e-9);
    REQUIRE(s.undo());
    CHECK_NEAR(s.data().find_track(a)->gain_db, gain_before, 1e-9);
    s.set_solo(b, true);
    CHECK(!s.audible(*s.data().find_track(a)));
    CHECK(s.audible(*s.data().find_track(b)));
    s.set_mute(b, true);
    CHECK(!s.audible(*s.data().find_track(b)));
}

TEST_CASE("save, reopen, autosave and recovery")
{
    Session s(catalogs());
    REQUIRE(s.new_from_template("cinematic.mystery.investigation").ok);
    std::string err;
    CHECK(!s.save(&err));  // untitled
    std::string path = tmp("session.vcc2");
    REQUIRE(s.save_as(path, &err));
    CHECK(!s.dirty());
    CHECK_EQ(s.title(), std::string("session"));
    s.set_tempo(77);
    REQUIRE(s.write_autosave(VCC_TEST_TMP_DIR, &err));
    CHECK(Session::has_newer_autosave(path) || std::filesystem::exists(path + ".autosave"));
    Session other(catalogs());
    REQUIRE(other.open(path + ".autosave", &err));
    CHECK_NEAR(other.data().tempo.bpm_at(0), 77.0, 1e-9);
    REQUIRE(s.save(&err));
    CHECK(!std::filesystem::exists(path + ".autosave"));
    REQUIRE(other.open(path, &err));
    CHECK(!other.dirty());
    CHECK_EQ(other.data().tracks.size(), s.data().tracks.size());
}

TEST_CASE("score commits, generators and plugin refs go through one undo step each")
{
    Session s(catalogs());
    REQUIRE(s.new_from_template("cinematic.heroic.triumph").ok);
    std::string t = s.selected_track();
    REQUIRE(!t.empty());
    notation::Score working = s.data().find_track(t)->active_version()->score;
    editing::ScoreEditor ed(working);
    REQUIRE(ed.insert_step(notation::Step::G).succeeded());
    REQUIRE(s.commit_score(t, working, "Enter G"));
    CHECK_EQ(s.data().find_track(t)->active_version()->score.total_note_count(), 1u);
    CHECK_EQ(s.undo_label(), std::string("Enter G"));

    const auto* prog = catalogs().progressions.find("heroic_mixolydian");
    REQUIRE(prog);
    theory::ProgressionRenderOptions po;
    po.key = *theory::Key::parse("D major");
    po.start_bar = 3;
    auto render = theory::render_progression(*prog, s.data().meter, po);
    REQUIRE(render);
    auto r = s.send_progression(t, *render, prog->id, project::CollisionPolicy::Ask);
    CHECK(r.ok);
    CHECK_EQ(s.undo_label().find("chord_explorer"), 7u);  // "Insert chord_explorer:..."
    // The progression's chord symbols land over its bars, in the same undo step.
    REQUIRE(s.data().chords.size() == render->chords.size());
    CHECK_EQ(s.data().chords.front().tick, render->start);
    CHECK_EQ(s.data().chords.front().text, render->chords.front().chord.name());
    {
        const std::size_t before = s.project().history_size();
        s.undo();
        CHECK(s.data().chords.empty());
        CHECK_EQ(s.data().find_track(t)->active_version()->score.total_note_count(), 1u);
        s.redo();
        CHECK_EQ(s.project().history_size(), before);
        CHECK_EQ(s.data().chords.size(), render->chords.size());
    }

    project::PluginRef ref{"VST3", "VST3-Test-1234", "Test Synth", "Acme", ""};
    REQUIRE(s.set_instrument_plugin(t, ref));
    CHECK(s.data().find_track(t)->assignment.status == project::ResolutionStatus::Exact);
    std::size_t steps = s.project().history_size();
    s.store_plugin_state(t, "c3RhdGU=");
    CHECK_EQ(s.project().history_size(), steps);
    CHECK_EQ(s.data().find_track(t)->instrument_plugin.state_base64, std::string("c3RhdGU="));
    std::string path = tmp("plugin.vcc2");
    REQUIRE(s.save_as(path));
    Session again(catalogs());
    REQUIRE(again.open(path));
    CHECK_EQ(again.data().find_track(t)->instrument_plugin.name, std::string("Test Synth"));
    CHECK_EQ(again.data().find_track(t)->instrument_plugin.state_base64, std::string("c3RhdGU="));
}

TEST_CASE("adding and removing tracks keeps a valid selection")
{
    Session s(catalogs());
    s.new_empty();
    CHECK(s.selected_track().empty());
    std::string a = s.add_track("Flute", "woodwinds.flute");
    CHECK_EQ(s.selected_track(), a);
    std::string b = s.add_track("Snare", "percussion.snare_drum");
    CHECK_EQ(s.data().find_track(b)->midi_channel, 9);
    CHECK(s.remove_track(b));
    CHECK_EQ(s.selected_track(), a);
    CHECK(s.content_end() >= s.data().meter.bar_start(33));
    auto range = s.range_for(*s.data().find_track(a));
    CHECK_EQ(range.first, 59);
}

TEST_CASE("a reference track is selectable, holds no MIDI and stays out of deliverables")
{
    Session s(catalogs());
    REQUIRE(s.new_from_template("cinematic.noir.detective").ok);
    std::string ref = s.add_reference_track("Temp Score");
    CHECK_EQ(s.selected_track(), ref);
    const auto* t = s.data().find_track(ref);
    REQUIRE(t);
    CHECK(t->kind == project::TrackKind::Reference);
    CHECK(!t->include_in_export);
    CHECK(!s.insert_notes(ref, 0, kTicksPerWhole, {{0, 100, 60, 80, 0, {}}}, project::CollisionPolicy::Ask, "x").ok);
    for (const auto* e : s.project().export_tracks()) CHECK(e->id != ref);
}

TEST_CASE("console settings, buses and sends round-trip and undo as one step per control")
{
    Session s(catalogs());
    s.new_empty();
    const std::string t = s.add_track("Violins", "strings.violins");
    const std::string fx = s.add_bus("Hall", "fx");
    const std::string grp = s.add_bus("Strings", "group");
    REQUIRE(!fx.empty());
    REQUIRE(!grp.empty());
    CHECK_EQ(s.data().find_bus(fx)->inserts.size(), 1u);
    CHECK(s.add_bus("x", "master").empty());

    // A knob drag: many calls, one undo step.
    for (int i = 1; i <= 10; ++i)
        CHECK(s.update_track_mix(t, "HMF gain Violins", [&](project::Track& x) { x.strip.hmf.gain_db = i * 0.5; }));
    CHECK(s.update_track_mix(t, "Send Violins", [&](project::Track& x) {
        x.sends.emplace_back(fx, -6.0);
        x.sends.back().pre_fader = true;
        x.output_bus_id = grp;
        x.strip.eq_on = true;
        x.strip.trim_db = 99.0;  // clamped
        project::InsertSlot comp;
        comp.kind = "compressor";
        comp.suggestion = false;
        comp.params["ratio"] = 4.0;
        x.inserts.push_back(comp);
    }));
    CHECK_EQ(s.data().find_track(t)->strip.trim_db, 20.0);
    CHECK(s.update_bus(grp, "Pan Strings", [](project::Bus& b) {
        b.pan = -0.25;
        b.solo = true;
        b.kind = "fx";  // ignored
    }));
    CHECK_EQ(s.data().find_bus(grp)->kind, std::string("group"));

    REQUIRE(s.save_as(tmp("console.vcc2")));
    Session r(catalogs());
    REQUIRE(r.open(tmp("console.vcc2")));
    const auto* rt = r.data().find_track(t);
    REQUIRE(rt);
    CHECK_EQ(rt->strip.hmf.gain_db, 5.0);
    CHECK(rt->strip.eq_on);
    REQUIRE(rt->sends.size() == 1u);
    CHECK(rt->sends[0].pre_fader);
    CHECK_EQ(rt->sends[0].level_db, -6.0);
    CHECK_EQ(rt->output_bus_id, grp);
    REQUIRE(rt->inserts.size() == 1u);
    CHECK_EQ(rt->inserts[0].params.at("ratio"), 4.0);
    CHECK_EQ(r.data().find_bus(grp)->pan, -0.25);
    CHECK(r.data().find_bus(grp)->solo);

    // Undo peels the bus pan, the send/routing step, then the whole knob drag.
    CHECK(s.undo());
    CHECK(s.undo());
    CHECK_EQ(s.data().find_track(t)->strip.hmf.gain_db, 5.0);
    CHECK(s.undo());
    CHECK_EQ(s.data().find_track(t)->strip.hmf.gain_db, 0.0);
    CHECK(s.redo());
    CHECK(s.redo());

    // Removing a bus re-routes what fed it and drops sends to it.
    CHECK(s.remove_bus(grp));
    CHECK(s.remove_bus(fx));
    CHECK_EQ(s.data().find_track(t)->output_bus_id, std::string("bus-master"));
    CHECK(s.data().find_track(t)->sends.empty());
    CHECK(!s.remove_bus("bus-master"));
}

TEST_CASE("recorded clips land on the playlist as takes, one undo step per pass")
{
    Session s(catalogs());
    s.new_empty();
    const std::string a = s.add_audio_track("Vox", false);
    const std::string b = s.add_audio_track("Gtr", true);
    CHECK(s.data().find_track(a)->kind == project::TrackKind::Audio);
    CHECK(s.data().find_track(b)->input_stereo);
    CHECK(s.audio_folder().empty());

    s.set_record_armed(a, true);
    CHECK(s.data().find_track(a)->record_armed);
    CHECK(!s.can_undo() || s.undo_label() != "Arm");

    project::AudioClip c;
    c.file = "Audio Files/Vox_01.wav";
    c.start_seconds = 2.0;
    c.length = 4.0;
    c.channels = 1;
    project::AudioClip d = c;
    d.file = "Audio Files/Gtr_01.wav";
    d.channels = 2;
    auto ids = s.add_audio_clips({{a, c}, {b, d}}, "Record");
    REQUIRE(ids.size() == 2u);
    CHECK_EQ(s.undo_label(), std::string("Record"));

    // A second pass over the first: the first becomes take lane 1.
    project::AudioClip c2 = c;
    c2.file = "Audio Files/Vox_02.wav";
    c2.start_seconds = 3.0;
    auto ids2 = s.add_audio_clips({{a, c2}}, "Record");
    REQUIRE(ids2.size() == 1u);
    const auto& clips = s.data().find_track(a)->active_version()->audio_clips;
    REQUIRE(clips.size() == 2u);
    CHECK_EQ(clips[0].id, ids2[0]);
    CHECK_EQ(clips[0].lane, 0);
    CHECK_EQ(clips[1].lane, 1);

    CHECK(s.update_audio_clip(a, ids2[0], "Fade", [](project::AudioClip& x) {
        x.fade_in = 10.0;  // clamped to the clip
        x.gain_db = -3.0;
    }));
    CHECK_EQ(s.data().find_track(a)->active_version()->audio_clips[0].fade_in, 4.0);
    CHECK(!s.add_audio_clips({{a, project::AudioClip{}}}, "Record").size());

    std::filesystem::create_directories(tmp("audio"));
    REQUIRE(s.save_as(tmp("audio/session.vcc2")));
    CHECK_EQ(s.audio_folder(), tmp("audio") + "/Audio Files");
    CHECK_EQ(s.resolve_audio_path("Audio Files/x.wav"), tmp("audio") + "/Audio Files/x.wav");
    CHECK_EQ(s.resolve_audio_path("/abs/x.wav"), std::string("/abs/x.wav"));
    Session r(catalogs());
    REQUIRE(r.open(tmp("audio/session.vcc2")));
    const auto& rc = r.data().find_track(a)->active_version()->audio_clips;
    REQUIRE(rc.size() == 2u);
    CHECK_EQ(rc[0].gain_db, -3.0);
    CHECK_EQ(rc[0].file, std::string("Audio Files/Vox_02.wav"));
    CHECK(!r.data().find_track(a)->record_armed);  // arming is not saved state worth trusting

    CHECK(s.remove_audio_clip(a, ids2[0]));
    CHECK(s.undo());
    CHECK(s.undo());  // the fade
    CHECK(s.undo());  // the second pass
    CHECK_EQ(s.data().find_track(a)->active_version()->audio_clips.size(), 1u);
    CHECK_EQ(s.data().find_track(a)->active_version()->audio_clips[0].lane, 0);
}

TEST_CASE("the delay-compensation switch is saved with the project and undoable")
{
    session::Session s(catalogs());
    s.new_empty();
    CHECK(s.data().delay_compensation);  // on for a new project
    CHECK(!s.set_delay_compensation(true));  // already on: no undo step
    REQUIRE(s.set_delay_compensation(false));
    CHECK(!s.data().delay_compensation);
    CHECK_EQ(s.undo_label(), std::string("Delay compensation off"));
    CHECK(s.dirty());
    REQUIRE(s.undo());
    CHECK(s.data().delay_compensation);
    REQUIRE(s.redo());

    const auto path = tmp("pdc-off.vcc2.json");
    std::string err;
    REQUIRE(s.save_as(path, &err));
    session::Session r(catalogs());
    REQUIRE(r.open(path, &err));
    CHECK(!r.data().delay_compensation);

    // A file from before the switch was saved has it on.
    auto doc = json::parse(*files::read_file(path));
    REQUIRE(doc);
    REQUIRE(doc->remove("delay_compensation"));
    REQUIRE(files::write_file_atomic(path, json::write(*doc)));
    session::Session old(catalogs());
    REQUIRE(old.open(path, &err));
    CHECK(old.data().delay_compensation);
}

TEST_CASE("one selection: notes give a range, edits prune it, a new track clears the notes")
{
    session::Session s(catalogs());
    s.new_empty();
    const std::string t = s.add_track("Flute", "woodwinds.flute");
    notation::Score score = s.data().find_track(t)->active_version()->score;
    editing::ScoreEditor editor(score);
    editor.set_cursor(editing::EditCursor{});
    editor.set_duration(notation::NoteValue::Quarter);
    std::vector<notation::EventId> ids;
    for (auto step : {notation::Step::C, notation::Step::D, notation::Step::E}) {
        notation::Pitch p;
        p.step = step;
        ids.push_back(editor.insert_pitch(p).new_event);
    }
    REQUIRE(s.commit_score(t, score, "Notes"));

    int notices = 0;
    const int id = s.add_listener([&](session::Change c) {
        if (c == session::Change::Selection) ++notices;
    });
    s.select_notes(t, {ids[1], ids[2], 999999});
    CHECK_EQ(s.selection().notes.size(), 2u);  // the unknown id is dropped
    CHECK_EQ(s.selection().from, kTicksPerQuarter);
    CHECK_EQ(s.selection().to, 3 * kTicksPerQuarter);
    CHECK_EQ(notices, 1);
    s.select_notes(t, {ids[2], ids[1]});  // the same selection: no notice
    CHECK_EQ(notices, 1);

    // Deleting a selected note takes it out of the selection.
    score = s.data().find_track(t)->active_version()->score;
    REQUIRE(score.remove_note(ids[2]));
    REQUIRE(s.commit_score(t, score, "Delete"));
    CHECK(s.selection().notes == std::vector<notation::EventId>{ids[1]});

    // The ruler range alone.
    s.set_selection_range(0, 4 * kTicksPerQuarter);
    CHECK(s.selection().has_range() && s.selection().notes.size() == 1);

    // Another track: its notes, not these.
    const std::string u = s.add_track("Oboe", "woodwinds.oboe");
    s.select_track(u);
    CHECK(s.selection().notes.empty());
    CHECK(s.selection().has_range());
    s.remove_listener(id);
}

TEST_CASE("new project setup: template defaults overridden, timecode and sample rate set, saved in its own folder")
{
    Session s(catalogs());
    const std::string root = std::string(VCC_TEST_TMP_DIR) + "/setup";
    std::filesystem::remove_all(root);
    ProjectSetup setup;
    setup.name = "1M1 Arrival";
    setup.folder = root;
    setup.template_id = "cinematic.suspense.tension";
    setup.key = "D minor";
    setup.meter = TimeSignature{6, 8};
    setup.tempo = 72.0;
    setup.frame_rate = FrameRate::Fps23_976;
    setup.start_timecode = "00:59:58:00";
    setup.sample_rate = 96000;
    const auto r = s.create_project(setup);
    REQUIRE(r.ok);
    CHECK_EQ(r.project_file, root + "/1M1 Arrival/1M1 Arrival.vcc2");
    CHECK(std::filesystem::exists(r.project_file));
    CHECK_EQ(s.path(), r.project_file);
    CHECK_EQ(s.data().name, std::string("1M1 Arrival"));
    CHECK_EQ(s.data().key, std::string("D minor"));
    CHECK(s.data().meter.at_bar(1) == (TimeSignature{6, 8}));
    CHECK_NEAR(s.data().tempo.bpm_at(0), 72.0, 1e-9);
    CHECK(s.data().frame_rate == FrameRate::Fps23_976);
    CHECK_EQ(s.data().start_timecode, std::string("00:59:58:00"));
    CHECK_EQ(s.data().sample_rate, 96000);
    CHECK(s.music_tracks().size() >= 3u);
    CHECK(!s.can_undo());  // a new document, not an edit
    // Every staff has the project's key and meter.
    for (const auto* t : s.music_tracks())
        if (const auto* v = t->active_version())
            CHECK(v->score.time_signature_at(0).numerator == 6);

    // Never over an existing project; bad settings refused before anything is made.
    CHECK(!s.create_project(setup).ok);
    ProjectSetup bad = setup;
    bad.name = "Other";
    bad.key = "H major";
    CHECK(!s.create_project(bad).ok);
    bad.key.reset();
    bad.start_timecode = "not a timecode";
    CHECK(!s.create_project(bad).ok);
    CHECK(!std::filesystem::exists(root + "/Other"));

    // Empty project, template defaults kept when nothing is overridden.
    ProjectSetup plain;
    plain.name = "Sketch";
    REQUIRE(s.create_project(plain).ok);
    CHECK(s.path().empty());
    CHECK(s.music_tracks().empty());
    CHECK_EQ(s.data().name, std::string("Sketch"));
    ProjectSetup tpl;
    tpl.name = "T";
    tpl.template_id = "cinematic.suspense.tension";
    REQUIRE(s.create_project(tpl).ok);
    CHECK(s.data().tempo.bpm_at(0) > 0);
}

TEST_CASE("mix groups link faders, mutes, solos and arms; VCAs trim members and travel with the project")
{
    Session s(catalogs());
    s.new_empty();
    const auto a = s.add_track("Violins I", "strings.violins_1");
    const auto b = s.add_track("Violins II", "strings.violins_2");
    const auto c = s.add_track("Horn", "brass.horn_f");
    const auto grp = s.add_mix_group("Violins", {a, b, "nope"});
    REQUIRE(!grp.empty());
    CHECK_EQ(s.data().mix_groups.front().members.size(), 2u);  // unknown tracks are left out
    s.set_gain(b, -6.0);  // relative: a starts at 0
    CHECK_NEAR(s.data().find_track(a)->gain_db, -6.0, 1e-9);
    s.set_gain(a, -3.0);
    CHECK_NEAR(s.data().find_track(b)->gain_db, -3.0, 1e-9);
    CHECK_NEAR(s.data().find_track(c)->gain_db, 0.0, 1e-9);
    s.set_mute(a, true);
    CHECK(s.data().find_track(b)->mute);
    CHECK(!s.data().find_track(c)->mute);
    s.undo();
    CHECK(!s.data().find_track(a)->mute);
    CHECK(!s.data().find_track(b)->mute);
    s.set_record_armed(a, true);
    CHECK(!s.data().find_track(b)->record_armed);  // arm is not linked by default
    s.update_mix_group(grp, "Link arm", [](project::MixGroup& g) { g.link_arm = true; });
    s.set_record_armed(a, false);
    s.set_record_armed(a, true);
    CHECK(s.data().find_track(b)->record_armed);
    s.update_mix_group(grp, "Group off", [](project::MixGroup& g) { g.active = false; });
    s.set_solo(a, true);
    CHECK(!s.data().find_track(b)->solo);

    const auto vca = s.add_vca("Strings", {a, b});
    REQUIRE(!vca.empty());
    s.set_vca_gain(vca, -10.0);
    CHECK_NEAR(s.data().vca_trim_db(a), -10.0, 1e-9);
    CHECK_NEAR(s.data().vca_trim_db(c), 0.0, 1e-9);
    CHECK_NEAR(s.data().find_track(a)->gain_db, -3.0, 1e-9);  // the member's own fader is untouched
    s.update_vca(vca, "Mute VCA", [](project::Vca& v) { v.mute = true; });
    CHECK(s.data().vca_muted(b));
    CHECK(!s.data().vca_muted(c));

    const auto cue = s.add_bus("Drummer cans", "cue");
    REQUIRE(!cue.empty());
    CHECK_EQ(s.data().find_bus(cue)->output, std::string("hw:1"));
    CHECK(s.add_send(c, cue, true, -6.0));

    const std::string path = std::string(VCC_TEST_TMP_DIR) + "/groups.vcc2";
    REQUIRE(s.save_as(path));
    Session r(catalogs());
    REQUIRE(r.open(path));
    REQUIRE(r.data().vcas.size() == 1u);
    CHECK(r.data().vcas[0].mute);
    CHECK_NEAR(r.data().vcas[0].gain_db, -10.0, 1e-9);
    REQUIRE(r.data().mix_groups.size() == 1u);
    CHECK(!r.data().mix_groups[0].active);
    CHECK(r.data().mix_groups[0].link_arm);
    CHECK_EQ(r.data().find_bus(cue)->kind, std::string("cue"));
    CHECK(s.remove_vca(vca));
    CHECK(s.remove_mix_group(grp));
    CHECK(s.data().vcas.empty());
}

#include "vcc/session/Retrospective.h"

TEST_CASE("retrospective MIDI: the last performance, where it was played or from the playhead")
{
    const TempoMap tempo(120.0);  // a quarter = 0.5 s
    const Ticks Q = kTicksPerQuarter;
    std::vector<session::LiveMidi> ev = {
        // An earlier noodle, then a long pause...
        {0x90, 60, 90, 0.0, -1}, {0x80, 60, 0, 0.4, -1},
        // ...then the phrase worth keeping, played while stopped.
        {0x90, 64, 100, 10.0, -1}, {0x80, 64, 0, 10.5, -1}, {0xB0, 64, 127, 10.5, -1},
        {0x90, 67, 80, 11.0, -1}, {0x90, 71, 70, 11.0, -1},  // 71 is still held
        {0x80, 67, 0, 12.0, -1}};
    auto r = session::capture_performance(ev, tempo, 4 * Q);
    REQUIRE(r.has_value());
    REQUIRE(r->notes.size() == 3u);
    CHECK_EQ(r->notes[0].pitch, 64);
    CHECK_EQ(r->notes[0].start, 4 * Q);       // from the playhead
    CHECK_EQ(r->notes[0].duration, Q);         // 0.5 s at 120 BPM
    CHECK_EQ(r->notes[1].start, 6 * Q);
    CHECK_EQ(r->notes[0].velocity, 100);
    const auto held = std::find_if(r->notes.begin(), r->notes.end(), [](const NoteEvent& n) { return n.pitch == 71; });
    REQUIRE(held != r->notes.end());
    CHECK_EQ(held->end(), 8 * Q);              // closed at the last event
    REQUIRE(r->controllers.size() == 1u);
    CHECK_EQ(r->controllers[0].controller, 64);
    CHECK_EQ(r->start, 4 * Q);
    CHECK(r->end >= 8 * Q);

    // Played along with the transport: the notes land where they were heard.
    std::vector<session::LiveMidi> along = {{0x90, 60, 90, 0.0, 10 * Q}, {0x80, 60, 0, 0.25, 10 * Q + Q / 2}};
    r = session::capture_performance(along, tempo, 0);
    REQUIRE(r.has_value());
    CHECK_EQ(r->notes[0].start, 10 * Q);
    CHECK_EQ(r->notes[0].duration, Q / 2);
    CHECK(!session::capture_performance({{0xB0, 1, 10, 0.0, -1}}, tempo, 0).has_value());

    // Into a track as a region, one undo step.
    Session s(catalogs());
    s.new_empty();
    const auto t = s.add_track("Piano", "keys.piano");
    const auto ids = s.add_recorded_regions({{t, *session::capture_performance(ev, tempo, 0)}}, "Capture MIDI", false);
    CHECK_EQ(ids.size(), 1u);
    CHECK_EQ(s.undo_label(), std::string("Capture MIDI"));
}

TEST_CASE("clip groups move together across tracks; regions follow by time")
{
    Session s(catalogs());
    s.new_empty();
    const auto a = s.add_audio_track("Dialogue", false);
    const auto b = s.add_audio_track("FX", false);
    const auto m = s.add_track("Piano", "keys.piano");
    project::AudioClip c;
    c.file = "Audio Files/x.wav";
    c.length = 2.0;
    c.start_seconds = 1.0;
    const auto ca = s.add_audio_clips({{a, c}}, "Import", false).front();
    c.start_seconds = 3.0;
    const auto cb = s.add_audio_clips({{b, c}}, "Import", false).front();
    project::MidiRegion r;
    r.start = s.data().tempo.tick_at_seconds(1.0);
    r.end = r.start + kTicksPerQuarter;
    r.notes.push_back({r.start, kTicksPerQuarter, 60, 90, 0, {}});
    const auto rm = s.add_recorded_regions({{m, r}}, "Record", false).front();
    const auto g = s.group_items({{a, ca}, {b, cb}, {m, rm}});
    REQUIRE(!g.empty());
    CHECK_EQ(s.undo_label(), std::string("Group"));

    s.update_audio_clip(a, ca, "Move clip", [](project::AudioClip& x) { x.start_seconds += 0.5; });
    auto clip_at = [&](const std::string& t, const std::string& id) {
        for (const auto& x : s.data().find_track(t)->active_version()->audio_clips)
            if (x.id == id) return x.start_seconds;
        return -1.0;
    };
    CHECK_NEAR(clip_at(a, ca), 1.5, 1e-9);
    CHECK_NEAR(clip_at(b, cb), 3.5, 1e-9);
    const auto& reg = s.data().find_track(m)->active_version()->regions.front();
    CHECK_NEAR(s.data().tempo.seconds_at(reg.start), 1.5, 1e-6);
    CHECK_EQ(reg.notes.front().start, reg.start);
    s.undo();
    CHECK_NEAR(clip_at(b, cb), 3.0, 1e-9);  // one undo step for the whole group

    s.nudge_audio_clips(b, {cb}, -0.25);
    CHECK_NEAR(clip_at(a, ca), 0.75, 1e-9);
    CHECK(s.ungroup_items({{a, ca}}));
    s.nudge_audio_clips(b, {cb}, 0.25);
    CHECK_NEAR(clip_at(a, ca), 0.75, 1e-9);  // ungrouped: alone
    CHECK(s.group_items({{a, ca}}).empty());  // a group needs two

    // Saved and reopened, the group holds.
    s.group_items({{a, ca}, {b, cb}});
    const std::string path = std::string(VCC_TEST_TMP_DIR) + "/clipgroups.vcc2";
    REQUIRE(s.save_as(path));
    Session o(catalogs());
    REQUIRE(o.open(path));
    CHECK(!o.data().find_track(a)->active_version()->audio_clips.front().group_id.empty());
}
