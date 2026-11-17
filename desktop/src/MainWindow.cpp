#include "MainWindow.h"
#include "Preview.h"
#include "system/ResourceMonitor.h"

#include "ui/Dialogs.h"
#include "ui/hw/HwTheme.h"
#include "ui/shell/Layouts.h"

namespace vcc::desktop {

MainWindow::MainWindow(const juce::String& file, bool show_welcome)
    : juce::DocumentWindow("VC Composer", colours::background, juce::DocumentWindow::allButtons)
{
    juce::LookAndFeel::setDefaultLookAndFeel(&theme_);
    catalogs_ = load_embedded_catalogs();
    session_ = std::make_unique<session::Session>(*catalogs_);
    plugins_ = std::make_unique<PluginHost>();
    engine_ = std::make_unique<PlaybackEngine>(*session_, *plugins_);
    context_ = std::make_unique<AppContext>(AppContext{*catalogs_, *session_, *engine_, *plugins_, commands_,
                                                       composer::make_composer2_profile(), {}, {}, {}});
    library_ = std::make_unique<LibraryService>(*context_);
    context_->library = library_.get();
    resources_ = std::make_unique<ResourceMonitor>(context_->plugins);
    context_->resources = resources_.get();
    main_ = std::make_unique<MainComponent>(*context_);

    commands_.registerAllCommandsForTarget(main_.get());
    commands_.setFirstCommandTarget(main_.get());
    addKeyListener(commands_.getKeyMappings());

    setUsingNativeTitleBar(true);
    setContentNonOwned(main_.get(), false);
    setResizable(true, false);
    setResizeLimits(960, 600, 10000, 10000);
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(main_.get());
#else
    setMenuBar(main_.get());
#endif
    // Never larger than the screen: a window taller than a laptop display
    // would open with its title bar and menus above the top edge.
    dialogs::fit_to_screen(*this, 1440, 900);

    listener_ = session_->add_listener([this](session::Change c) {
        if (c == session::Change::File || c == session::Change::Document) update_title();
        commands_.commandStatusChanged();
    });
    update_title();

    if (catalogs_->error_count() > 0)
        context_->status(juce::String(catalogs_->error_count()) + " content problem(s): some templates are unavailable");

    // The interface, rate, buffer and MIDI inputs chosen last time (Studio >
    // Audio Setup / MIDI Setup), saved again whenever they change.
    const auto saved_devices = juce::parseXML(hw::ui_settings().getValue("audio_device_state", ""));
    const juce::String audio_error = engine_->open_default_device(saved_devices.get());
    engine_->on_devices_changed = [this] {
        if (auto state = engine_->device_state()) {
            hw::ui_settings().setValue("audio_device_state", state->toString());
            hw::ui_settings().saveIfNeeded();
        }
    };
    engine_->on_devices_changed();
    if (audio_error.isNotEmpty()) context_->status("No audio output (" + audio_error + ") - choose a device in Tools > Audio");

    setVisible(true);
    if (file.isNotEmpty() && juce::File(file).existsAsFile()) {
        main_->open_file(juce::File(file));
    } else {
        session_->new_empty();
        if (show_welcome) juce::MessageManager::callAsync([this] { show_template_browser(*context_, true); });
    }
    // The layout it opens with (View > Layouts > Open VC Composer With), or
    // the one it was left in; then the mixer on its display, if asked.
    if (const auto layout = layouts::to_open_with()) main_->apply_layout(*layout);
    if (hw::ui_settings().getBoolValue("console_at_startup", false))
        main_->show_console_window(hw::ui_settings().getValue("console_display", ""));
    if (main_->workspace() == MainComponent::Workspace::Composer) main_->composer().grabKeyboardFocus();
}

MainWindow::~MainWindow()
{
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(nullptr);
#else
    setMenuBar(nullptr);
#endif
    session_->remove_listener(listener_);
    clearContentComponent();
    main_.reset();
    engine_.reset();
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
}

void MainWindow::update_title()
{
    const auto p = preview::status();
    setName(juce::String(session_->title()) + " - VC Composer" + (p.isEmpty() ? juce::String() : " (" + p + ")"));
}

bool MainWindow::ask_to_close()
{
    if (!main_->confirm_discard()) return false;
    layouts::set_last(main_->capture_layout());
    // A clean exit leaves no autosave behind for this document.
    if (!session_->path().empty()) juce::File(session_->path() + ".autosave").deleteFile();
    return true;
}

void MainWindow::closeButtonPressed()
{
    if (ask_to_close()) juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

void MainWindow::open_file(const juce::File& file)
{
    if (main_->confirm_discard()) main_->open_file(file);
}

}  // namespace vcc::desktop
