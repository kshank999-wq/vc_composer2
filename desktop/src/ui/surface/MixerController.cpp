#include "ui/surface/MixerController.h"

#include <cmath>

#include "audio/PlaybackEngine.h"
#include "ui/Dialogs.h"
#include "ui/Theme.h"
#include "ui/hw/HwTheme.h"

namespace vcc::desktop {

namespace mk = project::mackie;

namespace {
juce::String input_id_for(const juce::String& name)
{
    for (const auto& d : juce::MidiInput::getAvailableDevices())
        if (d.name == name) return d.identifier;
    return {};
}
juce::String output_id_for(const juce::String& name)
{
    for (const auto& d : juce::MidiOutput::getAvailableDevices())
        if (d.name == name) return d.identifier;
    return {};
}
std::string level_text(double db)
{
    if (db <= -95.0) return "  -inf";
    char buf[16];
    std::snprintf(buf, sizeof buf, "%+.1f", db);
    return buf;
}
}  // namespace

MixerController::MixerController(AppContext& context) : context_(context)
{
    lights_.fill(-1);
    auto& st = hw::ui_settings();
    offset_ = st.getIntValue("mixer_controller_offset", 0);
    const auto in = st.getValue("mixer_controller_in", ""), out = st.getValue("mixer_controller_out", "");
    if (in.isNotEmpty()) connect(in, out);
    startTimerHz(30);
}

MixerController::~MixerController()
{
    stopTimer();
    context_.engine.mixer_surface_input().configure_all({});
}

void MixerController::connect(const juce::String& input_name, const juce::String& output_name)
{
    input_name_ = input_name;
    output_name_ = output_name;
    auto& st = hw::ui_settings();
    st.setValue("mixer_controller_in", input_name);
    st.setValue("mixer_controller_out", output_name);
    st.saveIfNeeded();
    open_ports();
    refresh_all();
}

void MixerController::disconnect() { connect({}, {}); }

void MixerController::open_ports()
{
    // The input must be on in the device manager for its messages to arrive at all.
    input_id_ = input_id_for(input_name_);
    if (input_id_.isNotEmpty()) context_.engine.devices().setMidiInputDeviceEnabled(input_id_, true);
    context_.engine.mixer_surface_input().configure_all(input_id_);
    out_.reset();
    if (const auto id = output_id_for(output_name_); id.isNotEmpty()) out_ = juce::MidiOutput::openDevice(id);
}

juce::String MixerController::status() const
{
    if (input_name_.isEmpty()) return "No mixer controller chosen";
    const auto tracks = mixer_tracks();
    const juce::String where = tracks.empty() ? juce::String("no tracks yet")
                                              : "tracks " + juce::String(offset_ + 1) + "-" +
                                                    juce::String(std::min<int>(offset_ + mk::kStrips, static_cast<int>(tracks.size()))) +
                                                    " of " + juce::String(static_cast<int>(tracks.size()));
    if (input_id_.isEmpty()) return input_name_ + " is not connected (it is found again when plugged in)";
    return "Connected: " + input_name_ + (out_ || sender ? juce::String() : juce::String(" (no output: faders cannot follow)")) +
           " - " + where;
}

std::vector<std::string> MixerController::mixer_tracks() const
{
    std::vector<std::string> ids;
    for (const auto& t : context_.session.data().tracks)
        if (t.kind != project::TrackKind::Folder && t.kind != project::TrackKind::Video) ids.push_back(t.id);
    return ids;
}

std::array<std::string, mk::kStrips> MixerController::strip_tracks() const
{
    std::array<std::string, mk::kStrips> out{};
    const auto ids = mixer_tracks();
    for (int i = 0; i < mk::kStrips; ++i) {
        const auto k = static_cast<std::size_t>(offset_ + i);
        if (k < ids.size()) out[static_cast<std::size_t>(i)] = ids[k];
    }
    return out;
}

void MixerController::set_bank_offset(int offset)
{
    const int n = static_cast<int>(mixer_tracks().size());
    offset_ = std::max(0, std::min(offset, std::max(0, n - 1)));
    hw::ui_settings().setValue("mixer_controller_offset", offset_);
    refresh_all();
}

void MixerController::send(const mk::Bytes& b)
{
    if (sender) {
        sender(b);
        return;
    }
    if (!out_ || b.empty()) return;
    if (b.front() == 0xF0) out_->sendMessageNow(juce::MidiMessage::createSysExMessage(b.data() + 1, static_cast<int>(b.size()) - 2));
    else if (b.size() == 2) out_->sendMessageNow(juce::MidiMessage(b[0], b[1]));
    else out_->sendMessageNow(juce::MidiMessage(b[0], b[1], b[2]));
}

void MixerController::refresh_all()
{
    for (auto& s : sent_) s = {};
    lights_.fill(-1);
    tick();
}

void MixerController::handle(const mk::Event& e)
{
    auto& session = context_.session;
    auto& engine = context_.engine;
    const auto strips = strip_tracks();
    auto track_on = [&](int strip) -> std::string {
        return strip >= 0 && strip < mk::kStrips ? strips[static_cast<std::size_t>(strip)] : std::string();
    };
    switch (e.kind) {
        case mk::Event::Kind::Fader: {
            const double db = mk::fader_to_db(e.value);
            if (e.strip == mk::kMasterStrip) session.set_bus_gain("bus-master", std::max(-96.0, db));
            else if (const auto id = track_on(e.strip); !id.empty()) session.set_gain(id, std::max(-96.0, db));
            sent_[static_cast<std::size_t>(e.strip)].fader = e.value;  // where the hand put it
            break;
        }
        case mk::Event::Kind::FaderTouch:
            touched_[static_cast<std::size_t>(e.strip)] = e.pressed;
            break;
        case mk::Event::Kind::VPot:
            if (const auto id = track_on(e.strip); !id.empty())
                if (const auto* t = session.data().find_track(id))
                    session.set_pan(id, std::max(-1.0, std::min(1.0, t->pan + 0.02 * e.delta)));
            break;
        case mk::Event::Kind::Jog: {
            // A sixteenth per detent.
            const auto step = static_cast<Ticks>(kTicksPerQuarter / 4);
            const auto at = std::max<Ticks>(0, engine.position() + step * e.delta);
            engine.locate(at);
            break;
        }
        case mk::Event::Kind::Button: {
            if (!e.pressed) break;
            const int b = e.button;
            auto strip_key = [&](int first) { return b >= first && b < first + mk::kStrips ? b - first : -1; };
            if (const int s = strip_key(mk::RecArm1); s >= 0) {
                if (const auto id = track_on(s); !id.empty())
                    if (const auto* t = session.data().find_track(id)) session.set_record_armed(id, !t->record_armed);
            } else if (const int s2 = strip_key(mk::Solo1); s2 >= 0) {
                if (const auto id = track_on(s2); !id.empty())
                    if (const auto* t = session.data().find_track(id)) session.set_solo(id, !t->solo);
            } else if (const int s3 = strip_key(mk::Mute1); s3 >= 0) {
                if (const auto id = track_on(s3); !id.empty())
                    if (const auto* t = session.data().find_track(id)) session.set_mute(id, !t->mute);
            } else if (const int s4 = strip_key(mk::Select1); s4 >= 0) {
                if (const auto id = track_on(s4); !id.empty()) session.select_track(id);
            } else if (const int s5 = strip_key(mk::VPotPush1); s5 >= 0) {
                if (const auto id = track_on(s5); !id.empty()) session.set_pan(id, 0.0);
            } else {
                auto& cmds = context_.commands;
                switch (b) {
                    case mk::BankLeft: set_bank_offset(offset_ - mk::kStrips); break;
                    case mk::BankRight: set_bank_offset(offset_ + mk::kStrips); break;
                    case mk::ChannelLeft: set_bank_offset(offset_ - 1); break;
                    case mk::ChannelRight: set_bank_offset(offset_ + 1); break;
                    case mk::Play: engine.play(); break;
                    case mk::Stop: engine.stop(); break;
                    case mk::Record: cmds.invokeDirectly(cmd::toggleRecord, true); break;
                    case mk::Rewind: cmds.invokeDirectly(cmd::rewind, true); break;
                    case mk::FastForward: {
                        const auto& m = session.data().meter;
                        engine.locate(m.bar_start(m.bar_at(engine.position()) + 1));
                        break;
                    }
                    case mk::Cycle: cmds.invokeDirectly(cmd::toggleLoop, true); break;
                    case mk::Click: cmds.invokeDirectly(cmd::toggleMetronome, true); break;
                    case mk::Undo: cmds.invokeDirectly(cmd::undo, true); break;
                    case mk::Save: cmds.invokeDirectly(cmd::save, true); break;
                    case mk::Marker: cmds.invokeDirectly(cmd::addHit, true); break;
                    default: break;
                }
            }
            break;
        }
    }
}

void MixerController::tick()
{
    // What the surface sent.
    context_.engine.mixer_surface_input().drain([this](const SurfaceInput::Raw& r) {
        if (const auto e = mk::decode(r.status, r.data1, r.data2)) handle(*e);
    });
    // Found again when plugged back in.
    if (++ticks_ % 30 == 0 && input_name_.isNotEmpty() && input_id_.isEmpty() && input_id_for(input_name_).isNotEmpty()) {
        open_ports();
        refresh_all();
        return;
    }
    if (!out_ && !sender) return;

    // What the mix is now, sent where it changed.
    const auto& d = context_.session.data();
    const auto strips = strip_tracks();
    const auto selected = context_.session.selected_track();
    auto light = [this](int note, bool on) {
        const int v = on ? 1 : 0;
        if (lights_[static_cast<std::size_t>(note)] == v) return;
        lights_[static_cast<std::size_t>(note)] = v;
        send(mk::led(note, on ? mk::Led::On : mk::Led::Off));
    };
    const bool meters_now = ticks_ % 3 == 0;  // ~10 a second
    for (int i = 0; i < mk::kStrips; ++i) {
        auto& s = sent_[static_cast<std::size_t>(i)];
        const auto& id = strips[static_cast<std::size_t>(i)];
        const auto* t = id.empty() ? nullptr : d.find_track(id);
        const double fader = t ? mk::db_to_fader(t->gain_db) : 0.0;
        if (!touched_[static_cast<std::size_t>(i)] && std::abs(fader - s.fader) > 1e-4) {
            send(mk::fader(i, fader));
            s.fader = fader;
        }
        const double pan = t ? t->pan : 0.0;
        if (std::abs(pan - s.pan) > 1e-4) {
            send(t ? mk::vpot_pan(i, pan) : mk::Bytes{0xB0, static_cast<std::uint8_t>(0x30 + i), 0x00});
            s.pan = pan;
        }
        light(mk::Mute1 + i, t && t->mute);
        light(mk::Solo1 + i, t && t->solo);
        light(mk::RecArm1 + i, t && t->record_armed);
        light(mk::Select1 + i, t && t->id == selected);
        const std::string name = t ? mk::short_name(t->name) : std::string();
        if (name != s.name) {
            send(mk::lcd(i, 0, name));
            s.name = name;
        }
        const std::string value = t ? level_text(t->gain_db) : std::string();
        if (value != s.value) {
            send(mk::lcd(i, 1, value));
            s.value = value;
        }
        if (meters_now && t) {
            const auto m = context_.engine.take_deck_meter(id, 4);
            send(mk::meter(i, std::max(m.left, m.right)));
        }
    }
    // The master fader: the master bus.
    auto& master = sent_[mk::kMasterStrip];
    double master_db = 0.0;
    for (const auto& b : d.buses)
        if (b.id == "bus-master") master_db = b.gain_db;
    const double mf = mk::db_to_fader(master_db);
    if (!touched_[mk::kMasterStrip] && std::abs(mf - master.fader) > 1e-4) {
        send(mk::fader(mk::kMasterStrip, mf));
        master.fader = mf;
    }
    // Transport lights.
    auto& engine = context_.engine;
    light(mk::Play, engine.playing());
    light(mk::Stop, !engine.playing());
    light(mk::Record, engine.record_enabled());
    light(mk::Click, engine.metronome());
}

// --- The window ---------------------------------------------------------------------------------

namespace {

class ControllerPanel final : public juce::Component, private juce::Timer {
public:
    ControllerPanel(AppContext& c, MixerController& m) : context_(c), controller_(m)
    {
        title_.setText("Mixer controller (Mackie Control)", juce::dontSendNotification);
        title_.setFont(juce::FontOptions(15.0f, juce::Font::bold));
        addAndMakeVisible(title_);
        about_.setText("SSL UF8 / UF1, Avid S1, Icon Platform, Behringer X-Touch, PreSonus FaderPort, the Mackie "
                       "MCU and other controllers in Mackie Control mode. Set the controller to Mackie Control (MCU) "
                       "mode, then choose its MIDI ports here. Faders, V-Pots (pan), Rec / Solo / Mute / Select, "
                       "the scribble strips and meters follow the mixer's tracks, eight at a time (Bank and Channel "
                       "move them); the transport keys and jog wheel run the transport.",
                       juce::dontSendNotification);
        about_.setFont(juce::FontOptions(12.5f));
        about_.setColour(juce::Label::textColourId, colours::text_dim);
        about_.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(about_);
        for (auto* l : {&in_label_, &out_label_}) {
            l->setColour(juce::Label::textColourId, colours::text_dim);
            addAndMakeVisible(*l);
        }
        in_label_.setText("MIDI from it", juce::dontSendNotification);
        out_label_.setText("MIDI to it", juce::dontSendNotification);
        in_.setComponentID("controller_in");
        out_.setComponentID("controller_out");
        in_.onChange = out_.onChange = [this] { apply(); };
        addAndMakeVisible(in_);
        addAndMakeVisible(out_);
        status_.setComponentID("controller_status");
        status_.setColour(juce::Label::textColourId, colours::accent);
        addAndMakeVisible(status_);
        fill();
        startTimer(1000);
        setSize(560, 300);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(14, 10);
        title_.setBounds(r.removeFromTop(22));
        about_.setBounds(r.removeFromTop(92));
        r.removeFromTop(6);
        auto row = r.removeFromTop(28);
        in_label_.setBounds(row.removeFromLeft(100));
        in_.setBounds(row.reduced(0, 2));
        row = r.removeFromTop(28);
        out_label_.setBounds(row.removeFromLeft(100));
        out_.setBounds(row.reduced(0, 2));
        r.removeFromTop(8);
        status_.setBounds(r.removeFromTop(24));
    }
    void paint(juce::Graphics& g) override { g.fillAll(colours::background); }

private:
    void timerCallback() override
    {
        status_.setText(controller_.status(), juce::dontSendNotification);
        // Plugged or unplugged: the lists follow.
        juce::StringArray ins, outs;
        for (const auto& d : juce::MidiInput::getAvailableDevices()) ins.add(d.name);
        for (const auto& d : juce::MidiOutput::getAvailableDevices()) outs.add(d.name);
        if (ins != ins_ || outs != outs_) fill();
    }

