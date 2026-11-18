#include "ui/SoundsWindow.h"

#include "audio/SoundPack.h"
#include "ui/Credits.h"
#include "ui/Dialogs.h"
#include "ui/Theme.h"

namespace vcc::desktop {

namespace {
juce::Component::SafePointer<dialogs::ToolWindow>& window_slot()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}
SoundsPanel*& panel_slot()
{
    static SoundsPanel* p = nullptr;
    return p;
}
}  // namespace

SoundsPanel::SoundsPanel()
{
    panel_slot() = this;
    setComponentID("sounds_panel");
    title_.setText("Built-in Sounds", juce::dontSendNotification);
    title_.setFont(juce::FontOptions(17.0f, juce::Font::bold));
    title_.setColour(juce::Label::textColourId, colours::text);
    addAndMakeVisible(title_);
    about_.setText("A real grand piano (Salamander) and a full General MIDI orchestra with drum kits "
                   "(MuseScore General), so chords, templates and ostinatos sound like their instruments "
                   "before any library is installed. Tracks with a plug-in keep their plug-in.",
                   juce::dontSendNotification);
    about_.setColour(juce::Label::textColourId, colours::text_dim);
    about_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(about_);
    status_.setColour(juce::Label::textColourId, colours::text);
    status_.setComponentID("sounds_status");
    addAndMakeVisible(status_);
    addAndMakeVisible(progress_);
    download_.setComponentID("sounds_download");
    download_.onClick = [] { sounds::download_async(sounds::installed()); };
    addAndMakeVisible(download_);
    folder_.onClick = [] {
        sounds::folder().createDirectory();
        sounds::folder().revealToUser();
    };
    addAndMakeVisible(folder_);
    credits_.onClick = [] { show_credits_window(); };
    addAndMakeVisible(credits_);
    listener_ = sounds::add_listener([this] { refresh(); });
    refresh();
    startTimerHz(4);
    setSize(460, 250);
}

SoundsPanel::~SoundsPanel()
{
    sounds::remove_listener(listener_);
    if (panel_slot() == this) panel_slot() = nullptr;
}

void SoundsPanel::refresh()
{
    const auto st = sounds::state();
    status_.setText(sounds::status(), juce::dontSendNotification);
    const bool busy = st == sounds::State::Downloading || st == sounds::State::Loading;
    progress_value_ = st == sounds::State::Downloading ? sounds::progress() : (st == sounds::State::Loading ? -1.0 : 0.0);
    progress_.setVisible(busy);
    download_.setEnabled(!busy && sounds::source_url().isNotEmpty());
    download_.setButtonText(sounds::installed() ? "Download Again" : "Download");
}

void SoundsPanel::paint(juce::Graphics& g) { g.fillAll(colours::background); }

void SoundsPanel::resized()
{
    auto r = getLocalBounds().reduced(14);
    title_.setBounds(r.removeFromTop(26));
    about_.setBounds(r.removeFromTop(74));
    r.removeFromTop(6);
    status_.setBounds(r.removeFromTop(24));
    r.removeFromTop(4);
    progress_.setBounds(r.removeFromTop(20));
    auto keys = r.removeFromBottom(30);
    download_.setBounds(keys.removeFromLeft(140));
    keys.removeFromLeft(8);
    folder_.setBounds(keys.removeFromLeft(120));
    credits_.setBounds(keys.removeFromRight(110));
}

void show_sounds_window()
{
    dialogs::show_tool(window_slot(), "Built-in Sounds", 460, 250, [] { return new SoundsPanel(); });
}

SoundsPanel* sounds_panel() { return panel_slot(); }

}  // namespace vcc::desktop
