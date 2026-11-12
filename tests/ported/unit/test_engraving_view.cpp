// Hit testing and SVG output (spec 02 sections 5.2, 5.3).
//
// Mouse entry needs two answers a drawing layer cannot give: what is under
// the pointer, and what a click on empty staff means musically. Both come
// from the layout that was drawn, so they cannot disagree with it.
//
// The SVG case is the visual verification the notation view otherwise could
// not have: it runs with no window, no display and no music font, and
// leaves a file a human can open.
#include "vcc/engraving/GlyphSubstitute.h"
#include "vcc/engraving/HitTest.h"
#include "vcc/engraving/SvgRenderer.h"

#include <cstdio>
#include <fstream>
#include <string>

#include "vcc_test.h"

using namespace vcc;
using namespace vcc::engraving;

namespace {

constexpr notation::Ticks kQuarter = notation::kTicksPerQuarter;
constexpr notation::Ticks kBar = 4 * kQuarter;

notation::ScoreNote note_at(notation::Ticks tick, const notation::Duration& duration,
                            const notation::Pitch& pitch)
{
    notation::ScoreNote note;
    note.start_tick = tick;
    note.duration = duration;
    note.pitch = pitch;
    return note;
}

/// A short phrase exercising most of what the engine draws, so the SVG the
/// suite leaves behind is worth looking at.
notation::Score sample_score()
{
    notation::Score score;
    score.set_key_signature(0, notation::KeySignature{2, false});  // D major.
    score.set_time_signature(0, vcc::music::TimeSignature{4, 4});

    const notation::Step steps[8] = {notation::Step::D, notation::Step::E, notation::Step::F,
                                     notation::Step::G, notation::Step::A, notation::Step::B,
                                     notation::Step::C, notation::Step::D};
    for (int index = 0; index < 8; ++index) {
        notation::Pitch pitch;
        pitch.step = steps[index];
        pitch.octave = static_cast<std::int8_t>(index >= 6 ? 5 : 4);
        pitch.alteration = static_cast<std::int8_t>(
            pitch.step == notation::Step::F || pitch.step == notation::Step::C ? 1 : 0);

        notation::ScoreNote note = note_at(static_cast<notation::Ticks>(index) * (kQuarter / 2),
                                           notation::eighth(), pitch);
        if (index == 0)
            note.articulations = notation::to_mask(notation::Articulation::Accent);
        score.insert_note(0, note);
    }

    notation::ScoreNote held = note_at(kBar, notation::half(), notation::Pitch{notation::Step::D, 5, 0});
    held.tied_to_next = true;
    score.insert_note(0, held);
    score.insert_note(0, note_at(kBar + kQuarter * 2, notation::half(),
                                 notation::Pitch{notation::Step::D, 5, 0}));

    score.add_dynamic(notation::DynamicMarking{0, notation::DynamicLevel::Piano, 0, ""});
    notation::Hairpin hairpin;
    hairpin.start_tick = 0;
    hairpin.end_tick = kBar;
    score.add_hairpin(hairpin);
    score.add_slur(notation::Slur{0, (2 * kQuarter), 0});

    return score;
}

}  // namespace

// --- Hit testing ------------------------------------------------------------

VCC_TEST(a_click_on_a_notehead_resolves_to_its_note)
{
    notation::Score score;
    const notation::EventId first =
        score.insert_note(0, note_at(0, notation::quarter(),
                                     notation::Pitch{notation::Step::G, 4, 0}));
    const notation::EventId second =
        score.insert_note(0, note_at(kQuarter, notation::quarter(),
                                     notation::Pitch{notation::Step::C, 5, 0}));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    for (const notation::EventId id : {first, second}) {
        const LaidOutNote* laid = nullptr;
        for (const LaidOutNote& note : result.notes) {
            if (note.id == id)
                laid = &note;
        }
        VCC_CHECK(laid != nullptr);

        const HitResult hit = hit_test(result, laid->centre);
        VCC_CHECK(hit.hit);
        VCC_CHECK(hit.is_note());
        VCC_CHECK_EQ(hit.id, id);
        VCC_CHECK_EQ(hit.role, ItemRole::Notehead);
        VCC_CHECK_EQ(result.notes[hit.note_index].id, id);
    }
}

