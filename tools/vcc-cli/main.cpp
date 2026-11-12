// vcc - command-line front end to the VC Composer 2 engines.
//
// This is a development and audition tool, not the product UI: it lets the
// template catalog, Chord Explorer and Ostinato Creator be exercised and
// heard (through any MIDI player or DAW) before the desktop shell exists.
//
//   vcc templates [query] [--category C]       search the template catalog
//   vcc template <id>                          show one template
//   vcc new <template_id> <project.json>       create a project from a template
//   vcc chords <progression> --key "G major" [--bars 4] [--start 1]
//              [--style closed|open|drop2|drop3|spread]
//              [--rhythm block|pulse|arpeggio|strum] [--meter 4/4] [--midi out.mid]
//   vcc ostinato <preset> --key "D minor" [--bars 16] [--seed N] [--meter 7/8]
//              [--progression id] [--range 48-88] [--midi out.mid]
//   vcc validate                               load and check every content file
//   vcc export-midi <project.json> <out.mid>   deliverable MIDI (temp tracks excluded)
//
// Content is read from --content <dir>, $VCC_CONTENT_DIR, or the source tree.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "vcc/core/Files.h"
#include "vcc/ostinato/Ostinato.h"
#include "vcc/project/Render.h"
#include "vcc/templates/Templates.h"
#include "vcc/theory/Progression.h"

using namespace vcc;

namespace {

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;

    std::string opt(const std::string& k, const std::string& fallback = {}) const
    {
        auto it = options.find(k);
        return it == options.end() ? fallback : it->second;
    }
};

