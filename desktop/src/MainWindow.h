// The application window: owns the content catalogs, the document session,
// plug-in hosting, the audio engine and the main component.
#pragma once

#include <memory>

#include "ui/AppContext.h"
#include "ui/MainComponent.h"
#include "ui/Theme.h"

namespace vcc::desktop {

class MainWindow final : public juce::DocumentWindow {
public:
    /// `file` may be empty; then the template browser opens (New Project).
    explicit MainWindow(const juce::String& file, bool show_welcome = true);
    ~MainWindow() override;

    void closeButtonPressed() override;
    bool ask_to_close();
    void open_file(const juce::File& file);

    MainComponent& main() noexcept { return *main_; }
    AppContext& context() noexcept { return *context_; }

private:
    void update_title();

    Theme theme_;
    std::unique_ptr<session::Catalogs> catalogs_;
    std::unique_ptr<session::Session> session_;
    std::unique_ptr<PluginHost> plugins_;
    std::unique_ptr<PlaybackEngine> engine_;
    juce::ApplicationCommandManager commands_;
    juce::TooltipWindow tooltips_;
    std::unique_ptr<AppContext> context_;
    std::unique_ptr<MainComponent> main_;
    int listener_ = 0;
};

}  // namespace vcc::desktop
