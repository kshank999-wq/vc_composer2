// Copyright (c) VC Composer. All rights reserved.
//
// A part on pages (spec 02 sections 5.1, 5.3).
//
// The layout engine lays out one strip: a range of ticks on a timeline,
// which is what a window scrolls and what the plug-in draws around the
// playhead. A printed part is the other shape notation takes - systems of a
// few bars each, one under the other, on pages of a fixed size, with the
// part's name at the top and the bar number at the start of each line.
// This breaks a score into those, lays each system out with the engine,
// and stacks them, so the SVG renderer can write a page a composer can
// print or send to a player.
//
// It never draws and never re-engraves: every system is the engine's own
// layout of that range, moved onto the page, so what prints is what the
// window showed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "vcc/engraving/LayoutEngine.h"
#include "vcc/engraving/SvgRenderer.h"

namespace vcc::engraving {

/// How the pages are set, in staff spaces throughout.
///
/// The defaults are A4 with a staff of seven millimetres: a staff space of
/// 1.78 mm gives 118 by 167 spaces, which is the rastral size a printed
/// part is commonly engraved at.
struct PrintOptions {
    double page_width = 118.0;
    double page_height = 167.0;

    /// The same on every side.
    double margin = 8.0;

    /// Between one system's page area and the next's. The engine's own
    /// margins above and below each system are inside its area.
    double system_gap = 4.0;

    /// Bars on a line, at most. A line whose music would be narrower than
    /// a notehead at that count takes fewer bars, down to one.
    int bars_per_system = 4;

    /// The part's name, over the first page, and a second line under it:
    /// the instrument, say. Either may be empty.
    std::string title;
    std::string subtitle;
    double title_size = 3.0;

    /// A number at the foot of every page, when there is more than one.
    bool number_pages = true;

    bool is_valid() const noexcept
    {
        return page_width > 4.0 * margin && page_height > 4.0 * margin && bars_per_system >= 1;
    }
};

/// One system as it sits on a page.
struct PrintedSystem {
    std::int32_t first_bar = 1;
    std::int32_t last_bar = 1;

    /// y of the top of the system's area on the page, and of its first
    /// staff's top line.
    double top_y = 0.0;
    double staff_top_y = 0.0;
};

/// One page: a layout result whose extent is the page, so the SVG renderer
/// sizes the image to the paper and not to the ink.
struct PrintedPage {
    LayoutResult layout;
    std::vector<PrintedSystem> systems;
};

struct PrintResult {
    std::vector<PrintedPage> pages;
    std::size_t system_count = 0;
    std::int32_t bar_count = 0;

    /// Systems that were still crowded with one bar on the line, so the
    /// glyphs on them overlap. Reported, not hidden: a part with a run of
    /// sixty-fourths at this page width needs a wider page, and nothing
    /// here can decide that.
    std::size_t crowded_system_count = 0;
};

/// Breaks the score into systems and pages and lays each out with the
/// engine's glyphs and metrics. The engine's ruler and cursor are left off
/// whatever its options say: a page has neither. An empty score is one
/// page with one empty system.
PrintResult lay_out_pages(const notation::Score& score, const LayoutEngine& engine,
                          const PrintOptions& options = {});

/// Renders every page to its own SVG document, in order.
std::vector<std::string> render_pages_svg(const PrintResult& pages, const SvgOptions& options = {},
                                          const GlyphTable* glyphs = nullptr);

/// Appends one layout to another, moved by `dx` and `dy`. The display
/// list's bounds grow to take the moved items in; the extent and drawn
/// bounds are the caller's to set.
void append_translated(const LayoutResult& from, double dx, double dy, LayoutResult& into);

}  // namespace vcc::engraving
