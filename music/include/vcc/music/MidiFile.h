// Copyright (c) VC Composer. All rights reserved.
//
// Standard MIDI File writing (spec 01 section 7).
//
// Every DAW scripting API that can place generated material does so by
// file, and VC Composer has to be able to write a standard MIDI file anyway
// for its export and drag-and-drop fallback path. One mechanism serves both:
// the PTSL client's `import_midi` takes a path, and this is what puts a file
// at that path.
//
// Written here rather than taken from a library for the reason the rest of
// this repository has no third-party dependencies: the build has to work on
// a machine with no package manager and no network, the signing machine
// included. The format is small, completely specified, and thirty years
// stable, so there is little to gain by depending on someone else's copy of
// it.
//
// Nothing here is real-time safe, and nothing here is meant to be: writing a
// file allocates and touches the disk. It runs on a UI or worker thread, as
// the rest of the export path does.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vcc/music/MidiEvent.h"
#include "vcc/music/MusicalTime.h"

namespace vcc::music {

/// The greatest delta time a variable-length quantity can carry. The format
/// allows four bytes of seven bits each.
constexpr std::int64_t kSmfMaxDeltaTicks = 0x0FFFFFFF;

/// The greatest division (ticks per quarter note) an SMF header can carry:
/// the field is 16 bits with the top bit reserved for SMPTE division.
constexpr std::uint16_t kSmfMaxDivision = 0x7FFF;

/// Meta-event type bytes this writer produces. The format defines more; only
/// what VC Composer has a use for is named, because a named constant nobody
/// writes is a constant nobody has checked.
namespace smf_meta {
constexpr std::uint8_t kText = 0x01;
constexpr std::uint8_t kCopyright = 0x02;
constexpr std::uint8_t kTrackName = 0x03;
constexpr std::uint8_t kInstrumentName = 0x04;
constexpr std::uint8_t kMarker = 0x06;
constexpr std::uint8_t kEndOfTrack = 0x2F;
constexpr std::uint8_t kTempo = 0x51;
constexpr std::uint8_t kTimeSignature = 0x58;
constexpr std::uint8_t kKeySignature = 0x59;
}  // namespace smf_meta

enum class SmfFormat : std::uint8_t {
    /// One track holding everything. Every reader understands it, but the
    /// tempo map and the notes share a track.
    SingleTrack = 0,
    /// Several tracks on one timeline, the first conventionally carrying the
    /// tempo map and nothing else. This is what a DAW expects.
    MultiTrack = 1,
};

/// A channel-voice event at a musical position.
struct SmfEvent {
    /// Absolute position in the file's own ticks, not a delta. Deltas are
    /// the file format's business and are computed when writing, so a
    /// caller never has to keep a running total - which is the single
    /// easiest thing to get wrong about this format.
    std::int64_t tick = 0;
    MidiEvent event{};
};

/// A meta event at a musical position.
///
/// Kept as a type byte and a payload rather than a closed enum, because the
/// meta-event space is open-ended and a writer that only understands the
/// events it was born knowing is a writer that needs editing to add a
/// marker. The helper functions below build the ones VC Composer uses.
struct SmfMetaEvent {
    std::int64_t tick = 0;
    std::uint8_t type = smf_meta::kText;
    std::vector<std::uint8_t> data;
};

/// Sets the tempo from `tick` onward, in quarter notes per minute.
///
/// The file stores microseconds per quarter note, so the conversion is done
/// here and rounded once. `quarter_notes_per_minute` must be positive and
/// must land inside the 24-bit field; `ok` is false otherwise, and the
/// writer refuses the event rather than writing a tempo nobody asked for.
SmfMetaEvent smf_tempo(std::int64_t tick, double quarter_notes_per_minute, bool& ok);

/// As above, for a caller that has already checked the tempo. An unusable
/// tempo yields an event the writer will refuse.
SmfMetaEvent smf_tempo(std::int64_t tick, double quarter_notes_per_minute);

/// A meter change.
///
/// The denominator must be a power of two, because the file stores its
/// base-two logarithm. 7/8 and 5/4 are representable; a notional 4/6 is not,
/// and `ok` says so rather than the writer inventing something close.
SmfMetaEvent smf_time_signature(std::int64_t tick, const TimeSignature& time_signature,
                                bool& ok);
SmfMetaEvent smf_time_signature(std::int64_t tick, const TimeSignature& time_signature);

/// A key change. `fifths` is -7..7, negative for flats.
SmfMetaEvent smf_key_signature(std::int64_t tick, int fifths, bool is_minor, bool& ok);
SmfMetaEvent smf_key_signature(std::int64_t tick, int fifths, bool is_minor);

/// Text-carrying meta events. The payload is the string's bytes as given:
/// meta events are length-prefixed, so no byte needs escaping and UTF-8
/// survives intact.
SmfMetaEvent smf_text(std::int64_t tick, std::uint8_t type, const std::string& text);
SmfMetaEvent smf_track_name(const std::string& name);
SmfMetaEvent smf_marker(std::int64_t tick, const std::string& text);

/// True when a meta event's payload length is what its type requires.
///
/// Unknown types are accepted with any payload, since the meta space is
/// open-ended and this writer is not the authority on it. The types it does
/// know are checked, so a tempo event built from an unusable tempo is
/// refused rather than written as three bytes of nothing.
bool is_valid(const SmfMetaEvent& meta) noexcept;

/// One track chunk.
struct SmfTrack {
    /// Written as a track-name meta event at tick 0 when not empty. A
    /// convenience over pushing one onto `meta` by hand, since every track
    /// wants one and a DAW shows it as the imported track's name.
    std::string name;