VCC_TEST(a_notehead_wins_over_the_staff_line_it_sits_on)
{
    notation::Score score;
    // B4 is on the middle line, so a click on it lands on both.
    const notation::EventId id =
        score.insert_note(0, note_at(0, notation::quarter(),
                                     notation::Pitch{notation::Step::B, 4, 0}));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    const HitResult hit = hit_test(result, result.notes.front().centre);
    VCC_CHECK_EQ(hit.role, ItemRole::Notehead);
    VCC_CHECK_EQ(hit.id, id);
}

VCC_TEST(the_nearer_of_two_noteheads_wins)
{
    notation::Score score;
    const notation::EventId low =
        score.insert_note(0, note_at(0, notation::quarter(),
                                     notation::Pitch{notation::Step::E, 4, 0}));
    const notation::EventId high =
        score.insert_note(0, note_at(0, notation::quarter(),
                                     notation::Pitch{notation::Step::A, 5, 0}));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    for (const LaidOutNote& note : result.notes) {
        const HitResult hit = hit_test(result, note.centre);
        VCC_CHECK_EQ(hit.id, note.id);
    }
    VCC_CHECK(low != high);
}

VCC_TEST(a_click_on_nothing_is_a_miss)
{
    notation::Score score;
    score.insert_note(0, note_at(0, notation::quarter(), notation::Pitch{notation::Step::G, 4, 0}));

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    // Well below everything drawn.
    const HitResult hit = hit_test(result, Point{5.0, result.extent.bottom() + 20.0});
    VCC_CHECK(!hit.hit);
    VCC_CHECK(!hit.is_note());
    VCC_CHECK_EQ(hit.id, notation::kInvalidEventId);
}

VCC_TEST(a_click_on_the_staff_resolves_to_a_pitch_and_a_tick)
{
    notation::Score score;
    LayoutEngine engine;
    const TimelineMap timeline(8.0);
    const LayoutResult result = engine.layout(score, timeline, 0, kBar);

    StaffMetrics metrics;
    metrics.top_y = result.staff_top_y.front();

    // The top line of a treble staff is F5.
    const Point point{timeline.x_for_tick(kQuarter + kQuarter / 24), metrics.y_for_staff_position(8)};
    const InsertionPoint insertion =
        insertion_point_at(result, score, timeline, point, notation::sixteenth().ticks());

    VCC_CHECK(insertion.valid);
    VCC_CHECK_EQ(insertion.staff_index, std::size_t{0});
    VCC_CHECK_EQ(insertion.staff_position, 8);
    VCC_CHECK_EQ(insertion.pitch.step, notation::Step::F);
    VCC_CHECK_EQ(insertion.pitch.octave, 5);
    VCC_CHECK_EQ(insertion.pitch.alteration, 0);

    // A little past the quarter, quantized to a sixteenth grid, is the
    // quarter, not the next sixteenth: rounded to the nearest grid line
    // rather than truncated.
    VCC_CHECK_EQ(insertion.tick, notation::Ticks{kQuarter});
}

