// Ostinato Creator "Send to Track": written on the staff (notation first).
#include "vcc/ostinato/Ostinato.h"
#include "vcc/project/Percussion.h"
#include "vcc/session/Session.h"
#include "vcc_test.h"

using namespace vcc;

namespace {
const session::Catalogs& catalogs()
{
    static auto c = session::Catalogs::load_from_directory(VCC_CONTENT_DIR);
    return *c;
}
}  // namespace

TEST_CASE("ostinato send: the figure is written on the staff, as notes the score plays")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto track = s.add_track("Celli", "strings.celli");
    REQUIRE(!track.empty());
    ostinato::OstinatoOptions o;
    o.length_bars = 2;
    o.range_low = 36;
    o.range_high = 60;
    const auto v = ostinato::Generator(o).generate(7, "A");
    REQUIRE(!v.notes.empty());
    const auto before_regions = s.data().find_track(track)->active_version()->regions.size();
    const auto r = s.send_ostinato(track, v, "check", project::CollisionPolicy::Ask);
    CHECK(r.ok);
    const auto* t = s.data().find_track(track);
    // On the staff: as many sounding notes as the figure has (ties may split some).
    std::size_t written = 0;
    for (const auto& n : t->active_version()->score.notes(0)) written += n.is_rest ? 0 : 1;
    CHECK(written >= v.notes.size());
    // And not as a MIDI region beside it.
    CHECK_EQ(t->active_version()->regions.size(), before_regions);
    // One undo takes it away.
    CHECK(s.undo());
    std::size_t after = 0;
    for (const auto& n : s.data().find_track(track)->active_version()->score.notes(0)) after += n.is_rest ? 0 : 1;
    CHECK_EQ(after, 0u);
}

TEST_CASE("ostinato send: drums go on the drum staff by the kit's map")
{
    session::Session s(catalogs());
    s.new_empty();
    std::string kit_id;
    for (const auto& i : catalogs().instruments.all())
        if (!project::default_percussion_map(i.id).empty()) {
            kit_id = i.id;
            break;
        }
    REQUIRE(!kit_id.empty());
    const auto track = s.add_track("Kit", kit_id);
    ostinato::OstinatoOptions o;
    o.length_bars = 1;
    o.percussion = true;
    const auto v = ostinato::Generator(o).generate(3, "A");
    REQUIRE(!v.notes.empty());
    CHECK(s.send_ostinato(track, v, "drums", project::CollisionPolicy::Ask).ok);
    std::size_t written = 0;
    for (const auto& n : s.data().find_track(track)->active_version()->score.notes(0)) written += n.is_rest ? 0 : 1;
    CHECK(written > 0);
}
