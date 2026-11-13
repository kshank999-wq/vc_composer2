// Copyright (c) VC Composer. All rights reserved.
//
// Reading MIDI back into notation (spec 02 sections 5.2, 16).
//
// The inverse of `ScoreMidiFile.h`, and much the harder direction. Going out
// is a translation: a written note has one sounding pitch and one exact
// length. Coming in is an *interpretation*, because MIDI has thrown away
// everything notation needs:
//
//   * A sounding pitch does not say how it was written. Middle C sharp and
//     D flat are the same key. `spell_midi_note` picks the spelling the key
//     signature implies, which is the closest a mechanical process gets to
//     what a composer meant.
//   * A length in ticks is not a note value. A performance is never exactly
//     on the grid, so onsets and lengths are quantized - and how far each
//     note had to move is reported, because an import that silently tidied
//     a phrase by a sixteenth is an import nobody should trust.
//   * Nothing in the file says which notes are a chord, which are a tie, or
//     which voice they belong to. Chords and ties are inferred. Voices are
//     inferred only where the file forces them: a note that begins while
//     another still sounds cannot share its voice, so such lines are told
//     apart, the lower taking the down-stem voice; everything else lands
//     in voice 0.
//   * A velocity is not a dynamic. One velocity is a note's own loudness; a
//     dynamic is a level that *holds*. So a level is written where two
//     onsets in a row sit at it, and a single louder or softer note between
//     them - an accent, a brushed key - writes nothing. The score's dynamics
//     are score-wide, so a file with several parts has its levels read from
//     all of them together, and says so.
//
// So this reports what it did, in the same three levels the score reader
// uses: refused, read with warnings, read cleanly. An import a composer
// cannot tell from their own writing is worse than one that says what it
// guessed.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "vcc/music/MidiFile.h"
#include "vcc/notation/Score.h"

namespace vcc::notation {

struct MidiImportOptions {
    /// Smallest note value an onset or a length is snapped to.
    ///
    /// A sixteenth is the usual choice for a recorded performance: fine
    /// enough to keep the rhythm, coarse enough to remove the milliseconds
    /// a player is never exactly on.
    NoteValue grid = NoteValue::Sixteenth;

    /// Also consider the triplet subdivision of the grid, and snap to
    /// whichever is nearer. Without it a triplet passage reads as a
    /// stumbling run of straight notes.
    ///
    /// A note that lands on the triplet grid is written as a triplet
    /// duration, since no number of plain and dotted values adds up to one
    /// (an eighth-note triplet is 320 ticks; a sixteenth and a 64th make
    /// 300). It is not grouped under a bracket: which notes form a tuplet
    /// is a decision about beats and beaming, and this reads one note at a
    /// time.
    bool allow_triplets = true;

    /// Snap lengths as well as onsets. Off, a note keeps the length it was
    /// played for, which almost never notates as anything.
    ///
    /// On, the note's *release* is what gets snapped, which recovers the
    /// written length exactly from a file whose note-offs sit at it - a
    /// piano-roll export, or one of VC Composer's own with no articulation
    /// scaling. It cannot recover the written length from a file that
    /// shortens its notes by a proportion: five per cent of a dotted half
    /// is more than a sixteenth grid can see, so a long note comes back
    /// shorter than it was written. Nothing in a MIDI file says what the
    /// proportion was, so that is a limit of the format rather than of the
    /// quantizer (see docs/aax/17-midi-import.md).
    bool quantize_lengths = true;

    /// Notes this short after quantization are dropped rather than written.
    ///
    /// A two-tick note is a stuck key or a graced release, not notation.
    /// Zero means "drop only what quantizes to nothing".
    Ticks drop_shorter_than = 0;

    /// Spelled against this when the file names no key signature. A file
    /// that names one is spelled against what it says.
    KeySignature default_key{};

    /// Guess a clef from where each part sits, rather than putting a bass
    /// line on a treble staff.
    bool guess_clefs = true;

    /// Read the note velocities as dynamic markings, where a level holds.
    ///
    /// Off, a part comes back with no dynamics and plays at the renderer's
    /// default, which is what a composer who means to write their own
    /// wants. On, the levels are the ones the default expression map
    /// renders the written dynamics at, so a part this application
    /// exported comes back with the dynamics it was written with.
    bool read_dynamics = true;

    /// Read the file's marker and text meta events as text markings, at
    /// their ticks, so a cue's memory locations and section names come
    /// into the part. On by default. A track's own name, copyright and
    /// instrument name are never read as text: they name the track, not
    /// a place in the music.
    bool read_markers = true;

    /// Split a keyboard track into two staves at this MIDI note number -
    /// the notes from it up on the upper staff, the rest on the lower -
    /// or leave every track as one staff when negative. Off by default:
    /// a single-line part with a low note is not a keyboard part, and the
    /// composer is the one who knows which they recorded.
    ///
    /// Only a track with notes on both sides of the point is split; one
    /// that lies wholly above or below it is one staff whatever this says.
    /// Both staves carry the track's name, and each gets the clef its
    /// own notes suggest (or a treble staff, when clefs are not guessed).
    int split_point = kNoSplit;

