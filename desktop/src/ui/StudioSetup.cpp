#include "ui/StudioSetup.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include "audio/MidiOut.h"
#include "audio/PlaybackEngine.h"
#include "ui/Dialogs.h"
#include "ui/Theme.h"

namespace vcc::desktop {

namespace {

juce::Component::SafePointer<dialogs::ToolWindow>& setup_window()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}
StudioSetup*& setup_slot()
{
    static StudioSetup* p = nullptr;
    return p;
}

/// The standard rates, in the order the keys show them.
constexpr double kRates[] = {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};

juce::Label& dim(juce::Label& l, float size = 13.0f)
{
    l.setFont(juce::FontOptions(size));
    l.setColour(juce::Label::textColourId, colours::text_dim);
    l.setJustificationType(juce::Justification::topLeft);
    l.setMinimumHorizontalScale(1.0f);
    return l;
}

juce::String driver_note()
{
#if JUCE_MAC
    return "Every Core Audio interface appears here - Universal Audio Apollo, MOTU, RME, Focusrite, Antelope, "
           "Apogee and others - over USB, USB-C, Thunderbolt or AVB. Install the maker's driver or control "
           "software first where it has one.";
#elif JUCE_WINDOWS
    return "Interfaces such as Universal Audio Apollo, MOTU, RME, Focusrite and Antelope appear under Windows "
           "Audio once their driver is installed. ASIO, their lowest-latency driver, appears as its own audio "
           "system in builds that include it.";
#else
    return "ALSA and JACK devices appear here.";
#endif
}

}  // namespace

juce::String rate_label(double hz)
{
    const double k = hz / 1000.0;
    return (std::abs(k - std::round(k)) < 0.01 ? juce::String(juce::roundToInt(k)) : juce::String(k, 1)) + " kHz";
}

// --- Audio ------------------------------------------------------------------------------------

class StudioSetup::AudioPage final : public juce::Component, private juce::Timer {
public:
    explicit AudioPage(AppContext& c)
        : context_(c),
          selector_(c.engine.devices(), 0, 64, 0, 16, false, false, true, false)
    {
        title_.setText("Audio interface", juce::dontSendNotification);
        title_.setFont(juce::FontOptions(15.0f, juce::Font::bold));
        title_.setColour(juce::Label::textColourId, colours::text);
        addAndMakeVisible(title_);
        summary_.setComponentID("audio_summary");
        summary_.setColour(juce::Label::textColourId, colours::accent);
        summary_.setFont(juce::FontOptions(13.5f));
        addAndMakeVisible(summary_);
        rates_label_.setText("Sample rate", juce::dontSendNotification);
        addAndMakeVisible(dim(rates_label_));
        for (const double hz : kRates) {
            auto* b = rate_keys_.add(new juce::TextButton(rate_label(hz)));
            b->setClickingTogglesState(false);
            b->setColour(juce::TextButton::buttonOnColourId, colours::accent.withAlpha(0.6f));
            b->setTooltip("Run the interface at " + rate_label(hz));
            b->onClick = [this, hz] { choose(hz); };
            addAndMakeVisible(b);
        }
        project_rate_.setComponentID("project_rate");
        addAndMakeVisible(dim(project_rate_, 12.5f));
        use_project_.setButtonText("Use the Project's Rate");
        use_project_.onClick = [this] { choose(static_cast<double>(context_.session.data().sample_rate)); };
        addAndMakeVisible(use_project_);
        note_.setText(driver_note(), juce::dontSendNotification);
        addAndMakeVisible(dim(note_, 12.5f));
        addAndMakeVisible(selector_);
        refresh();
        startTimer(500);  // the interface can change under us (unplugged, another app)
    }

    juce::String summary() const { return summary_.getText(); }

    juce::StringArray offered() const
    {
        juce::StringArray out;
        for (int i = 0; i < rate_keys_.size(); ++i)
            if (rate_keys_[i]->isEnabled()) out.add(rate_keys_[i]->getButtonText());
        return out;
    }

    bool choose(double hz)
    {
        auto& devices = context_.engine.devices();
        auto* device = devices.getCurrentAudioDevice();
        if (device == nullptr || !device->getAvailableSampleRates().contains(hz)) {
            context_.status(rate_label(hz) + " is not offered by this interface");
            return false;
        }
        auto setup = devices.getAudioDeviceSetup();
        setup.sampleRate = hz;
        const auto error = devices.setAudioDeviceSetup(setup, true);
        if (error.isNotEmpty()) {
            context_.status("The interface refused " + rate_label(hz) + ": " + error);
            return false;
        }
        refresh();
        context_.status("Audio now runs at " + rate_label(hz));
        return true;
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(14, 10);
        title_.setBounds(r.removeFromTop(22));
        summary_.setBounds(r.removeFromTop(22));
        r.removeFromTop(6);
        auto row = r.removeFromTop(28);
        rates_label_.setBounds(row.removeFromLeft(92).withTrimmedTop(6));
        for (auto* b : rate_keys_) {
            b->setBounds(row.removeFromLeft(72).reduced(2, 0));
        }
        row = r.removeFromTop(26);
        row.removeFromLeft(92);
        use_project_.setBounds(row.removeFromRight(170).reduced(0, 2));
        project_rate_.setBounds(row.withTrimmedTop(6));
        r.removeFromTop(4);
        note_.setBounds(r.removeFromTop(36));
        r.removeFromTop(4);
        selector_.setBounds(r);
    }

private:
    void timerCallback() override { refresh(); }

