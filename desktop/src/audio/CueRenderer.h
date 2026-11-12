// Render-by-cue (Main Spec §14 "render-by-cue, bounce naming presets and batch
// export"; §16 audio deliverables).
//
// Each cue's music range (plus a release tail) is bounced offline through a
// private playback engine - never the one driving the audio device - to a
// WAV file named by a preset. Temp/reference tracks are not part of the
// mix. An approved or locked cue's existing file is never overwritten unless
// the caller says the composer confirmed it (Main Spec §15).
#pragma once

#include <string>
#include <vector>

#include <juce_core/juce_core.h>

namespace vcc::desktop {

struct AppContext;

struct CueRenderRequest {
    std::vector<std::string> cue_ids;
    juce::File folder;
    std::string name_preset;  ///< Tokens: see vcc::cues::render_name. Empty = default.
    int sample_rate = 48000;
    int bit_depth = 24;       ///< 16 or 24.
    double tail_seconds = 3.0;
    bool overwrite_protected = false;  ///< Composer confirmed overwriting approved/locked deliverables.
};

struct CueRenderResult {
    std::string cue_id;
    juce::File file;
    bool ok = false;
    bool skipped_protected = false;  ///< Not written: would overwrite an approved/locked deliverable.
    juce::String message;
    float peak = 0.0f;               ///< Linear peak of the bounce (silence check, clipping warning).
};

/// Bounces the requested cues (synchronously; call from the message thread).
std::vector<CueRenderResult> render_cues(AppContext& context, const CueRenderRequest& request);

}  // namespace vcc::desktop
