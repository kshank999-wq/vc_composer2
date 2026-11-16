// Editing tools (Main Spec §9):
//
//   - Reverse and Time Stretch on the selected clips: each clip's audio is
//     processed into a new file in the project's audio folder and the clip
//     pointed at it, all clips as one undo step; the original recording is
//     never touched, so undo simply points back at it. Stretch keeps the
//     pitch (WSOLA, vcc/audio/TimeStretch.h), scales the fades, and lengthens
//     or shortens the clip from its start.
//   - Group / Ungroup the selected clips: they then move and nudge together.
//   - Transport > Capture MIDI: the last thing played on a MIDI keyboard
//     while not recording becomes a region on the selected track
//     (vcc/session/Retrospective.h).
//   - Tools > MIDI Input Filter...: channels, note range, minimum velocity
//     and message types let through to play and record. Remembered.
#pragma once

#include "ui/AppContext.h"

namespace vcc::desktop {

/// Reverses clips ({track, clip}); returns what happened.
juce::String reverse_clips(AppContext& context, const std::vector<std::pair<std::string, std::string>>& clips);
/// Stretches clips to `ratio` times their length (pitch kept).
juce::String stretch_clips(AppContext& context, const std::vector<std::pair<std::string, std::string>>& clips, double ratio);
/// Asks for a percentage, then stretches.
void ask_time_stretch(AppContext& context, const std::vector<std::pair<std::string, std::string>>& clips);

/// The engine's recent MIDI input as a region on `track_id`.
juce::String capture_midi(AppContext& context, const std::string& track_id);

/// Tools > MIDI Input Filter.
void show_midi_input_filter(AppContext& context);
juce::Component* midi_input_filter_panel();
/// The saved filter, applied to the engine (at startup).
void restore_midi_input_filter(AppContext& context);

}  // namespace vcc::desktop
