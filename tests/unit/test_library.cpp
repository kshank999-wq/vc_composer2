// Library browser and discovery (Main Spec §10, §11).
#include <algorithm>
#include <filesystem>

#include "vcc/core/Files.h"
#include "vcc/library/Library.h"
#include "vcc/library/TemplatePackage.h"
#include "vcc/session/Session.h"
#include "vcc_test.h"

using namespace vcc;
namespace fs = std::filesystem;

namespace {
const session::Catalogs& catalogs()
{
    static auto c = session::Catalogs::load_from_directory(VCC_CONTENT_DIR);
    return *c;
}
library::Library make()
{
    library::Library lib(catalogs().instruments);
    const auto text = files::read_file(std::string(VCC_CONTENT_DIR) + "/library/providers.json");
    REQUIRE(text.has_value());
    const auto doc = json::parse(*text);
    REQUIRE(doc.has_value());
    std::string err;
    REQUIRE(lib.load_catalog(*doc, &err));
    return lib;
}
library::InstalledPlugin plug(const std::string& name, const std::string& vendor)
{
    return {name, vendor, "VST3", "VST3-" + name + "-id"};
}
}  // namespace

TEST_CASE("library: the shipped catalog loads and every patch names a known instrument")
{
    auto lib = make();
    CHECK(lib.players().size() >= 8u);
    CHECK(lib.libraries().size() >= 5u);
}

TEST_CASE("library: installed, detected and known are told apart")
{
    auto lib = make();
    lib.set_installed({plug("Kontakt 8", "Native Instruments"), plug("BBC Symphony Orchestra", "Spitfire Audio"),
                       plug("Opus", "EastWest"), plug("Pianoteq 8", "Modartt"), plug("Solo Cello Legato", "Indie"),
                       plug("Display Tool", "Nobody")});
    CHECK(lib.plugin_for_player("kontakt") != nullptr);
    CHECK(lib.plugin_for_player("play") == nullptr);  // "play" is not a word in "Display Tool"
    CHECK(lib.plugin_for_player("sine") == nullptr);

    // Opus is here, but Hollywood Orchestra's folders are not: installed, not detected.
    auto hw = lib.find("catalog:eastwest.hollywood_orchestra/Horns");
    REQUIRE(hw.has_value());
    CHECK(hw->player_installed);
    CHECK(!hw->library_detected);
    CHECK(hw->metadata_known);
    CHECK_EQ(library::status_text(*hw), std::string("Player installed - library not detected - metadata known"));
    // A library with a plug-in of its own is detected by the plug-in.
    auto bbc = lib.find("catalog:spitfire.bbcso/Horns a4");
    REQUIRE(bbc.has_value());
    CHECK(bbc->library_detected);
    // Not installed at all: Synchron.
    auto syn = lib.find("catalog:vsl.synchron_strings/Violins I");
    REQUIRE(syn.has_value());
    CHECK(!syn->player_installed);

    // Library folders are found on disk.
    const std::string root = std::string(VCC_TEST_TMP_DIR) + "/libdetect";
    fs::remove_all(root);
    fs::create_directories(root + "/Samples/EastWest/Hollywood Orchestra Opus Edition");
    lib.library_folders().push_back(root);
    const auto found = lib.detect();
    CHECK(std::find(found.begin(), found.end(), "eastwest.hollywood_orchestra") != found.end());
    CHECK(lib.find("catalog:eastwest.hollywood_orchestra/Horns")->library_detected);

    // Plug-ins: the name suggests an instrument (to verify), or nothing.
    auto cello = lib.find("plugin:VST3-Solo Cello Legato-id");
    REQUIRE(cello.has_value());
    CHECK_EQ(cello->instrument_id, std::string("strings.cello"));
    CHECK_EQ(cello->validation, std::string("inferred"));
    CHECK(!cello->metadata_known);
    CHECK_EQ(lib.find("plugin:VST3-Pianoteq 8-id")->instrument_id, std::string());  // a catalog library's plug-in
    CHECK_EQ(lib.find("plugin:VST3-Display Tool-id")->instrument_id, std::string());
}