    void refresh()
    {
        auto& devices = context_.engine.devices();
        auto* device = devices.getCurrentAudioDevice();
        const int project_hz = context_.session.data().sample_rate;
        if (device == nullptr) {
            summary_.setText("No audio interface is open - choose one below", juce::dontSendNotification);
            for (auto* b : rate_keys_) b->setEnabled(false);
            project_rate_.setText("This project is set to " + rate_label(project_hz), juce::dontSendNotification);
            use_project_.setEnabled(false);
        } else {
            const double hz = device->getCurrentSampleRate();
            const int buffer = device->getCurrentBufferSizeSamples();
            const double ms = hz > 0 ? 1000.0 * buffer / hz : 0.0;
            const int ins = device->getActiveInputChannels().countNumberOfSetBits();
            const int outs = device->getActiveOutputChannels().countNumberOfSetBits();
            summary_.setText(device->getTypeName() + ": " + device->getName() + "  -  " + rate_label(hz) + "  -  " +
                                 juce::String(buffer) + " samples (" + juce::String(ms, 1) + " ms)  -  " +
                                 juce::String(ins) + " in / " + juce::String(outs) + " out",
                             juce::dontSendNotification);
            const auto rates = device->getAvailableSampleRates();
            for (int i = 0; i < rate_keys_.size(); ++i) {
                rate_keys_[i]->setEnabled(rates.contains(kRates[i]));
                rate_keys_[i]->setToggleState(std::abs(kRates[i] - hz) < 1.0, juce::dontSendNotification);
            }
            const bool differs = std::abs(hz - project_hz) > 1.0;
            project_rate_.setText("This project is set to " + rate_label(project_hz) +
                                      (differs ? ": the interface runs at another rate, so audio files are "
                                                 "converted as they play"
                                               : ", as the interface runs"),
                                  juce::dontSendNotification);
            project_rate_.setColour(juce::Label::textColourId, differs ? colours::accent_warm : colours::text_dim);
            use_project_.setEnabled(differs && rates.contains(static_cast<double>(project_hz)));
        }
    }

    AppContext& context_;
    juce::Label title_, summary_, rates_label_, project_rate_, note_;
    juce::OwnedArray<juce::TextButton> rate_keys_;
    juce::TextButton use_project_;
    juce::AudioDeviceSelectorComponent selector_;
};

// --- MIDI -------------------------------------------------------------------------------------

class StudioSetup::MidiPage final : public juce::Component, private juce::Timer {
public:
    explicit MidiPage(AppContext& c) : context_(c)
    {
        title_.setText("MIDI devices", juce::dontSendNotification);
        title_.setFont(juce::FontOptions(15.0f, juce::Font::bold));
        title_.setColour(juce::Label::textColourId, colours::text);
        addAndMakeVisible(title_);
        about_.setText("Keyboards and controllers over USB or USB-C, an interface's MIDI ports, Bluetooth and "
                       "network MIDI. Class-compliant devices need no driver: plug in and they appear.",
                       juce::dontSendNotification);
        addAndMakeVisible(dim(about_, 12.5f));
        auto_.setButtonText("Turn on MIDI devices as they are plugged in");
        auto_.setComponentID("midi_auto_enable");
        auto_.setToggleState(c.engine.auto_enable_midi(), juce::dontSendNotification);
        auto_.onClick = [this] {
            context_.engine.set_auto_enable_midi(auto_.getToggleState());
            if (context_.engine.on_devices_changed) context_.engine.on_devices_changed();
        };
        addAndMakeVisible(auto_);
        inputs_label_.setText("INPUTS (notes, controllers, pedals)", juce::dontSendNotification);
        addAndMakeVisible(dim(inputs_label_, 11.5f));
        outputs_label_.setText("OUTPUTS (MIDI tracks can send to these)", juce::dontSendNotification);
        addAndMakeVisible(dim(outputs_label_, 11.5f));
        outputs_.setMultiLine(true);
        outputs_.setReadOnly(true);
        outputs_.setColour(juce::TextEditor::backgroundColourId, colours::panel);
        addAndMakeVisible(outputs_);
        filter_.setButtonText("MIDI Input Filter...");
        filter_.onClick = [this] { context_.commands.invokeDirectly(cmd::midiInputFilter, true); };
        addAndMakeVisible(filter_);
        surfaces_.setButtonText("Control Surfaces...");
        surfaces_.onClick = [this] { context_.commands.invokeDirectly(cmd::showControlSurfaces, true); };
        addAndMakeVisible(surfaces_);
        rebuild();
        startTimer(1000);  // plugged and unplugged devices
    }

