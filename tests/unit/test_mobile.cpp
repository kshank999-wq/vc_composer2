// The iPad companion's facade (Add-On spec §6): the same project files, the
// same score editor and the same engraving as the desktop, through a header
// that needs only the standard library.
#include <cmath>
#include <filesystem>

#include "vcc/mobile/Document.h"
#include "vcc_test.h"

using namespace vcc;
using mobile::Value;

namespace {
std::unique_ptr<mobile::Document> make()
{
    std::string error;
    auto d = mobile::Document::create(VCC_CONTENT_DIR, {}, &error);
    REQUIRE(d != nullptr);
    return d;
}
}  // namespace

TEST_CASE("mobile: a new score engraves, a tap places a note, entry plays, saves and reopens")
{
    auto doc = make();
    const auto tracks = doc->tracks();
    REQUIRE(tracks.size() == 1u);  // a piano to start with
    const auto& piano = tracks.front();
    CHECK_EQ(piano.staves, 2);
    auto drawing = doc->layout_track(piano.id);
    CHECK(!drawing.glyphs.empty());  // clefs, time signature, rests
    CHECK(!drawing.lines.empty());   // staff lines
    REQUIRE(drawing.staff_top.size() == 2u);
    for (const auto& g : drawing.glyphs) CHECK(g.codepoint >= 0xE000u);  // SMuFL range

    // A tap on the middle line of the treble staff, near the start of bar 1.
    const double x = 12.0, y = drawing.staff_top[0] + 2.0;
    const auto at = doc->place(piano.id, x, y, Value::Quarter);
    REQUIRE(at.valid);
    CHECK_EQ(at.staff, 0);
    CHECK_EQ(at.pitch_name, std::string("B4"));
    CHECK_EQ(at.midi, 71);
    CHECK(std::abs(doc->y_for(piano.id, at.staff, at.staff_position) - y) < 0.3);
    CHECK(std::abs(doc->x_for_tick(piano.id, at.tick) - x) < 6.0);
    const auto before = doc->revision();
    REQUIRE(doc->enter_note(piano.id, at, Value::Quarter));
    CHECK(doc->revision() != before);
    CHECK_EQ(doc->undo_label(), std::string("Enter note"));
    auto played = doc->performance();
    REQUIRE(played.size() == 1u);
    CHECK_EQ(played[0].pitch, 71);
    CHECK(played[0].duration > 0.4 && played[0].duration < 0.6);  // a quarter at 120

    // The engraving now shows it, and a tap on it finds it.
    drawing = doc->layout_track(piano.id);
    std::uint64_t id = 0;
    double nx = 0, ny = 0;
    for (const auto& g : drawing.glyphs)
        if (g.source != 0 && g.role == 5) id = g.source, nx = g.x + 0.5, ny = g.y;  // 5 = notehead
    REQUIRE(id != 0);
    CHECK_EQ(doc->note_at(piano.id, nx, ny), id);
    REQUIRE(doc->set_accidental(piano.id, id, -1));
    CHECK_EQ(doc->performance()[0].pitch, 70);  // Bb4

    // A rest after it, and a sharp written explicitly.
    auto next = doc->place(piano.id, x + 6.0 * 2, drawing.staff_top[0] + 1.5, Value::Quarter);
    REQUIRE(next.valid);
    REQUIRE(doc->enter_note(piano.id, next, Value::Eighth, 0, 1));
    CHECK_EQ(doc->performance().size(), 2u);

    // The desktop's own file format, both ways.
    const auto dir = std::filesystem::temp_directory_path() / "vcc_mobile_test";
    std::filesystem::create_directories(dir);
    const auto path = (dir / "Sketch.vcc2").string();
    std::string error;
    REQUIRE(doc->save_to(path, &error));
    auto back = make();
    REQUIRE(back->load(path, &error));
    CHECK_EQ(back->performance().size(), 2u);
    CHECK_EQ(back->performance()[0].pitch, 70);
    std::filesystem::remove_all(dir);

    // Undo, as many steps as were taken.
    REQUIRE(doc->undo());
    REQUIRE(doc->undo());
    CHECK_EQ(doc->performance()[0].pitch, 71);
    REQUIRE(doc->remove_note(piano.id, id));
    CHECK(doc->performance().empty());
}

TEST_CASE("mobile: a desktop template opens with its staves, one per part")
{
    auto doc = make();
    std::string error;
    REQUIRE(doc->new_from_template("cinematic.orchestra.expanded", &error));
    const auto tracks = doc->tracks();
    CHECK_EQ(tracks.size(), 15u);  // the articulation tracks are the desktop's, not staves
    int expanded = 0;
    for (const auto& t : tracks) expanded += t.expanded ? 1 : 0;
    CHECK_EQ(expanded, 13);
    CHECK(!doc->layout_track(tracks.front().id).lines.empty());
}