    void fill()
    {
        ins_.clear();
        outs_.clear();
        for (const auto& d : juce::MidiInput::getAvailableDevices()) ins_.add(d.name);
        for (const auto& d : juce::MidiOutput::getAvailableDevices()) outs_.add(d.name);
        auto list = [](juce::ComboBox& box, const juce::StringArray& names, const juce::String& chosen) {
            box.clear(juce::dontSendNotification);
            box.addItem("(none)", 1);
            int id = 2, pick = 1;
            for (const auto& n : names) {
                box.addItem(n, id);
                if (n == chosen) pick = id;
                ++id;
            }
            if (chosen.isNotEmpty() && pick == 1) {
                box.addItem(chosen + " (not connected)", id);
                pick = id;
            }
            box.setSelectedId(pick, juce::dontSendNotification);
        };
        list(in_, ins_, controller_.input_name());
        list(out_, outs_, controller_.output_name());
        status_.setText(controller_.status(), juce::dontSendNotification);
    }

    void apply()
    {
        auto name = [](const juce::ComboBox& box) {
            return box.getSelectedId() <= 1 ? juce::String() : box.getText().upToFirstOccurrenceOf(" (not connected)", false, false);
        };
        controller_.connect(name(in_), name(out_));
        status_.setText(controller_.status(), juce::dontSendNotification);
        context_.status(controller_.status());
    }

    AppContext& context_;
    MixerController& controller_;
    juce::Label title_, about_, in_label_, out_label_, status_;
    juce::ComboBox in_, out_;
    juce::StringArray ins_, outs_;
};

juce::Component::SafePointer<dialogs::ToolWindow>& controller_window()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}

}  // namespace

void show_mixer_controller_window(AppContext& context, MixerController& controller)
{
    dialogs::show_tool(controller_window(), "Mixer Controller", 560, 300,
                       [&context, &controller] { return new ControllerPanel(context, controller); });
}

}  // namespace vcc::desktop
