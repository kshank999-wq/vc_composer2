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
/// The sketch tools for a track (the Piano, normally): Send Top / Middle /
/// Bass / All Notes to a track, and Spread Chords to Strings - over the
/// selected range, or the whole track.
juce::PopupMenu sketch_menu(AppContext& context, const std::string& source);
/// Spread Chords to Strings from `source`; the status line says what happened.
juce::String spread_to_strings(AppContext& context, const std::string& source);
/// The same to the strings (open), woodwinds or brass.
juce::String spread_to_section(AppContext& context, const std::string& source, session::Session::Section section);
/// The selected range of chosen tracks onto the selected track as chords.
void ask_implode(AppContext& context);
/// A repeat over bars, with optional first/second endings.
void ask_repeat(AppContext& context);
juce::String unroll(AppContext& context);
/// Expand Articulations to Tracks (Memory Spec §6): a dialog listing the
/// sounds the selected part's notes play, with note counts, to choose which
/// get a track of their own. The dialog's component ID is
/// "expand_articulations"; button 1 expands.
void ask_expand_articulations(AppContext& context);
/// Consolidate to One Track: the selected part (or the part an articulation
/// track belongs to) plays itself again.
juce::String consolidate_articulations(AppContext& context);
/// The part a track belongs to: itself, or its parent for an articulation track.
std::string articulation_part(const AppContext& context, const std::string& track_id);

}  // namespace vcc::desktop
