// Export master (docs/spec/ui, "Mix and master"): the whole mix - every
// track, bus and the master chain, exactly as it plays - rendered offline to
// one WAV or FLAC file, with TPDF dither when it is 16-bit, and measured as
// it is written (integrated loudness and true peak, BS.1770). MP3 waits for
// the encoder decision. The same render without a file is the panel's
// Analyse.
#pragma once

#include <functional>
#include <memory>

#include <juce_core/juce_core.h>

#include "vcc/audio/Loudness.h"
#include "vcc/core/Time.h"

namespace juce {
class AudioFormatWriter;
}

namespace vcc::desktop {

struct AppContext;
class PlaybackEngine;

struct MasterExportRequest {
    enum class Format { Wav, Flac };
    juce::File file;  ///< Empty: analyse only.
    Format format = Format::Wav;
    int sample_rate = 48000;
    int bit_depth = 24;  ///< 16 or 24.
    bool dither = true;  ///< TPDF dither, applied at 16-bit only.
    Ticks start = 0, end = 0;
    double tail_seconds = 3.0;
};

struct MasterExportResult {
    bool ok = false;
    bool cancelled = false;
    juce::String message;
    double integrated = audio::LoudnessMeter::kSilence;  ///< LUFS.
    double true_peak = audio::LoudnessMeter::kSilence;   ///< dBTP.
    double sample_peak = audio::LoudnessMeter::kSilence;  ///< dBFS.
    double max_momentary = audio::LoudnessMeter::kSilence;
    double seconds = 0.0;
};

/// One export (or analysis), a slice at a time.
class MasterExportJob {
public:
    MasterExportJob(AppContext& context, MasterExportRequest request);
    ~MasterExportJob();
    /// Renders for about `budget_ms`; false once done.
    bool step(double budget_ms = 40.0);
    void cancel() { cancelled_ = true; }
    double progress() const;
    bool finished() const noexcept { return finished_; }
    const MasterExportResult& result() const noexcept { return result_; }
    const MasterExportRequest& request() const noexcept { return request_; }

private:
    bool begin();
    void finish();

    AppContext& context_;
    MasterExportRequest request_;
    MasterExportResult result_;
    std::unique_ptr<PlaybackEngine> engine_;
    std::unique_ptr<juce::AudioFormatWriter> writer_;
    juce::File temp_;
    audio::LoudnessAnalyser analyser_;
    audio::LoudnessMeter meter_;
    juce::Random random_;
    std::int64_t samples_ = 0, written_ = 0;
    bool started_ = false, finished_ = false, cancelled_ = false;
};

/// Runs a job to the end (the checks, and quick analyses).
MasterExportResult export_master(AppContext& context, const MasterExportRequest& request);

}  // namespace vcc::desktop
