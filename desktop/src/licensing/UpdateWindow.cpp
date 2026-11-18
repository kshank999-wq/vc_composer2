#include "licensing/UpdateWindow.h"

#include <atomic>

#include "licensing/Account.h"
#include "licensing/AccountWindow.h"
#include "ui/Dialogs.h"
#include "ui/Theme.h"
#include "ui/hw/HwTheme.h"
#include "vcc/licensing/Update.h"

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
    automatic_.setButtonText("Install updates automatically (when nothing is unsaved or playing, else at the next start)");
    automatic_.setColour(juce::ToggleButton::textColourId, colours::text);
    automatic_.setToggleState(install_updates_automatically(), juce::dontSendNotification);
    automatic_.onClick = [this] { set_install_updates_automatically(automatic_.getToggleState()); };
    addAndMakeVisible(automatic_);
    setSize(560, 360);
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
    install_.setButtonText(Updater::may_download() || !offer_ ? "Install and Restart"
                                                                                                        : "Sign In to Install...");
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
    if (!Updater::may_download()) {
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
    r.removeFromBottom(4);
    automatic_.setBounds(r.removeFromBottom(24));
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
    dialogs::show_tool(update_window(), "Check for Updates", 560, 360,
                       [&context, before_quit] { return new UpdatePanel(context, before_quit); });
    if (auto* p = panel_slot()) p->check();
}

UpdatePanel* update_panel() { return panel_slot(); }

namespace {
std::atomic<bool>& auto_busy()
{
    static std::atomic<bool> b{false};
    return b;
}
}  // namespace

bool auto_update_busy() { return auto_busy().load(); }

namespace {
constexpr const char* kAutoInstall = "updates_install_automatically";
constexpr const char* kReadyFile = "update_ready_file";
constexpr const char* kReadyVersion = "update_ready_version";

void forget_ready_update()
{
    hw::ui_settings().removeValue(kReadyFile);
    hw::ui_settings().removeValue(kReadyVersion);
    hw::ui_settings().saveIfNeeded();
}

/// Installs `file` and quits. False (with the reason in the status line) if it could not start.
bool install_now(AppContext& ctx, const juce::File& file, const juce::String& version)
{
    if (const auto error = Updater::get().install(file); error.isNotEmpty()) {
        ctx.status(error);
        return false;
    }
    forget_ready_update();
    ctx.status("Installing VC Composer " + version + "...");
    juce::JUCEApplicationBase::quit();
    return true;
}
}  // namespace

bool install_updates_automatically() { return hw::ui_settings().getBoolValue(kAutoInstall, true); }

void set_install_updates_automatically(bool on)
{
    hw::ui_settings().setValue(kAutoInstall, on);
    hw::ui_settings().saveIfNeeded();
}

juce::String ready_update_version()
{
    const auto version = hw::ui_settings().getValue(kReadyVersion);
    const juce::File file(hw::ui_settings().getValue(kReadyFile));
    if (version.isEmpty() || !juce::File::isAbsolutePath(file.getFullPathName()) || !file.existsAsFile() ||
        !licensing::is_newer(version.toStdString(), Updater::current_version().toStdString()))
        return {};
    return version;
}

bool install_ready_update(AppContext& context, std::function<bool()> idle)
{
    const auto version = ready_update_version();
    if (version.isEmpty()) {
        if (hw::ui_settings().containsKey(kReadyVersion)) forget_ready_update();  // installed, or gone
        return false;
    }
    if (!install_updates_automatically() || (idle && !idle())) return false;
    return install_now(context, juce::File(hw::ui_settings().getValue(kReadyFile)), version);
}

void auto_update(AppContext& context, std::function<bool()> before_quit, std::function<bool()> idle)
{
    // Signed in, activated with a licence, or a preview: none of them is ever
    // asked to sign in for an update.
    if (!Updater::available() || !Updater::may_download() || auto_busy().exchange(true)) return;
    AppContext* ctx = &context;
    juce::Thread::launch([ctx, before_quit = std::move(before_quit), idle = std::move(idle)] {
        const auto found = Updater::get().check();
        if (!found.offer) {
            auto_busy() = false;
            return;
        }
        const auto offer = *found.offer;
        const juce::String version(offer.version);
        juce::MessageManager::callAsync([ctx, version] { ctx->status("Downloading VC Composer " + version + "..."); });
        int last = -1;
        const auto got = Updater::get().download(offer, [ctx, version, &last](double p) {
            const int pct = static_cast<int>(p * 10.0) * 10;
            if (pct == last) return;
            last = pct;
            juce::MessageManager::callAsync(
                [ctx, version, pct] { ctx->status("Downloading VC Composer " + version + ": " + juce::String(pct) + "%"); });
        });
        juce::MessageManager::callAsync([ctx, version, got, before_quit, idle] {
            auto_busy() = false;
            if (!got.ok) {
                ctx->status("The update to " + version + " did not download (" + got.message + "): Help > Check for Updates");
                return;
            }
            if (install_updates_automatically()) {
                // Remembered, so it goes in at the next start if not now.
                hw::ui_settings().setValue(kReadyFile, got.file.getFullPathName());
                hw::ui_settings().setValue(kReadyVersion, version);
                hw::ui_settings().saveIfNeeded();
                // Nothing unsaved and nothing playing: in now, no questions.
                // Otherwise it never interrupts: it installs the next time
                // VC Composer starts.
                if (idle && idle() && install_now(*ctx, got.file, version)) return;
                ctx->status("VC Composer " + version + " is downloaded: it installs by itself the next time VC Composer starts");
                return;
            }
            ctx->status("VC Composer " + version + " is downloaded and ready to install");
            juce::AlertWindow::showAsync(
                juce::MessageBoxOptions()
                    .withIconType(juce::MessageBoxIconType::InfoIcon)
                    .withTitle("VC Composer " + version + " is ready")
                    .withMessage("The new version is downloaded and checked. Install it now? VC Composer closes (asking "
                                 "about anything unsaved first) and opens again, updated.")
                    .withButton("Install and Restart")
                    .withButton("Later"),
                [ctx, version, file = got.file, before_quit](int r) {
                    if (r != 1) {
                        ctx->status("VC Composer " + version + " is ready: Help > Check for Updates installs it");
                        return;
                    }
                    if (before_quit && !before_quit()) return;
                    install_now(*ctx, file, version);
                });
        });
    });
}

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
