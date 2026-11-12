// Copyright (c) VC Composer. All rights reserved.
//
// A score as text (spec 02 sections 12, 13).
//
// The companion window is persistent across plug-in instances (spec 4.1),
// but a window that keeps a score only while it is running is not
// persistence - close the application and the cue is gone. This is what
// makes it survive.
//
// JSON, using this repository's own minimal reader and writer, for the
// reason everything else here avoids dependencies: the build has to work on
// a machine with no package manager. A text format is also the right choice
// on its own terms - a composer can read it, a diff shows what changed, and
// a future version can be migrated by something other than this program.
//
// This layer is Score to `json::Value` and back, and nothing else: no file,
// no track binding, no version header. Those belong to a *document*, which
// is `app/DocumentFile.h` - the notation engine is host-independent by
// construction (spec 13) and does not know that tracks exist.
//
// THE ONE PROPERTY THAT MATTERS
//
// Event ids survive. Spec 13 requires "stable IDs linking notation events to
// host MIDI events", and a reload that renumbered them would make the diff
// layer see a whole cue as deleted and re-added the first time the composer
// touched it - which is the whole-region rewrite that requirement exists to
// prevent. Notes are restored with the ids they were saved with, and the
// score's id counter comes back ahead of all of them.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "vcc/core/Json.h"
#include "vcc/notation/Score.h"

namespace vcc::notation {

/// What reading a score produced, and what it could not.
///
/// Three outcomes rather than two, because a file can be partly readable and
/// saying so is more useful than either refusing it or silently losing part
/// of a cue:
///
///   * `ok` false - the file's shape is wrong and nothing was read. `error`
///     says what.
///   * `ok` true with warnings - it was read, and something in it was
///     skipped or not understood. Each one is named.
///   * `ok` true with none - everything in the file is in the score.
struct ScoreLoadResult {
    bool ok = false;

    /// Why nothing could be read. Empty when `ok`.
    std::string error;

    /// What was skipped or not understood, in words fit for the UI: a note
    /// with no id, an articulation this build does not know.
    std::vector<std::string> warnings;

    std::size_t notes_read = 0;
    std::size_t markings_read = 0;
    std::size_t staves_read = 0;

    bool carried_everything() const noexcept { return ok && warnings.empty(); }
};

/// Writes a score.
///
/// Every field is written, including ones that hold their default. A file
/// whose shape does not depend on the values in it is one whose round trip
/// can be checked by comparing the notes, not by reasoning about which
/// fields the writer decided to leave out.
///
/// Enumerations are written by name - "treble", "staccato" - never by their
/// number. A file that says `0` stops meaning what it said the moment
/// somebody inserts a value into the enum; a file that says "treble" does
/// not.
json::Value score_to_json(const Score& score);

/// Reads a score, replacing whatever `out` held.
///
/// `out` is cleared first and is left empty when the read fails, so a
/// failed load never leaves half a cue behind for the composer to mistake
/// for the whole one.
ScoreLoadResult score_from_json(const json::Value& value, Score& out);

}  // namespace vcc::notation
