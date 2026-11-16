// Help > Account and Licence...: sign in with an emailed code, activate this
// computer (a serial is optional), see how the licence stands, sign out.
// Network calls run off the message thread.
#pragma once

#include "ui/AppContext.h"

namespace vcc::desktop {

class AccountPanel final : public juce::Component {
public:
    explicit AccountPanel(AppContext& context);
    ~AccountPanel() override;
    /// Shows the account's current state.
    void refresh();
    /// The status line (the desktop check reads it).
    juce::String status_text() const { return status_.getText(); }
    juce::String standing_text() const { return standing_.getText(); }
    /// The same as pressing the buttons; `done` runs on the message thread.
    void send_code(std::function<void()> done = {});
    void sign_in(std::function<void()> done = {});
    void activate(std::function<void()> done = {});
    void set_fields(const juce::String& email, const juce::String& code, const juce::String& serial);

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void run(std::function<juce::String()> work, std::function<void()> done);

    AppContext& context_;
    juce::Label title_, status_, standing_;
    juce::TextEditor email_, code_, serial_;
    juce::TextButton send_{"Email Me a Code"}, sign_in_{"Sign In"}, activate_{"Activate This Computer"},
        check_{"Check Licence"}, sign_out_{"Sign Out"}, site_{"Your Account Online"};
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

void show_account_window(AppContext& context);
AccountPanel* account_panel();
/// Whether saving is allowed now, telling the composer why not (preview
/// ended, or the licence). False means: do not save.
bool may_save(AppContext& context);

}  // namespace vcc::desktop