Args parse_args(int argc, char** argv)
{
    Args a;
    for (int i = 2; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0) {
            std::string key = s.substr(2);
            std::string value = (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0) ? argv[++i] : "1";
            a.options[key] = value;
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

std::string content_dir(const Args& a)
{
    if (!a.opt("content").empty()) return a.opt("content");
    if (const char* env = std::getenv("VCC_CONTENT_DIR")) return env;
    return VCC_DEFAULT_CONTENT_DIR;
}

int fail(const std::string& msg)
{
    std::fprintf(stderr, "vcc: %s\n", msg.c_str());
    return 1;
}

struct Catalogs {
    templates::InstrumentRegistry instruments;
    std::unique_ptr<templates::TemplateRegistry> templates;
    theory::ProgressionCatalog progressions;
    ostinato::PresetCatalog presets;
    std::vector<templates::Diagnostic> diagnostics;
    std::vector<std::string> errors;
};

std::unique_ptr<Catalogs> load_catalogs(const std::string& dir)
{
    auto c = std::make_unique<Catalogs>();
    c->instruments.load_directory(dir + "/instruments", c->diagnostics);
    c->templates = std::make_unique<templates::TemplateRegistry>(c->instruments);
    std::string err;
    if (!c->progressions.load_file(dir + "/progressions.json", &err)) c->errors.push_back(err);
    if (!c->presets.load_file(dir + "/ostinato_presets.json", &err)) c->errors.push_back(err);
    if (auto a = files::read_file(dir + "/articulations.json")) {
        if (auto doc = json::parse(*a)) {
            std::vector<std::string> ids;
            for (const auto& v : doc->get("articulations")->as_array()) ids.push_back(v.get_string("id"));
            c->templates->set_known_articulations(ids);
        }
    }
    std::vector<std::string> prog_ids;
    for (const auto& p : c->progressions.all()) prog_ids.push_back(p.id);
    c->templates->set_known_progressions(prog_ids);
    c->templates->set_known_ostinato_presets(c->presets.ids());
    c->templates->load_directory(dir + "/templates", c->diagnostics);
    return c;
}

bool write_midi(const std::string& path, const std::vector<MidiFileTrack>& tracks, const MeterMap& meter, double bpm)
{
    MidiFileOptions o;
    o.meter = meter;
    o.tempo = TempoMap(bpm);
    std::string err;
    if (!files::write_file_atomic(path, write_midi_file(tracks, o), &err)) {
        std::fprintf(stderr, "vcc: %s\n", err.c_str());
        return false;
    }
    std::printf("wrote %s\n", path.c_str());
    return true;
}

MeterMap meter_from(const Args& a)
{
    auto sig = TimeSignature::parse(a.opt("meter", "4/4"));
    return MeterMap(sig ? *sig : TimeSignature{});
}

// --- Commands --------------------------------------------------------------------

int cmd_templates(const Args& a, const Catalogs& c)
{
    templates::TemplateFilter f;
    for (const auto& p : a.positional) f.query += (f.query.empty() ? "" : " ") + p;
    f.category = a.opt("category");
    auto hits = c.templates->list(f);
    for (const auto* t : hits)
        std::printf("%-44s %-10s %-16s %2zu tracks  %s%s\n", t->template_id.c_str(), t->category.c_str(),
                    t->variant.c_str(), t->tracks.size(), t->display_name.c_str(),
                    t->instantiable() ? "" : "  [curation required]");
    std::printf("%zu template(s)\n", hits.size());
    return 0;
}

int cmd_template(const Args& a, const Catalogs& c)
{
    if (a.positional.empty()) return fail("usage: vcc template <id>");
    const auto* t = c.templates->get(a.positional[0]);
    if (!t) return fail("no template '" + a.positional[0] + "'");
    std::printf("%s\n  %s\n\n%s\n", t->display_name.c_str(), t->template_id.c_str(), t->description.c_str());
    if (!t->treatment.empty()) std::printf("Treatment: %s\n", t->treatment.c_str());
    if (!t->authenticity_note.empty()) std::printf("Note: %s\n", t->authenticity_note.c_str());
    std::printf("Tempo %d-%d bpm, meter", t->tempo_low, t->tempo_high);
    for (const auto& m : t->meter_options) std::printf(" %s", m.c_str());
    std::printf("\n\nTracks:\n");
    for (const auto& tr : t->tracks) {
        const auto* def = c.instruments.find(tr.instrument_standard_id);
        std::printf("  %-36s %-28s %-12s", tr.name.c_str(), def ? def->name.c_str() : "?", tr.role.c_str());
        for (const auto& art : tr.articulation_map) std::printf(" %s", art.c_str());
        std::printf("%s\n", tr.optional ? "  (optional)" : "");
    }
    if (!t->progression_ids.empty()) {
        std::printf("\nSuggested progressions:");
        for (const auto& p : t->progression_ids) std::printf(" %s", p.c_str());
        std::printf("\n");
    }
    if (!t->ostinato_preset_ids.empty()) {
        std::printf("Suggested ostinati:");
        for (const auto& p : t->ostinato_preset_ids) std::printf(" %s", p.c_str());
        std::printf("\n");
    }
    return 0;
}

int cmd_new(const Args& a, const Catalogs& c)
{
    if (a.positional.size() < 2) return fail("usage: vcc new <template_id> <project.json>");
    const auto* t = c.templates->get(a.positional[0]);
    if (!t) return fail("no template '" + a.positional[0] + "'");
    project::Project p;
    auto r = templates::instantiate(p, *t, c.instruments, {});
    if (!r.ok) return fail(r.error);
    if (!a.opt("key").empty()) p.edit("Set key", [&](project::ProjectData& d) { d.key = a.opt("key"); return true; });
    std::string err;
    if (!p.save(a.positional[1], &err)) return fail(err);
    std::printf("created %s: %zu tracks, %zu buses -> %s\n", t->display_name.c_str(), r.track_ids.size(),
                r.bus_ids.size(), a.positional[1].c_str());
    for (const auto& w : r.warnings) std::printf("  ! %s\n", w.c_str());
    return 0;
}

int cmd_chords(const Args& a, const Catalogs& c)
{
    if (a.positional.empty()) {
        for (const auto& p : c.progressions.all())
            std::printf("%-24s %-28s %2d bars  %s\n", p.id.c_str(), p.name.c_str(), p.bars, p.roman_text().c_str());
        return 0;
    }
    const auto* prog = c.progressions.find(a.positional[0]);
    if (!prog) return fail("no progression '" + a.positional[0] + "'");
    auto key = theory::Key::parse(a.opt("key", std::string("C ") + theory::to_string(prog->default_mode)));
    if (!key) return fail("cannot read key '" + a.opt("key") + "'");
    theory::ProgressionRenderOptions o;
    o.key = *key;
    o.length_bars = std::atoi(a.opt("bars", std::to_string(prog->bars)).c_str());
    o.start_bar = std::atoi(a.opt("start", "1").c_str());
    if (a.opt("expand") == "repeat") o.expand = theory::ExpandMode::Repeat;
    if (auto s = theory::voicing_style_from_string(a.opt("style", "closed"))) o.voicing.style = *s;
    if (auto r = theory::chord_rhythm_from_string(a.opt("rhythm", "block"))) o.rhythm = *r;
    o.voicing.free_inversions = !a.opt("free-inversions").empty();
    MeterMap meter = meter_from(a);
    std::string err;
    auto r = theory::render_progression(*prog, meter, o, &err);
    if (!r) return fail(err);
    std::printf("%s in %s, %d bars from bar %d\n", prog->name.c_str(), key->name().c_str(), o.length_bars, o.start_bar);
    for (const auto& rc : r->chords) {
        std::printf("  bar %-6.2f %-8s %-10s", 1.0 + static_cast<double>(meter.bar_at(rc.start) - 1) +
                    static_cast<double>(rc.start - meter.bar_start(meter.bar_at(rc.start))) /
                    static_cast<double>(meter.at_tick(rc.start).ticks_per_bar()),
                    rc.numeral.c_str(), rc.chord.name().c_str());
        for (int n : rc.voicing.notes) std::printf(" %d", n);
        std::printf("\n");
    }
    if (!a.opt("midi").empty())
        return write_midi(a.opt("midi"), {{prog->name, r->notes, 0, {}}}, meter, std::atof(a.opt("bpm", "90").c_str())) ? 0 : 1;
    return 0;
}

int cmd_ostinato(const Args& a, const Catalogs& c)
{
    if (a.positional.empty()) {
        for (const auto& id : c.presets.ids())
            std::printf("%-26s %s\n", id.c_str(), c.presets.find(id)->get_string("name").c_str());
        return 0;
    }
    const auto* preset = c.presets.find(a.positional[0]);
    if (!preset) return fail("no ostinato preset '" + a.positional[0] + "'");
    auto key = theory::Key::parse(a.opt("key", "D minor"));
    if (!key) return fail("cannot read key");
    ostinato::OstinatoOptions o;
    o.key = *key;
    o.meter = meter_from(a);
    o.length_bars = std::atoi(a.opt("bars", "16").c_str());
    o.apply_preset(*preset);
    std::string range = a.opt("range", "48-88");
    auto dash = range.find('-');
    if (dash != std::string::npos) {
        o.range_low = std::atoi(range.substr(0, dash).c_str());
        o.range_high = std::atoi(range.substr(dash + 1).c_str());
    }
    o.evolve = !a.opt("evolve").empty();
    o.call_response = !a.opt("call-response").empty();
    if (!a.opt("progression").empty()) {
        const auto* prog = c.progressions.find(a.opt("progression"));
        if (!prog) return fail("no progression '" + a.opt("progression") + "'");
        theory::ProgressionRenderOptions po;
        po.key = *key;
        po.length_bars = o.length_bars;
        po.expand = theory::ExpandMode::Repeat;
        std::string err;
        auto pr = theory::render_progression(*prog, o.meter, po, &err);
        if (!pr) return fail(err);
        for (const auto& rc : pr->chords) o.harmony.push_back({rc.start, rc.start + rc.duration, rc.chord});
    }
    std::uint64_t seed = std::strtoull(a.opt("seed", "1").c_str(), nullptr, 10);
    auto variants = ostinato::Generator(o).generate_variants(seed, std::atoi(a.opt("variants", "3").c_str()));
    std::vector<MidiFileTrack> tracks;
    for (const auto& v : variants) {
        std::printf("Variant %s: %zu notes, seed %s\n  ", v.id.c_str(), v.notes.size(),
                    v.to_json().get_string("seed").c_str());
        for (std::size_t i = 0; i < v.notes.size() && i < 16; ++i) std::printf("%d ", v.notes[i].pitch);
        std::printf("...\n");
        auto notes = v.notes;
        for (auto& n : notes) n.channel = static_cast<int>(tracks.size());
        tracks.push_back({std::string(preset->get_string("name")) + " " + v.id, notes, o.percussion ? -1 : 48, {}});
    }
    if (!a.opt("midi").empty())
        return write_midi(a.opt("midi"), tracks, o.meter, std::atof(a.opt("bpm", "110").c_str())) ? 0 : 1;
    return 0;
}

int cmd_validate(const Catalogs& c)
{
    int errors = 0, warnings = 0;
    for (const auto& d : c.diagnostics) {
        std::printf("%s\n", d.to_string().c_str());
        (d.level == templates::Diagnostic::Level::Error ? errors : warnings)++;
    }
    for (const auto& e : c.errors) {
        std::printf("error %s\n", e.c_str());
        ++errors;
    }
    std::printf("%zu instruments, %zu templates, %zu progressions, %zu ostinato presets; %d errors, %d warnings\n",
                c.instruments.all().size(), c.templates->all().size(), c.progressions.all().size(),
                c.presets.ids().size(), errors, warnings);
    return errors == 0 ? 0 : 1;
}

int cmd_export_midi(const Args& a)
{
    if (a.positional.size() < 2) return fail("usage: vcc export-midi <project.json> <out.mid> [--keyswitches]");
    std::string err;
    auto p = project::Project::load(a.positional[0], &err);
    if (!p) return fail(err);
    // Written notation and MIDI regions together, through each track's
    // expression map; temp/reference tracks are left out.
    std::string smf = project::export_project_midi(*p, !a.opt("keyswitches").empty());
    if (!files::write_file_atomic(a.positional[1], smf, &err)) return fail(err);
    for (const auto* t : p->export_tracks()) {
        if (t->kind != project::TrackKind::Instrument && t->kind != project::TrackKind::MidiOnly) continue;
        for (const auto& w : project::render_track(*t).warnings) std::printf("  ! %s: %s\n", t->name.c_str(), w.c_str());
    }
    std::printf("wrote %s\n", a.positional[1].c_str());
    return 0;
}

void usage()
{
    std::puts(
        "vcc - VC Composer 2 engine tool\n"
        "  vcc templates [query] [--category cinematic|regional|era|composer|modern]\n"
        "  vcc template <id>\n"
        "  vcc new <template_id> <project.json> [--key \"D minor\"]\n"
        "  vcc chords [progression] --key \"G major\" [--bars 4|8] [--start N] [--style closed|open|drop2|drop3|spread]\n"
        "             [--rhythm block|pulse|arpeggio|strum] [--expand repeat] [--meter 4/4] [--midi out.mid]\n"
        "  vcc ostinato [preset] --key \"D minor\" [--bars 16] [--seed N] [--meter 7/8] [--progression id]\n"
        "             [--range 48-88] [--evolve] [--call-response] [--midi out.mid]\n"
        "  vcc validate\n"
        "  vcc export-midi <project.json> <out.mid>\n"
        "Options: --content <dir> (or $VCC_CONTENT_DIR)");
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || std::strcmp(argv[1], "help") == 0 || std::strcmp(argv[1], "--help") == 0) {
        usage();
        return argc < 2 ? 1 : 0;
    }
    std::string cmd = argv[1];
    Args a = parse_args(argc, argv);
    if (cmd == "export-midi") return cmd_export_midi(a);
    auto c = load_catalogs(content_dir(a));
    if (cmd == "templates") return cmd_templates(a, *c);
    if (cmd == "template") return cmd_template(a, *c);
    if (cmd == "new") return cmd_new(a, *c);
    if (cmd == "chords") return cmd_chords(a, *c);
    if (cmd == "ostinato") return cmd_ostinato(a, *c);
    if (cmd == "validate") return cmd_validate(*c);
    usage();
    return 1;
}
