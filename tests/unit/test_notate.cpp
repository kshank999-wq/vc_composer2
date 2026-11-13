// MIDI -> notation (Main Spec §8): piano-roll edits of score notes become
// notation when they can be written, and performance overrides when they
// cannot; MIDI regions are notated into the score. Ambiguous cases offer
// their readings instead of guessing.
#include <algorithm>

#include "vcc/editing/ScoreEditor.h"
#include "vcc/project/Render.h"
#include "vcc/session/Session.h"
#include "vcc_test.h"

using namespace vcc;
using session::NoteTarget;

namespace {
const session::Catalogs& catalogs()
{
    static auto c = session::Catalogs::load_from_directory(VCC_CONTENT_DIR);
    return *c;
}

notation::Pitch P(notation::Step step, int octave = 4)
{
    notation::Pitch p;
    p.step = step;
    p.octave = static_cast<std::int8_t>(octave);
    return p;
}

/// Quarter notes in 4/4 from the start of bar 1.
std::vector<notation::EventId> write_line(session::Session& s, const std::string& track,
                                          const std::vector<notation::Step>& steps)
{
    notation::Score score = s.data().find_track(track)->active_version()->score;
    editing::ScoreEditor editor(score);
    editor.set_cursor(editing::EditCursor{});
    editor.set_duration(notation::NoteValue::Quarter);
    std::vector<notation::EventId> ids;
    for (auto step : steps) {
        auto r = editor.insert_pitch(P(step));
        if (!r.succeeded()) return {};
        ids.push_back(r.new_event);
    }
    if (!s.commit_score(track, score, "Enter notes")) return {};
    return ids;
}

const project::Track& track(const session::Session& s, const std::string& id) { return *s.data().find_track(id); }
const notation::Score& score_of(const session::Session& s, const std::string& id)
{
    return track(s, id).active_version()->score;
}

NoteEvent played(const session::Session& s, const std::string& t, notation::EventId id)
{
    const auto r = project::render_track(track(s, t));
    for (std::size_t i = 0; i < r.notes.size(); ++i)
        if (r.note_ids[i] == id) return r.notes[i];
    return {};
}

NoteTarget target_of(const NoteEvent& n) { return {n.start, n.duration, n.pitch}; }

bool has_option(const session::NotatePlan& p, const std::string& id)
{
    return std::any_of(p.options.begin(), p.options.end(), [&](const auto& o) { return o.id == id; });
}
std::size_t option_index(const session::NotatePlan& p, const std::string& id)
{
    for (std::size_t i = 0; i < p.options.size(); ++i)
        if (p.options[i].id == id) return i;
    return p.options.size();
}

constexpr Ticks Q = kTicksPerQuarter;

struct Fixture {
    session::Session s{catalogs()};
    std::string t;
    std::vector<notation::EventId> ids;
    explicit Fixture(const std::string& instrument = "woodwinds.flute",
                     std::vector<notation::Step> steps = {notation::Step::C, notation::Step::D, notation::Step::E,
                                                          notation::Step::F})
    {
        s.new_empty();
        t = s.add_track("Part", instrument);
        ids = write_line(s, t, steps);
    }
};
}  // namespace

TEST_CASE("a grid move of a score note is written into the score")
{
    Fixture f("woodwinds.flute", {notation::Step::C, notation::Step::D});
    REQUIRE(f.ids.size() == 2);
    auto n = played(f.s, f.t, f.ids[1]);
    auto target = target_of(n);
    target.start += Q / 4;  // a sixteenth later (the last note: nothing to run into)
    const auto plan = f.s.plan_note_edit(f.t, f.ids[1], target);
    REQUIRE(plan.ok());
    CHECK(plan.ambiguity.empty());
    CHECK_EQ(plan.options.size(), 1u);
    CHECK_EQ(plan.options[0].id, std::string("notate"));
    REQUIRE(f.s.apply_notate(f.t, plan, 0, "Move note"));
    const auto* written = score_of(f.s, f.t).find_note(f.ids[1]);
    REQUIRE(written);  // the note keeps its id
    CHECK_EQ(written->start_tick, Q + Q / 4);
    CHECK_EQ(played(f.s, f.t, f.ids[1]).start, n.start + Q / 4);
    CHECK(f.s.performance_of(f.t)->empty());  // notation, not a performance
    REQUIRE(f.s.undo());
    CHECK_EQ(score_of(f.s, f.t).find_note(f.ids[1])->start_tick, Q);
}

TEST_CASE("an off-grid move is kept as a performance override")
{
    Fixture f;
    auto n = played(f.s, f.t, f.ids[2]);
    auto target = target_of(n);
    target.start -= 37;
    const auto plan = f.s.plan_note_edit(f.t, f.ids[2], target);
    REQUIRE(plan.ok());
    CHECK(plan.ambiguity.empty());
    REQUIRE(plan.options.size() == 1);
    CHECK_EQ(plan.options[0].id, std::string("override"));
    CHECK(!plan.options[0].changes_notation);
    REQUIRE(f.s.apply_notate(f.t, plan, 0, "Nudge"));
    CHECK_EQ(score_of(f.s, f.t).find_note(f.ids[2])->start_tick, 2 * Q);  // the page is unchanged
    CHECK_EQ(f.s.performance_of(f.t)->at(f.ids[2]).onset, -37);
    CHECK_EQ(played(f.s, f.t, f.ids[2]).start, n.start - 37);
}

