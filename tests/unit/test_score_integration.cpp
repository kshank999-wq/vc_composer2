// The ported notation engine inside VC Composer 2 projects (Main Spec §8,
// §20, §22 work orders 21-22): written scores live in track versions, are
// set up from the instrument registry, follow the project's meter and key,
// survive save/reopen, and render to the MIDI the Sequencer and export see.
#include <set>

#include "vcc/core/Files.h"
#include "vcc/editing/ScoreEditor.h"
#include "vcc/notation/ScoreFile.h"
#include "vcc/project/Render.h"
#include "vcc/templates/Templates.h"
#include "vcc_test.h"

using namespace vcc;
using notation::Articulation;
using notation::Clef;

namespace {

const templates::InstrumentRegistry& instruments()
{
    static templates::InstrumentRegistry reg = [] {
        templates::InstrumentRegistry r;
        std::vector<templates::Diagnostic> d;
        r.load_directory(std::string(VCC_CONTENT_DIR) + "/instruments", d);
        return r;
    }();
    return reg;
}

/// A small template covering the interesting staff cases.
templates::Template staff_cases()
{
    static templates::TemplateRegistry reg(instruments());
    if (!reg.get("test.staff_cases")) {
        auto doc = json::parse(R"({"schema_version":1,"pack":"test","templates":[{
          "template_id":"test.staff_cases","schema_version":1,"content_version":"1.0.0",
          "display_name":"Staff cases","category":"cinematic","subcategory":"test","tags":[],
          "variant":"hybrid","description":"d","tempo_range_bpm":[60,90],"meter_options":["4/4"],
          "license_metadata":{},
          "tracks":[
            {"id":"viola","name":"Viola","instrument_standard_id":"strings.viola","role":"harmony"},
            {"id":"clarinet","name":"Clarinet in Bb","instrument_standard_id":"woodwinds.clarinet_bb","role":"melody"},
            {"id":"horn","name":"Horn in F","instrument_standard_id":"brass.horn_f","role":"melody"},
            {"id":"guitar","name":"Guitar","instrument_standard_id":"rhythm.acoustic_guitar","role":"rhythm"},
            {"id":"piano","name":"Piano","instrument_standard_id":"keys.piano","role":"harmony"},
            {"id":"bass","name":"Double Bass","instrument_standard_id":"strings.double_bass","role":"bass"},
            {"id":"kit","name":"Drums","instrument_standard_id":"rhythm.drum_kit","role":"percussion"}
          ]}]})");
        std::vector<templates::Diagnostic> d;
        reg.load_pack(*doc, "test", d);
        for (const auto& x : d) std::fprintf(stderr, "    %s\n", x.to_string().c_str());
    }
    return *reg.get("test.staff_cases");
}

const project::Track& by_name(const project::Project& p, const std::string& name)
{
    const project::Track* t = p.data().find_track_by_name(name);
    if (!t) std::fprintf(stderr, "    no track %s\n", name.c_str());
    return *t;
}

/// Writes notes through the ported editor, as keyboard entry would.
bool write_notes(project::Project& p, const std::string& track_id, const std::vector<notation::Pitch>& pitches,
                 Articulation articulation = Articulation::None, std::size_t articulated = 0)
{
    return p.edit("Enter notes", [&](project::ProjectData& d) {
        notation::Score& score = d.find_track(track_id)->active_version()->score;
        editing::ScoreEditor editor(score);
        editing::EditCursor cursor;
        cursor.tick = score.end_tick();
        editor.set_cursor(cursor);
        editor.set_duration(notation::NoteValue::Quarter);
        std::vector<notation::EventId> ids;
        for (const auto& pitch : pitches) {
            auto r = editor.insert_pitch(pitch);
            if (!r.succeeded()) return false;
            ids.push_back(r.new_event);
        }
        if (articulation != Articulation::None) {
            for (std::size_t i = 0; i < articulated && i < ids.size(); ++i) {
                editor.set_selection({ids[i]});
                if (!editor.toggle_articulation(articulation).succeeded()) return false;
            }
        }
        return true;
    });
}

notation::Pitch P(notation::Step step, int octave = 4)
{
    notation::Pitch p;
    p.step = step;
    p.octave = static_cast<std::int8_t>(octave);
    return p;
}

}  // namespace

