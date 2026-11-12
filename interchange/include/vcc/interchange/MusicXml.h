// MusicXML 4.0 import and export (uncompressed .musicxml / .xml).
//
// Export writes one <part> per exported track, measure by measure, from the
// track's active version score, laid out on the project's meter map. Import
// reads score-partwise (and score-timewise, converted) into one notation
// Score per part plus the tempo, meter and key changes the project needs.
//
// Model conventions this file relies on (see notation/Score.h):
//
//   * ScoreNote::pitch is the WRITTEN pitch. A staff's
//     `transposition_semitones` (written -> sounding, B-flat clarinet = -2)
//     and its octave clef's `octave_shift` are applied only for playback.
//     MusicXML's <pitch> is also written pitch with <transpose> giving the
//     written -> sounding interval, so a written export copies pitches and
//     emits <transpose>; a concert-pitch export transposes them.
//   * Octave clefs: our pitch is placed on the staff as in the plain clef
//     (a guitar's E4 sits on the bottom line of its treble-8vb staff and
//     sounds E3). MusicXML writes notes under a clef-octave-change at
//     their sounding octave, so export subtracts the clef's octave and
//     import adds it back.
//   * The score's key signature is the CONCERT key (the project key is
//     pushed into every score). Written export transposes it for a
//     transposing part; import converts a part's written key to concert.
//   * Every staff of a score shares one clef; a clef change mid-part is
//     not representable, so import keeps the first clef and warns.
//
// Pickups: a project has no negative bars, so an anacrusis is represented
// as a short first bar whose time signature is its length (a one-beat
// pickup in 4/4 is a bar of 1/4, then 4/4 from bar 2). Export recognises
// that shape (bar 1 shorter than bar 2) and writes it back as an implicit
// measure "0" under bar 2's meter; import does the reverse for an implicit
// first measure whose content is shorter than its time signature.
//
// Never throws: import returns ok=false with a line-numbered error for a
// malformed file, export reports what it could not express as warnings.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "vcc/notation/Score.h"
#include "vcc/project/Project.h"

namespace vcc::interchange {

// --- Export -------------------------------------------------------------------

struct ExportOptions {
    /// Only these tracks, in project order. Empty: every track the project
    /// exports (Project::export_tracks semantics) that holds a score -
    /// instrument, MIDI and opted-in reference tracks; audio, aux, folder
    /// and video tracks have no notation and are skipped.
    std::vector<std::string> track_ids;

    /// True: pitches and keys at sounding pitch, no <transpose>. False
    /// (default): written pitch, as players read it, with <transpose>.
    bool concert_pitch = false;

    /// "YYYY-MM-DD" for <encoding-date>; empty omits the element. Passed in
    /// rather than read from the clock so exports are reproducible.
    std::string encoding_date;

    /// Write a short first bar followed by a longer one as a pickup
    /// (implicit measure 0 under the second bar's meter).
    bool detect_pickup = true;

    /// Per-track details the project alone does not hold (the session fills
    /// them from the instrument registry). Keyed by track id.
    struct PartInfo {
        std::string abbreviation;
        int midi_program = -1;  ///< 0-127 General MIDI program, -1 unknown.
    };
    std::map<std::string, PartInfo> part_info;
};

/// MusicXML 4.0 score-partwise document with the standard DOCTYPE.
std::string export_musicxml(const project::ProjectData& data, const ExportOptions& options = {},
                            std::vector<std::string>* warnings = nullptr);

/// The tracks `export_musicxml` would write, in order.
std::vector<const project::Track*> musicxml_export_tracks(const project::ProjectData& data,
                                                          const ExportOptions& options = {});

// --- Import -------------------------------------------------------------------

struct ImportedPart {
    std::string id;            ///< MusicXML part id ("P1").
    std::string name;          ///< <part-name>, or the id when it has none.
    std::string abbreviation;  ///< <part-abbreviation>.
    int midi_program = -1;     ///< 0-127 (MusicXML's 1-128 minus one), -1 unknown.
    int midi_channel = -1;     ///< 0-15 (MusicXML's 1-16 minus one), -1 unknown.
    bool is_percussion = false;
    /// Staves (clef, name, transposition), notes and markings, on the
    /// meter and keys of the file.
    notation::Score score;
};

struct ImportedTempo {
    Ticks tick = 0;
    double bpm = 120.0;  ///< Quarter notes per minute.
};

struct ImportedTimeSignature {
    Ticks tick = 0;
    int bar = 1;  ///< 1-based bar of the imported layout (a pickup is bar 1).
    int numerator = 4;
    int denominator = 4;
};

struct ImportedKey {
    Ticks tick = 0;
    int fifths = 0;            ///< Concert pitch.
    std::string mode = "major";  ///< MusicXML mode: major, minor, dorian, ...
};

struct ImportResult {
    bool ok = false;
    std::string error;  ///< "line 42: mismatched end tag </note>, expected </pitch>".

    std::string title;     ///< movement-title, else work-title.
    std::string composer;  ///< <creator type="composer">.

    std::vector<ImportedPart> parts;
    std::vector<ImportedTempo> tempos;                  ///< Sorted by tick.
    std::vector<ImportedTimeSignature> time_signatures; ///< First at tick 0.
    std::vector<ImportedKey> keys;                      ///< Of the first part; first at tick 0.

    int measure_count = 0;
    bool has_pickup = false;
    Ticks pickup_ticks = 0;  ///< Length of the pickup bar when `has_pickup`.

    /// What was approximated or left out, counted ("12 lyrics ignored").
    std::vector<std::string> warnings;
};

/// Reads a MusicXML document. Never throws.
///
/// Cue notes are skipped (they belong to another player's part and would
/// otherwise sound); their time is kept. Whole-measure rests are left as
/// empty bars, which is how the score model shows a bar rest; other rests
/// are imported as rests. Invisible rests (print-object="no") are skipped.
ImportResult import_musicxml(const std::string& document);

}  // namespace vcc::interchange
