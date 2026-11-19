// The persistent left-hand Track & Library panel (Main Spec §4, §10, §15):
// the same track IDs, order, labels, versions, mute/solo and instrument
// status as every other view, plus the instrument library.
#pragma once

#include "ui/AppContext.h"

namespace vcc::desktop {

class LibraryBrowser;

class TrackPanel final : public juce::Component {
public:
    explicit TrackPanel(AppContext& context);
    ~TrackPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /// Popup menu of track actions (also used by other views).
    static void show_track_menu(AppContext& context, const std::string& track_id);
    /// The instruments a track can play: built-in, virtual instruments by
    /// maker, the instrument window, the library, a scan.
    static juce::PopupMenu instrument_menu(AppContext& context, const std::string& track_id);
    /// The instrument range report (Add-On spec §4): the notes outside each
    /// track's range (one track, or every track when `track_id` is empty),
    /// with Move into range. Non-modal; returns the text it shows.
    static juce::String show_range_report(AppContext& context, const std::string& track_id);
    /// The report's text alone.
    static juce::String range_report(AppContext& context, const std::string& track_id);
    /// Popup menu to add a track from the instrument registry.
    static void show_add_track_menu(AppContext& context, juce::Component* target);
    /// Instruments to add to the Sketch area (a piano first), playing straight
    /// to the master, outside the orchestra's groups.
    static juce::PopupMenu sketch_area_menu(AppContext& context);

    /// The sound library (Main Spec §10); it shows in its own window (Window > Library).
    LibraryBrowser& library() noexcept { return *library_; }

private:
    class TrackList;
    AppContext& context_;
    juce::TabbedComponent tabs_{juce::TabbedButtonBar::TabsAtTop};
    std::unique_ptr<TrackList> tracks_;
    std::unique_ptr<LibraryBrowser> library_;
};

}  // namespace vcc::desktop
