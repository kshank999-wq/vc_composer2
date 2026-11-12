// Sequencer workspace (Main Spec §9): the same tracks as the Composer view
// on a timeline - bar ruler, one lane per track with its rendered notes
// (score-linked notes and MIDI-region notes told apart), playhead, and a
// piano roll for editing the selected track's MIDI regions.
#pragma once

#include "ui/AppContext.h"

namespace vcc::desktop {

class SequencerView final : public juce::Component {
public:
    explicit SequencerView(AppContext& context);
    ~SequencerView() override;

    /// Called by the main window ~30 times a second with the transport position.
    void set_playhead(Ticks tick, bool playing);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    AppContext& context_;
};

}  // namespace vcc::desktop
