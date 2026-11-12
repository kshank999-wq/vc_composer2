// End-to-end: the Templates Supplement's definition of done, headless.
// "A composer can choose a scoring approach, instantiate appropriately named
// tracks and articulations, hear and place key-correct chord progressions,
// generate and audition a 16-bar ostinato, and mix the resulting project."
#include "vcc/core/Files.h"
#include "vcc/ostinato/Ostinato.h"
#include "vcc/templates/Templates.h"
#include "vcc/theory/Progression.h"
#include "vcc_test.h"

using namespace vcc;

TEST_CASE("template -> chords -> ostinato -> save -> reopen -> export")
{
    const std::string root = VCC_CONTENT_DIR;
    std::vector<templates::Diagnostic> diags;
    templates::InstrumentRegistry instruments;
    REQUIRE(instruments.load_directory(root + "/instruments", diags));
    templates::TemplateRegistry registry(instruments);
    REQUIRE(registry.load_directory(root + "/templates", diags));
    theory::ProgressionCatalog progressions;
    REQUIRE(progressions.load_file(root + "/progressions.json"));
    ostinato::PresetCatalog presets;
    REQUIRE(presets.load_file(root + "/ostinato_presets.json"));

    // 1. A temp track exists before anything else; it must never be touched.
    project::Project p;
    std::string temp = p.add_track("Temp Score (ref)", project::TrackKind::Reference);

    // 2. Suspense template, created into the project.
    templates::TemplateFilter f;
    f.query = "suspense";
    f.category = "cinematic";
    auto hits = registry.list(f);
    REQUIRE(!hits.empty());
    const templates::Template& tpl = *hits.front();
    templates::InstantiateOptions io;
    io.mode = templates::InstantiateMode::Merge;  // the temp track is already there
    auto inst = templates::instantiate(p, tpl, instruments, io);
    REQUIRE(inst.ok);
    REQUIRE(!inst.track_ids.empty());

    // Pick a harmony-capable track and a string-ish track for the ostinato.
    std::string harmony_track, ostinato_track;
    for (const auto& id : inst.track_ids) {
        const auto* t = p.data().find_track(id);
        const auto* def = instruments.find(t->instrument_standard_id);
        if (def->unpitched) continue;
        if (harmony_track.empty() && (t->family == "keys" || t->family == "synth" || t->role == "pad" || t->role == "harmony"))
            harmony_track = id;
        if (ostinato_track.empty() && t->family == "strings" && id != harmony_track) ostinato_track = id;
    }
    if (harmony_track.empty()) harmony_track = p.add_track("Harmony", project::TrackKind::Instrument, "keys.piano");
    if (ostinato_track.empty()) ostinato_track = p.add_track("Ostinato", project::TrackKind::Instrument, "strings.violas");

    // 3. Chord Explorer: preview (no write), then send exactly 8 bars at bar 5.
    theory::Key key = *theory::Key::parse("D minor");
    theory::ProgressionRenderOptions po;
    po.key = key;
    po.start_bar = 5;
    po.length_bars = 8;
    auto before = json::write(p.to_json());
    auto preview = theory::render_progression(*progressions.find("suspense_pedal"), p.data().meter, po);
    REQUIRE(preview);
    CHECK_EQ(json::write(p.to_json()), before);  // audition never writes
    std::size_t history = p.history_size();
    auto sent = p.insert_notes(harmony_track, preview->start, preview->end, preview->notes,
                               project::CollisionPolicy::Ask, "chord_explorer:suspense_pedal");
    REQUIRE(sent.ok);
    CHECK_EQ(p.history_size(), history + 1);  // acceptance 13: one undoable edit
    CHECK_EQ(preview->end - preview->start, 8 * kTicksPerWhole);

    // 4. Ostinato Creator: 16 bars following the chords, three variants, send B.
    const auto* def = instruments.find(p.data().find_track(ostinato_track)->instrument_standard_id);
    ostinato::OstinatoOptions oo;
    oo.key = key;
    oo.meter = p.data().meter;
    oo.start_bar = 5;
    oo.length_bars = 16;
    oo.range_low = def->range_low;
    oo.range_high = def->range_high;
    oo.apply_preset(*presets.find("urgent_string_spiccato"));
    for (const auto& c : preview->chords) oo.harmony.push_back({c.start, c.start + c.duration, c.chord});
    auto variants = ostinato::Generator(oo).generate_variants(4242, 3);
    REQUIRE(variants.size() == 3);
    const auto& chosen = variants[1];
    auto ost = p.insert_notes(ostinato_track, chosen.start, chosen.end, chosen.notes, project::CollisionPolicy::Ask,
                              "ostinato:urgent_string_spiccato/" + chosen.id);
    if (!ost.ok) std::fprintf(stderr, "    %s\n", ost.error.c_str());
    REQUIRE(ost.ok);

    // 5. Edit the generated MIDI like hand-entered MIDI (acceptance 14).
    REQUIRE(p.edit("Nudge first ostinato note", [&](project::ProjectData& d) {
        auto& n = d.find_track(ostinato_track)->active_version()->regions.back().notes.front();
        n.velocity = 127;
        return true;
    }));

    // 6. Save, reopen, and nothing has drifted.
    std::string path = std::string(VCC_TEST_TMP_DIR) + "/workflow.vcc2.json";
    std::string err;
    REQUIRE(p.save(path, &err));
    auto reopened = project::Project::load(path, &err);
    REQUIRE(reopened);
    CHECK_EQ(json::write(reopened->to_json()), json::write(p.to_json()));
    CHECK_EQ(reopened->data().find_track(ostinato_track)->active_version()->regions.back().notes.front().velocity, 127);
    CHECK(reopened->data().find_track(temp) != nullptr);

    // 7. Export: deliverables exclude the temp track; the MIDI file is well formed.
    std::vector<MidiFileTrack> tracks;
    for (const auto* t : reopened->export_tracks()) {
        if (t->kind != project::TrackKind::Instrument) continue;
        MidiFileTrack mt{t->name, {}, t->assignment.placeholder_program};
        for (const auto& r : t->active_version()->regions)
            for (auto n : r.notes) {
                n.channel = t->midi_channel;
                mt.notes.push_back(n);
            }
        tracks.push_back(mt);
        CHECK(t->kind != project::TrackKind::Reference);
    }
    MidiFileOptions mo;
    mo.meter = reopened->data().meter;
    mo.tempo = reopened->data().tempo;
    mo.markers = {{preview->start, "Chords in"}};
    std::string smf = write_midi_file(tracks, mo);
    CHECK_EQ(smf.substr(0, 4), std::string("MThd"));
    CHECK(smf.find("Temp Score") == std::string::npos);
    REQUIRE(files::write_file_atomic(std::string(VCC_TEST_TMP_DIR) + "/workflow.mid", smf, &err));
}
