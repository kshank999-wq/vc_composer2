// Help > Check for Updates...: the version this is, whether vc-composer.com
// has a newer one and what changed, and Install and Restart (download through
// the account, checksum, install over this copy, start again). The main
// window also checks quietly a little after start-up and says so in the
// status bar when an update is waiting.
#pragma once

#include "licensing/Updater.h"
#include "ui/AppContext.h"

namespace vcc::desktop {

class UpdatePanel final : public juce::Component {
public:
    /// `before_quit` asks about unsaved work; false stops the install.
    UpdatePanel(AppContext& context, std::function<bool()> before_quit);
    ~UpdatePanel() override;

    void check(std::function<void()> done = {});
    /// Download and verify; with `then_install`, install and quit after.
    void download(bool then_install, std::function<void()> done = {});

    juce::String status_text() const { return status_.getText(); }
    bool has_offer() const { return offer_.has_value(); }
    juce::File downloaded() const { return file_; }

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void update_buttons();

    AppContext& context_;
    std::function<bool()> before_quit_;
    juce::Label title_, version_, status_, notes_;
    double progress_value_ = 0.0;
    juce::ProgressBar progress_{progress_value_};
    juce::TextButton check_{"Check Again"}, install_{"Install and Restart"}, later_{"Later"};
    std::optional<licensing::UpdateOffer> offer_;
    juce::File file_;
    bool busy_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

void show_update_window(AppContext& context, std::function<bool()> before_quit);
UpdatePanel* update_panel();
/// The quiet check after start-up: calls `found` (message thread) with the
/// offered version when there is one. Does nothing in builds that cannot update.
void check_for_updates_quietly(std::function<void(juce::String version)> found);

}  // namespace vcc::desktop
