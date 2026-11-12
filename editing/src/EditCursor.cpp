#include "vcc/editing/EditCursor.h"

namespace vcc::editing {

bool EditCursor::operator==(const EditCursor& other) const noexcept
{
    return staff_index == other.staff_index && tick == other.tick && voice == other.voice
        && duration == other.duration && rest_mode == other.rest_mode
        && grace_entry == other.grace_entry
        && tie_to_next == other.tie_to_next
        && has_pending_accidental == other.has_pending_accidental
        && pending_alteration == other.pending_alteration
        && entry_octave == other.entry_octave;
}

}  // namespace vcc::editing
