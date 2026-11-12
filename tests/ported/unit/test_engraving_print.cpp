// A part on pages (spec 02 sections 5.1, 5.3).
//
// What is checked here is what the page layout decides: how a score
// breaks into systems and pages, where each lands, what the first page
// carries over the music, and that every page is the paper rather than the
// ink - and that the SVG a page becomes carries the font.
#include "vcc/engraving/PrintLayout.h"

#include <string>
#include <vector>

#include "vcc_test.h"

using namespace vcc;
using namespace vcc::engraving;

namespace {

constexpr notation::Ticks kBar = 4 * notation::kTicksPerQuarter;

notation::ScoreNote note_at(notation::Ticks tick, const notation::Duration& duration,
                            const notation::Pitch& pitch)
{
    notation::ScoreNote note;
    note.start_tick = tick;
    note.duration = duration;
    note.pitch = pitch;
    return note;
}

/// A quarter note on every beat of `bars` bars.
notation::Score bars_of_quarters(int bars)
{
    notation::Score score;
    for (int bar = 0; bar < bars; ++bar) {
        for (int beat = 0; beat < 4; ++beat) {
            score.insert_note(0, note_at(bar * kBar + beat * notation::kTicksPerQuarter,
                                         notation::quarter(), {notation::Step::G, 4, 0}));
        }
    }
    return score;
}

std::vector<const TextItem*> texts_of(const LayoutResult& layout)
{
    std::vector<const TextItem*> out;
    for (const TextItem& item : layout.display_list.texts)
        out.push_back(&item);
    return out;
}

bool has_text(const LayoutResult& layout, const std::string& text)
{
    for (const TextItem* item : texts_of(layout)) {
        if (item->text == text)
            return true;
    }
    return false;
}

}  // namespace

VCC_TEST(a_score_breaks_into_systems_of_the_asked_bars_down_one_page)
{
    const notation::Score score = bars_of_quarters(10);
    const LayoutEngine engine;
    PrintOptions options;
    options.title = "Cue 12";
    options.subtitle = "Violin I";

    const PrintResult printed = lay_out_pages(score, engine, options);
    VCC_CHECK_EQ(printed.bar_count, 10);
    VCC_CHECK_EQ(printed.pages.size(), std::size_t{1});
    VCC_CHECK_EQ(printed.system_count, std::size_t{3});  // 4, 4, 2.
    VCC_CHECK_EQ(printed.crowded_system_count, std::size_t{0});

    const PrintedPage& page = printed.pages.front();
    VCC_CHECK_EQ(page.systems.size(), std::size_t{3});
    VCC_CHECK_EQ(page.systems[0].first_bar, 1);
    VCC_CHECK_EQ(page.systems[0].last_bar, 4);
    VCC_CHECK_EQ(page.systems[1].first_bar, 5);
    VCC_CHECK_EQ(page.systems[2].first_bar, 9);
    VCC_CHECK_EQ(page.systems[2].last_bar, 10);

    // One under the other, inside the margins, and the full lines filling
    // the width while the short last line does not.
    VCC_CHECK(page.systems[1].top_y > page.systems[0].top_y);
    VCC_CHECK(page.systems[2].top_y > page.systems[1].top_y);
    double first_right = 0.0;
    double last_right = 0.0;
    for (const LineItem& line : page.layout.display_list.lines) {
        if (line.role != ItemRole::StaffLine)
            continue;
        VCC_CHECK(line.from.x >= options.margin - 1e-9);
        VCC_CHECK(line.to.x <= options.page_width - options.margin + 1e-9);
        if (std::abs(line.from.y - page.systems[0].staff_top_y) < 1e-9)
            first_right = std::max(first_right, line.to.x);
        if (std::abs(line.from.y - page.systems[2].staff_top_y) < 1e-9)
            last_right = std::max(last_right, line.to.x);
    }
    VCC_CHECK_NEAR(first_right, options.page_width - options.margin, 1e-6);
    VCC_CHECK(last_right < first_right - 10.0);

    // The title and subtitle over the music, a bar number at the start of
    // every line but the first, and no page number on a single page.
    VCC_CHECK(has_text(page.layout, "Cue 12"));
    VCC_CHECK(has_text(page.layout, "Violin I"));
    VCC_CHECK(!has_text(page.layout, "1"));
    VCC_CHECK(has_text(page.layout, "5"));
    VCC_CHECK(has_text(page.layout, "9"));
    for (const TextItem* item : texts_of(page.layout)) {
        if (item->text == "Cue 12")
            VCC_CHECK(item->origin.y < page.systems[0].staff_top_y);
        if (item->text == "5")
            VCC_CHECK_NEAR(item->origin.y, page.systems[1].staff_top_y - 1.0, 1e-9);
    }

    // The page is the paper.
    VCC_CHECK_NEAR(page.layout.extent.width, options.page_width, 1e-9);
    VCC_CHECK_NEAR(page.layout.extent.height, options.page_height, 1e-9);
    VCC_CHECK_NEAR(page.layout.drawn_bounds.height, options.page_height, 1e-9);

    // No ruler and no cursor on a page, whatever the engine was set to.
    bool ruler = false;
    for (const TextItem* item : texts_of(page.layout))
        ruler = ruler || item->role == ItemRole::RulerLabel;
    VCC_CHECK(!ruler);
}

