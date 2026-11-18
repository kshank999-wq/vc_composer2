// Studio > Built-in Sounds...: whether the piano and orchestra are installed,
// the download's progress on first launch, and keys to download them again,
// open their folder or read their credits.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace vcc::desktop {

class SoundsPanel final : public juce::Component, private juce::Timer {
public:
    SoundsPanel();
    ~SoundsPanel() override;

    void refresh();
    juce::String status_text() const { return status_.getText(); }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override { refresh(); }

    juce::Label title_, about_, status_;
    double progress_value_ = 0.0;
    juce::ProgressBar progress_{progress_value_};
    juce::TextButton download_{"Download"}, folder_{"Show Folder"}, credits_{"Credits..."};
    int listener_ = 0;
};

/// Shows (or brings forward) Studio > Built-in Sounds.
void show_sounds_window();
SoundsPanel* sounds_panel();

}  // namespace vcc::desktop
