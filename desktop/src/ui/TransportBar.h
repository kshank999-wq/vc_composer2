// Top toolbar (Main Spec §4): transport (with record and punch, §11),
// position in bars and timecode, tempo, meter, key, metronome, and the
// composer's tools.
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
    /// Shows whether punch in/out is on (the main component owns the range).
    void set_punch_state(bool on);

    /// The round record button: dark when off, lit red when record-enabled,
    /// pulsing while a pass is actually being recorded.
    class RecordButton final : public juce::Button {
    public:
        RecordButton() : juce::Button("Record") {}
        void set_state(bool enabled, bool recording);
        bool lit() const noexcept { return enabled_; }
        bool recording() const noexcept { return recording_; }
        void paintButton(juce::Graphics& g, bool highlighted, bool down) override;

    private:
        bool enabled_ = false, recording_ = false;
    };

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    AppContext& context_;
    int listener_ = 0;
    juce::TextButton rewind_{juce::String::fromUTF8("\xe2\x8f\xae")};
    juce::TextButton play_{"Play"};
    RecordButton record_;
    juce::TextButton punch_{"Punch"};
    juce::TextButton click_{"Click"};
    juce::Label bars_, timecode_;
    juce::Label rec_label_{{}, "Rec"}, rec_time_;
    juce::Label tempo_label_{{}, "Tempo"}, meter_label_{{}, "Meter"}, key_label_{{}, "Key"};
    juce::Label tempo_;
    juce::ComboBox meter_, key_;
    juce::TextButton composer_{"Score"}, sequencer_{"Sequencer"}, mixer_{"Mixer"}, picture_{"Picture"}, cues_{"Cues"};
    juce::TextButton chords_{"Chords"}, ostinato_{"Ostinato"};
};

}  // namespace vcc::desktop
