// The persistent left-hand Track & Library panel (Main Spec §4, §10, §15):
// the same track IDs, order, labels, versions, mute/solo and instrument
// status as every other view, plus the instrument library.
#pragma once

#include "ui/AppContext.h"

namespace vcc::desktop {

class TrackPanel final : public juce::Component {
public:
    explicit TrackPanel(AppContext& context);
    ~TrackPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /// Popup menu of track actions (also used by other views).
    static void show_track_menu(AppContext& context, const std::string& track_id);
    /// Popup menu to add a track from the instrument registry.
    static void show_add_track_menu(AppContext& context, juce::Component* target);

private:
    class TrackList;
    class Library;
    AppContext& context_;
    juce::TabbedComponent tabs_{juce::TabbedButtonBar::TabsAtTop};
    std::unique_ptr<TrackList> tracks_;
    std::unique_ptr<Library> library_;
};

}  // namespace vcc::desktop
