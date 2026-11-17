#include "licensing/UpdateWindow.h"

#include "licensing/Account.h"
#include "licensing/AccountWindow.h"
#include "ui/Dialogs.h"
#include "ui/Theme.h"

namespace vcc::desktop {

namespace {

juce::Component::SafePointer<dialogs::ToolWindow>& update_window()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}
UpdatePanel*& panel_slot()
{
    static UpdatePanel* p = nullptr;
    return p;
}

}  // namespace

UpdatePanel::UpdatePanel(AppContext& context, std::function<bool()> before_quit)
    : context_(context), before_quit_(std::move(before_quit))
{
    setComponentID("update_panel");
    panel_slot() = this;
    title_.setText("VC Composer updates", juce::dontSendNotification);
    title_.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    version_.setText("This is VC Composer " + Updater::current_version() +
                         (Updater::channel() == "preview" ? " (a preview: it follows the preview builds)" : juce::String()),
                     juce::dontSendNotification);
    for (auto* l : {&title_, &version_, &status_, &notes_}) {
        l->setColour(juce::Label::textColourId, l == &notes_ ? colours::text_dim : colours::text);
        l->setJustificationType(juce::Justification::topLeft);
        l->setMinimumHorizontalScale(1.0f);
        addAndMakeVisible(*l);
    }
    progress_.setTitle("Download progress");
    addChildComponent(progress_);
    check_.onClick = [this] { check(); };
    install_.onClick = [this] { download(true); };
    later_.onClick = [this] {
        if (auto* w = findParentComponentOfClass<juce::DocumentWindow>()) w->closeButtonPressed();
    };
    for (auto* b : {&check_, &install_, &later_}) addAndMakeVisible(*b);
    setSize(560, 330);
    update_buttons();
}

UpdatePanel::~UpdatePanel()
{
    *alive_ = false;
    if (panel_slot() == this) panel_slot() = nullptr;
}

void UpdatePanel::update_buttons()
{
    check_.setEnabled(!busy_);
    install_.setEnabled(!busy_ && offer_.has_value());
    install_.setButtonText(Account::get().signed_in() || !offer_ ? "Install and Restart" : "Sign In to Install...");
}

void UpdatePanel::check(std::function<void()> done)
{
    busy_ = true;
    status_.setText("Asking vc-composer.com...", juce::dontSendNotification);
    update_buttons();
    juce::Thread::launch([this, alive = alive_, done = std::move(done)] {
        auto result = std::make_shared<Updater::Check>(Updater::get().check());
        juce::MessageManager::callAsync([this, alive, result, done] {
            if (*alive) {
                busy_ = false;
                offer_ = result->offer;
                status_.setText(result->message, juce::dontSendNotification);
                notes_.setText(offer_ && !offer_->notes.empty() ? "What's new: " + juce::String(offer_->notes) : juce::String(),
                               juce::dontSendNotification);
                update_buttons();
            }
            if (done) done();
        });
    });
}

void UpdatePanel::download(bool then_install, std::function<void()> done)
{
    if (!offer_) return;
    if (!Account::get().signed_in()) {
        status_.setText("Updates download through your account: sign in, then press Install again.", juce::dontSendNotification);
        show_account_window(context_);
        if (done) done();
        return;
    }
    busy_ = true;
    progress_value_ = 0.0;
    progress_.setVisible(true);
    status_.setText("Downloading VC Composer " + juce::String(offer_->version) + "...", juce::dontSendNotification);
    update_buttons();
    const auto offer = *offer_;
    juce::Thread::launch([this, alive = alive_, offer, then_install, done = std::move(done)] {
        auto result = std::make_shared<Updater::Downloaded>(Updater::get().download(offer, [this, alive](double p) {
            juce::MessageManager::callAsync([this, alive, p] {
                if (*alive) progress_value_ = p;
            });
        }));
        juce::MessageManager::callAsync([this, alive, result, then_install, done] {
            if (!*alive) {
                if (done) done();
                return;
            }
            busy_ = false;
            progress_.setVisible(false);
            status_.setText(result->message, juce::dontSendNotification);
            update_buttons();
            if (result->ok) file_ = result->file;
            if (result->ok && then_install) {
                // Unsaved work is asked about first; the installer then takes over.
                if (before_quit_ && !before_quit_()) {
                    status_.setText("Not installed: downloaded and ready for when you are", juce::dontSendNotification);
                } else if (const auto error = Updater::get().install(result->file); error.isNotEmpty()) {
                    status_.setText(error, juce::dontSendNotification);
                } else {
                    context_.status("Installing VC Composer " + juce::String(offer_ ? offer_->version : std::string()) + "...");
                    juce::JUCEApplicationBase::quit();
                }
            }
            if (done) done();
        });
    });
}

void UpdatePanel::paint(juce::Graphics& g) { g.fillAll(colours::background); }

void UpdatePanel::resized()
{
    auto r = getLocalBounds().reduced(16);
    title_.setBounds(r.removeFromTop(26));
    version_.setBounds(r.removeFromTop(24));
    r.removeFromTop(6);
    status_.setBounds(r.removeFromTop(44));
    auto buttons = r.removeFromBottom(32);
    install_.setBounds(buttons.removeFromLeft(170).reduced(0, 2));
    buttons.removeFromLeft(8);
    check_.setBounds(buttons.removeFromLeft(120).reduced(0, 2));
    later_.setBounds(buttons.removeFromRight(90).reduced(0, 2));
    r.removeFromBottom(8);
    progress_.setBounds(r.removeFromBottom(20));
    r.removeFromBottom(6);
    notes_.setBounds(r);
}

void show_update_window(AppContext& context, std::function<bool()> before_quit)
{
    dialogs::show_tool(update_window(), "Check for Updates", 560, 330,
                       [&context, before_quit] { return new UpdatePanel(context, before_quit); });
    if (auto* p = panel_slot()) p->check();
}

UpdatePanel* update_panel() { return panel_slot(); }

void check_for_updates_quietly(std::function<void(juce::String)> found)
{
    if (!Updater::available()) return;
    juce::Thread::launch([found = std::move(found)] {
        const auto result = Updater::get().check();
        if (!result.offer) return;
        const juce::String version(result.offer->version);
        juce::MessageManager::callAsync([found, version] { found(version); });
    });
}

}  // namespace vcc::desktop
