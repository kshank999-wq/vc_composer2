#include "ui/MixerComponent.h"

#include "ui/Theme.h"

namespace vcc::desktop {

// Placeholder: the full mixer is being built in a follow-up commit.
struct MixerComponent::Impl {};

MixerComponent::MixerComponent(AppContext& context) : impl_(std::make_unique<Impl>()), context_(context)
{
    setSize(900, 420);
}
MixerComponent::~MixerComponent() = default;
void MixerComponent::paint(juce::Graphics& g) { g.fillAll(colours::background); }
void MixerComponent::resized() {}

MixerWindow::MixerWindow(AppContext& context)
    : juce::DocumentWindow("Mixer", colours::background, juce::DocumentWindow::allButtons)
{
    setUsingNativeTitleBar(true);
    setContentOwned(new MixerComponent(context), true);
    setResizable(true, true);
    setResizeLimits(480, 300, 4000, 2000);
}

void MixerWindow::closeButtonPressed() { setVisible(false); }

}  // namespace vcc::desktop
