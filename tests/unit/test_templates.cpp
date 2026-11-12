#include <set>

#include "vcc/core/Files.h"
#include "vcc/ostinato/Ostinato.h"
#include "vcc/templates/Templates.h"
#include "vcc/theory/Progression.h"
#include "vcc_test.h"

using namespace vcc;
using namespace vcc::templates;

namespace {

struct Content {
    InstrumentRegistry instruments;
    std::unique_ptr<TemplateRegistry> templates;
    std::vector<Diagnostic> diagnostics;
};

const Content& content()
{
    static Content c = [] {
        Content x;
        const std::string root = VCC_CONTENT_DIR;
        x.instruments.load_directory(root + "/instruments", x.diagnostics);
        x.templates = std::make_unique<TemplateRegistry>(x.instruments);
        std::vector<std::string> artic, progs;
        auto a = json::parse(*files::read_file(root + "/articulations.json"));
        for (const auto& v : a->get("articulations")->as_array()) artic.push_back(v.get_string("id"));
        theory::ProgressionCatalog pc;
        pc.load_file(root + "/progressions.json");
        for (const auto& p : pc.all()) progs.push_back(p.id);
        ostinato::PresetCatalog oc;
        oc.load_file(root + "/ostinato_presets.json");
        x.templates->set_known_articulations(artic);
        x.templates->set_known_progressions(progs);
        x.templates->set_known_ostinato_presets(oc.ids());
        x.templates->load_directory(root + "/templates", x.diagnostics);
        return x;
    }();
    return c;
}

const Template& tpl(const std::string& id)
{
    const Template* t = content().templates->get(id);
    if (!t) std::fprintf(stderr, "    missing template %s\n", id.c_str());
    static Template empty;
    return t ? *t : empty;
}

std::vector<const Template*> search(const std::string& q, const std::string& category = {})
{
    TemplateFilter f;
    f.query = q;
    f.category = category;
    return content().templates->list(f);
}

bool has_track(const project::Project& p, const std::string& instrument_prefix)
{
    for (const auto& t : p.data().tracks)
        if (t.instrument_standard_id.rfind(instrument_prefix, 0) == 0) return true;
    return false;
}

}  // namespace

TEST_CASE("P0 every shipped pack validates and loads offline")
{
    int errors = 0;
    for (const auto& d : content().diagnostics) {
        if (d.level == Diagnostic::Level::Error) ++errors;
        std::fprintf(stderr, "    %s\n", d.to_string().c_str());
    }
    CHECK_EQ(errors, 0);
    CHECK(content().instruments.all().size() >= 300);
    CHECK(content().templates->all().size() >= 150);
}

TEST_CASE("P1 every available template instantiates with correct trees, then undoes as a unit")
{
    for (const auto& t : content().templates->all()) {
        if (!t.instantiable()) continue;
        project::Project p;
        InstantiateOptions o;
        auto r = instantiate(p, t, content().instruments, o);
        CHECK(r.ok);
        if (!r.ok) { std::fprintf(stderr, "    %s: %s\n", t.template_id.c_str(), r.error.c_str()); continue; }
        CHECK_EQ(r.track_ids.size(), t.tracks.size());
        // Every instrument track sits in a folder and routes to an existing bus.
        for (const auto& tr : p.data().tracks) {
            if (tr.kind != project::TrackKind::Instrument) continue;
            CHECK(p.data().find_track(tr.folder_id) != nullptr);
            CHECK(p.data().find_bus(tr.output_bus_id) != nullptr);
            CHECK(tr.assignment.status != project::ResolutionStatus::Unresolved);
            CHECK(!tr.assignment.message.empty());
        }
        // Folders follow the family order.
        int last = -1;
        for (const auto& tr : p.data().tracks) {
            if (tr.kind != project::TrackKind::Folder) continue;
            auto it = std::find(family_order().begin(), family_order().end(), tr.family);
            int rank = static_cast<int>(it - family_order().begin());
            CHECK(rank >= last);
            last = rank;
        }
        auto saved = project::Project::from_json(p.to_json());
        CHECK(saved.has_value());
        CHECK(p.undo());
        CHECK(p.data().tracks.empty());
        CHECK_EQ(p.data().buses.size(), 1u);  // master only
    }
}