    static constexpr int kNoSplit = -1;

    /// Put notes the file forces apart into voices of their own.
    ///
    /// Two notes that sound together without starting and ending together
    /// cannot share a voice - a voice is one line, and one line cannot
    /// begin a note while another of its own still sounds. On, each such
    /// passage is split into as many voices as it needs (four at most,
    /// which is what a staff holds; more is counted and said), and the
    /// voices are numbered by pitch over the passage as a whole: the
    /// highest line takes the first voice, with up stems, the next the
    /// second, with down stems. Where nothing overlaps, everything stays
    /// in the first voice, as it always did. Off, everything lands in the
    /// first voice whatever overlaps, and notes starting together are one
    /// chord whatever their lengths.
    bool infer_voices = true;
};

/// What an import produced, and what it could not.
struct MidiImportResult {
    bool ok = false;

    /// Why nothing could be read. Empty when `ok`.
    std::string error;

    /// What was inferred, moved or dropped, each named. An import that
    /// reads cleanly has none.
    std::vector<std::string> warnings;

    std::size_t notes_read = 0;
    std::size_t staves_read = 0;

    /// Tracks split into two staves at `split_point`.
    std::size_t tracks_split = 0;

    /// Tracks with no notes on them: the conductor track, and any empty
    /// one. Their tempo, meter and key still reached the score.
    std::size_t tracks_without_notes = 0;

    std::size_t notes_dropped = 0;

    /// The furthest any note's **onset** was moved by quantization, in
    /// ticks. Reported because it is the number that says whether the
    /// rhythm survived: nothing moved means the file was already notation,
    /// and a move approaching the worst the grid can do means it was played
    /// by hand.
    Ticks largest_move = 0;

    /// The furthest any note's **release** was moved, in ticks. Zero when
    /// `quantize_lengths` is off, since then nothing touches a length.
    ///
    /// Kept apart from `largest_move` because the two mean different
    /// things. An onset that moved is a rhythm that was interpreted. A
    /// release that moved is ordinary: every sequencer and every renderer
    /// writes note-offs short of the written length, so a length always
    /// has to be decided rather than read, and doing so is what
    /// `quantize_lengths` was asked for. It is reported rather than
    /// warned about for that reason - but it is reported, because a part
    /// whose lengths all moved by a lot was written for an articulation
    /// this import knows nothing about.
    Ticks largest_length_move = 0;

    /// Note-ons the file never released. Closed at the end of their track,
    /// which is a guess, and counted so it is not a silent one.
    std::size_t unreleased_notes = 0;

    /// Dynamic markings written from the velocities. Zero when
    /// `read_dynamics` is off.
    std::size_t dynamics_read = 0;

    /// Text markings written from the file's markers and text events.
    /// Zero when `read_markers` is off.
    std::size_t markers_read = 0;

    /// Notes written in a voice other than the first, because they sounded
    /// over or under another line. Zero when `infer_voices` is off, and
    /// zero for a file in which nothing overlaps.
    std::size_t notes_in_other_voices = 0;

    bool carried_everything() const noexcept { return ok && warnings.empty(); }
};

/// Reads a MIDI file into a score, replacing whatever it held.
///
/// One staff per track that has notes on it, named after the track. Tempo,
/// meter and key are taken from wherever in the file they appear - by
/// convention that is the first track, but a file is not obliged to put
/// them there.
///
/// The file's own resolution is rescaled to the engine's `kTicksPerQuarter`
/// (2880). A file at 960 - the host's, and the usual - or at any other
/// divisor of 2880 converts exactly; anything else rounds, and the
/// quantizer then has the last word.
///
/// Notes are written at **sounding** pitch. A part for an instrument that
/// transposes would need to be told which, and nothing in a MIDI file says
/// so; setting the staff's transposition afterwards is the composer's to do.
///
/// `out` is left empty when the read fails, so there is never half a cue to
/// mistake for a whole one.
MidiImportResult import_midi(const music::SmfFile& file, Score& out,
                             const MidiImportOptions& options = {});

/// Snaps a tick to the nearest point on the grid the options allow.
///
/// Exposed because it is the decision that shapes every imported rhythm,
/// and is worth checking on its own. With triplets allowed, the nearer of
/// the straight and triplet candidates wins.
Ticks quantize_tick(Ticks tick, Ticks grid, bool allow_triplets) noexcept;

/// Writes `length` as a tie chain of written values (plain, dotted, and
/// undotted triplets), longest first. Returns how many were written and
/// leaves in `unwritten` whatever no value could reach.
std::size_t notate_length(Ticks length, Duration* out, std::size_t capacity, Ticks& unwritten);

/// The grid's length in ticks, or zero for anything that is not a written
/// note value - including an integer cast into `NoteValue` from outside its
/// range, which is how a bad setting would arrive.
Ticks grid_ticks(NoteValue value) noexcept;

/// The written level nearest a velocity, by the default expression map's
/// own table - the inverse of what the renderer does with a dynamic, so a
/// part exported and imported again keeps its levels. A velocity midway
/// between two levels goes to the quieter.
DynamicLevel dynamic_for_velocity(std::uint8_t velocity) noexcept;

}  // namespace vcc::notation
