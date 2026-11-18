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
    CHECK_EQ(tracks.size(), 16u);  // the Piano sketch and 15 parts; the articulation tracks are the desktop's, not staves
    int expanded = 0;
    for (const auto& t : tracks) expanded += t.expanded ? 1 : 0;
    CHECK_EQ(expanded, 13);
    CHECK(!doc->layout_track(tracks.front().id).lines.empty());
}

// --- Handwriting -----------------------------------------------------------------

namespace {
using mobile::InkPoint;
using mobile::Stroke;
using mobile::StrokeShape;

Stroke line(double x1, double y1, double x2, double y2, int n = 12)
{
    Stroke s;
    for (int i = 0; i <= n; ++i) {
        const double t = static_cast<double>(i) / n;
        s.push_back({x1 + (x2 - x1) * t, y1 + (y2 - y1) * t});
    }
    return s;
}
/// A notehead scribbled in, as people fill one with a pencil.
Stroke filled_head(double cx, double cy)
{
    Stroke s;
    for (int i = 0; i <= 10; ++i) {
        const double y = cy - 0.4 + 0.08 * i;
        const double half = 0.6 * std::sqrt(std::max(0.05, 1.0 - std::pow((y - cy) / 0.45, 2)));
        s.push_back({i % 2 ? cx + half : cx - half, y});
    }
    return s;
}
Stroke hollow_head(double cx, double cy)
{
    Stroke s;
    for (int i = 0; i <= 36; ++i) {
        const double a = 2 * 3.14159265 * i / 36.0;
        s.push_back({cx + 0.65 * std::cos(a), cy + 0.45 * std::sin(a)});
    }
    return s;
}
Stroke flag(double x, double y)  // from a stem top, curving down and right
{
    Stroke s;
    for (int i = 0; i <= 12; ++i) {
        const double t = i / 12.0;
        s.push_back({x + 0.9 * std::sin(t * 3.14159265 * 0.8), y + 1.8 * t});
    }
    return s;
}
Stroke quarter_rest(double cx, double top)
{
    return {{cx - 0.3, top}, {cx + 0.4, top + 0.8}, {cx - 0.4, top + 1.6}, {cx + 0.4, top + 2.3}, {cx - 0.2, top + 3.0}};
}
Stroke flat(double x, double y)  // a stem from above, a bowl at the bottom
{
    Stroke s = line(x, y - 2.2, x, y + 0.5, 10);
    for (int i = 1; i <= 10; ++i) {
        const double a = 3.14159265 * i / 10.0;
        s.push_back({x + 0.7 * std::sin(a), y + 0.5 - 0.6 * std::sin(a) * 0.9 + (i > 5 ? 0.0 : 0.0)});
    }
    return s;
}
}  // namespace

TEST_CASE("handwriting: each stroke is read by its shape")
{
    CHECK(mobile::classify(filled_head(0, 0)) == StrokeShape::FilledHead);
    CHECK(mobile::classify(hollow_head(0, 0)) == StrokeShape::HollowHead);
    CHECK(mobile::classify(line(0, 0, 0, 3.5)) == StrokeShape::Stem);
    CHECK(mobile::classify(flag(0, 0)) == StrokeShape::Flag);
    CHECK(mobile::classify(quarter_rest(0, 0)) == StrokeShape::QuarterRest);
    CHECK(mobile::classify(line(0, 0, 3, 0.4)) == StrokeShape::Beam);
    CHECK(mobile::classify({{0, 0}, {0.1, 0.05}, {0.15, 0.1}}) == StrokeShape::Dot);
    CHECK(mobile::classify(flat(0, 0)) == StrokeShape::Flat);
}