VCC_TEST(a_long_part_runs_onto_more_pages_with_numbers_at_the_foot)
{
    const notation::Score score = bars_of_quarters(60);
    const LayoutEngine engine;
    const PrintResult printed = lay_out_pages(score, engine);

    VCC_CHECK_EQ(printed.system_count, std::size_t{15});
    VCC_CHECK(printed.pages.size() >= 2);

    // Every page is the same size, numbered, and holds the systems that
    // fit inside its margins; the systems run on in bar order.
    std::int32_t next_bar = 1;
    for (std::size_t index = 0; index < printed.pages.size(); ++index) {
        const PrintedPage& page = printed.pages[index];
        VCC_CHECK(!page.systems.empty());
        VCC_CHECK(has_text(page.layout, std::to_string(index + 1)));
        VCC_CHECK_NEAR(page.layout.extent.height, PrintOptions{}.page_height, 1e-9);
        for (const PrintedSystem& system : page.systems) {
            VCC_CHECK_EQ(system.first_bar, next_bar);
            next_bar = system.last_bar + 1;
            VCC_CHECK(system.top_y >= PrintOptions{}.margin - 1e-9);
        }
        VCC_CHECK(page.layout.display_list.bounds.y + page.layout.display_list.bounds.height
                  <= PrintOptions{}.page_height + 1e-9);
    }
    VCC_CHECK_EQ(next_bar, 61);
}

VCC_TEST(a_crowded_line_takes_fewer_bars_and_a_line_that_cannot_help_it_is_counted)
{
    // Thirty-seconds all the way: four bars of them will not fit a line at
    // a readable spacing, so the lines take fewer, and at sixty-fourths on
    // a narrow page even one bar is crowded, which is said.
    notation::Score dense;
    for (int bar = 0; bar < 4; ++bar) {
        for (int i = 0; i < 32; ++i) {
            notation::Duration thirty_second;
            thirty_second.value = notation::NoteValue::ThirtySecond;
            dense.insert_note(0, note_at(bar * kBar + i * (notation::kTicksPerQuarter / 8),
                                         thirty_second, {notation::Step::G, 4, 0}));
        }
    }
    const LayoutEngine engine;
    const PrintResult printed = lay_out_pages(dense, engine);
    VCC_CHECK(printed.system_count > 1);
    VCC_CHECK_EQ(printed.crowded_system_count, std::size_t{0});
    for (const PrintedSystem& system : printed.pages.front().systems)
        VCC_CHECK(system.last_bar - system.first_bar + 1 < 4);

    PrintOptions narrow;
    narrow.page_width = 40.0;
    narrow.margin = 2.0;
    const PrintResult cramped = lay_out_pages(dense, engine, narrow);
    VCC_CHECK(cramped.crowded_system_count > 0);
    for (const PrintedPage& page : cramped.pages) {
        for (const PrintedSystem& system : page.systems)
            VCC_CHECK_EQ(system.last_bar, system.first_bar);
    }
}

