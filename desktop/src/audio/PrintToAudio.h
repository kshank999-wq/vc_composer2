// Print to audio (docs/spec/ui, "Print score to audio"): each chosen
// instrument or MIDI track's sound, rendered offline from its MIDI and
// written as a 48 kHz / 24-bit WAV onto the track's own audio lane.
//
// Each track renders through its own private engine with a print tap (only
// that track, its instrument's signal, before the channel strip - or after
// the strip and inserts with effects), so a print never carries the
// fader, pan, sends or anything else in the mix, and the clip plays back
// through the same strip as the MIDI did. All the prints land as one undo
// step; with "play printed audio" the tracks then play their audio lanes.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>

#include "vcc/core/Time.h"

namespace vcc::desktop {

struct AppContext;

struct PrintRequest {
    std::vector<std::string> tracks;  ///< Instrument or MIDI tracks, in order.
    Ticks start = 0, end = 0;         ///< The range: the selection, or the song.
    /// A selection print keeps at least the range; a song print ends where the sound does.
    bool whole_range = false;
    bool include_fx = false;          ///< With the channel strip and inserts (bypassed afterwards while the audio plays).
    bool play_audio = true;           ///< The tracks play their printed audio when done.
    double tail_seconds = 2.0;        ///< Kept after the range for release tails (trailing silence trimmed).
    int sample_rate = 48000;
    int bit_depth = 24;
};

struct PrintItem {
    std::string track_id;
    juce::String track_name;
    juce::File file;
    std::string clip_id;
    bool ok = false;
    float peak = 0.0f;
    double seconds = 0.0;  ///< The clip's length: the range, and the sound ringing past it.
    juce::String message;
};

/// One print, a slice at a time, so a window can show its progress.
class PrintJob {
public:
    PrintJob(AppContext& context, PrintRequest request);
    ~PrintJob();

    /// Renders for about `budget_ms`. False once every track is printed (or
    /// it was cancelled) and the clips are placed.
    bool step(double budget_ms = 40.0);
    void cancel() { cancelled_ = true; }
    bool cancelled() const noexcept { return cancelled_; }
    bool finished() const noexcept { return finished_; }

    /// 0..1 across every track.
    double progress() const;
    /// For the progress LCD: "CELLI  2 / 3  BAR 14".
    juce::String status() const;
    int current_index() const noexcept { return index_; }
    int track_count() const noexcept { return static_cast<int>(request_.tracks.size()); }
    const std::vector<PrintItem>& items() const noexcept { return items_; }
    /// What happened, for the status strip.
    juce::String summary() const;

private:
    struct Render;
    bool begin_track();
    void finish_track();
    void place();

    AppContext& context_;
    PrintRequest request_;
    std::vector<PrintItem> items_;
    std::unique_ptr<Render> render_;
    int index_ = 0;
    bool cancelled_ = false, finished_ = false;
};

/// Prints synchronously (the checks, and menu commands that need no window).
std::vector<PrintItem> print_to_audio(AppContext& context, const PrintRequest& request);

/// The file a print of `track_name` goes to: "<Track>_PRINT_01.wav" in the
/// project's Audio Files folder, numbered past those already there.
juce::File print_file_for(const AppContext& context, const juce::String& track_name);

}  // namespace vcc::desktop