    std::vector<SmfEvent> events;
    std::vector<SmfMetaEvent> meta;

    bool is_empty() const noexcept { return events.empty() && meta.empty(); }
};

/// A whole file, before it is bytes.
struct SmfFile {
    SmfFormat format = SmfFormat::MultiTrack;

    /// Ticks per quarter note. Defaults to the notation engine's own
    /// resolution so that an export is exact: no tick is ever rescaled, and
    /// a dotted triplet sixteenth lands where it was written.
    std::uint16_t ticks_per_quarter = 960;

    std::vector<SmfTrack> tracks;

    std::size_t event_count() const noexcept;
};

/// What writing produced, or why it did not.
///
/// Refusals are ordinary outcomes, not exceptions: a score with a tempo of
/// zero in it is a thing that can happen, and the caller wants to be told
/// which event was wrong.
struct SmfWriteResult {
    bool ok = false;

    /// Non-sensitive explanation for the UI and the logs. Never a path,
    /// which is why writing to a file reports the failure without echoing
    /// the name back (spec 01 sections 9, 10).
    std::string error;

    std::size_t byte_count = 0;
    std::size_t track_count = 0;

    /// Channel-voice and meta events actually written.
    std::size_t event_count = 0;

    /// Events left out because they were not valid MIDI, or because a meta
    /// event carried an unusable payload. Counted rather than passed over,
    /// so a caller can say "12 of your notes were unplayable" instead of
    /// producing a file that is quietly short.
    std::size_t dropped_events = 0;
};

/// Serializes a file into `out`, which is cleared first.
SmfWriteResult write_smf(const SmfFile& file, std::vector<std::uint8_t>& out);

/// Serializes and writes to `path`.
///
/// The file is written whole or not at all: it is built in memory first, so
/// a refusal never leaves a truncated file behind for Pro Tools to import.
SmfWriteResult write_smf_file(const SmfFile& file, const std::string& path);

/// What reading a file produced, or why it did not.
struct SmfReadResult {
    bool ok = false;

    /// Why nothing could be read. Never a path (spec 01 sections 9, 10).
    std::string error;

    std::size_t byte_count = 0;
    std::size_t track_count = 0;
    std::size_t event_count = 0;

    /// Bytes the reader understood but has nowhere to put: system-exclusive
    /// blocks, and chunks that are not MThd or MTrk. Counted rather than
    /// passed over, so a file that carries more than this can hold says so.
    std::size_t skipped_events = 0;

    int format = -1;
};

/// Parses a standard MIDI file.
///
/// Accepts what this writer never produces. **Running status** in
/// particular: real files use it everywhere, and a reader that rejected it
/// would reject most of the files a composer owns. The writer's refusal to
/// emit it and the reader's obligation to accept it are the same decision
/// seen from two sides - be strict in what you send.
///
/// Refuses what cannot be carried into musical time rather than guessing:
/// an SMPTE division, which measures in frames rather than beats, and
/// format 2, whose tracks are independent sequences and not parts of one
/// piece.
///
/// Meta events this library does not interpret are kept, so reading and
/// writing a file does not quietly discard what was in it. The end-of-track
/// event is the exception: the writer owns it, so the reader consumes it.
SmfReadResult read_smf(const std::vector<std::uint8_t>& bytes, SmfFile& out);

/// Reads from `path`.
SmfReadResult read_smf_file(const std::string& path, SmfFile& out);

// ---------------------------------------------------------------------------
// The pieces, exposed because they are worth testing on their own
// ---------------------------------------------------------------------------

/// Appends a variable-length quantity, the format's own integer encoding:
/// seven bits per byte, high bit set on every byte but the last.
///
/// Returns false for a negative value or one above `kSmfMaxDeltaTicks`,
/// which is a delta the format cannot express.
bool append_variable_length(std::int64_t value, std::vector<std::uint8_t>& out);

/// Reads one back. Returns false when the bytes run out or the quantity is
/// longer than the four bytes the format allows. Present so the writer's
/// own encoding can be checked against a reader rather than against a table
/// of expected bytes written by the same hand.
bool read_variable_length(const std::uint8_t* bytes, std::size_t length, std::size_t& at,
                          std::int64_t& out);

/// The base-two logarithm of a time-signature denominator, or -1 when it is
/// not a power of two.
int smf_denominator_exponent(int denominator) noexcept;

}  // namespace vcc::music