TEST_CASE("templates give every track the staff its instrument reads")
{
    project::Project p;
    auto r = templates::instantiate(p, staff_cases(), instruments(), {});
    REQUIRE(r.ok);
    auto staff = [&](const char* name, std::size_t i = 0) {
        return by_name(p, name).active_version()->score.staff(i);
    };
    CHECK(staff("Viola").clef == Clef::Alto);
    CHECK_EQ(staff("Clarinet in Bb").total_transposition(), -2);
    CHECK_EQ(staff("Horn in F").total_transposition(), -7);
    CHECK(staff("Guitar").clef == Clef::TrebleOctaveDown);
    CHECK_EQ(staff("Guitar").total_transposition(), -12);
    CHECK_EQ(staff("Double Bass").total_transposition(), -12);
    CHECK(staff("Drums").clef == Clef::Percussion);
    const auto& piano = by_name(p, "Piano").active_version()->score;
    REQUIRE(piano.staff_count() == 2);
    CHECK(piano.staff(0).clef == Clef::Treble);
    CHECK(piano.staff(1).clef == Clef::Bass);
}

TEST_CASE("vertical slice: three notes with staccato and dynamics render to MIDI")
{
    project::Project p;
    std::string vln = p.add_track("Violin I", project::TrackKind::Instrument, "strings.violin");
    REQUIRE(write_notes(p, vln, {P(notation::Step::C), P(notation::Step::D), P(notation::Step::E)},
                        Articulation::Staccato, 2));
    REQUIRE(p.edit("Dynamics", [&](project::ProjectData& d) {
        auto& score = d.find_track(vln)->active_version()->score;
        score.add_dynamic({0, notation::DynamicLevel::Piano, 0, {}, 0});
        score.add_dynamic({2 * kTicksPerQuarter, notation::DynamicLevel::Fortissimo, 0, {}, 0});
        return true;
    }));
    auto r = project::render_track(*p.data().find_track(vln));
    REQUIRE(r.notes.size() == 3);
    CHECK_EQ(r.notes[0].pitch, 60);
    CHECK_EQ(r.notes[1].pitch, 62);
    CHECK_EQ(r.notes[2].pitch, 64);
    CHECK_EQ(r.notes[1].start, kTicksPerQuarter);
    CHECK_EQ(r.notes[2].start, 2 * kTicksPerQuarter);
    // Staccato is shorter than the plain third note.
    CHECK(r.notes[0].duration < r.notes[2].duration);
    CHECK(r.notes[1].duration < r.notes[2].duration);
    // p -> ff is audible: louder velocity or a rising dynamics controller.
    bool louder = r.notes[2].velocity > r.notes[0].velocity;
    if (!louder && r.controllers.size() >= 2) louder = r.controllers.back().value > r.controllers.front().value;
    CHECK(louder);
    // Undo removes the dynamics and then the notes, one step each.
    REQUIRE(p.undo());
    REQUIRE(p.undo());
    CHECK(project::render_track(*p.data().find_track(vln)).notes.empty());
}

TEST_CASE("a transposing instrument is written in its key and sounds in concert pitch")
{
    project::Project p;
    REQUIRE(templates::instantiate(p, staff_cases(), instruments(), {}).ok);
    const std::string cl = by_name(p, "Clarinet in Bb").id;
    const std::string gtr = by_name(p, "Guitar").id;
    REQUIRE(write_notes(p, cl, {P(notation::Step::D)}));
    REQUIRE(write_notes(p, gtr, {P(notation::Step::E)}));
    CHECK_EQ(project::render_track(*p.data().find_track(cl)).notes.at(0).pitch, 60);   // written D4 sounds C4
    CHECK_EQ(project::render_track(*p.data().find_track(gtr)).notes.at(0).pitch, 52);  // written E4 sounds E3
}

TEST_CASE("Main Spec 20: notes and articulations survive ten edit/save/reopen cycles")
{
    const std::string path = std::string(VCC_TEST_TMP_DIR) + "/score-cycles.vcc2.json";
    project::Project p;
    std::string hn = p.add_track("Horns", project::TrackKind::Instrument, "brass.horn_section");
    for (int cycle = 0; cycle < 10; ++cycle) {
        REQUIRE(write_notes(p, hn, {P(notation::Step::G), P(notation::Step::A)}, Articulation::Marcato, 1));
        const auto before = project::render_track(*p.data().find_track(hn));
        std::string err;
        REQUIRE(p.save(path, &err));
        auto loaded = project::Project::load(path, &err);
        if (!loaded) std::fprintf(stderr, "    %s\n", err.c_str());
        REQUIRE(loaded);
        CHECK(json::write(loaded->to_json()) == json::write(p.to_json()));
        const auto after = project::render_track(*loaded->data().find_track(hn));
        CHECK(before.notes == after.notes);
        p = std::move(*loaded);
    }
    const auto& score = p.data().find_track(hn)->active_version()->score;
    const auto& notes = score.notes(0);
    REQUIRE(notes.size() == 20u);
    std::set<notation::EventId> ids;
    for (std::size_t i = 0; i < notes.size(); ++i) {
        ids.insert(notes[i].id);
        bool marcato = (notes[i].articulations & notation::to_mask(Articulation::Marcato)) != 0;
        CHECK_EQ(marcato, i % 2 == 0);
    }
    CHECK_EQ(ids.size(), 20u);  // every note kept its own stable id
}