    std::vector<std::pair<juce::String, bool>> inputs() const
    {
        std::vector<std::pair<juce::String, bool>> out;
        for (auto* t : toggles_) out.emplace_back(t->getButtonText(), t->getToggleState());
        return out;
    }

    void set_input(const juce::String& name, bool on)
    {
        for (const auto& d : juce::MidiInput::getAvailableDevices())
            if (d.name == name) context_.engine.devices().setMidiInputDeviceEnabled(d.identifier, on);
        rebuild();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(14, 10);
        title_.setBounds(r.removeFromTop(22));
        about_.setBounds(r.removeFromTop(34));
        auto_.setBounds(r.removeFromTop(26));
        r.removeFromTop(8);
        auto buttons = r.removeFromBottom(30);
        filter_.setBounds(buttons.removeFromLeft(160).reduced(0, 2));
        buttons.removeFromLeft(8);
        surfaces_.setBounds(buttons.removeFromLeft(160).reduced(0, 2));
        r.removeFromBottom(8);
        auto outs = r.removeFromBottom(std::min(130, r.getHeight() / 3));
        outputs_label_.setBounds(outs.removeFromTop(18));
        outputs_.setBounds(outs);
        r.removeFromBottom(8);
        inputs_label_.setBounds(r.removeFromTop(18));
        if (toggles_.isEmpty()) none_.setBounds(r.removeFromTop(24));
        for (auto* t : toggles_) t->setBounds(r.removeFromTop(24));
    }

private:
    void timerCallback() override
    {
        // Rebuild only when the devices or their state changed.
        if (signature() != signature_) rebuild();
    }

    juce::String signature() const
    {
        juce::String s;
        for (const auto& d : juce::MidiInput::getAvailableDevices())
            s << d.identifier << (context_.engine.devices().isMidiInputDeviceEnabled(d.identifier) ? "+" : "-") << ";";
        s << "|" << MidiOutRouter::device_names().joinIntoString(";");
        return s;
    }

    void rebuild()
    {
        signature_ = signature();
        toggles_.clear();
        for (const auto& d : juce::MidiInput::getAvailableDevices()) {
            auto* t = toggles_.add(new juce::ToggleButton(d.name));
            t->setToggleState(context_.engine.devices().isMidiInputDeviceEnabled(d.identifier), juce::dontSendNotification);
            const auto id = d.identifier;
            t->onClick = [this, t, id] {
                context_.engine.devices().setMidiInputDeviceEnabled(id, t->getToggleState());
                signature_ = signature();
            };
            addAndMakeVisible(t);
        }
        none_.setText("No MIDI input is connected. Plug in a keyboard or controller: it appears here.",
                      juce::dontSendNotification);
        dim(none_, 12.5f);
        addAndMakeVisible(none_);
        none_.setVisible(toggles_.isEmpty());
        const auto outs = MidiOutRouter::device_names();
        outputs_.setText(outs.isEmpty() ? juce::String("No MIDI outputs connected") : outs.joinIntoString("\n"), false);
        resized();
    }

    AppContext& context_;
    juce::Label title_, about_, inputs_label_, outputs_label_, none_;
    juce::ToggleButton auto_;
    juce::OwnedArray<juce::ToggleButton> toggles_;
    juce::TextEditor outputs_;
    juce::TextButton filter_, surfaces_;
    juce::String signature_;
};

// --- The window -------------------------------------------------------------------------------

StudioSetup::StudioSetup(AppContext& context) : context_(context)
{
    setComponentID("studio_setup");
    setup_slot() = this;
    audio_ = new AudioPage(context);
    midi_ = new MidiPage(context);
    tabs_.addTab("Audio", colours::background, audio_, true);
    tabs_.addTab("MIDI", colours::background, midi_, true);
    tabs_.setTabBarDepth(30);
    tabs_.setOutline(0);
    addAndMakeVisible(tabs_);
    setSize(720, 640);
}

StudioSetup::~StudioSetup()
{
    if (setup_slot() == this) setup_slot() = nullptr;
}

void StudioSetup::show_page(int page) { tabs_.setCurrentTabIndex(juce::jlimit(0, 1, page)); }
int StudioSetup::page() const { return tabs_.getCurrentTabIndex(); }
juce::String StudioSetup::summary() const { return audio_->summary(); }
juce::StringArray StudioSetup::rate_keys() const { return audio_->offered(); }
bool StudioSetup::choose_rate(double hz) { return audio_->choose(hz); }
std::vector<std::pair<juce::String, bool>> StudioSetup::midi_inputs() const { return midi_->inputs(); }
void StudioSetup::set_midi_input(const juce::String& name, bool on) { midi_->set_input(name, on); }

void StudioSetup::resized() { tabs_.setBounds(getLocalBounds()); }
void StudioSetup::paint(juce::Graphics& g) { g.fillAll(colours::background); }

void show_studio_setup(AppContext& context, int page)
{
    dialogs::show_tool(setup_window(), "Audio and MIDI Setup", 720, 640,
                       [&context] { return new StudioSetup(context); });
    if (auto* s = setup_slot()) s->show_page(page);
}

StudioSetup* studio_setup() { return setup_slot(); }

}  // namespace vcc::desktop