TEST_CASE("library: search filters by words, provider, family, technique, installed and favorites")
{
    auto lib = make();
    lib.set_installed({plug("BBC Symphony Orchestra", "Spitfire Audio"), plug("Opus", "EastWest")});
    library::Filter f;
    f.query = "horns";
    auto r = lib.search(f);
    CHECK(r.size() >= 2u);
    f.provider = "Spitfire Audio";
    r = lib.search(f);
    REQUIRE(r.size() == 1u);
    CHECK_EQ(r[0].key, std::string("catalog:spitfire.bbcso/Horns a4"));
    library::Filter g;
    g.family = "strings";
    g.technique = "sul_ponticello";
    r = lib.search(g);
    REQUIRE(r.size() == 1u);  // only Synchron lists it
    g.installed_only = true;
    CHECK(lib.search(g).empty());
    lib.set_favorite("catalog:spitfire.bbcso/Celli", true);
    library::Filter fav;
    fav.favorites_only = true;
    r = lib.search(fav);
    REQUIRE(r.size() == 1u);
    CHECK(r[0].favorite);
    lib.set_tags("catalog:spitfire.bbcso/Celli", {"warm"});
    library::Filter tag;
    tag.query = "warm";
    CHECK_EQ(lib.search(tag).size(), 1u);
}

TEST_CASE("library: candidates for a template track rank saved, catalog, fallback and inferred")
{
    auto lib = make();
    lib.set_installed({plug("BBC Symphony Orchestra", "Spitfire Audio"), plug("Opus", "EastWest"),
                       plug("Horn Solo", "Indie")});
    library::UserPatch mine;
    mine.display = "My Horns";
    mine.plugin = {"VST3", "VST3-BBC Symphony Orchestra-id", "BBC Symphony Orchestra", "Spitfire Audio", "c3RhdGU="};
    mine.instrument_id = "brass.horn_section";
    mine.articulations = {"legato", "marcato"};
    mine.validation = "verified";
    const auto id = lib.add_patch(mine);
    auto c = lib.candidates("brass.horn_section", {"legato", "marcato", "cuivre"}, {"brass.horn_f"});
    REQUIRE(c.size() >= 3u);
    CHECK_EQ(c[0].entry.key, "patch:" + id);
    CHECK(c[0].match == library::Match::Exact);
    CHECK(c[0].missing_articulations == std::vector<std::string>{"cuivre"});
    CHECK(c[1].entry.kind == library::EntryKind::CatalogPatch);
    CHECK(c[1].match == library::Match::Inferred);
    CHECK(c[1].entry.key == "catalog:spitfire.bbcso/Horns a4");  // detected beats not detected
    // The plug-in named "Horn Solo" is a fallback's inferred candidate, last.
    CHECK(c.back().match == library::Match::Inferred);
    CHECK(c.back().entry.kind == library::EntryKind::Plugin);
    CHECK(c.back().reason.find("verify") != std::string::npos);

    // An unloadable library never appears: Synchron is not installed.
    for (const auto& k : lib.candidates("strings.violins_1", {}, {}))
        CHECK(k.entry.player_installed);
}

