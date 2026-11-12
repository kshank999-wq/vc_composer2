#include "vcc/engraving/PrintLayout.h"

#include <algorithm>

#include "vcc/engraving/TimelineMap.h"

namespace vcc::engraving {
namespace {

/// The same estimate the display list's own text bounds use: half the
/// size per character of a proportional face.
double text_width(const std::string& text, double size)
{
    return static_cast<double>(text.size()) * size * 0.5;
}

Point moved(const Point& point, double dx, double dy)
{
    return Point{point.x + dx, point.y + dy};
}

/// The last bar the score reaches into, and 1 for an empty score.
std::int32_t last_bar_of(const notation::Score& score)
{
    const notation::Ticks end = score.end_tick();
    if (end <= 0)
        return 1;
    return score.measure_at(end - 1).number;
}

bool meter_changes_at(const notation::Score& score, notation::Ticks tick)
{
    for (const notation::TimeSignatureChange& change : score.time_signature_changes()) {
        if (change.tick == tick)
            return true;
    }
    return false;
}

/// How wide the header came out in a layout: the music origin less the
/// left edge of the page, which is what the engine reserves for the clef,
/// the key and the meter.
double header_width_of(const LayoutResult& result, const TimelineMap& timeline,
                       notation::Ticks from)
{
    return timeline.x_for_tick(from) - result.extent.x;
}

}  // namespace

void append_translated(const LayoutResult& from, double dx, double dy, LayoutResult& into)
{
    for (GlyphItem item : from.display_list.glyphs) {
        item.origin = moved(item.origin, dx, dy);
        item.bounds.x += dx;
        item.bounds.y += dy;
        into.display_list.add(item);
    }
    for (LineItem item : from.display_list.lines) {
        item.from = moved(item.from, dx, dy);
        item.to = moved(item.to, dx, dy);
        into.display_list.add(item);
    }
    for (QuadItem item : from.display_list.quads) {
        for (Point& corner : item.corners)
            corner = moved(corner, dx, dy);
        into.display_list.add(item);
    }
    for (CurveItem item : from.display_list.curves) {
        item.start = moved(item.start, dx, dy);
        item.control_one = moved(item.control_one, dx, dy);
        item.control_two = moved(item.control_two, dx, dy);
        item.end = moved(item.end, dx, dy);
        into.display_list.add(item);
    }
    for (TextItem item : from.display_list.texts) {
        item.origin = moved(item.origin, dx, dy);
        into.display_list.add(item);
    }
    for (LaidOutNote note : from.notes) {
        note.centre = moved(note.centre, dx, dy);
        note.notehead_bounds.x += dx;
        note.notehead_bounds.y += dy;
        note.stem_x += dx;
        note.stem_end_y += dy;
        into.notes.push_back(note);
    }
    for (const Glyph missing : from.display_list.missing_glyphs)
        into.display_list.missing_glyphs.push_back(missing);
    into.display_list.is_crowded = into.display_list.is_crowded || from.display_list.is_crowded;
    into.crowded_note_count += from.crowded_note_count;
}

PrintResult lay_out_pages(const notation::Score& score, const LayoutEngine& engine,
                          const PrintOptions& options)
{
    PrintResult result;
    if (!options.is_valid() || score.staff_count() == 0)
        return result;

    // The engine as given, with what a page has no use for turned off: the
    // bar ruler is a window's, and so is the cursor.
    LayoutOptions layout_options = engine.options();
    layout_options.draw_ruler = false;
    layout_options.draw_cursor = false;
    layout_options.draw_header = true;

    const double usable_width = options.page_width - 2.0 * options.margin;
    const double page_bottom = options.page_height - options.margin;
    const double footer = options.number_pages ? 2.5 : 0.0;

    const std::int32_t last_bar = last_bar_of(score);
    result.bar_count = last_bar;

    // The title block, on the first page only.
    PrintedPage page;
    page.layout.extent = Rect{0.0, 0.0, options.page_width, options.page_height};
    double y = options.margin;
    if (!options.title.empty()) {
        TextItem title;
        title.text = options.title;
        title.size = options.title_size;
        title.bold = true;
        title.origin = Point{options.page_width * 0.5 - text_width(options.title, title.size) * 0.5,
                             y + title.size};
        title.role = ItemRole::Text;
        page.layout.display_list.add(title);
        y += title.size * 1.6;
    }
    if (!options.subtitle.empty()) {
        TextItem subtitle;
        subtitle.text = options.subtitle;
        subtitle.size = options.title_size * 0.55;
        subtitle.italic = true;
        subtitle.origin =
            Point{options.page_width * 0.5 - text_width(options.subtitle, subtitle.size) * 0.5,
                  y + subtitle.size};
        subtitle.role = ItemRole::Text;
        page.layout.display_list.add(subtitle);
        y += subtitle.size * 1.8;
    }
    if (!options.title.empty() || !options.subtitle.empty())
        y += options.system_gap;

    double previous_scale = 0.0;
    std::int32_t bar = 1;
    bool first_system = true;

    while (bar <= last_bar) {
        // The widest line the music allows: the full count of bars, or
        // fewer where the shortest note would be narrower than a notehead.
        LayoutResult laid;
        TimelineMap timeline;
        notation::Ticks from = score.measure_start_tick(bar);
        notation::Ticks to = from;
        std::int32_t bars = 0;
        bool crowded = false;

        for (int count = options.bars_per_system; count >= 1; --count) {
            bars = std::min<std::int32_t>(count, last_bar - bar + 1);
            to = score.measure(bar + bars - 1).end_tick();
            const double quarters =
                static_cast<double>(to - from) / static_cast<double>(notation::kTicksPerQuarter);

            layout_options.header_time_signature = first_system || meter_changes_at(score, from);
            LayoutEngine system_engine(engine.glyph_table(), layout_options);
            system_engine.set_staff_metrics(engine.staff_metrics());

            // Laid out once to learn the header's width, then again at the
            // scale that fills the line - or, for a last line shorter than
            // the rest, at the scale the line before it had, since a bar
            // stretched across a whole page reads as a mistake.
            timeline = TimelineMap(8.0);
            timeline.set_origin_tick(from);
            laid = system_engine.layout(score, timeline, from, to);
            const double header = header_width_of(laid, timeline, from);

            const bool short_last_line = bars < count && previous_scale > 0.0;
            const double scale = short_last_line
                                     ? previous_scale
                                     : std::max(1.0, (usable_width - header) / quarters);
            timeline = TimelineMap(scale);
            timeline.set_origin_tick(from);
            laid = system_engine.layout(score, timeline, from, to);

            crowded = laid.display_list.is_crowded;
            if (!crowded || bars == 1) {
                if (!short_last_line)
                    previous_scale = scale;
                break;
            }
        }
        if (crowded)
            ++result.crowded_system_count;

        // Onto the page, or the next one when it does not fit under what
        // is there. A system taller than a page goes on a page of its own
        // rather than nowhere.
        const double height = laid.extent.height;
        if (!page.systems.empty() && y + height > page_bottom - footer) {
            result.pages.push_back(std::move(page));
            page = PrintedPage{};
            page.layout.extent = Rect{0.0, 0.0, options.page_width, options.page_height};
            y = options.margin;
        }

        const double dx = options.margin - laid.extent.x;
        const double dy = y - laid.extent.y;
        append_translated(laid, dx, dy, page.layout);
        if (page.layout.staff_top_y.empty()) {
            for (const double top : laid.staff_top_y)
                page.layout.staff_top_y.push_back(top + dy);
        }

        PrintedSystem system;
        system.first_bar = bar;
        system.last_bar = bar + bars - 1;
        system.top_y = y;
        system.staff_top_y = laid.staff_top_y.empty() ? y : laid.staff_top_y.front() + dy;
        page.systems.push_back(system);

        // The bar number at the start of every line but the first, where
        // it would say 1.
        if (bar > 1) {
            TextItem number;
            number.text = std::to_string(bar);
            number.size = 1.2;
            number.origin = Point{options.margin, system.staff_top_y - 1.0};
            number.role = ItemRole::Text;
            page.layout.display_list.add(number);
        }

        y += height + options.system_gap;
        bar += bars;
        first_system = false;
        ++result.system_count;
    }
    result.pages.push_back(std::move(page));

    // Page numbers, now that the count is known, and the page as the image:
    // the renderer sizes to the drawn bounds, and a page is its paper, not
    // its ink.
    for (std::size_t index = 0; index < result.pages.size(); ++index) {
        PrintedPage& numbered = result.pages[index];
        if (options.number_pages && result.pages.size() > 1) {
            TextItem number;
            number.text = std::to_string(index + 1);
            number.size = 1.4;
            number.origin = Point{options.page_width * 0.5 - text_width(number.text, number.size) * 0.5,
                                  options.page_height - options.margin * 0.5};
            number.role = ItemRole::Text;
            numbered.layout.display_list.add(number);
        }
        numbered.layout.extent = Rect{0.0, 0.0, options.page_width, options.page_height};
        numbered.layout.drawn_bounds = numbered.layout.extent;
    }
    return result;
}

std::vector<std::string> render_pages_svg(const PrintResult& pages, const SvgOptions& options,
                                          const GlyphTable* glyphs)
{
    std::vector<std::string> documents;
    documents.reserve(pages.pages.size());
    for (std::size_t index = 0; index < pages.pages.size(); ++index) {
        SvgOptions page_options = options;
        if (pages.pages.size() > 1)
            page_options.title += " - page " + std::to_string(index + 1);
        documents.push_back(render_svg(pages.pages[index].layout, page_options, glyphs));
    }
    return documents;
}

}  // namespace vcc::engraving
