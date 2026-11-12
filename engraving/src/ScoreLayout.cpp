#include "vcc/engraving/ScoreLayout.h"

#include <algorithm>
#include <set>

#include "vcc/engraving/TimelineMap.h"

namespace vcc::engraving {

namespace {

constexpr double kStaffHeight = 4.0;

double text_width(const std::string& text, double size)
{
    // Proportional estimate, as the part layout uses; UTF-8 continuation
    // bytes do not count as characters.
    std::size_t chars = 0;
    for (char c : text)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++chars;
    return static_cast<double>(chars) * size * 0.5;
}

std::int32_t last_bar_of(const notation::Score& score)
{
    const notation::Ticks end = score.end_tick();
    return end <= 0 ? 1 : score.measure_at(end - 1).number;
}

bool meter_changes_at(const notation::Score& score, notation::Ticks tick)
{
    for (const auto& change : score.time_signature_changes())
        if (change.tick == tick) return true;
    return false;
}

bool has_notes(const notation::Score& score, notation::Ticks from, notation::Ticks to)
{
    for (std::size_t s = 0; s < score.staff_count(); ++s)
        for (const auto& n : score.notes_in_range(s, from, to))
            if (!n.is_rest) return true;
    return false;
}

void add_text(LayoutResult& page, const std::string& text, double x, double y, double size, bool bold = false,
              bool italic = false)
{
    TextItem t;
    t.text = text;
    t.origin = Point{x, y};
    t.size = size;
    t.bold = bold;
    t.italic = italic;
    t.role = ItemRole::Text;
    page.display_list.add(t);
}

void add_line(LayoutResult& page, Point a, Point b, double thickness, ItemRole role = ItemRole::Barline)
{
    LineItem l;
    l.from = a;
    l.to = b;
    l.thickness = thickness;
    l.role = role;
    page.display_list.add(l);
}

/// A section bracket: a thick bar with hooks turning right at both ends.
void add_bracket(LayoutResult& page, double x, double top, double bottom)
{
    QuadItem bar;
    bar.role = ItemRole::Barline;
    bar.corners[0] = Point{x - 0.5, top - 0.4};
    bar.corners[1] = Point{x, top - 0.4};
    bar.corners[2] = Point{x, bottom + 0.4};
    bar.corners[3] = Point{x - 0.5, bottom + 0.4};
    page.display_list.add(bar);
    for (const double sign : {-1.0, 1.0}) {
        const double y = sign < 0 ? top - 0.4 : bottom + 0.4;
        CurveItem hook;
        hook.role = ItemRole::Barline;
        hook.start = Point{x - 0.5, y};
        hook.control_one = Point{x + 0.2, y + sign * 0.05};
        hook.control_two = Point{x + 0.8, y + sign * 0.3};
        hook.end = Point{x + 1.2, y + sign * 0.9};
        hook.thickness = 0.3;
        hook.tapered = true;
        hook.end_thickness = 0.06;
        page.display_list.add(hook);
    }
}

/// A keyboard brace: two tapered curves meeting in a point at the middle.
void add_brace(LayoutResult& page, double x, double top, double bottom)
{
    const double mid = (top + bottom) * 0.5, h = bottom - top;
    for (const double sign : {-1.0, 1.0}) {
        const double end_y = sign < 0 ? top : bottom;
        CurveItem c;
        c.role = ItemRole::Barline;
        c.start = Point{x - 1.3, mid};
        c.control_one = Point{x - 0.2, mid + sign * h * 0.06};
        c.control_two = Point{x - 1.6, end_y - sign * h * 0.3};
        c.end = Point{x, end_y};
        c.thickness = std::min(1.1, 0.3 + h * 0.035);
        c.tapered = true;
        c.end_thickness = 0.05;
        page.display_list.add(c);
    }
}

struct LaidPart {
    std::size_t index = 0;  ///< into the parts list
    LayoutResult laid;
};

}  // namespace

void page_size_for(double width_mm, double height_mm, double staff_space_mm, ScoreOptions& options)
{
    if (staff_space_mm <= 0.0) return;
    options.page_width = width_mm / staff_space_mm;
    options.page_height = height_mm / staff_space_mm;
    options.margin = 12.0 / staff_space_mm;  // 12 mm
}

PrintResult lay_out_score(const std::vector<ScorePart>& all_parts, const LayoutEngine& engine,
                          const ScoreOptions& options)
{
    PrintResult result;
    std::vector<ScorePart> parts;
    for (const auto& p : all_parts)
        if (p.score && p.score->staff_count() > 0) parts.push_back(p);
    if (!options.is_valid() || parts.empty()) return result;

    const notation::Score& reference = *parts.front().score;
    std::int32_t last_bar = 1;
    for (const auto& p : parts) last_bar = std::max(last_bar, last_bar_of(*p.score));
    result.bar_count = last_bar;

    LayoutOptions base = engine.options();
    base.draw_ruler = false;
    base.draw_cursor = false;
    base.draw_header = true;
    base.staff_gap = options.staff_distance;
    base.margin_below = options.staff_distance * 0.5;

    // Indents for the instrument names: full on the first system.
    double name_width = 0.0, abbreviation_width = 0.0;
    const double name_size = 1.7;
    for (const auto& p : parts) {
        name_width = std::max(name_width, text_width(p.name, name_size));
        abbreviation_width = std::max(abbreviation_width, text_width(p.abbreviation, name_size));
    }
    const bool any_group = std::any_of(parts.begin(), parts.end(), [](const ScorePart& p) { return !p.group.empty(); });
    const double furniture = (any_group ? 2.0 : 0.0) + 2.2;  // bracket/brace and a gap before the names

    const double page_bottom = options.page_height - options.margin;
    const double footer = options.number_pages ? 3.0 : 0.0;

    PrintedPage page;
    page.layout.extent = Rect{0.0, 0.0, options.page_width, options.page_height};
    double y = options.margin;
    // Title block.
    if (!options.title.empty()) {
        add_text(page.layout, options.title, options.page_width * 0.5 - text_width(options.title, options.title_size) * 0.5,
                 y + options.title_size, options.title_size, true);
        y += options.title_size * 1.6;
    }
    if (!options.subtitle.empty()) {
        const double s = options.title_size * 0.5;
        add_text(page.layout, options.subtitle, options.page_width * 0.5 - text_width(options.subtitle, s) * 0.5, y + s, s,
                 false, true);
        y += s * 1.8;
    }
    if (!options.composer.empty()) {
        const double s = options.title_size * 0.45;
        add_text(page.layout, options.composer, options.page_width - options.margin - text_width(options.composer, s),
                 y + s, s);
        y += s * 1.8;
    }
    if (!options.title.empty() || !options.subtitle.empty() || !options.composer.empty()) y += options.system_gap;

    double previous_scale = 0.0;
    std::int32_t bar = 1;
    bool first_system = true;
    while (bar <= last_bar) {
        const double indent = (first_system ? name_width : abbreviation_width) + furniture;
        const double usable = options.page_width - 2.0 * options.margin - indent;
        const notation::Ticks from = reference.measure_start_tick(bar);

        std::vector<LaidPart> laid;
        TimelineMap timeline;
        std::int32_t bars = 1;
        bool crowded = false;
        for (int count = options.bars_per_system; count >= 1; --count) {
            bars = std::min<std::int32_t>(count, last_bar - bar + 1);
            const notation::Ticks to = reference.measure(bar + bars - 1).end_tick();
            const double quarters = static_cast<double>(to - from) / static_cast<double>(notation::kTicksPerQuarter);

            // Which parts are on this system.
            std::vector<std::size_t> visible;
            for (std::size_t i = 0; i < parts.size(); ++i)
                if (first_system || !options.hide_empty_staves || has_notes(*parts[i].score, from, to)) visible.push_back(i);
            if (visible.empty())
                for (std::size_t i = 0; i < parts.size(); ++i) visible.push_back(i);

            LayoutOptions lo = base;
            lo.header_time_signature = first_system || meter_changes_at(reference, from);
            auto lay_all = [&](double scale, double header) {
                std::vector<LaidPart> out;
                timeline = TimelineMap(scale);
                timeline.set_origin_tick(from);
                for (std::size_t k = 0; k < visible.size(); ++k) {
                    LayoutOptions plo = lo;
                    plo.header_width = header;
                    // Room above the top staff for tempo and text marks.
                    plo.margin_above = k == 0 ? std::max(6.0, options.staff_distance * 0.5) : options.staff_distance * 0.5;
                    plo.draw_texts = k == 0;
                    LayoutEngine e(engine.glyph_table(), plo);
                    e.set_staff_metrics(engine.staff_metrics());
                    out.push_back({visible[k], e.layout(*parts[visible[k]].score, timeline, from, to)});
                }
                return out;
            };
            // Once to learn the widest header, then at the scale that fills
            // the line with every part sharing that header.
            double header = base.header_width;
            for (const auto& lp : lay_all(8.0, base.header_width))
                header = std::max(header, timeline.x_for_tick(from) - lp.laid.extent.x);
            const bool short_last_line = bars < count && previous_scale > 0.0;
            const double scale = short_last_line ? previous_scale : std::max(1.0, (usable - header) / quarters);
            laid = lay_all(scale, header);
            crowded = std::any_of(laid.begin(), laid.end(), [](const LaidPart& lp) { return lp.laid.display_list.is_crowded; });
            if (!crowded || bars == 1) {
                if (!short_last_line) previous_scale = scale;
                break;
            }
        }
        if (crowded) ++result.crowded_system_count;

        double height = 0.0;
        for (const auto& lp : laid) height += lp.laid.extent.height;
        if (!page.systems.empty() && y + height > page_bottom - footer) {
            result.pages.push_back(std::move(page));
            page = PrintedPage{};
            page.layout.extent = Rect{0.0, 0.0, options.page_width, options.page_height};
            y = options.margin;
        }

        // Stack the parts; remember each one's staves on the page.
        struct Placed {
            std::size_t index;
            double top, bottom;  // first staff's top line, last staff's bottom line
        };
        std::vector<Placed> placed;
        double left = 0.0;
        std::set<long long> barline_x;  // in 1/1000 space, from the first part
        const double system_top = y;
        for (std::size_t k = 0; k < laid.size(); ++k) {
            const auto& lp = laid[k];
            const double dx = options.margin + indent - lp.laid.extent.x;
            const double dy = y - lp.laid.extent.y;
            append_translated(lp.laid, dx, dy, page.layout);
            if (lp.laid.staff_top_y.empty()) continue;
            placed.push_back({lp.index, lp.laid.staff_top_y.front() + dy, lp.laid.staff_top_y.back() + dy + kStaffHeight});
            left = options.margin + indent;
            if (k == 0)
                for (const auto& line : lp.laid.display_list.lines)
                    if (line.role == ItemRole::Barline && line.from.x == line.to.x)
                        barline_x.insert(static_cast<long long>((line.from.x + dx) * 1000.0 + 0.5));
            y += lp.laid.extent.height;
        }
        if (placed.empty()) break;

        // The system's left barline, through every staff.
        add_line(page.layout, Point{left, placed.front().top}, Point{left, placed.back().bottom}, 0.2);
        // Section barlines through the gaps, and brackets; braces for parts
        // with several staves.
        for (std::size_t k = 0; k < placed.size(); ++k) {
            const auto& part = parts[placed[k].index];
            if (part.score->staff_count() > 1) add_brace(page.layout, left - (any_group ? 2.0 : 0.6), placed[k].top, placed[k].bottom);
            if (k + 1 < placed.size() && !part.group.empty() && part.group == parts[placed[k + 1].index].group)
                for (const long long xm : barline_x) {
                    const double x = static_cast<double>(xm) / 1000.0;
                    if (x <= left + 0.5) continue;
                    add_line(page.layout, Point{x, placed[k].bottom}, Point{x, placed[k + 1].top}, 0.16);
                }
        }
        for (std::size_t k = 0; k < placed.size();) {
            const std::string& g = parts[placed[k].index].group;
            std::size_t end = k;
            while (end + 1 < placed.size() && !g.empty() && parts[placed[end + 1].index].group == g) ++end;
            if (!g.empty()) add_bracket(page.layout, left - 0.9, placed[k].top, placed[end].bottom);
            k = end + 1;
        }
        // Names, right-aligned before the brackets, centred on their staves.
        for (const auto& pl : placed) {
            const auto& part = parts[pl.index];
            const std::string& name = first_system ? part.name : part.abbreviation;
            if (name.empty()) continue;
            add_text(page.layout, name, left - furniture - text_width(name, name_size) + 0.6,
                     (pl.top + pl.bottom) * 0.5 + name_size * 0.35, name_size);
        }
        if (options.bar_numbers && bar > 1) add_text(page.layout, std::to_string(bar), left, placed.front().top - 1.4, 1.3);

        PrintedSystem system;
        system.first_bar = bar;
        system.last_bar = bar + bars - 1;
        system.top_y = system_top;
        system.staff_top_y = placed.front().top;
        page.systems.push_back(system);
        if (page.layout.staff_top_y.empty())
            for (const auto& pl : placed) page.layout.staff_top_y.push_back(pl.top);

        y += options.system_gap;
        bar += bars;
        first_system = false;
        ++result.system_count;
    }
    result.pages.push_back(std::move(page));

    for (std::size_t i = 0; i < result.pages.size(); ++i) {
        auto& p = result.pages[i];
        if (options.number_pages && result.pages.size() > 1) {
            const std::string n = std::to_string(i + 1);
            add_text(p.layout, n, options.page_width * 0.5 - text_width(n, 1.6) * 0.5,
                     options.page_height - options.margin * 0.5, 1.6);
        }
        p.layout.extent = Rect{0.0, 0.0, options.page_width, options.page_height};
        p.layout.drawn_bounds = p.layout.extent;
    }
    return result;
}

}  // namespace vcc::engraving