TEST_CASE("library: saved patches persist, travel in mapping packs and drive template instantiation")
{
    auto lib = make();
    lib.set_installed({plug("BBC Symphony Orchestra", "Spitfire Audio")});
    library::UserPatch p;
    p.display = "Celli - warm";
    p.plugin = {"VST3", "VST3-BBC Symphony Orchestra-id", "BBC Symphony Orchestra", "Spitfire Audio", "c3RhdGU="};
    p.instrument_id = "strings.celli";
    p.articulations = {"legato", "long", "spiccato"};
    p.expression_map = "orchestral_cc";
    p.validation = "verified";
    notation::ExpressionMap ks = notation::make_default_expression_map();
    ks.name = "BBC celli KS";
    ks.has_natural = true;
    ks.natural.action = notation::ArticulationAction::Keyswitch;
    ks.natural.keyswitch_note = 24;
    p.articulation_map = ks;
    const auto id = lib.add_patch(p);
    lib.set_favorite("patch:" + id, true);
    lib.library_folders().push_back("/Volumes/Samples");

    library::Library back(catalogs().instruments);
    back.load_user(lib.user_json());
    REQUIRE(back.find_patch(id) != nullptr);
    CHECK_EQ(back.find_patch(id)->plugin.state_base64, std::string("c3RhdGU="));
    CHECK(back.favorite("patch:" + id));
    CHECK_EQ(back.library_folders().size(), 1u);
    CHECK(back.add_patch(p) != id);  // ids keep counting

    // A pack leaves states out unless asked; importing it twice updates, not duplicates.
    const auto pack = lib.export_pack("Spitfire horns");
    CHECK(pack["patches"].at(0)["plugin"].get("state") == nullptr);
    library::Library other(catalogs().instruments);
    std::vector<std::string> warnings;
    CHECK_EQ(other.import_pack(pack, &warnings), 1);
    CHECK_EQ(other.import_pack(pack, &warnings), 1);
    CHECK_EQ(other.patches().size(), 1u);
    CHECK(warnings.empty());
    CHECK_EQ(other.import_pack(json::Value::make_object(), &warnings), 0);
    CHECK(!warnings.empty());

    // Templates resolve to the saved patch and load its sound.
    session::Session s(catalogs());
    s.set_installed_patches(lib.installed_patches());
    std::string celli;
    for (const auto* t : catalogs().templates->list())
        for (const auto& tt : t->tracks)
            if (tt.instrument_standard_id == "strings.celli" && celli.empty()) celli = t->template_id;
    REQUIRE(!celli.empty());
    REQUIRE(s.new_from_template(celli).ok);
    bool found = false;
    for (const auto& t : s.data().tracks)
        if (t.instrument_standard_id == "strings.celli") {
            found = true;
            CHECK(t.assignment.status == project::ResolutionStatus::Exact);
            CHECK_EQ(t.instrument_plugin.name, std::string("BBC Symphony Orchestra"));
            CHECK_EQ(t.instrument_plugin.state_base64, std::string("c3RhdGU="));
            REQUIRE(t.articulation_map.has_value());
            CHECK_EQ(t.articulation_map->name, std::string("BBC celli KS"));
        }
    CHECK(found);
    // The map is saved with the project and played after reopening.
    const std::string path = std::string(VCC_TEST_TMP_DIR) + "/map-project.vcc2";
    REQUIRE(s.save_as(path));
    session::Session r(catalogs());
    REQUIRE(r.open(path));
    for (const auto& t : r.data().tracks)
        if (t.instrument_standard_id == "strings.celli") {
            REQUIRE(t.articulation_map.has_value());
            CHECK(t.articulation_map->has_natural);
            CHECK_EQ(static_cast<int>(t.articulation_map->natural.keyswitch_note), 24);
        }
    // And through a mapping pack.
    library::Library packed(catalogs().instruments);
    packed.import_pack(lib.export_pack("with map"));
    REQUIRE(packed.patches().size() == 1u);
    CHECK(packed.patches()[0].articulation_map.has_value());
}