TEST_CASE("versions keep separate scores and switching never mixes them")
{
    project::Project p;
    std::string ob = p.add_track("Oboe", project::TrackKind::Instrument, "woodwinds.oboe");
    REQUIRE(write_notes(p, ob, {P(notation::Step::A), P(notation::Step::B)}));
    std::string v1 = p.data().find_track(ob)->active_version_id;
    auto v2 = p.duplicate_version(ob, "Darker");
    REQUIRE(v2);
    REQUIRE(p.edit("Transpose v2 down", [&](project::ProjectData& d) {
        auto& score = d.find_track(ob)->active_version()->score;
        editing::ScoreEditor editor(score);
        editor.select_all();
        return editor.transpose_selection_octaves(-1).succeeded();
    }));
    for (int i = 0; i < 5; ++i) {
        REQUIRE(p.set_active_version(ob, v1));
        CHECK_EQ(project::render_track(*p.data().find_track(ob)).notes.at(0).pitch, 69);
        REQUIRE(p.set_active_version(ob, *v2));
        CHECK_EQ(project::render_track(*p.data().find_track(ob)).notes.at(0).pitch, 57);
    }
}

TEST_CASE("the project's meter and key are written into every score")
{
    project::Project p;
    std::string a = p.add_track("Strings");
    std::string b = p.add_track("Brass");
    REQUIRE(p.edit("Meter and key", [](project::ProjectData& d) {
        d.meter = MeterMap(TimeSignature{3, 4});
        d.meter.set(5, TimeSignature{7, 8});
        d.key = "E minor";
        return true;
    }));
    for (const std::string& id : {a, b}) {
        const auto& score = p.data().find_track(id)->active_version()->score;
        REQUIRE(score.time_signature_changes().size() == 2u);
        CHECK_EQ(score.time_signature_changes()[1].tick, p.data().meter.bar_start(5));
        CHECK_EQ(score.time_signature_at(p.data().meter.bar_start(6)).numerator, 7);
        CHECK_EQ(static_cast<int>(score.key_signature_at(0).fifths), 1);
        CHECK(score.key_signature_at(0).is_minor);
        CHECK_EQ(score.measure_start_tick(5), p.data().meter.bar_start(5));
    }
    // Back to 4/4: the 7/8 change is gone from the scores too.
    REQUIRE(p.edit("Simplify", [](project::ProjectData& d) { d.meter = MeterMap(); return true; }));
    CHECK_EQ(p.data().find_track(a)->active_version()->score.time_signature_changes().size(), 1u);
}

TEST_CASE("a project saved before scores existed still loads; a corrupt score is refused")
{
    project::Project p;
    std::string t = p.add_track("Celli");
    REQUIRE(write_notes(p, t, {P(notation::Step::C, 3)}));
    json::Value doc = p.to_json();
    json::Value old = doc;
    old.object()["tracks"].array()[0].object()["versions"].array()[0].object().erase("score");
    auto loaded = project::Project::from_json(old);
    REQUIRE(loaded);
    CHECK_EQ(loaded->data().tracks[0].active_version()->score.total_note_count(), 0u);

    json::Value corrupt = doc;
    auto& score = corrupt.object()["tracks"].array()[0].object()["versions"].array()[0].object()["score"];
    score = json::Value("not a score");
    std::string err;
    CHECK(!project::Project::from_json(corrupt, &err));
    CHECK(err.find("score") != std::string::npos);
}

TEST_CASE("export carries written dynamics as controllers through an orchestral CC map")
{
    project::Project p;
    std::string st = p.add_track("Strings");
    REQUIRE(write_notes(p, st, {P(notation::Step::C), P(notation::Step::E), P(notation::Step::G)}));
    REQUIRE(p.edit("Map and dynamics", [&](project::ProjectData& d) {
        project::Track* t = d.find_track(st);
        t->expression_map = "orchestral_cc";
        t->active_version()->score.add_dynamic({0, notation::DynamicLevel::Piano, 0, {}, 0});
        t->active_version()->score.add_hairpin({0, 3 * kTicksPerQuarter, notation::HairpinKind::Crescendo});
        return true;
    }));
    auto r = project::render_track(*p.data().find_track(st));
    CHECK(!r.controllers.empty());
    std::string smf = project::export_project_midi(p);
    CHECK(smf.find('\xB0') != std::string::npos);
    std::string err;
    CHECK(files::write_file_atomic(std::string(VCC_TEST_TMP_DIR) + "/score-integration.mid", smf, &err));
}