VCC_TEST(a_click_is_spelled_in_the_key_in_force)
{
    notation::Score score;
    VCC_CHECK(score.set_key_signature(0, notation::KeySignature{2, false}));   // D major.
    VCC_CHECK(score.set_key_signature(kBar, notation::KeySignature{-3, false}));  // E flat.

    LayoutEngine engine;
    const TimelineMap timeline(6.0);
    const LayoutResult result = engine.layout(score, timeline, 0, 2 * kBar);

    StaffMetrics metrics;
    metrics.top_y = result.staff_top_y.front();

    // The top line in D major is F sharp.
    const InsertionPoint sharp = insertion_point_at(
        result, score, timeline,
        Point{timeline.x_for_tick(0), metrics.y_for_staff_position(8)}, (kQuarter / 2));
    VCC_CHECK(sharp.valid);
    VCC_CHECK_EQ(sharp.pitch.step, notation::Step::F);
    VCC_CHECK_EQ(sharp.pitch.alteration, 1);

    // In the next bar the key has changed, and the same line is E flat's
    // natural F.
    const InsertionPoint flat = insertion_point_at(
        result, score, timeline,
        Point{timeline.x_for_tick(kBar), metrics.y_for_staff_position(8)}, (kQuarter / 2));
    VCC_CHECK(flat.valid);
    VCC_CHECK_EQ(flat.pitch.step, notation::Step::F);
    VCC_CHECK_EQ(flat.pitch.alteration, 0);

    // And a B in E flat major is a B flat.
    const InsertionPoint b_flat = insertion_point_at(
        result, score, timeline,
        Point{timeline.x_for_tick(kBar), metrics.y_for_staff_position(4)}, (kQuarter / 2));
    VCC_CHECK_EQ(b_flat.pitch.step, notation::Step::B);
    VCC_CHECK_EQ(b_flat.pitch.alteration, -1);
}

VCC_TEST(a_click_far_from_any_staff_is_not_an_insertion)
{
    notation::Score score;
    LayoutEngine engine;
    const TimelineMap timeline(6.0);
    const LayoutResult result = engine.layout(score, timeline, 0, kBar);

    const InsertionPoint high =
        insertion_point_at(result, score, timeline, Point{10.0, 0.0}, (kQuarter / 2));
    VCC_CHECK(!high.valid);

    // Just outside the staff is still an insertion: notes live above and
    // below it on ledger lines.
    StaffMetrics metrics;
    metrics.top_y = result.staff_top_y.front();
    const InsertionPoint ledger = insertion_point_at(
        result, score, timeline,
        Point{10.0, metrics.y_for_staff_position(12)}, (kQuarter / 2));
    VCC_CHECK(ledger.valid);
    VCC_CHECK_EQ(ledger.staff_position, 12);
}

VCC_TEST(a_click_picks_the_nearest_staff_of_a_grand_staff)
{
    notation::Score score;
    score.make_grand_staff();

    LayoutEngine engine;
    const TimelineMap timeline(6.0);
    const LayoutResult result = engine.layout(score, timeline, 0, kBar);

    StaffMetrics upper;
    upper.top_y = result.staff_top_y[0];
    StaffMetrics lower;
    lower.top_y = result.staff_top_y[1];

    const InsertionPoint on_upper = insertion_point_at(
        result, score, timeline, Point{10.0, upper.y_for_staff_position(4)}, (kQuarter / 2));
    VCC_CHECK_EQ(on_upper.staff_index, std::size_t{0});
    VCC_CHECK_EQ(on_upper.pitch.step, notation::Step::B);  // Middle line, treble.

    const InsertionPoint on_lower = insertion_point_at(
        result, score, timeline, Point{10.0, lower.y_for_staff_position(4)}, (kQuarter / 2));
    VCC_CHECK_EQ(on_lower.staff_index, std::size_t{1});
    VCC_CHECK_EQ(on_lower.pitch.step, notation::Step::D);  // Middle line, bass.
    VCC_CHECK_EQ(on_lower.pitch.octave, 3);
}

VCC_TEST(unquantized_entry_keeps_the_exact_tick)
{
    notation::Score score;
    LayoutEngine engine;
    const TimelineMap timeline(24.0);
    const LayoutResult result = engine.layout(score, timeline, 0, kBar);

    StaffMetrics metrics;
    metrics.top_y = result.staff_top_y.front();

    // No grid: a tuplet-aware entry mode needs the tick the pointer is
    // actually on.
    const InsertionPoint exact = insertion_point_at(
        result, score, timeline, Point{timeline.x_for_tick(1000), metrics.line_y(2)}, 0);
    VCC_CHECK(exact.valid);
    VCC_CHECK_EQ(exact.tick, notation::Ticks{1000});

    // And a click left of the origin cannot produce a negative tick.
    const InsertionPoint before = insertion_point_at(
        result, score, timeline, Point{-100.0, metrics.line_y(2)}, 0);
    VCC_CHECK_EQ(before.tick, notation::Ticks{0});
}