TEST_CASE("template packages: custom templates travel with the patches that play them")
{
    // The sender: a custom template made from a built-in, a patch for its
    // celli, and one patch for an instrument it does not use.
    auto sender = session::Catalogs::load_from_directory(VCC_CONTENT_DIR);
    auto lib = make();
    const templates::Template* base = nullptr;
    for (const auto* t : sender->templates->list())
        for (const auto& tt : t->tracks)
            if (tt.instrument_standard_id == "strings.celli" && !base) base = t;
    REQUIRE(base != nullptr);
    templates::Template mine = *base;
    mine.template_id = "user.my_strings";
    mine.display_name = "My Strings";
    REQUIRE(sender->templates->add_user_template(mine));
    library::UserPatch celli;
    celli.display = "Celli - warm";
    celli.plugin = {"VST3", "VST3-BBC-id", "BBC Symphony Orchestra", "Spitfire Audio", "c3RhdGU="};
    celli.instrument_id = "strings.celli";
    lib.add_patch(celli);
    library::UserPatch harp = celli;
    harp.display = "Harp";
    harp.instrument_id = "harp.harp";
    lib.add_patch(harp);

    library::PackageOptions o;
    o.name = "Strings for Sam";
    o.author = "K. Composer";
    o.created = "2026-11-16T10:00:00Z";
    // A built-in goes in too: it comes back as a copy, never over the original.
    const auto doc = library::export_template_package(*sender->templates, &lib, {"user.my_strings", base->template_id, "no.such"}, o);
    const auto summary = library::describe_template_package(json::parse(json::write(doc)).value());
    REQUIRE(summary.has_value());
    CHECK_EQ(summary->name, std::string("Strings for Sam"));
    CHECK_EQ(summary->template_names.size(), 2u);
    CHECK_EQ(summary->patches, 1);  // the celli, not the harp
    CHECK(doc["patches"]["patches"].at(0)["plugin"].get("state") == nullptr);  // settings stay home unless asked

    // The receiver.
    auto receiver = session::Catalogs::load_from_directory(VCC_CONTENT_DIR);
    library::Library theirs(receiver->instruments);
    const std::size_t before = receiver->templates->all().size();
    auto r = library::import_template_package(doc, *receiver->templates, &theirs);
    REQUIRE(r.ok);
    CHECK_EQ(r.template_ids.size(), 2u);
    CHECK_EQ(r.patches, 1);
    CHECK_EQ(receiver->templates->all().size(), before + 2);
    const auto* copy = receiver->templates->get("user." + base->template_id);
    REQUIRE(copy != nullptr);
    CHECK(!copy->built_in);
    CHECK(copy->display_name.find("(imported)") != std::string::npos);
    CHECK(receiver->templates->get(base->template_id)->built_in);
    const auto* got = receiver->templates->get("user.my_strings");
    REQUIRE(got != nullptr);
    CHECK_EQ(got->tracks.size(), base->tracks.size());
    // Again: the custom one is replaced, nothing doubles.
    r = library::import_template_package(doc, *receiver->templates, &theirs);
    CHECK(r.ok && r.replaced == 2);
    CHECK_EQ(receiver->templates->all().size(), before + 2);
    CHECK_EQ(theirs.patches().size(), 1u);
    // Removing: custom yes, built-in no.
    CHECK(receiver->templates->remove_user_template("user.my_strings"));
    CHECK(!receiver->templates->remove_user_template(base->template_id));

    // Not a package, or a broken template: said, not imported.
    CHECK(!library::import_template_package(json::Value::make_object(), *receiver->templates, &theirs).ok);
    json::Value bad = doc;
    json::Value broken = json::Object{{"template_id", "user.broken"}, {"display_name", "Broken"}};
    bad.set("templates", json::Object{{"schema_version", 1}, {"pack", "x"}, {"templates", json::Array{broken}}});
    const auto br = library::import_template_package(bad, *receiver->templates, nullptr);
    CHECK(!br.ok && !br.error.empty());
}

TEST_CASE("library: sound assignments - an instrument's articulation to a saved sound; kept, exported, imported")
{
    auto lib = make();
    library::UserPatch legato;
    legato.display = "Violins I - Legato (mine)";
    legato.plugin = {"VST3", "VST3-Opus-id", "Opus", "EastWest", "c3RhdGU="};
    legato.instrument_id = "strings.violins_1";
    legato.articulations = {"legato"};
    const auto lid = lib.add_patch(legato);
    library::UserPatch pizz = legato;
    pizz.display = "Violins I - Pizz (mine)";
    pizz.articulations = {"pizzicato"};
    const auto pid = lib.add_patch(pizz);
    lib.set_assignment("strings.violins_1", "legato", lid);
    lib.set_assignment("strings.violins_1", "pizzicato", pid);
    lib.set_assignment("strings.violins_1", "staccato", "nope");  // no such sound: not kept
    REQUIRE(lib.assigned("strings.violins_1", "legato") != nullptr);
    CHECK_EQ(lib.assigned("strings.violins_1", "legato")->display, legato.display);
    CHECK(lib.assigned("strings.violins_1", "staccato") == nullptr);
    CHECK_EQ(lib.assignments().size(), 2u);
    // Kept with the user's library.
    library::Library back(catalogs().instruments);
    back.load_user(lib.user_json());
    REQUIRE(back.assigned("strings.violins_1", "pizzicato") != nullptr);
    CHECK_EQ(back.assigned("strings.violins_1", "pizzicato")->display, pizz.display);
    // Exported (with the sounds' settings) and imported on another computer.
    const auto pack = lib.export_pack("My sounds", {}, true);
    library::Library other(catalogs().instruments);
    other.import_pack(*json::parse(json::write(pack)));
    REQUIRE(other.assigned("strings.violins_1", "legato") != nullptr);
    CHECK_EQ(other.assigned("strings.violins_1", "legato")->plugin.state_base64, std::string("c3RhdGU="));
    // Clearing one, and removing a sound, take their assignments away.
    lib.set_assignment("strings.violins_1", "legato", "");
    CHECK(lib.assigned("strings.violins_1", "legato") == nullptr);
    lib.remove_patch(pid);
    CHECK(lib.assignments().empty());
}