TEST_CASE("handwriting: a phrase is previewed, doubts are flagged, and the accepted phrase is written as one step")
{
    auto doc = make();
    const auto piano = doc->tracks().front().id;
    const auto drawing = doc->layout_track(piano);
    const double top = drawing.staff_top[0];
    auto y = [&](int pos) { return top + 4.0 - 0.5 * pos; };

    std::vector<Stroke> ink;
    // B4 quarter: head + stem.
    ink.push_back(filled_head(0.4, y(4)));
    ink.push_back(line(1.0, y(4), 1.0, y(4) - 3.4));
    // A sharp, then G#4 eighth: head + stem + flag.
    ink.push_back(line(3.2, y(2) - 1.2, 3.2, y(2) + 1.2));
    ink.push_back(line(3.7, y(2) - 1.3, 3.7, y(2) + 1.1));
    ink.push_back(line(2.9, y(2) - 0.2, 4.0, y(2) - 0.5));
    ink.push_back(line(2.9, y(2) + 0.5, 4.0, y(2) + 0.2));
    ink.push_back(filled_head(5.0, y(2)));
    ink.push_back(line(5.6, y(2), 5.6, y(2) - 3.4));
    ink.push_back(flag(5.6, y(2) - 3.4));
    // D5 half: hollow head + stem, then a barline.
    ink.push_back(hollow_head(8.0, y(6)));
    ink.push_back(line(8.6, y(6), 8.6, y(6) + 3.4));
    ink.push_back(line(10.5, top - 0.1, 10.5, top + 4.1));
    // Bar 2: a quarter rest, and a head written between a line and a space, no stem.
    ink.push_back(quarter_rest(12.0, top + 0.5));
    ink.push_back(filled_head(14.0, y(4) - 0.25));

    const auto rec = doc->recognize(piano, ink);
    REQUIRE(rec.items.size() == 5u);
    CHECK(rec.unused_strokes.empty());
    const auto& b = rec.items[0];
    CHECK(b.kind == mobile::Recognized::Kind::Note);
    CHECK(b.value == 3);
    CHECK(b.positions == std::vector<int>{4});
    CHECK_EQ(b.tick, 0);
    CHECK(!b.doubtful);
    const auto& gs = rec.items[1];
    CHECK_EQ(gs.value, 4);
    CHECK_EQ(gs.alteration, 1);
    CHECK(gs.positions == std::vector<int>{2});
    const std::int64_t Q = mobile::Document::ticks_per_quarter();
    CHECK_EQ(gs.tick, Q);
    const auto& d = rec.items[2];
    CHECK_EQ(d.value, 2);
    CHECK(d.positions == std::vector<int>{6});
    CHECK_EQ(d.tick, Q + Q / 2);
    const auto& rest = rec.items[3];
    CHECK(rest.kind == mobile::Recognized::Kind::Rest);
    CHECK_EQ(rest.value, 3);
    CHECK_EQ(rest.tick, 4 * Q);  // after the barline: bar 2
    const auto& unsure = rec.items[4];
    CHECK(unsure.doubtful);
    CHECK(unsure.has_alternative);  // between B4's line and the C5 space
    CHECK(unsure.positions == std::vector<int>{5});  // read first as C5...
    CHECK_EQ(unsure.alternative_position, 4);         // ...or B4
    CHECK(unsure.why == "between a line and a space");
    CHECK_EQ(rec.doubtful_count(), 1);
    CHECK(rec.summary.find("1 to check") != std::string::npos);

    // The composer picks the other reading for the doubtful head, then accepts.
    auto fixed = rec;
    std::swap(fixed.items[4].positions[0], fixed.items[4].alternative_position);
    REQUIRE(doc->commit(piano, fixed));
    CHECK_EQ(doc->undo_label(), std::string("Handwrite 5 symbols"));
    const auto played = doc->performance();
    REQUIRE(played.size() == 4u);
    CHECK_EQ(played[0].pitch, 71);  // B4
    CHECK_EQ(played[1].pitch, 68);  // G#4
    CHECK_EQ(played[2].pitch, 74);  // D5
    CHECK_EQ(played[3].pitch, 71);  // the reading chosen: B4
    CHECK(std::abs(played[3].start - 2.5) < 0.01);  // bar 2, beat 2 at 120 bpm
    // One undo removes the whole handwritten phrase.
    REQUIRE(doc->undo());
    CHECK(doc->performance().empty());
}

TEST_CASE("handwriting: a bar written too full is flagged, never silently cut")
{
    auto doc = make();
    const auto piano = doc->tracks().front().id;
    const double top = doc->layout_track(piano).staff_top[0];
    std::vector<Stroke> ink;
    for (int i = 0; i < 3; ++i) {  // three quarters, then a half that will not fit in 4/4
        ink.push_back(filled_head(0.4 + 3.0 * i, top + 2.0));
        ink.push_back(line(1.0 + 3.0 * i, top + 2.0, 1.0 + 3.0 * i, top - 1.4));
    }
    ink.push_back(hollow_head(9.4, top + 2.0));
    ink.push_back(line(10.0, top + 2.0, 10.0, top - 1.4));
    const auto rec = doc->recognize(piano, ink);
    REQUIRE(rec.items.size() == 4u);
    for (int i = 0; i < 3; ++i) CHECK(!rec.items[static_cast<std::size_t>(i)].doubtful);
    CHECK(rec.items[3].doubtful);
    CHECK_EQ(rec.items[3].why, std::string("the bar is too full"));
}