VCC_TEST(the_time_signature_is_on_the_first_line_and_where_the_meter_changes)
{
    notation::Score score = bars_of_quarters(8);
    VCC_CHECK(score.set_time_signature(4 * kBar, music::TimeSignature{3, 4}));
    const LayoutEngine engine;
    PrintOptions options;
    options.bars_per_system = 2;
    const PrintResult printed = lay_out_pages(score, engine, options);

    // The quarters were written for 4/4, so from bar 5 they fill 3/4 bars:
    // ten bars, five lines of two. The first line and the third (bar 5,
    // where 3/4 begins) carry a time signature; the others do not.
    const PrintedPage& page = printed.pages.front();
    VCC_CHECK_EQ(page.systems.size(), std::size_t{5});
    VCC_CHECK_EQ(page.systems[2].first_bar, 5);
    std::vector<int> signatures(page.systems.size(), 0);
    for (const GlyphItem& glyph : page.layout.display_list.glyphs) {
        if (glyph.role != ItemRole::TimeSignature)
            continue;
        for (std::size_t index = 0; index < page.systems.size(); ++index) {
            const double top = page.systems[index].staff_top_y;
            if (glyph.origin.y >= top - 1.0 && glyph.origin.y <= top + 5.0)
                ++signatures[index];
        }
    }
    VCC_CHECK(signatures[0] > 0);
    VCC_CHECK_EQ(signatures[1], 0);
    VCC_CHECK(signatures[2] > 0);
    VCC_CHECK_EQ(signatures[3], 0);
    VCC_CHECK_EQ(signatures[4], 0);
}

VCC_TEST(an_empty_score_is_one_page_with_one_empty_line_and_a_bad_page_is_nothing)
{
    const notation::Score empty;
    const LayoutEngine engine;
    const PrintResult printed = lay_out_pages(empty, engine);
    VCC_CHECK_EQ(printed.pages.size(), std::size_t{1});
    VCC_CHECK_EQ(printed.system_count, std::size_t{1});
    VCC_CHECK_EQ(printed.bar_count, 1);

    PrintOptions bad;
    bad.page_width = 10.0;
    VCC_CHECK(!bad.is_valid());
    VCC_CHECK(lay_out_pages(bars_of_quarters(2), engine, bad).pages.empty());
}

VCC_TEST(pages_render_to_svg_each_carrying_the_font_and_its_own_title)
{
    const notation::Score score = bars_of_quarters(60);
    const LayoutEngine engine;
    const PrintResult printed = lay_out_pages(score, engine);

    SvgOptions svg;
    svg.title = "Cue 12";
    svg.embedded_font = std::string("OTTO\x00\x01", 6);  // Any bytes: what matters is that they travel.
    const std::vector<std::string> documents = render_pages_svg(printed, svg, &engine.glyph_table());
    VCC_CHECK_EQ(documents.size(), printed.pages.size());
    for (std::size_t index = 0; index < documents.size(); ++index) {
        const std::string& page = documents[index];
        VCC_CHECK(page.find("<svg") != std::string::npos);
        VCC_CHECK(page.find("<title>Cue 12 - page " + std::to_string(index + 1) + "</title>")
                  != std::string::npos);
        VCC_CHECK(page.find("@font-face") != std::string::npos);
        VCC_CHECK(page.find("base64,T1RUTwAB") != std::string::npos);  // "OTTO\0\1" encoded.
        VCC_CHECK(page.find("format(\"opentype\")") != std::string::npos);
    }

    // Without a font to embed, no @font-face - the file names the font.
    const std::vector<std::string> plain = render_pages_svg(printed, SvgOptions{});
    VCC_CHECK(plain.front().find("@font-face") == std::string::npos);
    VCC_CHECK(plain.front().find("Bravura") != std::string::npos);
    VCC_CHECK(plain.front().find("<title>VC Composer notation - page 1</title>") != std::string::npos);
}

VCC_TEST_MAIN("engraving/PrintLayout")