TEST_CASE("a triplet landing asks: triplet, straight, or performance")
{
    Fixture f;
    auto n = played(f.s, f.t, f.ids[0]);
    auto target = target_of(n);
    // The end dragged back by a third of a beat: a triplet quarter written
    // (the edit moves the written end by what it moves the played end).
    target.duration -= Q / 3;
    const auto plan = f.s.plan_note_edit(f.t, f.ids[0], target);
    REQUIRE(plan.ok());
    CHECK_EQ(plan.ambiguity, std::string("tuplet"));
    CHECK(!plan.question.empty());
    CHECK(has_option(plan, "triplet"));
    CHECK(has_option(plan, "straight"));
    CHECK(has_option(plan, "override"));
    CHECK_EQ(plan.options.front().id, std::string("triplet"));
    REQUIRE(f.s.apply_notate(f.t, plan, option_index(plan, "triplet"), "Triplet"));
    const auto* w = score_of(f.s, f.t).find_note(f.ids[0]);
    REQUIRE(w);
    CHECK(w->duration.tuplet.is_tuplet());
    CHECK_EQ(w->duration.ticks(), Q * 2 / 3);
}

TEST_CASE("stretching a note into the next asks, performance first")
{
    Fixture f;
    auto n = played(f.s, f.t, f.ids[0]);
    auto target = target_of(n);
    target.duration += Q / 2;  // runs into the D
    const auto plan = f.s.plan_note_edit(f.t, f.ids[0], target);
    REQUIRE(plan.ok());
    CHECK_EQ(plan.ambiguity, std::string("overlap"));
    CHECK_EQ(plan.options.front().id, std::string("override"));
    REQUIRE(has_option(plan, "voice"));
    REQUIRE(f.s.apply_notate(f.t, plan, option_index(plan, "voice"), "Second voice"));
    const auto* w = score_of(f.s, f.t).find_note(f.ids[0]);
    REQUIRE(w);
    CHECK_EQ(static_cast<int>(w->voice), 1);
    CHECK_EQ(w->duration.ticks(), Q + Q / 2);
    CHECK_EQ(w->duration.dots, 1);  // a dotted quarter
}

TEST_CASE("a new pitch is notation; a chromatic one offers both spellings")
{
    Fixture f;
    auto target = target_of(played(f.s, f.t, f.ids[0]));
    target.pitch = 67;  // G, diatonic in C
    auto plan = f.s.plan_note_edit(f.t, f.ids[0], target);
    REQUIRE(plan.ok());
    CHECK(plan.ambiguity.empty());
    REQUIRE(f.s.apply_notate(f.t, plan, 0, "Pitch"));
    CHECK(score_of(f.s, f.t).find_note(f.ids[0])->pitch == P(notation::Step::G));

    target.pitch = 61;  // C# or Db
    plan = f.s.plan_note_edit(f.t, f.ids[0], target);
    REQUIRE(plan.ok());
    CHECK_EQ(plan.ambiguity, std::string("spelling"));
    REQUIRE(plan.options.size() == 2);
    const auto& a = plan.options[0].score.find_note(f.ids[0])->pitch;
    const auto& b = plan.options[1].score.find_note(f.ids[0])->pitch;
    CHECK(a.step != b.step);
    CHECK_EQ(notation::midi_note_number(a), 61);
    CHECK_EQ(notation::midi_note_number(b), 61);
}

TEST_CASE("a transposing part is edited at sounding pitch and written transposed")
{
    Fixture f("woodwinds.clarinet_bb", {notation::Step::D});
    REQUIRE(f.ids.size() == 1);
    auto n = played(f.s, f.t, f.ids[0]);
    CHECK_EQ(n.pitch, 60);  // written D sounds C
    auto target = target_of(n);
    target.pitch = 62;  // sounding D
    const auto plan = f.s.plan_note_edit(f.t, f.ids[0], target);
    REQUIRE(plan.ok());
    REQUIRE(f.s.apply_notate(f.t, plan, 0, "Pitch"));
    CHECK(score_of(f.s, f.t).find_note(f.ids[0])->pitch == P(notation::Step::E));
    CHECK_EQ(played(f.s, f.t, f.ids[0]).pitch, 62);
}

TEST_CASE("a note stretched over a barline is tied across it")
{
    Fixture f("woodwinds.flute", {notation::Step::C, notation::Step::D, notation::Step::E, notation::Step::F});
    auto target = target_of(played(f.s, f.t, f.ids[3]));
    target.duration += Q;  // F on beat 4 now lasts into bar 2
    const auto plan = f.s.plan_note_edit(f.t, f.ids[3], target);
    REQUIRE(plan.ok());
    REQUIRE(f.s.apply_notate(f.t, plan, 0, "Lengthen"));
    const auto& sc = score_of(f.s, f.t);
    const auto* head = sc.find_note(f.ids[3]);
    REQUIRE(head);
    CHECK(head->tied_to_next);
    CHECK_EQ(head->end_tick(), 4 * Q);  // stops at the barline
    bool continued = false;
    for (const auto& n : sc.notes(0)) continued = continued || (n.start_tick == 4 * Q && n.duration.ticks() == Q);
    CHECK(continued);
    CHECK_EQ(played(f.s, f.t, f.ids[3]).duration > Q, true);  // still one sounding note
}

