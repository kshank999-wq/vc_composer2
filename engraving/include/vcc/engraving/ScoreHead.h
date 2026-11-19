// The head of a finished score or part (Score > Score Info): the title (styled),
// subtitle, composer at the right, words at the left, the arranger under the
// composer, a small line at the top left (date, revision), the tempo over the
// first system, the copyright at the foot of the first page.
#pragma once

#include <string>

#include "vcc/engraving/DisplayList.h"

namespace vcc::engraving {

struct ScoreHead {
    bool title_bold = true;
    bool title_italic = false;
    std::string lyricist, arranger, info_line, copyright;
    /// "Allegro" before the metronome mark (a quarter note = bpm); bpm 0: none.
    std::string tempo_text;
    double tempo_bpm = 0.0;
};

/// Lays the head out from `y` (staff spaces) on a page `page_width` wide;
/// returns the y the music starts at. `page_bottom` places the copyright.
double lay_out_head(DisplayList& list, const ScoreHead& head, const std::string& title, const std::string& subtitle,
                    const std::string& composer, double title_size, double page_width, double margin, double y,
                    double copyright_baseline);

}  // namespace vcc::engraving
