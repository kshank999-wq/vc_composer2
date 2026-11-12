// Stem export (Main Spec §16 audio deliverables): the mix and each stem
// printed to its own WAV, for the whole project, a range, or every cue.
//
// Each file is bounced offline through its own private playback engine (so
// no reverb tail or plug-in state carries from one print into the next),
// with the stem's sources soloed by a render-only override: the full mix
// path - strip processing, inserts, sends, automation, delay compensation,
// master bus - prints exactly as it plays, and the document is never
// touched. The stems are summed as they are printed and compared with the
// mix, so a deliverable that would not add back up is reported, not shipped
// silently.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>

#include "vcc/core/Time.h"

namespace vcc::desktop {

struct AppContext;

struct StemRange {
    std::string cue_id;  ///< Empty: a project range (named by `label`).
    std::string label;   ///< "1M1", "Full", "Bars 9-24".
    Ticks start = 0;
    Ticks end = 0;
};

struct StemRenderRequest {
    std::vector<std::string> stem_ids;  ///< Empty: every stem (Session::stems()).
    bool include_mix = true;
    std::vector<StemRange> ranges;
    juce::File folder;
    /// Tokens of vcc::cues::render_name plus {stem}. A project range stands
    /// in as a cue whose code and title are the range's label.
    std::string name_preset = "{production}_{code}_{stem}_v{rev}";
    int sample_rate = 48000;
    int bit_depth = 24;  ///< 16 or 24.
    double tail_seconds = 3.0;
    bool overwrite_protected = false;  ///< Composer confirmed replacing approved/locked deliverables.
    bool mark_deliverables = true;     ///< Tick a cue's "Stems" deliverable when all its files printed.
    /// Called between files with progress 0..1; return false to stop.
    std::function<bool(double progress, const juce::String& now)> progress;
};

struct StemFileResult {
    std::string range_label;
    std::string stem;  ///< "Mix" for the full mix.
    juce::File file;
    bool ok = false;
    bool skipped_protected = false;
    float peak = 0.0f;
    juce::String message;
};

struct StemRangeSummary {
    std::string label;
    bool sum_checked = false;
    /// The stems' sum against the mix: residual level relative to the mix,
    /// in dB (-inf for a perfect null). Below about -60 dB they add back up.
    double sum_error_db = 0.0;
};

struct StemRenderResult {
    std::vector<StemFileResult> files;
    std::vector<StemRangeSummary> ranges;
    std::vector<juce::String> warnings;  ///< Coverage and master-bus notes, before rendering.
    bool cancelled = false;
};

/// Renders synchronously on the message thread.
StemRenderResult render_stems(AppContext& context, const StemRenderRequest& request);

/// The ranges for the common choices: the whole project (bar 1 to the end of
/// the music), and every cue.
StemRange whole_project_range(const AppContext& context);
std::vector<StemRange> cue_ranges(const AppContext& context, const std::vector<std::string>& cue_ids = {});

}  // namespace vcc::desktop
