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

namespace {

/// A part's score with its runs of empty bars each made one bar, and where
/// every bar of it came from: (first original bar, how many).
struct Condensed {
    notation::Score score;
    std::vector<std::pair<std::int32_t, std::int32_t>> bars;  ///< Index: condensed bar - 1.
};

bool bar_is_empty(const notation::Score& score, std::int32_t bar)
{
    const notation::Measure m = score.measure(bar);
    for (std::size_t s = 0; s < score.staff_count(); ++s)
        for (const auto& n : score.notes(s))
            if (!n.is_rest && n.start_tick < m.end_tick() && std::max(n.end_tick(), n.start_tick + 1) > m.start_tick) return false;
    auto inside = [&](notation::Ticks t) { return t >= m.start_tick && t < m.end_tick(); };
    for (const auto& x : score.dynamics()) if (inside(x.tick)) return false;
    for (const auto& x : score.texts()) if (inside(x.tick)) return false;
    for (const auto& x : score.tempos()) if (inside(x.tick)) return false;
    for (const auto& x : score.hairpins()) if (x.start_tick < m.end_tick() && x.end_tick > m.start_tick) return false;
    return true;
}

Condensed condense(const notation::Score& score, std::int32_t last_bar)
{
    // Which bars stay: a run of empty bars in one meter and key, crossing no
    // repeat sign, keeps its first.
    auto boundary = [&](std::int32_t bar) {
        const notation::Ticks t = score.measure_start_tick(bar);
        for (const auto& c : score.time_signature_changes()) if (c.tick == t) return true;
        for (const auto& c : score.key_signature_changes()) if (c.tick == t) return true;
        for (const auto& r : score.repeats())
            if (r.start_bar == bar || r.end_bar + 1 == bar || (r.ending_bars > 0 && (bar == r.end_bar - r.ending_bars + 1 ||
                                                                                       bar == r.end_bar + r.ending_bars + 1)))
                return true;
        return false;
    };
    Condensed out;
    for (std::int32_t bar = 1; bar <= last_bar;) {
        std::int32_t run = 1;
        if (bar_is_empty(score, bar))
            while (bar + run <= last_bar && !boundary(bar + run) && bar_is_empty(score, bar + run)) ++run;
        if (run < 2) run = 1;
        out.bars.emplace_back(bar, run);
        bar += run;
    }
    // The condensed score: each kept bar's music at its new place.
    out.score.reset_staves(score.staves());
    std::vector<notation::Ticks> new_start(out.bars.size() + 1, 0);
    for (std::size_t j = 0; j < out.bars.size(); ++j)
        new_start[j + 1] = new_start[j] + score.measure(out.bars[j].first).length_ticks;
    for (std::size_t j = 0; j < out.bars.size(); ++j) {
        const notation::Measure m = score.measure(out.bars[j].first);
        if (j == 0 || !(m.time_signature == score.measure(out.bars[j - 1].first).time_signature))
            out.score.set_time_signature(new_start[j], m.time_signature);
        if (j == 0 || !(m.key == score.measure(out.bars[j - 1].first).key)) out.score.set_key_signature(new_start[j], m.key);
    }
    auto place = [&](notation::Ticks t, notation::Ticks& to) {
        for (std::size_t j = 0; j < out.bars.size(); ++j) {
            const notation::Measure m = score.measure(out.bars[j].first);
            if (t >= m.start_tick && t < m.end_tick()) {
                to = new_start[j] + (t - m.start_tick);
                return true;
            }
        }
        return false;
    };
    for (std::size_t s = 0; s < score.staff_count(); ++s)
        for (auto n : score.notes(s)) {
            notation::Ticks t = 0;
            if (!place(n.start_tick, t)) continue;
            n.id = notation::kInvalidEventId;
            n.start_tick = t;
            out.score.insert_note(s, n);
        }
    for (auto x : score.dynamics()) if (place(x.tick, x.tick)) out.score.add_dynamic(x);
    for (auto x : score.texts()) if (place(x.tick, x.tick)) out.score.add_text(x);
    for (auto x : score.tempos()) if (place(x.tick, x.tick)) out.score.add_tempo(x);
    for (auto x : score.hairpins()) {
        const auto len = x.end_tick - x.start_tick;
        if (place(x.start_tick, x.start_tick)) {
            x.end_tick = x.start_tick + len;
            out.score.add_hairpin(x);
        }
    }
    for (auto x : score.slurs()) {
        const auto len = x.end_tick - x.start_tick;
        if (place(x.start_tick, x.start_tick)) {
            x.end_tick = x.start_tick + len;
            out.score.add_slur(x);
        }
    }
    auto condensed_bar = [&](std::int32_t original) {
        for (std::size_t j = 0; j < out.bars.size(); ++j)
            if (original >= out.bars[j].first && original < out.bars[j].first + out.bars[j].second) return static_cast<std::int32_t>(j) + 1;
        return original;
    };
    std::vector<notation::RepeatSection> reps;
    for (auto r : score.repeats()) {
        r.start_bar = condensed_bar(r.start_bar);
        r.end_bar = condensed_bar(r.end_bar);
        reps.push_back(r);
    }
    out.score.set_repeats(reps);
    return out;
}

PrintResult lay_out(const notation::Score& score, const LayoutEngine& engine, const PrintOptions& options,
                    const std::vector<std::pair<std::int32_t, std::int32_t>>& bars);

}  // namespace

