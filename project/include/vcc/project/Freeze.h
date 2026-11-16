// Freeze (Memory Management spec §5): a track's printed audio stands in for
// its instrument. A print is only as good as what it was made from, so the
// print records a signature of its inputs and the track reads Stale when
// they change.
#pragma once

#include <string>

#include "vcc/project/Project.h"

namespace vcc::project {

/// A digest of everything that shapes what a track's instrument plays: the
/// active version's score, regions and performance tweaks, the articulation
/// and percussion maps, transposition, the instrument plug-in and its
/// settings, the track's automation, the tempo map and meter, and - when the
/// print includes them - the channel strip and inserts.
std::string render_signature(const ProjectData& d, const Track& t);

enum class FreezeState {
    Live,    ///< The instrument plays (no print, or the print is not what plays).
    Frozen,  ///< The print plays and matches what it was made from.
    Stale,   ///< The print plays, but the music or instrument changed since.
};
FreezeState freeze_state(const ProjectData& d, const Track& t);
const char* to_string(FreezeState s) noexcept;

}  // namespace vcc::project