TEST_CASE("T01 / acceptance 7: Horror without any orchestral plugins builds named placeholders")
{
    const Template h = tpl("cinematic.horror.psychological");
    project::Project p;
    auto r = instantiate(p, h, content().instruments, {});
    REQUIRE(r.ok);
    CHECK(!r.warnings.empty());
    bool low_strings_technique = false, woodwinds = false, texture = false;
    for (const auto& t : p.data().tracks) {
        if (t.kind != project::TrackKind::Instrument) continue;
        CHECK(t.assignment.status == project::ResolutionStatus::Placeholder);
        bool tech = std::find(t.articulation_intents.begin(), t.articulation_intents.end(), "sul_ponticello") !=
                        t.articulation_intents.end() ||
                    std::find(t.articulation_intents.begin(), t.articulation_intents.end(), "tremolo") !=
                        t.articulation_intents.end();
        if (t.family == "strings" && tech) low_strings_technique = true;
        if (t.family == "woodwinds") woodwinds = true;
        if (t.family == "synth" || t.role == "texture" || t.role == "drone") texture = true;
    }
    CHECK(low_strings_technique);
    CHECK(woodwinds);
    CHECK(texture);
    bool section_bus = false;
    for (const auto& b : p.data().buses) section_bus = section_bus || b.kind == "group";
    CHECK(section_bus);
}

TEST_CASE("T02 Comedy with only a generic GM synth keeps roles and maps fallbacks")
{
    const Template c = tpl("cinematic.comedy.light_quirky");
    REQUIRE(!c.tracks.empty());
    // A composer whose only "library" is a GM string ensemble and a GM piano.
    InstantiateOptions o;
    o.installed = {{"gm-strings", "GM Strings", "strings.string_ensemble", {"sustain", "pizzicato"}, false, "verified"},
                   {"gm-piano", "GM Piano", "keys.piano", {}, false, "verified"}};
    project::Project p;
    auto r = instantiate(p, c, content().instruments, o);
    REQUIRE(r.ok);
    std::set<std::string> roles;
    int fallbacks = 0;
    for (const auto& t : p.data().tracks) {
        if (t.kind != project::TrackKind::Instrument) continue;
        roles.insert(t.role);
        if (t.assignment.status == project::ResolutionStatus::Fallback) {
            ++fallbacks;
            CHECK(t.assignment.message.find("fallback") != std::string::npos);
        }
    }
    CHECK(fallbacks >= 1);
    CHECK(roles.size() >= 2);
}

TEST_CASE("acceptance 8: Comedy content, and distinct mood templates")
{
    const Template c = tpl("cinematic.comedy.light_quirky");
    project::Project p;
    REQUIRE(instantiate(p, c, content().instruments, {}).ok);
    bool pizz = false;
    for (const auto& t : p.data().tracks)
        if (t.family == "strings" && std::find(t.articulation_intents.begin(), t.articulation_intents.end(), "pizzicato") != t.articulation_intents.end())
            pizz = true;
    CHECK(pizz);
    CHECK(has_track(p, "woodwinds.oboe"));
    CHECK(has_track(p, "woodwinds.bassoon"));
    CHECK(has_track(p, "percussion.xylophone") || has_track(p, "percussion.glockenspiel") || has_track(p, "percussion.marimba"));
    std::set<std::string> ids;
    for (const char* mood : {"whimsical", "romance", "sad", "heroic", "suspense"}) {
        auto hits = search(mood, "cinematic");
        CHECK(!hits.empty());
        if (!hits.empty()) ids.insert(hits.front()->template_id);
    }
    CHECK_EQ(ids.size(), 5u);
}

TEST_CASE("acceptance 9: regional traditions are separate and searchable")
{
    for (const char* q : {"ireland", "japan", "china", "german folk", "germanic reconstruction"}) {
        auto hits = search(q, "regional");
        CHECK(!hits.empty());
        if (hits.empty()) std::fprintf(stderr, "    no regional hit for '%s'\n", q);
    }
    std::set<std::string> african;
    for (const auto* t : content().templates->list({})) {
        std::string region = t->cultural_context.get_string("region");
        if (t->category == "regional" && region.find("Africa") != std::string::npos) african.insert(t->template_id);
        if (t->category == "regional" && (region.find("Ghana") != std::string::npos)) african.insert(t->template_id);
    }
    CHECK(african.size() >= 3);
    // Reconstructions are labelled as such.
    auto g = search("germanic", "regional");
    REQUIRE(!g.empty());
    CHECK(g.front()->authenticity_note.find("reconstruct") != std::string::npos ||
          g.front()->authenticity_note.find("Reconstruct") != std::string::npos);
}

TEST_CASE("curated-only packs are listed but refuse instantiation")
{
    int curated = 0;
    for (const auto& t : content().templates->all()) {
        if (t.status != "curation_required") continue;
        ++curated;
        project::Project p;
        auto r = instantiate(p, t, content().instruments, {});
        CHECK(!r.ok);
        CHECK(p.data().tracks.empty());
    }
    CHECK(curated >= 1);
}

