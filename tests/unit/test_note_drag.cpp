// Moving notes by dragging, deleting by clicking again, select all + delete.
#include <algorithm>

#include "vcc/editing/ScoreEditor.h"
#include "vcc_test.h"

using namespace vcc;
using notation::Ticks;
constexpr Ticks kQ = notation::kTicksPerQuarter;

namespace {
notation::Pitch P(notation::Step step, int octave = 4)
{
    notation::Pitch p;
    p.step = step;
    p.octave = static_cast<std::int8_t>(octave);
    return p;
}
notation::Score one_staff()
{
    notation::Score s;
    s.add_staff(notation::Staff{});
    return s;
}
}  // namespace

TEST_CASE("note drag: a note moves later and up, the old place empties, one undo puts it back")
{
    auto score = one_staff();
    editing::ScoreEditor editor(score);
    editor.set_duration(notation::NoteValue::Quarter);
    const auto c = editor.insert_at(0, 0, P(notation::Step::C)).new_event;
    const auto e = editor.insert_at(0, kQ, P(notation::Step::E)).new_event;
    REQUIRE(c != notation::kInvalidEventId);
    REQUIRE(e != notation::kInvalidEventId);

    // C, a beat later (onto E) and a step up: D on beat 2; E gives way.
    auto r = editor.move_notes(c, kQ, 1);
    CHECK(r.succeeded());
    REQUIRE(editor.selection().size() == 1u);
    const auto* moved = score.find_note(editor.selection().front());
    REQUIRE(moved != nullptr);
    CHECK_EQ(moved->start_tick, kQ);
    CHECK(moved->pitch.step == notation::Step::D);
    CHECK(score.find_note(c) == nullptr);
    CHECK(score.find_note(e) == nullptr);
    int sounding = 0;
    for (const auto& n : score.notes(0)) sounding += n.is_rest ? 0 : 1;
    CHECK_EQ(sounding, 1);

    CHECK(editor.undo().succeeded());
    CHECK(score.find_note(c) != nullptr);
    CHECK(score.find_note(e) != nullptr);
    CHECK_EQ(score.find_note(c)->start_tick, 0);
}

TEST_CASE("note drag: the selection moves together and never before the start")
{
    auto score = one_staff();
    editing::ScoreEditor editor(score);
    editor.set_duration(notation::NoteValue::Quarter);
    const auto a = editor.insert_at(0, kQ, P(notation::Step::A)).new_event;
    const auto b = editor.insert_at(0, 2 * kQ, P(notation::Step::B)).new_event;
    editor.set_selection(std::vector<notation::EventId>{a, b});
    CHECK(editor.move_notes(b, -4 * kQ, 0).succeeded());  // clamped to the start
    REQUIRE(editor.selection().size() == 2u);
    std::vector<Ticks> starts;
    for (const auto id : editor.selection()) starts.push_back(score.find_note(id)->start_tick);
    std::sort(starts.begin(), starts.end());
    CHECK_EQ(starts[0], 0);
    CHECK_EQ(starts[1], kQ);

    // Only pitch: spelled by the key.
    const auto id = editor.selection().front();
    editor.set_selection(std::vector<notation::EventId>{id});
    CHECK(editor.move_notes(id, 0, -2).succeeded());
    CHECK(!editor.move_notes(editor.selection().front(), 0, 0).succeeded());
}

TEST_CASE("note drag: select all and delete empties the staff")
{
    auto score = one_staff();
    editing::ScoreEditor editor(score);
    editor.set_duration(notation::NoteValue::Eighth);
    for (int i = 0; i < 6; ++i) editor.insert_at(0, i * kQ / 2, P(notation::Step::G));
    CHECK(editor.select_all().succeeded());
    CHECK(editor.delete_selection().succeeded());
    int sounding = 0;
    for (const auto& n : score.notes(0)) sounding += n.is_rest ? 0 : 1;
    CHECK_EQ(sounding, 0);
}
