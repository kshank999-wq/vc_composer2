// The built-in orchestra's articulations: written pizzicato, tremolo, mutes and
// harmonics switch to General MIDI's own programs, and back for other notes.
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

/// Celli: C3 arco, D3 pizzicato, E3 arco.
std::string celli(session::Session& s)
{
    const auto id = s.add_track("Celli", "strings.celli");
    s.update_track_mix(id, "Write", [](project::Track& t) {
        editing::ScoreEditor editor(t.active_version()->score);
        editor.set_duration(notation::NoteValue::Quarter);
        const notation::Step steps[] = {notation::Step::C, notation::Step::D, notation::Step::E};
        for (int i = 0; i < 3; ++i) {
            notation::Pitch p;
            p.step = steps[i];
            p.octave = 3;
            editor.insert_at(0, i * notation::kTicksPerQuarter, p);
            if (i == 1) editor.toggle_articulation(notation::Articulation::Pizzicato);
        }
    });
    return id;
}
}  // namespace

TEST_CASE("gm articulations: bowed strings, brass and guitars get their General MIDI sounds")
{
    using A = notation::Articulation;
    auto has = [](int program, A a, int expect) {
        for (const auto& g : project::gm_articulation_programs(program))
            if (g.articulation == a) return g.program == expect;
        return false;
    };
    CHECK(has(42, A::Pizzicato, 45));  // cello
    CHECK(has(48, A::Tremolo, 44));    // string ensemble
    CHECK(has(56, A::Mute, 59));       // trumpet
    CHECK(has(25, A::Harmonic, 31));   // steel guitar
    CHECK(project::gm_articulation_programs(73).empty());  // flute: none
    CHECK(project::gm_articulation_programs(0).empty());   // piano: none
}

TEST_CASE("gm articulations: a pizzicato note switches to Pizzicato Strings, the next arco note switches back")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto id = celli(s);
    const auto& t = *s.data().find_track(id);
    const auto map = project::with_gm_articulations(project::expression_map_for(t), 42);
    const auto r = project::render_track(t, map);
    REQUIRE(r.notes.size() == 3u);
    // The program in force at each note's start.
    auto program_at = [&](Ticks tick) {
        int p = -1;
        for (const auto& c : r.controllers)
            if (c.controller == vcc::kProgramChangeController && c.tick <= tick) p = c.value;
        return p;
    };
    CHECK_EQ(program_at(r.notes[0].start), 42);
    CHECK_EQ(program_at(r.notes[1].start), 45);
    CHECK_EQ(program_at(r.notes[2].start), 42);
    // The written score is untouched, and a map without switches renders none.
    const auto plain = project::render_track(t);
    std::size_t programs = 0;
    for (const auto& c : plain.controllers) programs += c.controller == vcc::kProgramChangeController ? 1 : 0;
    CHECK_EQ(programs, 0u);
}