TEST_CASE("acceptance 10 / T03: Bach, Vivaldi, Mozart, Beethoven variants")
{
    for (const char* who : {"bach", "vivaldi", "mozart", "beethoven"}) {
        TemplateFilter f;
        f.category = "composer";
        f.query = who;
        auto hits = content().templates->list(f);
        CHECK(hits.size() >= 2);
        for (const auto* h : hits) CHECK(h->composer.is_object() && !h->authenticity_note.empty());
    }
    TemplateFilter f;
    f.category = "composer";
    f.query = "bach concerto";
    auto hits = content().templates->list(f);
    REQUIRE(!hits.empty());
    project::Project p;
    REQUIRE(instantiate(p, *hits.front(), content().instruments, {}).ok);
    bool continuo = false, strings = false, winds = false;
    for (const auto& t : p.data().tracks) {
        continuo = continuo || t.role == "continuo";
        strings = strings || t.family == "strings";
        winds = winds || t.family == "woodwinds" || t.family == "brass";
    }
    CHECK(continuo && strings && winds);
    // At least one era has more than one variant.
    TemplateFilter e;
    e.category = "era";
    e.query = "baroque";
    CHECK(content().templates->list(e).size() >= 2);
}

TEST_CASE("acceptance 11: blues, jazz eras, rock eras, industrial and dark hybrid are discoverable")
{
    CHECK(!search("blues", "modern").empty());
    CHECK(search("jazz", "modern").size() >= 3);
    CHECK(search("rock", "modern").size() >= 3);
    CHECK(!search("industrial", "modern").empty());
    CHECK(!search("minimal dark cinematic hybrid").empty());
}

TEST_CASE("acceptance 15 / merge: existing cues, buses and temp track survive")
{
    project::Project p;
    std::string temp = p.add_track("Temp Score", project::TrackKind::Reference);
    p.edit("cue", [](project::ProjectData& d) {
        project::Cue cue;
        cue.id = "cue-1";
        cue.code = "1M1";
        cue.title = "Opening";
        cue.end = kTicksPerWhole * 8;
        d.cues.push_back(cue);
        d.buses.push_back({"bus-dialog", "Dialogue Ref", "group", "bus-master", {}, {}, 0.0, 0.0, false, false, {}, {}});
        return true;
    });
    REQUIRE(instantiate(p, tpl("cinematic.heroic.triumph"), content().instruments, {}).ok);
    std::size_t tracks_after_first = p.data().tracks.size();
    // Create into a non-empty project is refused rather than overwriting.
    CHECK(!instantiate(p, tpl("cinematic.suspense.tension"), content().instruments, {}).ok);
    InstantiateOptions merge;
    merge.mode = InstantiateMode::Merge;
    auto r = instantiate(p, tpl("cinematic.suspense.tension"), content().instruments, merge);
    REQUIRE(r.ok);
    CHECK(p.data().find_track(temp) != nullptr);
    CHECK_EQ(p.data().cues.size(), 1u);
    CHECK(p.data().find_bus("bus-dialog") != nullptr);
    CHECK(p.data().tracks.size() > tracks_after_first);
    std::set<std::string> names;
    for (const auto& t : p.data().tracks) CHECK(names.insert(t.name).second);  // no duplicate names
    std::set<std::string> bus_names;
    for (const auto& b : p.data().buses) CHECK(bus_names.insert(b.name).second);
    // T09: undoing the merge removes exactly what it added.
    REQUIRE(p.undo());
    CHECK_EQ(p.data().tracks.size(), tracks_after_first);
    // Replace needs confirmation, and still keeps the temp track and cues.
    InstantiateOptions replace;
    replace.mode = InstantiateMode::Replace;
    CHECK(!instantiate(p, tpl("cinematic.suspense.tension"), content().instruments, replace).ok);
    replace.replace_confirmed = true;
    REQUIRE(instantiate(p, tpl("cinematic.suspense.tension"), content().instruments, replace).ok);
    CHECK(p.data().find_track(temp) != nullptr);
    CHECK_EQ(p.data().cues.size(), 1u);
    CHECK(p.data().find_bus("bus-dialog") != nullptr);
}

