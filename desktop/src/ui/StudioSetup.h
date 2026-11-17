// Studio > Audio Setup... and MIDI Setup...: one window, two pages.
//
// Audio: the audio system (Core Audio on macOS; Windows Audio, DirectSound
// and - where enabled - ASIO on Windows), the interface (Universal Audio,
// MOTU, RME, Focusrite, Antelope... over USB, USB-C, Thunderbolt or AVB), the
// sample rate (44.1, 48, 88.2, 96, 176.4 or 192 kHz, as the interface
// offers them), the buffer size with the latency it gives, the inputs and
// outputs in use, and a test tone. The project's own rate is shown beside it.
//
// MIDI: every MIDI input connected (USB and USB-C keyboards and controllers,
// an interface's MIDI ports, Bluetooth and network sessions), each on or
// off, and whether new ones are turned on as they are plugged in; the MIDI
// outputs MIDI tracks can send to. Choices are remembered (and the interface
// is found again at start-up).
#pragma once

#include "ui/AppContext.h"

namespace vcc::desktop {

class StudioSetup final : public juce::Component {
public:
    explicit StudioSetup(AppContext& context);
    ~StudioSetup() override;
    /// 0 Audio, 1 MIDI.
    void show_page(int page);
    int page() const;
    /// The audio page's one-line summary ("Core Audio: Apollo Twin - 48 kHz - 256 samples (5.3 ms)").
    juce::String summary() const;
    /// The sample rates the interface offers, as the rate keys show them.
    juce::StringArray rate_keys() const;
    /// Presses a rate key (Hz); false when the interface does not offer it.
    bool choose_rate(double hz);
    /// The MIDI inputs listed, and whether each is on.
    std::vector<std::pair<juce::String, bool>> midi_inputs() const;
    void set_midi_input(const juce::String& name, bool on);

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    class AudioPage;
    class MidiPage;
    AppContext& context_;
    juce::TabbedComponent tabs_{juce::TabbedButtonBar::TabsAtTop};
    AudioPage* audio_ = nullptr;
    MidiPage* midi_ = nullptr;
};

/// Opens (or brings forward) the window on `page` (0 Audio, 1 MIDI).
void show_studio_setup(AppContext& context, int page);
StudioSetup* studio_setup();

/// "48 kHz", "44.1 kHz".
juce::String rate_label(double hz);

}  // namespace vcc::desktop
