// Help > Account and Licence...: sign in with the vc-composer.com password
// (or a code emailed instead), activate this computer (a serial is
// optional), see how the licence stands, sign out. Signing in links the
// account's licence and then looks for a newer VC Composer straight away.
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
    void set_password(const juce::String& password) { password_.setText(password, false); }

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void run(std::function<juce::String()> work, std::function<void()> done);

    AppContext& context_;
    juce::Label title_, status_, standing_;
    void signed_in_follow_up(std::function<void()> done);

    juce::TextEditor email_, password_, code_, serial_;
    juce::TextButton send_{"Email Me a Code"}, sign_in_{"Sign In"}, activate_{"Activate This Computer"},
        check_{"Check Licence"}, sign_out_{"Sign Out"}, site_{"Your Account Online"}, forgot_{"Forgot Password?"};
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

void show_account_window(AppContext& context);
/// Runs after each successful sign-in (the app then looks for an update).
void set_after_sign_in(std::function<void()> fn);
AccountPanel* account_panel();
/// Whether saving is allowed now, telling the composer why not (preview
/// ended, or the licence). False means: do not save.
bool may_save(AppContext& context);

}  // namespace vcc::desktop
