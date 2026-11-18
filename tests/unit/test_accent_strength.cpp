// The staff tools' accent dial: how strongly a track's accents play (1-100).
#include "vcc/editing/ScoreEditor.h"
#include "vcc/project/ArticulationMap.h"
#include "vcc/project/Render.h"
#include "vcc/session/Session.h"
#include "vcc_test.h"

using namespace vcc;

namespace {
const session::Catalogs& catalogs()
{
    static auto c = session::Catalogs::load_from_directory(VCC_CONTENT_DIR);
    return *c;
}

/// A track with two quarter notes: C plain, E accented. Returns its id.
std::string accented_track(session::Session& s)
{
    const auto id = s.add_track("Flute", "woodwinds.flute");
    s.update_track_mix(id, "Write", [](project::Track& t) {
        auto& score = t.active_version()->score;
        editing::ScoreEditor editor(score);
        editor.set_duration(notation::NoteValue::Quarter);
        notation::Pitch c, e;
        c.step = notation::Step::C;
        c.octave = 5;
        e.step = notation::Step::E;
        e.octave = 5;
        editor.insert_at(0, 0, c);
        editor.insert_at(0, notation::kTicksPerQuarter, e);
        editor.toggle_articulation(notation::Articulation::Accent);
    });
    return id;
}

/// (plain velocity, accented velocity) as the track plays.
std::pair<int, int> velocities(const project::Track& t)
{
    const auto r = project::render_track(t);
    int plain = -1, accented = -1;
    for (const auto& n : r.notes) (n.start == 0 ? plain : accented) = n.velocity;
    return {plain, accented};
}
}  // namespace

TEST_CASE("accent strength: 50 plays the map's own accent, 100 lifts it more, 1 all but none")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto id = accented_track(s);
    const auto* t = s.data().find_track(id);
    REQUIRE(t != nullptr);
    CHECK_EQ(t->accent_strength, 50);
    const auto [plain, normal] = velocities(*t);
    REQUIRE(plain > 0);
    CHECK(normal > plain);

    CHECK(s.set_accent_strength(id, 100));
    const auto strong = velocities(*s.data().find_track(id)).second;
    CHECK(strong > normal);
    CHECK(s.set_accent_strength(id, 1));
    const auto weak = velocities(*s.data().find_track(id));
    CHECK(weak.second < normal);
    CHECK(weak.second - weak.first <= 1);
    // The plain note never changes.
    CHECK_EQ(weak.first, plain);
    // Clamped to 1-100.
    CHECK(s.set_accent_strength(id, 500));
    CHECK_EQ(s.data().find_track(id)->accent_strength, 100);
}

TEST_CASE("accent strength: a turn of the dial is one undo step, and it is saved with the project")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto id = accented_track(s);
    for (int v = 55; v <= 90; v += 5) s.set_accent_strength(id, v);
    CHECK_EQ(s.data().find_track(id)->accent_strength, 90);
    CHECK(s.undo());
    CHECK_EQ(s.data().find_track(id)->accent_strength, 50);
    CHECK(s.redo());

    const auto doc = s.project().to_json();
    const auto back = project::Project::from_json(doc);
    REQUIRE(back.has_value());
    CHECK_EQ(back->data().find_track(id)->accent_strength, 90);
    // The render cache key changes with it, so playback re-renders.
    project::Track a = *s.data().find_track(id), b = a;
    b.accent_strength = 50;
    CHECK(project::expression_map_key(a) != project::expression_map_key(b));
}