TEST_CASE("a played region is notated and still plays as recorded")
{
    Fixture f("woodwinds.flute", {});
    // A loose performance of C D E G in bar 2.
    const std::vector<NoteEvent> notes = {{4 * Q + 23, Q - 60, 60, 70, 0, {}},
                                          {5 * Q - 31, Q - 10, 62, 92, 0, {}},
                                          {6 * Q + 12, Q / 2 - 40, 64, 81, 0, {}},
                                          {6 * Q + Q / 2 - 8, Q + Q / 2 + 30, 67, 100, 0, {}}};
    REQUIRE(f.s.edit("Record", [&](project::ProjectData& d) {
        project::MidiRegion r;
        r.id = "rgn-played";
        r.start = 4 * Q;
        r.end = 8 * Q;
        r.notes = notes;
        r.source = "recorded";
        d.find_track(f.t)->active_version()->regions.push_back(r);
        return true;
    }));
    const auto before = project::render_track(track(f.s, f.t)).notes;
    const auto plan = f.s.plan_notate_region(f.t, "rgn-played");
    REQUIRE(plan.ok());
    CHECK(plan.ambiguity.empty());
    REQUIRE(f.s.apply_notate(f.t, plan, 0, "Notate region"));
    const auto& v = *track(f.s, f.t).active_version();
    CHECK(v.regions.empty());
    std::vector<Ticks> starts;
    for (const auto& n : v.score.notes(0))
        if (!n.is_rest) starts.push_back(n.start_tick);
    CHECK((starts == std::vector<Ticks>{4 * Q, 5 * Q, 6 * Q, 6 * Q + Q / 2}));
    // Kept feel: the notated passage sounds exactly as the region did.
    const auto after = project::render_track(track(f.s, f.t)).notes;
    CHECK(after == before);
    // Undo brings the region back.
    REQUIRE(f.s.undo());
    CHECK_EQ(track(f.s, f.t).active_version()->regions.size(), 1u);

    // Without the feel it plays as written.
    session::RegionNotateSettings plain;
    plain.keep_feel = false;
    const auto p2 = f.s.plan_notate_region(f.t, "rgn-played", plain);
    REQUIRE(f.s.apply_notate(f.t, p2, 0, "Notate region"));
    CHECK(f.s.performance_of(f.t)->empty());
    CHECK_EQ(project::render_track(track(f.s, f.t)).notes.front().start, 4 * Q);
}

TEST_CASE("a region over written notes asks where it goes; triplets offer straight time")
{
    Fixture f;
    REQUIRE(f.s.edit("Region", [&](project::ProjectData& d) {
        project::MidiRegion r;
        r.id = "rgn-over";
        r.start = 0;
        r.end = 4 * Q;
        r.notes = {{0, Q * 2 / 3, 72, 80, 0, {}}, {Q * 2 / 3, Q * 2 / 3, 74, 80, 0, {}}, {Q * 4 / 3, Q * 2 / 3, 76, 80, 0, {}}};
        d.find_track(f.t)->active_version()->regions.push_back(r);
        return true;
    }));
    const auto plan = f.s.plan_notate_region(f.t, "rgn-over");
    REQUIRE(plan.ok());
    CHECK_EQ(plan.ambiguity, std::string("placement"));
    CHECK(has_option(plan, "voice"));
    CHECK(has_option(plan, "replace"));
    CHECK(has_option(plan, "straight"));
    REQUIRE(f.s.apply_notate(f.t, plan, option_index(plan, "voice"), "Notate"));
    int voice2 = 0, triplets = 0;
    for (const auto& n : score_of(f.s, f.t).notes(0)) {
        if (n.voice == 1) ++voice2;
        if (n.duration.tuplet.is_tuplet()) ++triplets;
    }
    CHECK_EQ(voice2, 3);
    CHECK_EQ(triplets, 3);
    CHECK_EQ(score_of(f.s, f.t).find_note(f.ids[0])->voice, 0);  // the written line is untouched

    // A plan made before the score changed is refused.
    REQUIRE(f.s.undo());
    const auto stale = f.s.plan_notate_region(f.t, "rgn-over");
    REQUIRE(f.s.edit_performance(f.t, {f.ids[0]}, "x", [](notation::EventId, project::NoteOverride& o) { o.velocity = 3; }));
    notation::Score sc = score_of(f.s, f.t);
    sc.remove_note(f.ids[3]);
    REQUIRE(f.s.commit_score(f.t, sc, "Delete"));
    CHECK(!f.s.apply_notate(f.t, stale, 0, "Notate"));
}