TEST_CASE("resolver prefers favorites, then verified, then articulation coverage")
{
    std::vector<InstalledPatch> lib = {
        {"a", "Horns A", "brass.horn_section", {"sustain"}, false, "inferred"},
        {"b", "Horns B", "brass.horn_section", {"sustain", "marcato"}, false, "verified"},
        {"c", "Horns C", "brass.horn_section", {}, true, "user"},
    };
    InstrumentResolver r(content().instruments, lib);
    auto res = r.resolve("brass.horn_section", {"marcato"}, {});
    CHECK(res.status == project::ResolutionStatus::Exact);
    CHECK_EQ(res.patch->patch_id, std::string("c"));
    lib[2].favorite = false;
    InstrumentResolver r2(content().instruments, lib);
    auto res2 = r2.resolve("brass.horn_section", {"marcato"}, {});
    CHECK_EQ(res2.patch->patch_id, std::string("b"));
    CHECK(res2.unsupported_articulations.empty());
    auto none = r2.resolve("strings.violins_1", {"spiccato"}, {"strings.string_ensemble"});
    CHECK(none.status == project::ResolutionStatus::Placeholder);
    CHECK_EQ(none.placeholder_program, 48);
}

TEST_CASE("user templates save, re-import with validation, and built-ins stay immutable")
{
    project::Project p;
    REQUIRE(instantiate(p, tpl("cinematic.mystery.investigation"), content().instruments, {}).ok);
    Template mine = save_user_template(p, "user.my_mystery", "My Mystery Setup", "");
    InstrumentRegistry& instruments = const_cast<InstrumentRegistry&>(content().instruments);
    TemplateRegistry reg(instruments);
    std::string err;
    REQUIRE(reg.add_user_template(mine, &err));
    json::Value pack = reg.export_pack({"user.my_mystery"}, "my_pack");
    TemplateRegistry other(instruments);
    std::vector<Diagnostic> d;
    REQUIRE(other.import_pack(*json::parse(json::write(pack)), d));
    const Template* back = other.get("user.my_mystery");
    REQUIRE(back);
    CHECK(!back->built_in);
    CHECK_EQ(back->tracks.size(), tpl("cinematic.mystery.investigation").tracks.size());
    project::Project q;
    CHECK(instantiate(q, *back, content().instruments, {}).ok);
    // A built-in cannot be overwritten by a user template of the same id.
    Template clash = mine;
    clash.template_id = "cinematic.mystery.investigation";
    CHECK(!content().templates->get(clash.template_id)->built_in == false);
    TemplateRegistry with_builtins(instruments);
    std::vector<Diagnostic> d2;
    with_builtins.load_directory(std::string(VCC_CONTENT_DIR) + "/templates", d2);
    CHECK(!with_builtins.add_user_template(clash, &err));
}

TEST_CASE("T10 importing a malformed pack reports diagnostics and loads only valid templates")
{
    TemplateRegistry reg(content().instruments);
    auto doc = json::parse(R"({"schema_version":1,"pack":"bad","templates":[
      {"template_id":"user.ok","schema_version":1,"content_version":"1.0.0","display_name":"OK","category":"regional",
       "subcategory":"x","tags":["x"],"variant":"traditional","description":"d","tempo_range_bpm":[60,90],
       "meter_options":["4/4"],"license_metadata":{},
       "cultural_context":{"region":"Somewhere"},"authenticity_note":"Pending review.",
       "tracks":[{"id":"a","name":"A","instrument_standard_id":"strings.violin","role":"melody"}]},
      {"template_id":"user.no_note","schema_version":1,"content_version":"1.0.0","display_name":"Bad","category":"regional",
       "subcategory":"x","tags":[],"variant":"traditional","description":"d","tempo_range_bpm":[60,90],
       "meter_options":["4/4"],"license_metadata":{},"cultural_context":{"region":"Somewhere"},
       "tracks":[{"id":"a","name":"A","instrument_standard_id":"strings.violin","role":"melody"}]},
      {"template_id":"user.bad_inst","schema_version":1,"content_version":"1.0.0","display_name":"Bad2","category":"modern",
       "subcategory":"x","tags":[],"variant":"band","description":"d","tempo_range_bpm":[60,90],
       "meter_options":["4/3"],"license_metadata":{},
       "tracks":[{"id":"A b","name":"A","instrument_standard_id":"nope.nothing","role":"lead"}]}
    ]})");
    REQUIRE(doc);
    std::vector<Diagnostic> d;
    CHECK(!reg.import_pack(*doc, d));
    CHECK(reg.get("user.ok") != nullptr);
    CHECK(reg.get("user.no_note") == nullptr);
    CHECK(reg.get("user.bad_inst") == nullptr);
    int errors = 0;
    for (const auto& x : d) errors += x.level == Diagnostic::Level::Error;
    CHECK(errors >= 5);
}
