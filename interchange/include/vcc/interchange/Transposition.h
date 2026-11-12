// How a part is shown on paper (Main Spec §16). A track's score keeps each
// note's written pitch and the project's concert key; printing needs either
// the transposed view (written notes under the written key) or the concert
// view (sounding notes under the concert key).
#pragma once

#include "vcc/notation/Score.h"

namespace vcc::interchange {

/// A copy of `score` for printing. `concert` false: key signatures become
/// each staff's written key (a B-flat clarinet in concert F reads G major).
/// `concert` true: notes are moved to sounding pitch, spelled in the concert
/// key, and the staves lose their transposition (octave clefs and octave
/// shifts are kept, as a concert score still writes them). A score whose
/// staves do not transpose is returned unchanged.
notation::Score display_score(const notation::Score& score, bool concert);

/// The written key of a staff transposing by `semitones`, for a concert key.
notation::KeySignature written_key(const notation::KeySignature& concert, int semitones);

}  // namespace vcc::interchange
