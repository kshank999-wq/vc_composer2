// The Master panel (docs/spec/ui, "Mix and master"), shown in the right
// column at the Master stage:
// - Live loudness (integrated LUFS and true peak, plus momentary and
//   short-term) of the master as it plays.
// - The loudness target, with how far the mix is from it; Analyse renders the
//   song offline and Match sets the master fader to hit the target.
// - The master chain: EQ, glue compressor and limiter slots on the master
//   bus.
// - Export master: WAV or FLAC, 16 or 24 bit (dither on 16), 44.1, 48 or
//   96 kHz, song or selection. MP3 waits for the encoder decision.
#pragma once

#include <memory>
#include <vector>

#include "audio/MasterExport.h"
#include "ui/AppContext.h"
#include "ui/hw/HwControls.h"
#include "vcc/audio/Loudness.h"

namespace vcc::desktop::shell {

class MasterPanel final : public hw::Panel, private juce::Timer {
public:
    explicit MasterPanel(AppContext& context);
    ~MasterPanel() override;

    // Live loudness.
    void add_blocks(const std::vector<audio::LoudnessBlock>& blocks);
    void reset_meter();
    const audio::LoudnessMeter& meter() const noexcept { return meter_; }
    const hw::Lcd& integrated_lcd() const noexcept { return integrated_; }
    const hw::Lcd& true_peak_lcd() const noexcept { return true_peak_; }
    const hw::Lcd& deviation_lcd() const noexcept { return deviation_; }

    // Target, analysis and match.
    hw::Keycap& target_key() { return target_; }
    void analyse();
    void match_target();
    const MasterExportResult& last_analysis() const noexcept { return analysis_; }

    // The master chain: 0 EQ, 1 glue compressor, 2 limiter.
    hw::Keycap& chain_key(int i) { return *chain_[static_cast<std::size_t>(i)]; }
    /// The master bus's insert index for a chain slot, or -1.
    int chain_index(int i) const;

    // Export.
    hw::Keycap& format_key(int i) { return *formats_[static_cast<std::size_t>(i)]; }  ///< WAV, FLAC, MP3.
    hw::Keycap& bits_key(int i) { return *bits_[static_cast<std::size_t>(i)]; }      ///< 16, 24.
    hw::Keycap& rate_key(int i) { return *rates_[static_cast<std::size_t>(i)]; }     ///< 44.1, 48, 96.
    hw::Keycap& dither_key() { return dither_; }
    hw::Keycap& selection_key() { return selection_; }
    hw::Keycap& export_key() { return export_; }
    /// Exports to `file` with the panel's settings (Export master asks for the file).
    void export_to(const juce::File& file);
    /// Runs the job in progress to the end (the checks).
    void finish_now();
    bool busy() const noexcept { return job_ != nullptr; }
    const MasterExportResult& last_export() const noexcept { return exported_; }
    const hw::Lcd& progress_lcd() const noexcept { return progress_; }

    void resized() override;
    void paintOverChildren(juce::Graphics& g) override;

private:
    void timerCallback() override;
    void refresh();
    void show_readings();
    void start(MasterExportRequest rq, bool analysis);
    void job_done();
    std::string master_id() const;
    MasterExportRequest request_for(const juce::File& file) const;

    AppContext& context_;
    int listener_ = 0;
    audio::LoudnessMeter meter_;
    bool was_playing_ = false;
    hw::Lcd integrated_{"-", 26.0f}, momentary_{"-", 11.0f}, short_term_{"-", 11.0f}, true_peak_{"-", 11.0f},
        deviation_{"-", 11.0f}, progress_{"READY", 10.5f};
    hw::Keycap target_{"Target"}, reset_{"Reset"}, analyse_{"Analyse"}, match_{"Match"};
    std::vector<std::unique_ptr<hw::Keycap>> chain_, formats_, bits_, rates_;
    hw::Keycap dither_{"Dither", true}, song_{"Song", true}, selection_{"Selection", true}, export_{"Export master"};
    std::unique_ptr<MasterExportJob> job_;
    bool job_is_analysis_ = false;
    MasterExportResult analysis_, exported_;
    std::unique_ptr<juce::FileChooser> chooser_;
};

}  // namespace vcc::desktop::shell
