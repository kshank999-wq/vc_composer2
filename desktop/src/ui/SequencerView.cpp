#include "ui/SequencerView.h"

#include "ui/Theme.h"

namespace vcc::desktop {

// Placeholder: the full Sequencer is being built in a follow-up commit.
struct SequencerView::Impl {};

SequencerView::SequencerView(AppContext& context) : impl_(std::make_unique<Impl>()), context_(context) {}
SequencerView::~SequencerView() = default;
void SequencerView::set_playhead(Ticks, bool) {}
void SequencerView::paint(juce::Graphics& g) { g.fillAll(colours::background); }
void SequencerView::resized() {}

}  // namespace vcc::desktop
