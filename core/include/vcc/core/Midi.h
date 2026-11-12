// Note events and Standard MIDI File output.
//
// NoteEvent is the common currency between the composition assistants (Chord
// Explorer, Ostinato Creator), the sequencer model and export. Times are in
// project ticks (2880 PPQ) and converted to 960 PPQ only on the way out to a
// file, the same place the original VC Composer made that conversion.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vcc/core/Time.h"

namespace vcc {

struct NoteEvent {
    Ticks start = 0;
    Ticks duration = 0;
    int pitch = 60;      ///< MIDI note 0-127 (sounding).
    int velocity = 80;   ///< 1-127.
    int channel = 0;     ///< 0-15.
    /// Articulation intent carried with the note (e.g. "spiccato"). The
    /// playback interpreter turns it into keyswitches/CCs per library.
    std::string articulation;

    Ticks end() const noexcept { return start + duration; }
};

bool operator==(const NoteEvent& a, const NoteEvent& b) noexcept;
bool operator!=(const NoteEvent& a, const NoteEvent& b) noexcept;
/// Orders by start, then pitch, then duration - a stable canonical order.
bool operator<(const NoteEvent& a, const NoteEvent& b) noexcept;

/// Checks the invariants every generator must keep: positive durations,
/// pitch/velocity/channel in range, nothing before `range_start` or past
/// `range_end`, and no two notes of the same pitch and channel overlapping
/// (an overlap would make a note-off cut the following note short).
/// Returns an empty string when valid, else a description of the first fault.
std::string validate_notes(const std::vector<NoteEvent>& notes, Ticks range_start, Ticks range_end);

struct MidiFileTrack {
    std::string name;
    std::vector<NoteEvent> notes;
    int program = -1;  ///< General MIDI program, or -1 for none.
};

struct MidiFileOptions {
    MeterMap meter;
    TempoMap tempo;
    std::vector<std::pair<Ticks, std::string>> markers;
};

/// Builds a Type 1 Standard MIDI File at 960 PPQ: a conductor track with
/// tempo, meter and markers, then one track per MidiFileTrack.
std::string write_midi_file(const std::vector<MidiFileTrack>& tracks, const MidiFileOptions& options);

}  // namespace vcc
