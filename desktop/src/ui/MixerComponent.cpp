#include "ui/MixerComponent.h"

#include "AppResources.h"
#include "ui/Theme.h"
#include "ui/console/ConsoleView.h"

namespace vcc::desktop {

namespace {
juce::File window_state_file() { return user_data_directory().getChildFile("mixer.window"); }
}  // namespace

struct MixerComponent::Impl {
    explicit Impl(AppContext& context) : view(context) {}
    console::ConsoleView view;
};

MixerComponent::MixerComponent(AppContext& context) : context_(context)
{
    impl_ = std::make_unique<Impl>(context_);
    addAndMakeVisible(impl_->view);
    setSize(1400, 860);
}

MixerComponent::~MixerComponent() { impl_.reset(); }

void MixerComponent::paint(juce::Graphics& g) { g.fillAll(console::paint::backdrop); }

void MixerComponent::resized()
{
    if (impl_) impl_->view.setBounds(getLocalBounds());
}

MixerWindow::MixerWindow(AppContext& context)
    : juce::DocumentWindow("Mixer", console::paint::backdrop, juce::DocumentWindow::allButtons)
{
    setUsingNativeTitleBar(true);
    setContentOwned(new MixerComponent(context), true);
    setResizable(true, false);
    setResizeLimits(780, 480, 10000, 4000);
}

MixerWindow::~MixerWindow() { save_state(); }

void MixerWindow::visibilityChanged()
{
    juce::DocumentWindow::visibilityChanged();
    if (!isVisible() || placed_) return;
    placed_ = true;
    // The console wants room: restore where the user left it, else take most of the screen.
    const auto saved = window_state_file().loadFileAsString();
    if (saved.isNotEmpty() && restoreWindowStateFromString(saved)) return;
    auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() != nullptr
                    ? juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea
                    : juce::Rectangle<int>(0, 0, 1600, 1000);
    centreWithSize(juce::jmin(1600, area.getWidth() - 60), juce::jmin(980, area.getHeight() - 60));
}

void MixerWindow::save_state()
{
    if (!placed_) return;
    const auto file = window_state_file();
    file.getParentDirectory().createDirectory();
    file.replaceWithText(getWindowStateAsString());
}

void MixerWindow::closeButtonPressed()
{
    save_state();
    setVisible(false);
}

}  // namespace vcc::desktop
