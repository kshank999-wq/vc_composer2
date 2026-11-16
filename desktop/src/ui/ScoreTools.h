// Notation menu tools (Main Spec §7): Lyrics, Explode, Implode, Repeats and
// endings, Unroll Repeats. Each asks what it needs in a small dialog and
// runs one undoable session operation.
#pragma once

#include "ui/AppContext.h"

namespace vcc::desktop {

/// The selection's notes (or, with none, every note in the selected range
/// of the selected track) get the typed lyrics.
void ask_lyrics(AppContext& context);
/// The selected range of the selected track into N new tracks.
void ask_explode(AppContext& context);
/// The selected range of chosen tracks onto the selected track as chords.
void ask_implode(AppContext& context);
/// A repeat over bars, with optional first/second endings.
void ask_repeat(AppContext& context);
juce::String unroll(AppContext& context);

}  // namespace vcc::desktop