// --- SVG --------------------------------------------------------------------

VCC_TEST(every_glyph_has_a_stand_in_for_when_the_font_is_missing)
{
    // A glyph added to the set without a stand-in would silently vanish from
    // every font-free rendering, which is exactly the case the stand-ins
    // exist for.
    GlyphTable table;
    Glyph missing = Glyph::None;
    VCC_CHECK(table.is_complete(&missing));

    for (std::uint16_t raw = static_cast<std::uint16_t>(Glyph::None) + 1;
         raw <= static_cast<std::uint16_t>(Glyph::OrnamentTrill); ++raw) {
        const Glyph glyph = static_cast<Glyph>(raw);
        // Every glyph in the enum is in the table, which the loop bound
        // relies on.
        VCC_CHECK_MSG(table.metrics(glyph).codepoint != 0, smufl_name(glyph));

        const GlyphSubstitute stand_in = substitute_for(glyph);
        VCC_CHECK_MSG(stand_in.kind != SubstituteKind::None, smufl_name(glyph));
        if (stand_in.kind == SubstituteKind::Letters)
            VCC_CHECK_MSG(stand_in.text[0] != '\0', smufl_name(glyph));
        if (stand_in.kind == SubstituteKind::Box)
            VCC_CHECK_MSG(stand_in.height > 0.0, smufl_name(glyph));
        if (stand_in.kind == SubstituteKind::Triangle)
            VCC_CHECK_MSG(stand_in.height != 0.0, smufl_name(glyph));
    }

    VCC_CHECK_EQ(substitute_for(Glyph::None).kind, SubstituteKind::None);
}

VCC_TEST(svg_output_carries_every_item_the_layout_produced)
{
    const notation::Score score = sample_score();

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, 2 * kBar);

    const std::string svg = render_svg(result, SvgOptions{}, &engine.glyph_table());

    VCC_CHECK(svg.find("<?xml") == 0);
    VCC_CHECK(svg.find("<svg xmlns=\"http://www.w3.org/2000/svg\"") != std::string::npos);
    VCC_CHECK(svg.find("viewBox=") != std::string::npos);
    VCC_CHECK(svg.find("</svg>") != std::string::npos);

    const auto occurrences = [&svg](const std::string& needle) {
        std::size_t count = 0;
        for (std::size_t at = svg.find(needle); at != std::string::npos;
             at = svg.find(needle, at + needle.size()))
            ++count;
        return count;
    };

    VCC_CHECK_EQ(occurrences("<line "), result.display_list.lines.size());
    VCC_CHECK_EQ(occurrences("<polygon "), result.display_list.quads.size());
    VCC_CHECK_EQ(occurrences("<path "), result.display_list.curves.size());

    // A tapered curve is a filled lens of two cubics, closed; an untapered
    // one a stroke with no fill.
    std::size_t tapered = 0;
    for (const CurveItem& curve : result.display_list.curves)
        tapered += curve.tapered ? 1 : 0;
    VCC_CHECK(tapered > 0);
    VCC_CHECK_EQ(occurrences(" Z\" fill=\"#"), tapered);
    VCC_CHECK_EQ(occurrences("fill=\"none\" stroke="), result.display_list.curves.size() - tapered);

    // Every glyph with a codepoint becomes a text element, plus the text
    // items and the document title.
    VCC_CHECK_EQ(occurrences("<text "),
                 result.display_list.glyphs.size() + result.display_list.texts.size());

    // Glyphs are written as numeric character references, so the file is
    // pure ASCII whatever the font.
    VCC_CHECK(svg.find("&#x") != std::string::npos);
    for (const char character : svg)
        VCC_CHECK(static_cast<unsigned char>(character) < 128);
}

