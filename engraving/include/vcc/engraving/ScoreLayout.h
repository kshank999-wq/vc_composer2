// A full score on pages (Main Spec §16): every part's staves stacked in one
// system, sharing one timeline so every bar lines up down the page.
//
// Each part is laid out by the same engine that draws the window and the
// printed parts, over the same tick range and on the same timeline, with the
// same header width; so a note on beat three of the flute's bar sits exactly
// above beat three of the bass's. On top of that this draws what only a score
// has: brackets over each section, a brace for a keyboard's staves, barlines
// running through a section, instrument names (in full on the first system,
// abbreviated after) and, optionally, staves hidden where a part rests for
// the whole system.
#pragma once

#include <string>
#include <vector>

#include "vcc/engraving/PrintLayout.h"

namespace vcc::engraving {

/// One part (an instrument, a track) in the score, in score order.
struct ScorePart {
    const notation::Score* score = nullptr;  ///< One or more staves.
    std::string name;          ///< "Flute 1", shown on the first system.
    std::string abbreviation;  ///< "Fl. 1", shown on the rest.
    /// Parts with the same non-empty group next to each other share a
    /// bracket and their barlines run through: "woodwinds", "brass",
    /// "strings". Empty: no bracket.
    std::string group;
};

struct ScoreOptions {
    /// Page and margins in staff spaces. The default is A3 portrait at a
    /// staff space of 1.25 mm (a 5 mm staff), common for an orchestral
    /// score; use page_size_for() to set another paper and rastral.
    double page_width = 237.6;
    double page_height = 336.0;
    double margin = 10.0;
    double system_gap = 8.0;
    /// Between the staves of one system (top line to bottom line of the next
    /// staff is staff height + this).
    double staff_distance = 7.0;
    int bars_per_system = 4;
    std::string title;
    std::string subtitle;  ///< "Full score in C" / "Transposed score".
    std::string composer;  ///< Right-aligned under the title.
    double title_size = 4.0;
    bool number_pages = true;
    /// Leave out a part's staves on a system where it has no notes (never on
    /// the first system). A system where every part rests keeps them all.
    bool hide_empty_staves = true;
    bool bar_numbers = true;
    bool is_valid() const noexcept
    {
        return page_width > 4.0 * margin && page_height > 4.0 * margin && bars_per_system >= 1;
    }
};

/// Page width and height in staff spaces for a paper size in millimetres
/// and a staff space (staff height / 4) in millimetres.
void page_size_for(double width_mm, double height_mm, double staff_space_mm, ScoreOptions& options);

/// Lays every part out on pages. Parts with no score are skipped. The meter
/// and bars come from the first part. Pages print with render_pdf (use the
/// same staff space for PdfOptions::points_per_staff_space).
PrintResult lay_out_score(const std::vector<ScorePart>& parts, const LayoutEngine& engine,
                          const ScoreOptions& options = {});

}  // namespace vcc::engraving