PrintResult lay_out_pages(const notation::Score& score, const LayoutEngine& engine, const PrintOptions& options)
{
    if (!options.multirests || score.staff_count() == 0) return lay_out(score, engine, options, {});
    const auto condensed = condense(score, last_bar_of(score));
    auto result = lay_out(condensed.score, engine, options, condensed.bars);
    result.bar_count = last_bar_of(score);
    return result;
}

namespace {

PrintResult lay_out(const notation::Score& score, const LayoutEngine& engine, const PrintOptions& options,
                    const std::vector<std::pair<std::int32_t, std::int32_t>>& bars_map)
{
    // The original number of a laid-out bar, and how many it stands for.
    auto original = [&](std::int32_t b) {
        return b >= 1 && b <= static_cast<std::int32_t>(bars_map.size()) ? bars_map[static_cast<std::size_t>(b - 1)].first : b;
    };
    auto count_of = [&](std::int32_t b) {
        return b >= 1 && b <= static_cast<std::int32_t>(bars_map.size()) ? bars_map[static_cast<std::size_t>(b - 1)].second : 1;
    };
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
    double y = lay_out_head(page.layout.display_list, options.head, options.title, options.subtitle, options.composer,
                            options.title_size, options.page_width, options.margin, options.margin,
                            page_bottom - (options.number_pages ? 2.8 : 0.4));
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

        // Multi-bar rests: the bar's rest gives way to an H-bar and the count.
        for (std::int32_t b = bar; b < bar + bars; ++b) {
            if (count_of(b) < 2) continue;
            const notation::Measure m = score.measure(b);
            const double x0 = timeline.x_for_tick(m.start_tick), x1 = timeline.x_for_tick(m.end_tick());
            auto& glyphs = laid.display_list.glyphs;
            glyphs.erase(std::remove_if(glyphs.begin(), glyphs.end(),
                                        [&](const GlyphItem& g) { return g.role == ItemRole::Rest && g.origin.x >= x0 && g.origin.x < x1; }),
                         glyphs.end());
            for (const double top : laid.staff_top_y) {
                LineItem h;
                h.from = Point{x0 + 1.2, top + 2.0};
                h.to = Point{x1 - 1.2, top + 2.0};
                h.thickness = 0.8;
                h.role = ItemRole::Rest;
                laid.display_list.add(h);
                for (const double x : {x0 + 1.2, x1 - 1.2}) {
                    LineItem serif;
                    serif.from = Point{x, top + 1.0};
                    serif.to = Point{x, top + 3.0};
                    serif.thickness = 0.16;
                    serif.role = ItemRole::Rest;
                    laid.display_list.add(serif);
                }
                TextItem n;
                n.text = std::to_string(count_of(b));
                n.size = 2.0;
                n.bold = true;
                n.origin = Point{(x0 + x1) * 0.5 - text_width(n.text, n.size) * 0.5, top - 0.8};
                n.role = ItemRole::Text;
                laid.display_list.add(n);
            }
        }

        const double dx = options.margin - laid.extent.x;
        const double dy = y - laid.extent.y;
        append_translated(laid, dx, dy, page.layout);
        if (page.layout.staff_top_y.empty()) {
            for (const double top : laid.staff_top_y)
                page.layout.staff_top_y.push_back(top + dy);
        }

        PrintedSystem system;
        system.first_bar = original(bar);
        system.last_bar = original(bar + bars - 1) + count_of(bar + bars - 1) - 1;
        system.top_y = y;
        system.staff_top_y = laid.staff_top_y.empty() ? y : laid.staff_top_y.front() + dy;
        page.systems.push_back(system);

        // The bar number at the start of every line but the first, where
        // it would say 1.
        if (bar > 1) {
            TextItem number;
            number.text = std::to_string(original(bar));
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

}  // namespace

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
