// The desktop app's document controller, exercised without a window.
#include <filesystem>

#include "vcc/core/Files.h"
#include "vcc/editing/ScoreEditor.h"
#include "vcc/session/Session.h"
#include "vcc_test.h"

using namespace vcc;
using session::Change;
using session::Session;

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
    for (int i = 0; i < 50; ++i) s.set_gain(a, -0.2 * i);
    CHECK_NEAR(s.data().find_track(a)->gain_db, -9.8, 1e-9);
    REQUIRE(s.undo());
    CHECK_NEAR(s.data().find_track(a)->gain_db, tracks[0]->gain_db, 1e-9);
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
