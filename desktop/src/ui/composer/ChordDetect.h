// Chord symbols for the Composer view's Chords row (docs/spec/ui, "Ruler
// rows"): read from what is written, bar by bar, across every pitched
// track. Read-only: the project stores no chord track yet.
#pragma once

#include <string>
#include <vector>

#include "vcc/core/Midi.h"
#include "vcc/core/Time.h"

namespace vcc::desktop::composer_ui {

/// The chord a set of sounding notes spells, as a symbol ("D", "Bm",
/// "F#m7", "Asus4"), or empty when there is no clear chord. Notes weigh by
/// how long they sound; the root is spelled with flats when `flats`.
std::string chord_symbol(const std::vector<NoteEvent>& notes, Ticks from, Ticks to, bool flats);

/// One symbol per bar from 1 to `bars` (empty where none).
std::vector<std::string> chord_symbols(const std::vector<NoteEvent>& notes, const MeterMap& meter, int bars, bool flats);

}  // namespace vcc::desktop::composer_ui
