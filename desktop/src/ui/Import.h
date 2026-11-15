// Importing audio and MIDI files: File > Import Audio, and files dragged
// onto the window or onto a track in the sequencer's lanes.
#pragma once

#include <string>
#include <vector>

#include <juce_core/juce_core.h>

#include "ui/AppContext.h"

namespace vcc::desktop {

bool is_audio_file(const juce::File& f);
bool is_midi_file(const juce::File& f);
/// MusicXML (.musicxml, .xml, .mxl): imported into the score.
bool is_musicxml_file(const juce::File& f);
/// A VC Composer 2 project (.vcc2): opened.
bool is_project_file(const juce::File& f);

struct ImportResult {
    std::vector<std::string> ids;     ///< Clips placed (audio).
    std::vector<std::string> tracks;  ///< Tracks that received something.
    juce::String message;             ///< For the status strip.
    juce::StringArray unreadable;     ///< Files that could not be read.
};

/// Places audio files one after another from `at_seconds`, as one undo
/// step. On `track_id` when it is an audio or reference track (or an
/// instrument or MIDI track: its own audio lane); otherwise
/// the selected audio track, or (with `allow_selected` false, or none
/// selected) a new audio track named after the first file. Inside a saved
/// project the files are copied into "Audio Files".
ImportResult import_audio_files(AppContext& context, const juce::Array<juce::File>& files, const std::string& track_id,
                                double at_seconds, bool allow_selected = true);

/// Imports MIDI files (see Session::import_midi_file): onto `track_id` when
/// it is an instrument or MIDI track and the file has one part, otherwise
/// as new tracks, from the bar at `at`.
ImportResult import_midi_files(AppContext& context, const juce::Array<juce::File>& files, const std::string& track_id,
                               Ticks at);

}  // namespace vcc::desktop
