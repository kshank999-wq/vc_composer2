// Phrase blocks (docs/spec/ui, "Phrase blocks"): named spans that move with
// the music written in them, saved with the version.
#include "vcc/editing/ScoreEditor.h"
#include "vcc/session/Session.h"
#include "vcc_test.h"

using namespace vcc;

namespace {
const session::Catalogs& catalogs()
{
    static auto c = session::Catalogs::load_from_directory(VCC_CONTENT_DIR);
    return *c;
}
constexpr Ticks Q = kTicksPerQuarter;
constexpr Ticks BAR = 4 * Q;

/// Bar 1: C D E F (quarters), with a p at the start.
std::vector<notation::EventId> write_bar(session::Session& s, const std::string& t)
{
    notation::Score score = s.data().find_track(t)->active_version()->score;
    editing::ScoreEditor editor(score);
    editor.set_cursor(editing::EditCursor{});
    editor.set_duration(notation::NoteValue::Quarter);
    std::vector<notation::EventId> ids;
    for (auto step : {notation::Step::C, notation::Step::D, notation::Step::E, notation::Step::F}) {
        notation::Pitch p;
        p.step = step;
        ids.push_back(editor.insert_pitch(p).new_event);
    }
    score.add_dynamic({0, notation::DynamicLevel::Piano, 0, {}, 0});
    s.commit_score(t, score, "Notes");
    return ids;
}

const project::TrackVersion& version(const session::Session& s, const std::string& t)
{
    return *s.data().find_track(t)->active_version();
}
}  // namespace

TEST_CASE("a phrase moves with its notes and markings, and refuses to land on others")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto t = s.add_track("Flute", "woodwinds.flute");
    const auto ids = write_bar(s, t);
    const auto ph = s.add_phrase(t, "Flute . Verse call", 0, BAR);
    REQUIRE(!ph.empty());
    CHECK_EQ(version(s, t).phrases.size(), 1u);

    REQUIRE(s.move_phrase(t, ph, 2 * BAR));
    const auto& v = version(s, t);
    CHECK_EQ(v.phrases[0].start, 2 * BAR);
    CHECK_EQ(v.score.find_note(ids[0])->start_tick, 2 * BAR);
    CHECK_EQ(v.score.find_note(ids[3])->start_tick, 2 * BAR + 3 * Q);
    REQUIRE(v.score.dynamics().size() == 1);
    CHECK_EQ(v.score.dynamics()[0].tick, 2 * BAR);  // the p went with it
    REQUIRE(s.undo());
    CHECK_EQ(version(s, t).score.find_note(ids[0])->start_tick, 0);

    // Something written in bar 3: the phrase cannot land there.
    const auto other = s.add_phrase(t, "Second", 0, BAR);
    REQUIRE(!other.empty());
    REQUIRE(s.move_phrase(t, other, 2 * BAR));  // moves the notes to bar 3
    // An empty block over bar 5 cannot move onto bar 3's notes.
    std::string why;
    const auto blocked = s.add_phrase(t, "Blocked", 4 * BAR, 5 * BAR);
    CHECK(!s.move_phrase(t, blocked, -2 * BAR, &why));
    CHECK(!why.empty());
    CHECK(!s.move_phrase(t, blocked, -6 * BAR, &why));  // not before the start
}

TEST_CASE("trim, rename, split, duplicate and remove")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto t = s.add_track("Flute", "woodwinds.flute");
    const auto ids = write_bar(s, t);
    const auto ph = s.add_phrase(t, "Call", 0, BAR);
    REQUIRE(s.edit_performance(t, {ids[1]}, "Push", [](notation::EventId, project::NoteOverride& o) { o.onset = 20; }));

    // Trimming changes the span only; a drag of several steps is one undo step.
    REQUIRE(s.set_phrase_span(t, ph, 0, 3 * Q, "Trim phrase"));
    REQUIRE(s.set_phrase_span(t, ph, 0, 2 * Q, "Trim phrase"));
    CHECK_EQ(version(s, t).phrases[0].end, 2 * Q);
    CHECK_EQ(version(s, t).score.total_note_count(), 4u);
    REQUIRE(s.undo());
    CHECK_EQ(version(s, t).phrases[0].end, BAR);

    REQUIRE(s.rename_phrase(t, ph, "Flute . Chorus melody"));
    CHECK_EQ(version(s, t).phrases[0].name, std::string("Flute . Chorus melody"));

    // Duplicate: the notes, their dynamics and their performance, right after.
    const auto copy = s.duplicate_phrase(t, ph);
    REQUIRE(!copy.empty());
    const auto& v = version(s, t);
    CHECK_EQ(v.phrases.size(), 2u);
    CHECK_EQ(v.score.total_note_count(), 8u);
    CHECK_EQ(v.score.dynamics().size(), 2u);
    CHECK_EQ(v.performance.size(), 2u);
    std::string why;
    CHECK(s.duplicate_phrase(t, ph, &why).empty());  // bar 2 is now written
    CHECK(!why.empty());

    // Split, then remove: the notes stay.
    REQUIRE(s.split_phrase(t, ph, 2 * Q));
    CHECK_EQ(version(s, t).phrases.size(), 3u);
    CHECK(!s.split_phrase(t, ph, 0));
    REQUIRE(s.remove_phrase(t, ph));
    CHECK_EQ(version(s, t).phrases.size(), 2u);
    CHECK_EQ(version(s, t).score.total_note_count(), 8u);
}

TEST_CASE("phrases are saved, and a duplicated version gives them new ids")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto t = s.add_track("Flute", "woodwinds.flute");
    write_bar(s, t);
    const auto ph = s.add_phrase(t, "Verse", 0, BAR);
    const std::string path = std::string(VCC_TEST_TMP_DIR) + "/phrases.vcc2.json";
    std::string err;
    REQUIRE(s.save_as(path, &err));
    session::Session r(catalogs());
    REQUIRE(r.open(path, &err));
    REQUIRE(version(r, t).phrases.size() == 1);
    CHECK_EQ(version(r, t).phrases[0].name, std::string("Verse"));
    CHECK_EQ(version(r, t).phrases[0].end, BAR);

    REQUIRE(s.duplicate_version(t));
    const auto& now = version(s, t);
    REQUIRE(now.phrases.size() == 1);
    CHECK(now.phrases[0].id != ph);
    REQUIRE(s.save_as(path, &err));  // ids unique across versions, or saving would refuse to reload
    session::Session again(catalogs());
    CHECK(again.open(path, &err));
}

TEST_CASE("transposing notes respells them in the key and carries ties")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto t = s.add_track("Flute", "woodwinds.flute");
    const auto ids = write_bar(s, t);
    REQUIRE(s.transpose_notes(t, {ids[0], ids[1]}, 2));
    CHECK_EQ(notation::midi_note_number(version(s, t).score.find_note(ids[0])->pitch), 62);
    CHECK_EQ(notation::midi_note_number(version(s, t).score.find_note(ids[1])->pitch), 64);
    CHECK_EQ(notation::midi_note_number(version(s, t).score.find_note(ids[2])->pitch), 64);  // not selected
    REQUIRE(s.transpose_notes(t, {ids[0]}, -12));
    CHECK_EQ(notation::midi_note_number(version(s, t).score.find_note(ids[0])->pitch), 50);
    CHECK(!s.transpose_notes(t, {ids[0]}, 0));
}
