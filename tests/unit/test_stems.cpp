// Stems: which tracks each prints, defaults, coverage, and persistence.
#include <algorithm>
#include <filesystem>

#include "vcc/project/Stems.h"
#include "vcc/session/Session.h"
#include "vcc_test.h"

using namespace vcc;
using namespace vcc::project;

namespace {
const session::Catalogs& catalogs()
{
    static auto c = session::Catalogs::load_from_directory(VCC_CONTENT_DIR);
    return *c;
}
std::vector<std::string> names(const std::vector<const Track*>& ts)
{
    std::vector<std::string> out;
    for (const auto* t : ts) out.push_back(t->name);
    return out;
}
}  // namespace

TEST_CASE("stems resolve through buses and default from groups and families")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto vln = s.add_track("Violins", "strings.violins");
    const auto vc = s.add_track("Celli", "strings.cellos");
    const auto hn = s.add_track("Horns", "brass.horn_section");
    const auto syn = s.add_track("Pad", "synth.pad_warm");
    const auto ref = s.add_reference_track("Temp");
    const auto strings = s.add_bus("Strings Bus", "group");
    const auto inner = s.add_bus("Low Strings", "group");
    const auto hall = s.add_bus("Hall", "fx");
    REQUIRE(s.update_bus(inner, "route", [&](Bus& b) { b.output = strings; }));
    REQUIRE(s.update_track_mix(vln, "out", [&](Track& t) { t.output_bus_id = strings; t.sends.emplace_back(hall, -10); }));
    REQUIRE(s.update_track_mix(vc, "out2", [&](Track& t) { t.output_bus_id = inner; }));

    StemDef st;
    st.sources = {strings};
    CHECK(names(stem_tracks(st, s.data())) == (std::vector<std::string>{"Violins", "Celli"}));
    st.sources = {hn, ref};  // the reference track is not a deliverable
    CHECK(names(stem_tracks(st, s.data())) == (std::vector<std::string>{"Horns"}));

    const auto defaults = s.stems();
    REQUIRE(defaults.size() >= 3u);
    CHECK_EQ(defaults[0].name, std::string("Strings"));  // the outer group, not "Low Strings"
    CHECK(defaults[0].sources == std::vector<std::string>{strings});
    bool brass = false, synth = false;
    for (const auto& d : defaults) {
        brass |= d.name == "Brass" && d.sources == std::vector<std::string>{hn};
        synth |= d.name == "Synth" && d.sources == std::vector<std::string>{syn};
    }
    CHECK(brass);
    CHECK(synth);
    CHECK(stem_coverage(defaults, s.data()).complete());

    // Coverage reports what a deliverable would miss or double.
    std::vector<StemDef> bad = {defaults[0], defaults[0]};
    bad.push_back(StemDef{"", "Nothing", {"no-such"}, true});
    const auto cov = stem_coverage(bad, s.data());
    CHECK(!cov.complete());
    CHECK_EQ(cov.doubled.size(), 2u);
    CHECK(std::find(cov.missing.begin(), cov.missing.end(), "Horns") != cov.missing.end());
    CHECK(cov.empty == std::vector<std::string>{"Nothing"});

    // Saved stems replace the defaults, survive save/reopen, and undo.
    auto mine = defaults;
    mine[0].name = "Str";
    mine[0].include_effects = false;
    mine.push_back(StemDef{"", "FX", {hall}, true});
    REQUIRE(s.set_stems(mine));
    CHECK_EQ(s.stems()[0].name, std::string("Str"));
    CHECK(!s.stems().back().id.empty());
    std::filesystem::create_directories(std::string(VCC_TEST_TMP_DIR) + "/stems");
    const std::string path = std::string(VCC_TEST_TMP_DIR) + "/stems/p.vcc2";
    REQUIRE(s.save_as(path));
    session::Session r(catalogs());
    REQUIRE(r.open(path));
    REQUIRE(r.data().stems.size() == mine.size());
    CHECK(!r.data().stems[0].include_effects);
    CHECK(r.data().stems.back().sources == std::vector<std::string>{hall});
    CHECK(s.undo());
    CHECK(s.data().stems.empty());
}
