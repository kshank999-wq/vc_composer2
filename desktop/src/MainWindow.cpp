#include "MainWindow.h"

#include "ui/Dialogs.h"

namespace vcc::desktop {

MainWindow::MainWindow(const juce::String& file, bool show_welcome)
    : juce::DocumentWindow("VC Composer 2", colours::background, juce::DocumentWindow::allButtons)
{
    juce::LookAndFeel::setDefaultLookAndFeel(&theme_);
    catalogs_ = load_embedded_catalogs();
    session_ = std::make_unique<session::Session>(*catalogs_);
    plugins_ = std::make_unique<PluginHost>();
    engine_ = std::make_unique<PlaybackEngine>(*session_, *plugins_);
    context_ = std::make_unique<AppContext>(AppContext{*catalogs_, *session_, *engine_, *plugins_, commands_,
                                                       composer::make_vc_composer_profile(), {}, {}});
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
    centreWithSize(1440, 900);

    listener_ = session_->add_listener([this](session::Change c) {
        if (c == session::Change::File || c == session::Change::Document) update_title();
        commands_.commandStatusChanged();
    });
    update_title();

    if (catalogs_->error_count() > 0)
        context_->status(juce::String(catalogs_->error_count()) + " content problem(s): some templates are unavailable");

    const juce::String audio_error = engine_->open_default_device();
    if (audio_error.isNotEmpty()) context_->status("No audio output (" + audio_error + ") - choose a device in Tools > Audio");

    setVisible(true);
    if (file.isNotEmpty() && juce::File(file).existsAsFile()) {
        main_->open_file(juce::File(file));
    } else {
        session_->new_empty();
        if (show_welcome) juce::MessageManager::callAsync([this] { show_template_browser(*context_, true); });
    }
    main_->composer().grabKeyboardFocus();
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

void MainWindow::update_title() { setName(juce::String(session_->title()) + " - VC Composer 2"); }

bool MainWindow::ask_to_close()
{
    if (!main_->confirm_discard()) return false;
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
