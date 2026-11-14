// Consolidate (docs/05, step 11): clips on one lane of a track rendered
// into one new 24-bit WAV - each with its gain and fades, gaps silent -
// which replaces them as one undo step. The source files are never
// changed, so Undo brings the clips back exactly.
#pragma once

#include <string>
#include <vector>

#include <juce_core/juce_core.h>

namespace vcc::desktop {

struct AppContext;

struct ConsolidateResult {
    bool ok = false;
    std::string clip_id;  ///< The new clip.
    juce::File file;
    juce::String message;  ///< For the status strip.
};

/// Renders `clip_ids` (on one lane of `track_id`) into
/// "<Track>_CONSOL_NN.wav" in the project's Audio Files folder and puts
/// one clip in their place.
ConsolidateResult consolidate_clips(AppContext& context, const std::string& track_id,
                                    const std::vector<std::string>& clip_ids);

/// The clips a "Consolidate" on `clip_id` takes: the clip and every clip on
/// its lane chained to it by touching or overlapping.
std::vector<std::string> touching_clips(const AppContext& context, const std::string& track_id,
                                        const std::string& clip_id);

}  // namespace vcc::desktop
