#include <juce_gui_basics/juce_gui_basics.h>

#include "MainWindow.h"
#include "PlatformFixes.h"

namespace {

// Before JUCE opens the display (see PlatformFixes.h).
const bool display_prepared = vcc::desktop::prepare_display();

class VCComposerApplication final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String& command_line) override
    {
        window_ = std::make_unique<vcc::desktop::MainWindow>(command_line.unquoted());
    }

    void shutdown() override { window_.reset(); }

    void systemRequestedQuit() override
    {
        if (window_ == nullptr || window_->ask_to_close()) quit();
    }

    void anotherInstanceStarted(const juce::String& command_line) override
    {
        if (window_ != nullptr && command_line.isNotEmpty()) window_->open_file(juce::File(command_line.unquoted()));
    }

private:
    std::unique_ptr<vcc::desktop::MainWindow> window_;
};

}  // namespace

START_JUCE_APPLICATION(VCComposerApplication)
