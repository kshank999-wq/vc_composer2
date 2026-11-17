#include "licensing/AccountWindow.h"

#include "Preview.h"
#include "licensing/Account.h"
#include "ui/Dialogs.h"
#include "ui/Theme.h"

namespace vcc::desktop {

namespace {

juce::Component::SafePointer<dialogs::ToolWindow>& account_window()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}
AccountPanel*& panel_slot()
{
    static AccountPanel* p = nullptr;
    return p;
}

}  // namespace

AccountPanel::AccountPanel(AppContext& context) : context_(context)
{
    setComponentID("account_panel");
    panel_slot() = this;
    title_.setText("VC Composer account and licence", juce::dontSendNotification);
    title_.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    for (auto* l : {&title_, &status_, &standing_}) {
        l->setColour(juce::Label::textColourId, colours::text);
        l->setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(*l);
    }
    auto field = [this](juce::TextEditor& e, const juce::String& name, const juce::String& hint) {
        e.setTitle(name);
        e.setTextToShowWhenEmpty(hint, colours::text_dim);
        e.setIndents(8, 6);
        addAndMakeVisible(e);
    };
    field(email_, "Email", "you@example.com");
    field(code_, "Code from the email", "6-digit code");
    field(serial_, "Serial number (optional)", "VCC-XXXXX-XXXXX-XXXXX-XXXXX (optional)");
    email_.setText(Account::get().email(), false);
    send_.onClick = [this] { send_code(); };
    sign_in_.onClick = [this] { sign_in(); };
    activate_.onClick = [this] { activate(); };
    check_.onClick = [this] { run([] { return Account::get().check_standing().message; }, {}); };
    sign_out_.onClick = [this] {
        Account::get().sign_out();
        context_.status("Signed out");
        refresh();
    };
    site_.onClick = [] {
        const auto site = Account::get().config().site_url;
        juce::URL((site.isEmpty() ? juce::String("https://vc-composer.com") : site) + "/account").launchInDefaultBrowser();
    };
    for (auto* b : {&send_, &sign_in_, &activate_, &check_, &sign_out_, &site_}) addAndMakeVisible(*b);
    refresh();
    setSize(560, 460);
}

AccountPanel::~AccountPanel()
{
    *alive_ = false;
    if (panel_slot() == this) panel_slot() = nullptr;
}

void AccountPanel::set_fields(const juce::String& email, const juce::String& code, const juce::String& serial)
{
    email_.setText(email, false);
    code_.setText(code, false);
    serial_.setText(serial, false);
}

void AccountPanel::refresh()
{
    auto& a = Account::get();
    juce::String s;
    if (preview::is_preview())
        s = preview::status() + (a.signed_in() ? ". Signed in as " + a.email() + "." : ". Sign in for updates and shared projects; previews need no licence.");
    else if (!a.configured()) s = "This build is not connected to vc-composer.com: saving is not licensed.";
    else if (!a.signed_in()) s = "Not signed in.";
    else s = "Signed in as " + a.email() + (a.activated().value_or(false) ? ". This computer is activated." : ". This computer is not activated yet.");
    status_.setText(s, juce::dontSendNotification);
    const auto saving = a.saving();
    juce::String st;
    if (const auto standing = a.standing())
        st = "Licence: " + juce::String(standing->status) +
             (standing->expires_at.empty() ? juce::String(", no end date") : ", paid to " + juce::String(standing->expires_at.substr(0, 10))) +
             ". Last checked " + juce::String(standing->checked_at.substr(0, 10)) + ".";
    if (!saving.notice.empty()) st << (st.isEmpty() ? "" : "\n") << juce::String(saving.notice);
    standing_.setText(st, juce::dontSendNotification);
    // Previews need no licence but do sign in: updates and shared projects
    // go through the account.
    const bool on = a.configured();
    const bool can_sign_in = a.config().configured();
    for (auto* c : std::initializer_list<juce::Component*>{&email_, &code_, &send_, &sign_in_}) c->setEnabled(can_sign_in && !a.signed_in());
    sign_out_.setEnabled(can_sign_in && a.signed_in());
    for (auto* c : std::initializer_list<juce::Component*>{&serial_, &activate_, &check_}) c->setEnabled(on && a.signed_in());
}

void AccountPanel::run(std::function<juce::String()> work, std::function<void()> done)
{
    status_.setText("Working...", juce::dontSendNotification);
    juce::Thread::launch([work = std::move(work), done = std::move(done), alive = alive_, this, &context = context_] {
        const auto message = work();
        juce::MessageManager::callAsync([message, done, alive, this, &context] {
            context.status(message);
            if (*alive) {
                refresh();
                standing_.setText(message + (standing_.getText().isEmpty() ? "" : "\n" + standing_.getText()), juce::dontSendNotification);
            }
            if (done) done();
        });
    });
}

void AccountPanel::send_code(std::function<void()> done)
{
    const auto e = email_.getText();
    run([e] { return Account::get().send_code(e).message; }, std::move(done));
}

void AccountPanel::sign_in(std::function<void()> done)
{
    const auto e = email_.getText(), c = code_.getText();
    run([e, c] { return Account::get().verify_code(e, c).message; }, std::move(done));
}

void AccountPanel::activate(std::function<void()> done)
{
    const auto s = serial_.getText();
    run([s] { return Account::get().activate(s).message; }, std::move(done));
}

void AccountPanel::paint(juce::Graphics& g) { g.fillAll(colours::background); }

void AccountPanel::resized()
{
    auto r = getLocalBounds().reduced(16);
    title_.setBounds(r.removeFromTop(26));
    status_.setBounds(r.removeFromTop(40));
    auto row = [&r](juce::Component& a, juce::Component& b) {
        auto l = r.removeFromTop(32);
        b.setBounds(l.removeFromRight(190).reduced(0, 2));
        l.removeFromRight(8);
        a.setBounds(l.reduced(0, 2));
        r.removeFromTop(4);
    };
    row(email_, send_);
    row(code_, sign_in_);
    row(serial_, activate_);
    r.removeFromTop(6);
    auto buttons = r.removeFromBottom(32);
    check_.setBounds(buttons.removeFromLeft(130).reduced(0, 2));
    buttons.removeFromLeft(8);
    sign_out_.setBounds(buttons.removeFromLeft(100).reduced(0, 2));
    site_.setBounds(buttons.removeFromRight(170).reduced(0, 2));
    standing_.setBounds(r.reduced(0, 4));
}

void show_account_window(AppContext& context)
{
    dialogs::show_tool(account_window(), "Account and Licence", 560, 460, [&context] { return new AccountPanel(context); });
    if (auto* p = panel_slot()) p->refresh();
}

AccountPanel* account_panel() { return panel_slot(); }

bool may_save(AppContext& context)
{
    if (preview::ended()) {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::InfoIcon, "This preview has ended",
            "This preview of VC Composer ended on " + preview::expires() +
                ". Projects still open, play, print and export; to save, download the latest version from vc-composer.com.");
        return false;
    }
    const auto s = Account::get().saving();
    if (s.can_save) {
        if (s.warning && !s.notice.empty()) context.status(juce::String(s.notice));
        return true;
    }
    juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                     .withIconType(juce::MessageBoxIconType::InfoIcon)
                                     .withTitle(s.needs_activation ? "Activate VC Composer to save" : "Saving is off")
                                     .withMessage(juce::String(s.notice))
                                     .withButton("Account and Licence...")
                                     .withButton("Not Now"),
                                 [&context](int r) {
                                     if (r == 1) show_account_window(context);
                                 });
    return false;
}

}  // namespace vcc::desktop
