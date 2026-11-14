// Print score to audio (docs/spec/ui, "Print score to audio"), in the
// Producer view's right column: tick the tracks, choose the range and
// whether effects are printed, press the red key; a progress LCD follows the
// print, track by track. Each print lands on the track's own audio lane.
#pragma once

#include <memory>
#include <set>
#include <string>

#include "audio/PrintToAudio.h"
#include "ui/AppContext.h"
#include "ui/hw/HwControls.h"

namespace vcc::desktop::shell {

class PrintPanel final : public hw::Panel, private juce::Timer {
public:
    explicit PrintPanel(AppContext& context);
    ~PrintPanel() override;

    /// Starts printing these tracks (the header's Print and Freeze keys use
    /// this with one track; Freeze prints with effects).
    void print_tracks(const std::vector<std::string>& tracks, bool include_fx);
    /// Starts printing the ticked tracks with the panel's settings (the red key).
    void start();
    void cancel();
    bool printing() const noexcept { return job_ != nullptr || live_; }
    /// Offline (rendered, faster than real time) or Real-time (played live
    /// and recorded: the way to print external MIDI hardware).
    hw::Keycap& offline_key() { return offline_; }
    hw::Keycap& realtime_key() { return realtime_; }
    /// A real-time print's results arrived (the engine calls this when the range has played).
    void live_finished(const std::vector<PlaybackEngine::LivePrintResult>& results, bool cancelled);
    /// Runs the print in progress to the end (the checks).
    void finish_now();

    void set_ticked(const std::string& track_id, bool on);
    bool ticked(const std::string& track_id) const { return ticked_.count(track_id) > 0; }
    hw::Keycap& selection_key() { return selection_; }
    hw::Keycap& song_key() { return song_; }
    hw::Keycap& fx_key() { return fx_; }
    hw::Keycap& play_audio_key() { return play_audio_; }
    hw::Keycap& record_key() { return record_; }
    const hw::Lcd& progress_lcd() const noexcept { return lcd_; }
    const std::vector<PrintItem>& last_items() const noexcept { return last_; }

    void resized() override;
    void paintOverChildren(juce::Graphics& g) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;

private:
    void timerCallback() override;
    void refresh();
    void finished();
    juce::Rectangle<int> list_area() const;

    AppContext& context_;
    int listener_ = 0;
    struct Row {
        std::string id;
        juce::String name;
        juce::Colour colour;
    };
    std::vector<Row> rows_;
    std::set<std::string> ticked_;
    int scroll_ = 0;
    hw::Keycap selection_{"Selection", true}, song_{"Song", true}, fx_{"Include FX", true}, play_audio_{"Play audio", true},
        record_{"Record score to audio"}, offline_{"Offline", true}, realtime_{"Real-time", true};
    bool live_ = false, live_fx_ = false, live_play_ = true;
    Ticks live_start_ = 0;
    hw::Lcd lcd_{"READY", 11.0f};
    std::unique_ptr<PrintJob> job_;
    std::vector<PrintItem> last_;
};

}  // namespace vcc::desktop::shell