VCC_TEST(svg_uses_the_glyph_table_the_layout_was_made_with)
{
    notation::Score score;
    score.insert_note(0, note_at(0, notation::quarter(), notation::Pitch{notation::Step::G, 4, 0}));

    // A font that maps noteheadBlack somewhere unusual, as a font's metadata
    // is entitled to.
    GlyphTable table;
    GlyphMetrics moved = table.metrics(Glyph::NoteheadBlack);
    moved.codepoint = 0xF4BE;
    table.set_metrics(Glyph::NoteheadBlack, moved);

    LayoutEngine engine(std::move(table), LayoutOptions{});
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    const std::string with_table = render_svg(result, SvgOptions{}, &engine.glyph_table());
    VCC_CHECK(with_table.find("&#xF4BE;") != std::string::npos);

    // Rendering with the defaults instead would emit the wrong character,
    // which is why the table is a parameter rather than an assumption.
    const std::string with_defaults = render_svg(result, SvgOptions{});
    VCC_CHECK(with_defaults.find("&#xF4BE;") == std::string::npos);
}

VCC_TEST(svg_text_is_escaped)
{
    notation::Score score;
    score.add_dynamic(notation::DynamicMarking{0, notation::DynamicLevel::Piano, 0,
                                               "piu <forte> & \"so\""});

    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(6.0), 0, kBar);

    const std::string svg = render_svg(result);
    VCC_CHECK(svg.find("piu &lt;forte&gt; &amp; &quot;so&quot;") != std::string::npos);
    VCC_CHECK(svg.find("<forte>") == std::string::npos);
}

VCC_TEST(glyph_boxes_make_the_geometry_visible_without_the_font)
{
    const notation::Score score = sample_score();
    LayoutEngine engine;
    const LayoutResult result = engine.layout(score, TimelineMap(8.0), 0, kBar);

    SvgOptions options;
    options.draw_glyph_boxes = true;
    const std::string boxed = render_svg(result, options, &engine.glyph_table());
    const std::string plain = render_svg(result, SvgOptions{}, &engine.glyph_table());

    VCC_CHECK(boxed.size() > plain.size());
    VCC_CHECK(boxed.find("<rect ") != std::string::npos);
}

VCC_TEST(an_svg_of_a_real_phrase_is_written_for_inspection)
{
    // Visual verification needs something to look at. This writes the sample
    // phrase next to the test binary; the assertions below are what the
    // suite checks, and the file is what a human opens.
    const notation::Score score = sample_score();

    LayoutEngine engine;
    LayoutOptions options = engine.options();
    options.draw_cursor = true;
    options.cursor_tick = (2 * kQuarter);
    engine.set_options(options);

    const LayoutResult result = engine.layout(score, TimelineMap(9.0), 0, 2 * kBar);

    // Stand-ins rather than music-font characters: no build machine has a
    // music font installed, so the file the suite leaves behind has to be
    // readable without one.
    SvgOptions svg_options;
    svg_options.substitute_glyphs = true;
    svg_options.title = "VC Composer notation sample";
    const std::string svg = render_svg(result, svg_options, &engine.glyph_table());

    const char* path = "engraving-sample.svg";
    std::ofstream out(path, std::ios::binary);
    VCC_CHECK(out.good());
    out << svg;
    out.close();

    std::ifstream back(path, std::ios::binary | std::ios::ate);
    VCC_CHECK(back.good());
    VCC_CHECK(static_cast<std::size_t>(back.tellg()) == svg.size());

    std::printf("  wrote %s (%zu bytes, %zu items)\n", path, svg.size(),
                result.display_list.item_count());

    // The phrase exercises the layers the view is made of, and every one of
    // them reached the output.
    VCC_CHECK(!result.display_list.glyphs.empty());
    VCC_CHECK(!result.display_list.quads.empty());   // Beams.
    VCC_CHECK(!result.display_list.curves.empty());  // Tie and slur.
    VCC_CHECK(!result.display_list.texts.empty());   // Bar numbers.
    VCC_CHECK(result.display_list.missing_glyphs.empty());
}

VCC_TEST_MAIN("engraving/View")
