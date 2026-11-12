// How audio tracks look wherever they are drawn (track list, sequencer).
#pragma once

#include "ui/Theme.h"
#include "vcc/project/Project.h"

namespace vcc::desktop {

/// An audio track's colour: its own, or a teal of its own while it still has
/// the neutral default, so recorded audio stands apart from instrument tracks.
inline juce::Colour audio_track_colour(const project::Track& t)
{
    if (t.color.empty() || t.color == "#808080") return juce::Colour(0xff3fb3a6);
    return colour_from_hex(t.color, juce::Colour(0xff3fb3a6));
}

}  // namespace vcc::desktop
