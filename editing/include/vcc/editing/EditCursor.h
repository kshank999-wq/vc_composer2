// Copyright (c) VC Composer. All rights reserved.
//
// Where entry happens (spec 02 section 5.2).
//
// Spec 5.2 requires "keyboard-driven notation entry" with a selected
// duration, and "mouse/pointer entry" that places a note of that duration.
// Both need somewhere for the next note to go and something to say what it
// will be, which is what this holds: a position, and the state of the entry
// tools.
//
// It is deliberately a plain value. The editor hands one out, takes one
// back, and the undo history stores a copy either side of every edit - so
// undoing an edit puts the cursor back where it was, which is what makes a
// long undo sequence follow the work rather than jumping around.
#pragma once

#include <cstddef>
#include <cstdint>

#include "vcc/notation/MusicalTime.h"
#include "vcc/notation/Pitch.h"

namespace vcc::editing {

struct EditCursor {
    /// Staff the next note goes on, and the position and voice.
    std::size_t staff_index = 0;
    notation::Ticks tick = 0;
    std::uint8_t voice = 0;

    /// The duration entry uses. Dots and tuplets are part of it, so the
    /// selected value carries everything a written note needs.
    notation::Duration duration = notation::quarter();

    /// Entering rests rather than notes.
    bool rest_mode = false;

    /// Entering grace notes: each pitch typed is a grace note before the
    /// note at the cursor's tick, in order, and the cursor stays put for
    /// the note they ornament. Turned off by hand, like rest entry.
    bool grace_entry = false;

    /// The next note entered is tied into the one after it.
    bool tie_to_next = false;

    /// An accidental chosen before the pitch, as keyboard entry works:
    /// press the accidental, then the note. Cleared once used, because a
    /// sharp pressed once should not sharpen everything typed afterwards.
    bool has_pending_accidental = false;
    std::int8_t pending_alteration = 0;

    /// Octave for entry that names only a step - a letter key, or a pitch
    /// repeated from the previous note.
    std::int8_t entry_octave = 4;

    bool operator==(const EditCursor& other) const noexcept;
    bool operator!=(const EditCursor& other) const noexcept { return !(*this == other); }
};

}  // namespace vcc::editing
