// Top toolbar (Main Spec §4): transport, position in bars and timecode,
// tempo, meter, key, metronome, and the composer's tools.
#pragma once

#include "ui/AppContext.h"

namespace vcc::desktop {

class TransportBar final : public juce::Component {
public:
    explicit TransportBar(AppContext& context);
    ~TransportBar() override;

    /// Refreshes the position display and play button (called from a timer).
    void update_position();
    /// Refreshes tempo/meter/key from the document.
    void update_from_document();

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    AppContext& context_;
    int listener_ = 0;
    juce::TextButton rewind_{juce::String::fromUTF8("\xe2\x8f\xae")};
    juce::TextButton play_{"Play"};
    juce::TextButton click_{"Click"};
    juce::Label bars_, timecode_;
    juce::Label tempo_label_{{}, "Tempo"}, meter_label_{{}, "Meter"}, key_label_{{}, "Key"};
    juce::Label tempo_;
    juce::ComboBox meter_, key_;
    juce::TextButton composer_{"Score"}, sequencer_{"Sequencer"}, mixer_{"Mixer"};
    juce::TextButton chords_{"Chords"}, ostinato_{"Ostinato"};
};

}  // namespace vcc::desktop
