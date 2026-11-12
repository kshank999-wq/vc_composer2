// Copyright (c) VC Composer. All rights reserved.
//
// A score as a standard MIDI file (spec 01 section 7, spec 02 section 6).
//
// This is the export path, and it is the same path twice over: the PTSL
// client's `import_midi` places a file into the open Pro Tools session, and
// the drag-and-drop fallback hands the user the same file when no scripting
// channel is available. One writer serves both, which is why
// `MidiImportRequest` carries a path rather than events.
//
// The format work is in `vcc/music/MidiFile.h`, which knows nothing about
// notation. This is the part that knows what a score is: which staves become
// which tracks, where the tempo map comes from, and what a score can contain
// that a MIDI file cannot carry.
//
// The file is written at the host's 960 ticks per quarter note
// (`music::kTicksPerQuarter`), which the score's 2880 is three times, so
// every tick is divided by three on the way out: exact for everything but a
// nested tuplet, whose ticks can fall between two of the file's and are
// rounded to the nearer, a third of a file tick at most, and counted.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vcc/music/MidiFile.h"
#include "vcc/notation/MidiRenderer.h"
#include "vcc/notation/Score.h"

namespace vcc::notation {

struct ScoreMidiOptions {
    /// One MIDI track per staff, or everything on one.
    ///
    /// Off by default, because a Score is the notation for *one* bound Pro
    /// Tools track (spec 02 section 4.2) - a grand staff included, since
    /// both hands are one instrument playing one part. Turn it on to export
    /// a multi-staff score as separate tracks.
    bool one_track_per_staff = false;

    /// Write a first track carrying the tempo map, meters and keys, and no
    /// notes. This is what a DAW expects of a format 1 file, and it is what
    /// keeps the notes' track readable as a single instrument.
    bool write_conductor_track = true;

    std::string conductor_track_name = "Conductor";

    /// Name for the music track. Empty takes the staff's name, and failing
    /// that a fixed fallback - never an empty name, because a DAW shows it
    /// as the imported track's name.
    std::string track_name;

    /// MIDI channel for the first track. With `one_track_per_staff`, each
    /// staff takes the next channel, wrapping after 16.
    std::uint8_t first_channel = 0;

    /// Tempo written at the start when the score carries no tempo marking
    /// at all.
    ///
    /// A reader with no tempo event assumes 120, which is the same number -
    /// but a file that says what it means can be read by something that
    /// assumes differently.
    double default_tempo_bpm = 120.0;

    /// Shift the exported passage so that `render.from_tick` becomes tick 0
    /// of the file.
    ///
    /// On by default, because that is what an import wants: the caller says
    /// where to place the file and the file begins at what was exported.
    /// Turn it off when the file must carry absolute score positions -
    /// exporting a whole cue for archival, say.
    bool rebase_to_range_start = true;

    /// Write the score's text markings as marker meta events, which a DAW
    /// reads as memory locations - so a rehearsal letter or a cue point
    /// written in the part lands on the Pro Tools ruler. On by default:
    /// text a composer wrote is text they meant. Markers go on the
    /// conductor track when there is one, since that is where a DAW looks
    /// for them, and on the music track otherwise. A marking's voice is not
    /// carried; the format has no voice.
    bool write_text_as_markers = true;

    /// Passed to the renderer. Its `from_tick`/`to_tick` select the passage;
    /// an empty range exports the whole score.
    RenderOptions render{};
};

/// What the export contains, and what it could not carry.
struct ScoreMidiResult {
    music::SmfFile file;

    /// Notes whose pitch fell outside MIDI's range after transposition, as
    /// the renderer reported them. Passed through rather than swallowed, so
    /// the UI can name the unplayable note instead of the composer
    /// discovering silence.
    std::vector<EventId> out_of_range_notes;

    /// Articulations the expression map has no rule for.
    std::vector<Articulation> unmapped_articulations;

    /// Things the score holds that a MIDI file cannot express, in words fit
    /// for the UI - a meter whose denominator is not a power of two, a tempo
    /// outside the range the format can carry. Named rather than dropped
    /// quietly, which is the whole difference between an export a composer
    /// can trust and one they cannot.
    std::vector<std::string> unrepresentable;

    std::size_t note_count = 0;
    std::size_t controller_count = 0;
    std::size_t keyswitch_count = 0;
    std::size_t music_track_count = 0;
    bool has_conductor_track = false;

    /// Text markings written as markers.
    std::size_t marker_count = 0;

    /// Events whose position fell before the start of the exported range and
    /// were moved to tick 0.
    ///
    /// This happens to keyswitches, which the renderer deliberately places
    /// ahead of the note they apply to: exporting a passage that begins on
    /// such a note would otherwise put its switch before the file starts.
    /// Moving it to the start keeps it before the note, which is what it is
    /// for; dropping it would play the note with the wrong articulation.
    std::size_t clamped_to_start = 0;

    /// Events whose score tick fell between two of the file's ticks and
    /// were rounded to the nearer. Only a nested tuplet's can: the file
    /// counts 960 to the quarter and the score 2880, so everything else
    /// divides by three exactly. A third of a file tick at most, which is
    /// not audible, but a round trip through the file does not land such a
    /// note back where it was written, so it is counted.
    std::size_t rounded_to_file_ticks = 0;

    bool carried_everything() const noexcept
    {
        return unrepresentable.empty() && out_of_range_notes.empty();
    }
};

/// Builds the file image for a score.
///
/// `renderer` is used by value internally, so its own channel setting is not
/// disturbed and each track can be rendered on the channel this asks for.
ScoreMidiResult build_score_midi(const Score& score, const MidiRenderer& renderer,
                                 const ScoreMidiOptions& options = {});

/// Builds and writes it.
///
/// The write result says whether the bytes reached the disk; `built`, when
/// given, says what the export contained and what it could not carry. Both
/// matter: a file can be written successfully and still have left a note out
/// of range behind.
music::SmfWriteResult write_score_midi(const Score& score, const MidiRenderer& renderer,
                                       const std::string& path,
                                       const ScoreMidiOptions& options = {},
                                       ScoreMidiResult* built = nullptr);

}  // namespace vcc::notation
