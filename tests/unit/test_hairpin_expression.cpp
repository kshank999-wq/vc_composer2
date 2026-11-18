// Crescendos and diminuendos as swells: a CC11 (expression) ramp across each
// hairpin, 0 to 100 for a crescendo and 100 to 0 for a diminuendo by default,
// editable (the Producer's CC11 lane), saved with the score.
#include "vcc/editing/ScoreEditor.h"
#include "vcc/notation/ScoreFile.h"
#include "vcc/project/Render.h"
#include "vcc/session/Session.h"
#include "vcc_test.h"

using namespace vcc;
constexpr notation::Ticks kQ = notation::kTicksPerQuarter;

namespace {
const session::Catalogs& catalogs()
{
    static auto c = session::Catalogs::load_from_directory(VCC_CONTENT_DIR);
    return *c;
}

/// Violins: a whole note C5 (bar 1) under a hairpin of `kind`, then a quarter in bar 2.
std::string swell(session::Session& s, notation::HairpinKind kind)
{
    const auto id = s.add_track("Violins", "strings.violins_1");
    s.update_track_mix(id, "Write", [kind](project::Track& t) {
        editing::ScoreEditor editor(t.active_version()->score);
        notation::Pitch c;
        c.step = notation::Step::C;
        c.octave = 5;
        editor.set_duration(notation::NoteValue::Whole);
        editor.insert_at(0, 0, c);
        editor.set_duration(notation::NoteValue::Quarter);
        editor.insert_at(0, 4 * kQ, c);
        editor.add_hairpin(0, 0, 4 * kQ, kind);
    });
    return id;
}

std::vector<ControllerEvent> expression(const project::Track& t)
{
    std::vector<ControllerEvent> out;
    for (const auto& c : project::render_track(t).controllers)
        if (c.controller == 11) out.push_back(c);
    return out;
}

int value_at(const std::vector<ControllerEvent>& ev, Ticks t)
{
    int v = -1;
    for (const auto& c : ev)
        if (c.tick <= t) v = c.value;
    return v;
}
}  // namespace

TEST_CASE("hairpin expression: a crescendo swells the held note from 0 to 100")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto id = swell(s, notation::HairpinKind::Crescendo);
    const auto ev = expression(*s.data().find_track(id));
    REQUIRE(!ev.empty());
    CHECK_EQ(value_at(ev, 0), 0);
    const int mid = value_at(ev, 2 * kQ);
    CHECK(mid > 35);
    CHECK(mid < 65);
    CHECK_EQ(value_at(ev, 4 * kQ), 100);
    // Rising all the way: never down.
    for (std::size_t i = 1; i < ev.size(); ++i)
        if (ev[i].tick > 0 && ev[i].tick <= 4 * kQ) CHECK(ev[i].value >= ev[i - 1].value);
}

TEST_CASE("hairpin expression: a diminuendo dies away to 0, and the next note is back at 100")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto id = swell(s, notation::HairpinKind::Diminuendo);
    const auto ev = expression(*s.data().find_track(id));
    CHECK_EQ(value_at(ev, 0), 100);
    CHECK_EQ(value_at(ev, 4 * kQ - 1) <= 4, true);
    CHECK_EQ(value_at(ev, 4 * kQ), 100);  // the next note starts at 100 again
}

TEST_CASE("hairpin expression: edited values play, are one undo step a drag, and are saved")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto id = swell(s, notation::HairpinKind::Crescendo);
    for (int v = 40; v <= 120; v += 20) CHECK(s.set_hairpin_expression(id, 0, 0, 20, v));
    const auto& t = *s.data().find_track(id);
    REQUIRE(t.active_version()->score.hairpins().size() == 1u);
    auto ev = expression(t);
    CHECK_EQ(value_at(ev, 0), 20);
    CHECK_EQ(value_at(ev, 4 * kQ - 1) >= 115, true);
    CHECK(s.undo());
    ev = expression(*s.data().find_track(id));
    CHECK_EQ(value_at(ev, 0), 0);  // back to the default 0-100
    CHECK(s.redo());

    // Saved with the score, and read back.
    const auto& score = s.data().find_track(id)->active_version()->score;
    notation::Score back;
    const auto read = notation::score_from_json(notation::score_to_json(score), back);
    REQUIRE(read.ok);
    REQUIRE(back.hairpins().size() == 1u);
    CHECK_EQ(back.hairpins()[0].expression_start, 20);
    CHECK_EQ(back.hairpins()[0].expression_end, 120);
}

TEST_CASE("hairpin expression: a part without hairpins sends no expression")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto id = s.add_track("Flute", "woodwinds.flute");
    CHECK(expression(*s.data().find_track(id)).empty());
}
