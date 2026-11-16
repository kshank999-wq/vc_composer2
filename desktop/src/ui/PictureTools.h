// Scoring-to-picture tools (Main Spec §13):
//
//   - Transport > Tempo Finder...: from a start point, the steady tempos at
//     which the chosen hit points fall on the beat (a quarter, an eighth...)
//     within a tolerance in frames, best first; Use applies one from the
//     start through the last hit (the tempo after it kept), one undo step.
//   - File > Export Picture with Score...: the active cut with the stereo
//     score mix, for director review - timecode and bar burned in, streamers
//     and punches at the hits, a watermark - as .mp4 or .mov with a timecode
//     track. Only LGPL encoders (vcc/media/VideoEncoder.h).
//   - The transport's sub counter cycles min:sec, timecode and 35mm feet+frames.
#pragma once

#include <functional>
#include <vector>

#include "ui/AppContext.h"
#include "vcc/picture/Picture.h"

namespace vcc::desktop {

class TempoFinder final : public juce::Component {
public:
    explicit TempoFinder(AppContext& context);
    ~TempoFinder() override;

    /// Where the steady tempo starts (the selection's start, else the playhead).
    void set_start(Ticks tick);
    Ticks start() const noexcept { return start_; }
    /// The hit markers after the start, each on or off.
    void refresh();
    int hit_count() const noexcept { return static_cast<int>(hits_.size()); }
    void set_hit_enabled(int index, bool on);
    void set_range(double min_bpm, double max_bpm);
    /// Ticks a hit must land on (kTicksPerQuarter, kTicksPerQuarter / 2...).
    void set_grid(Ticks grid);
    void set_tolerance(double frames);
    /// Runs the search; the results, best first.
    const std::vector<picture::TempoCandidate>& find();
    const std::vector<picture::TempoCandidate>& results() const noexcept { return results_; }
    /// Applies result `index`; what happened.
    juce::String use(int index);

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    picture::TempoSearch search() const;
    void show_results();

    AppContext& context_;
    Ticks start_ = 0;
    struct Hit {
        std::string id;
        double seconds = 0.0;
        std::unique_ptr<juce::ToggleButton> toggle;
    };
    std::vector<Hit> hits_;
    std::vector<picture::TempoCandidate> results_;
    juce::Label title_, start_label_, results_label_, min_label_, max_label_, grid_label_, tol_label_;
    juce::Slider min_bpm_, max_bpm_, tolerance_;
    juce::ComboBox grid_;
    juce::TextButton find_{"Find Tempos"}, from_playhead_{"Start at Playhead"};
    std::vector<std::unique_ptr<juce::TextButton>> use_;
    std::vector<std::unique_ptr<juce::Label>> rows_;
};

void show_tempo_finder(AppContext& context);
TempoFinder* tempo_finder();

struct PictureExportRequest {
    juce::File file;              ///< .mp4 or .mov.
    double start_seconds = 0.0;   ///< Project time.
    double end_seconds = 0.0;
    bool burn_timecode = true;    ///< Timecode and bar|beat.
    bool burn_hits = true;        ///< Streamers into hits and a punch on them.
    juce::String watermark = "DRAFT - FOR REVIEW ONLY";
    int max_width = 1280;         ///< 0: the picture's own size.
    int sample_rate = 48000;
    /// Called once a frame with progress 0..1; return false to stop.
    std::function<bool(double)> progress;
};

struct PictureExportResult {
    bool ok = false;
    bool cancelled = false;
    juce::String message;
    std::int64_t frames = 0;
    juce::String codec;
    float peak = 0.0f;  ///< Of the score mix.
};

/// The whole active cut, in project time.
std::pair<double, double> picture_range(const AppContext& context);
/// Renders synchronously on the message thread.
PictureExportResult export_picture(AppContext& context, const PictureExportRequest& request);
/// File > Export Picture with Score...: asks for the range and overlays, then a file.
void show_picture_export(AppContext& context);

}  // namespace vcc::desktop
