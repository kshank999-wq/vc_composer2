#include "ui/surface/ControlSurface.h"

#include <cmath>

#include "ui/Dialogs.h"
#include "ui/hw/HwControls.h"
#include "ui/hw/HwTheme.h"

namespace vcc::desktop::surface_ui {

namespace sf = project::surface;

namespace {

juce::Component::SafePointer<dialogs::ToolWindow>& window_slot()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> slot;
    return slot;
}
juce::Component*& panel_slot()
{
    static juce::Component* p = nullptr;
    return p;
}

double now_seconds() { return juce::Time::getMillisecondCounterHiRes() * 0.001; }

juce::String encode(const sf::Profile& p)
{
    juce::StringArray parts;
    for (const auto& b : p.bindings)
        parts.add(juce::String(sf::to_string(b.action)) + ":" + juce::String(static_cast<int>(b.source.kind)) + ":" +
                  juce::String(b.source.channel) + ":" + juce::String(b.source.number) + ":" + sf::to_string(b.encoding));
    return parts.joinIntoString(";");
}

std::vector<sf::Binding> decode_bindings(const juce::String& text)
{
    std::vector<sf::Binding> out;
    for (const auto& part : juce::StringArray::fromTokens(text, ";", {})) {
        const auto f = juce::StringArray::fromTokens(part, ":", {});
        if (f.size() != 5) continue;
        const auto action = sf::action_from_string(f[0].toStdString());
        const auto encoding = sf::encoding_from_string(f[4].toStdString());
        const int kind = f[1].getIntValue();
        if (!action || !encoding || kind < 0 || kind > 2) continue;
        sf::Binding b;
        b.action = *action;
        b.source.kind = static_cast<sf::Source::Kind>(kind);
        b.source.channel = juce::jlimit(0, 16, f[2].getIntValue());
        b.source.number = juce::jlimit(0, 127, f[3].getIntValue());
        b.encoding = *encoding;
        out.push_back(b);
    }
    return out;
}

}  // namespace

ControlSurface::ControlSurface(AppContext& context) : context_(context)
{
    load();
    startTimerHz(60);
}

ControlSurface::~ControlSurface()
{
    stopTimer();
    context_.engine.surface_input().configure({}, {}, false);
    // The window's panel refers to this controller: it goes first.
    if (auto* w = window_slot().getComponent()) delete w;
}

void ControlSurface::load()
{
    auto& s = hw::ui_settings();
    device_id_ = s.getValue("surface_device");
    device_name_ = s.getValue("surface_device_name");
    const juce::String profile = s.getValue("surface_profile", "Generic MIDI (CC 16-23)");
    profile_ = sf::builtin_profiles().front();
    bool found = false;
    for (const auto& p : sf::builtin_profiles())
        if (profile == juce::String(p.name)) {
            profile_ = p;
            found = true;
        }
    if (!found || profile_.name == "Custom") {
        profile_ = sf::builtin_profiles().back();
        profile_.bindings = decode_bindings(s.getValue("surface_custom"));
    }
    auto& st = jog_.settings;
    st.step = sf::step_from_string(s.getValue("surface_step", "beat").toStdString()).value_or(sf::Step::Beat);
    st.detents_per_step = juce::jlimit(1, 8, s.getIntValue("surface_sensitivity", 1));
    st.reverse = s.getBoolValue("surface_reverse", false);
    st.acceleration = s.getBoolValue("surface_acceleration", true);
    st.dead_zone = juce::jlimit(0.0, 0.5, s.getDoubleValue("surface_dead_zone", 0.08));
    st.curve = s.getValue("surface_curve", "exponential") == "linear" ? sf::Curve::Linear : sf::Curve::Exponential;
    st.max_speed = juce::jlimit(0.5, 32.0, s.getDoubleValue("surface_max_speed", 8.0));
    st.audible = s.getBoolValue("surface_audible", true);
    scan_devices();
    configure_input();
}

void ControlSurface::save()
{
    auto& s = hw::ui_settings();
    s.setValue("surface_device", device_id_);
    s.setValue("surface_device_name", device_name_);
    s.setValue("surface_profile", juce::String(profile_.name));
    if (profile_.name == "Custom") s.setValue("surface_custom", encode(profile_));
    const auto& st = jog_.settings;
    s.setValue("surface_step", juce::String(sf::to_string(st.step)));
    s.setValue("surface_sensitivity", st.detents_per_step);
    s.setValue("surface_reverse", st.reverse);
    s.setValue("surface_acceleration", st.acceleration);
    s.setValue("surface_dead_zone", st.dead_zone);
    s.setValue("surface_curve", juce::String(st.curve == sf::Curve::Linear ? "linear" : "exponential"));
    s.setValue("surface_max_speed", st.max_speed);
    s.setValue("surface_audible", st.audible);
    s.saveIfNeeded();
}

void ControlSurface::configure_input()
{
    context_.engine.surface_input().configure(device_id_, profile_, learning_);
}

void ControlSurface::set_device(const juce::String& identifier, const juce::String& name)
{
    device_id_ = identifier;
    device_name_ = name;
    jog_.reset();
    connected_ = false;
    scan_devices();
    configure_input();
    save();
    if (on_changed) on_changed();
}

void ControlSurface::scan_devices()
{
    if (device_id_.isEmpty() && device_name_.isEmpty()) {
        connected_ = false;
        return;
    }
    bool present = false;
    for (const auto& d : juce::MidiInput::getAvailableDevices()) {
        if (d.identifier == device_id_) present = true;
        else if (!present && device_name_.isNotEmpty() && d.name == device_name_) {
            // Plugged back in under a new identifier: the same device.
            device_id_ = d.identifier;
            present = true;
            configure_input();
        }
        if (present) {
            context_.engine.enable_midi_input(device_id_);
            break;
        }
    }
    if (present == connected_) return;
    connected_ = present;
    if (!present) {
        // Unplugged mid-shuttle: stop where it is, never run on.
        jog_.reset();
        say(device_name_ + " is not connected");
    } else {
        say(device_name_ + " connected");
    }
    if (on_changed) on_changed();
}

void ControlSurface::use_profile(const juce::String& name)
{
    for (const auto& p : sf::builtin_profiles())
        if (name == juce::String(p.name)) {
            if (p.name == "Custom") {
                profile_.name = "Custom";
                profile_.bindings = decode_bindings(hw::ui_settings().getValue("surface_custom"));
            } else {
                profile_ = p;
            }
        }
    jog_.reset();
    configure_input();
    save();
    if (on_changed) on_changed();
}

void ControlSurface::bind(sf::Action action, const sf::Source& source, sf::Encoding encoding)
{
    if (profile_.name != "Custom") profile_.name = "Custom";  // a built-in, edited: the user's own from here
    // One control does one thing: a source bound elsewhere moves here.
    auto& b = profile_.bindings;
    b.erase(std::remove_if(b.begin(), b.end(), [&](const sf::Binding& x) { return x.action == action || x.source == source; }),
            b.end());
    b.push_back({action, source, encoding});
    configure_input();
    save();
    if (on_changed) on_changed();
}

void ControlSurface::unbind(sf::Action action)
{
    if (profile_.name != "Custom") profile_.name = "Custom";
    auto& b = profile_.bindings;
    b.erase(std::remove_if(b.begin(), b.end(), [&](const sf::Binding& x) { return x.action == action; }), b.end());
    configure_input();
    save();
    if (on_changed) on_changed();
}

void ControlSurface::learn(sf::Action action)
{
    learning_ = true;
    learn_action_ = action;
    configure_input();
    say(juce::String("Move the control for ") + sf::display_name(action) + " on " +
        (device_name_.isNotEmpty() ? device_name_ : juce::String("the device")));
    if (on_changed) on_changed();
}

void ControlSurface::cancel_learn()
{
    learning_ = false;
    configure_input();
    if (on_changed) on_changed();
}

void ControlSurface::say(const juce::String& text)
{
    last_action_ = text;
    if (context_.status) context_.status(text);
}

void ControlSurface::inject(const juce::MidiMessage& m)
{
    context_.engine.surface_input().take(device_id_, m);
}

void ControlSurface::timerCallback()
{
    process(now_seconds());
}

bool ControlSurface::audible_ok() const
{
    if (!jog_.settings.audible || context_.engine.recording() || context_.engine.record_enabled()) return false;
    // A strip writing automation would write while the scrub plays.
    const auto& d = context_.session.data();
    for (const auto& t : d.tracks)
        if (t.automation_mode == "write") return false;
    for (const auto& b : d.buses)
        if (b.automation_mode == "write") return false;
    return true;
}

void ControlSurface::end_varispeed()
{
    if (!varispeed_mode_) return;
    varispeed_mode_ = false;
    context_.engine.stop();  // back to normal speed too
    scrub_ = context_.engine.position_seconds();
}

void ControlSurface::move_to(double seconds, bool on_grid)
{
    scrub_ = std::max(0.0, seconds);
    pending_move_ = true;
    pending_on_grid_ = on_grid;
}

void ControlSurface::jog(int detents)
{
    const auto t = sf::timeline_of(context_.session.data());
    const double now = now_seconds();
    if (context_.engine.recording()) {
        say("The jog does not move the playhead while recording");
        return;
    }
    if (now > active_until_) scrub_ = context_.engine.position_seconds();
    move_to(jog_.jog(scrub_, detents, now, t), jog_.settings.step != sf::Step::Frame);
    active_until_ = now + 0.4;
}

void ControlSurface::handle(const sf::Event& e, double now)
{
    auto& engine = context_.engine;
    const auto& d = context_.session.data();
    const auto t = sf::timeline_of(d);
    if (now > active_until_ && !jog_.shuttling()) scrub_ = engine.position_seconds();
    if (engine.recording() && (e.action == sf::Action::Jog || e.action == sf::Action::Shuttle ||
                               e.action == sf::Action::NextMarker || e.action == sf::Action::PreviousMarker ||
                               e.action == sf::Action::ReturnToStart)) {
        jog_.reset();
        say("The surface does not move the playhead while recording");
        return;
    }
    switch (e.action) {
        case sf::Action::Jog: {
            move_to(jog_.jog(scrub_, e.steps, now, t), jog_.settings.step != sf::Step::Frame);
            active_until_ = now + 0.4;
            const Ticks at = d.tempo.tick_at_seconds(scrub_);
            const int bar = d.meter.bar_at(at);
            const auto sig = d.meter.at_bar(bar);
            const int beat = static_cast<int>((at - d.meter.bar_start(bar)) / sig.beat_ticks()) + 1;
            last_action_ = "Jog: bar " + juce::String(bar) + " beat " + juce::String(beat);
            return;
        }
        case sf::Action::Shuttle: {
            const bool was = jog_.shuttling();
            jog_.set_shuttle(e.value);
            // A stepped seek, never a race with playback: the shuttle stops play.
            if (jog_.shuttling() && !was) {
                if (engine.playing()) engine.stop();
                scrub_ = engine.position_seconds();
                last_now_ = now;
            }
            if (!jog_.shuttling()) end_varispeed();
            last_action_ = jog_.shuttling() ? "Shuttle " + juce::String(jog_.shuttle_speed(), 2) + "x" : juce::String("Shuttle at rest");
            return;
        }
        default: break;
    }
    // Keys: a transport key also brings the shuttle to rest.
    jog_.reset();
    end_varispeed();
    switch (e.action) {
        case sf::Action::PlayPause: engine.toggle_play(); last_action_ = engine.playing() ? "Play" : "Stop"; break;
        case sf::Action::Play:
            if (!engine.playing()) engine.play();
            last_action_ = "Play";
            break;
        case sf::Action::Stop: engine.stop(); last_action_ = "Stop"; break;
        case sf::Action::ReturnToStart: move_to(0.0, true); last_action_ = "Return to start"; break;
        case sf::Action::NextMarker:
        case sf::Action::PreviousMarker: {
            const auto to = e.action == sf::Action::NextMarker ? sf::next_marker(t, scrub_) : sf::previous_marker(t, scrub_);
            if (to) {
                move_to(*to);
                last_action_ = e.action == sf::Action::NextMarker ? "Next marker" : "Previous marker";
            } else {
                last_action_ = e.action == sf::Action::NextMarker ? "No marker after the playhead" : "No marker before the playhead";
            }
            break;
        }
        case sf::Action::ToggleLoop: context_.commands.invokeDirectly(cmd::toggleLoop, false); last_action_ = "Loop"; break;
        case sf::Action::Jog:
        case sf::Action::Shuttle: break;
    }
    active_until_ = now + 0.4;
}

void ControlSurface::process(double now)
{
    if (now - last_scan_ > 1.0) {
        last_scan_ = now;
        scan_devices();
    }
    // What arrived: learning takes the first control; otherwise decode.
    context_.engine.surface_input().drain([&](const SurfaceInput::Raw& r) {
        ++messages_;
        if (learning_) {
            const auto src = sf::source_of(r.status, r.data1, r.data2);
            if (!src) return;
            sf::Encoding enc = sf::Encoding::Absolute;
            if (learn_action_ == sf::Action::Jog) {
                // Relative: the first value tells the encoding (1 forward,
                // 65 Mackie back, 127 two's-complement back).
                enc = r.data2 == 65 || (r.data2 > 64 && r.data2 < 80) ? sf::Encoding::SignMagnitude
                                                                    : sf::Encoding::TwosComplement;
                if (src->kind != sf::Source::Kind::Controller) return;
            }
            if (learn_action_ == sf::Action::Shuttle && src->kind == sf::Source::Kind::Note) return;
            learning_ = false;
            bind(learn_action_, *src, enc);
            say("Learned " + juce::String(sf::describe(*src)) + " for " + sf::display_name(learn_action_));
            return;
        }
        if (const auto e = sf::decode(profile_, r.status, r.data1, r.data2)) handle(*e, now);
    });
    // The shuttle: travel since the last refresh, held within the project.
    const double speed = jog_.shuttle_speed();
    if (jog_.shuttling() && speed >= 0.25 && speed <= 4.0 && audible_ok()) {
        // Heard: the mix plays at the shuttle's speed; the engine moves the playhead.
        auto& engine = context_.engine;
        if (!varispeed_mode_) {
            engine.play();
            varispeed_mode_ = true;
        }
        engine.set_varispeed(speed);
        scrub_ = engine.position_seconds();
        const auto t = sf::timeline_of(context_.session.data());
        if (scrub_ >= t.end_seconds) {
            end_varispeed();
            jog_.reset();
            last_action_ = "Shuttle: at the end";
        }
        active_until_ = now + 0.4;
        last_now_ = now;
        if (on_moved) on_moved(engine.position(), false);
        return;
    }
    if (varispeed_mode_) end_varispeed();  // too fast, backwards, or no longer allowed
    if (jog_.shuttling()) {
        if (context_.engine.recording()) {
            jog_.reset();
        } else {
            const double dt = last_now_ < 0.0 ? 0.0 : now - last_now_;
            const auto t = sf::timeline_of(context_.session.data());
            const double to = jog_.advance(scrub_, dt, t);
            if (std::abs(to - scrub_) > 1e-9) move_to(to);
            active_until_ = now + 0.4;
            if ((to <= 0.0 && jog_.shuttle_speed() < 0) || (to >= t.end_seconds && jog_.shuttle_speed() > 0))
                last_action_ = to <= 0.0 ? "Shuttle: at the start" : "Shuttle: at the end";
        }
    }
    last_now_ = now;
    // One locate per refresh, however many detents arrived.
    if (pending_move_) {
        pending_move_ = false;
        const Ticks tick = context_.session.data().tempo.tick_at_seconds(scrub_);
        auto& engine = context_.engine;
        const bool user_playing = engine.playing() && !engine.grain_playing();
        // Heard: a short grain from where it landed (at most one per 60 ms).
        // (The forward shuttle past 4x stays silent: grains are for the jog
        // and for going backwards.)
        const bool grains = !jog_.shuttling() || jog_.shuttle_speed() < 0.0;
        if (grains && !user_playing && audible_ok() && (last_grain_ < 0.0 || now - last_grain_ >= 0.06 || now < last_grain_)) {
            engine.scrub_grain(tick, 0.09);
            last_grain_ = now;
        } else {
            engine.locate(tick);
        }
        if (on_moved) on_moved(tick, pending_on_grid_);
    }
}

// --- The window --------------------------------------------------------------------------------

namespace {

class Panel final : public juce::Component, private juce::Timer {
public:
    explicit Panel(ControlSurface& surface) : surface_(surface)
    {
        setComponentID("control_surfaces");
        panel_slot() = this;
        auto label = [this](juce::Label& l, const juce::String& text) {
            l.setText(text, juce::dontSendNotification);
            l.setColour(juce::Label::textColourId, hw::tok::text);
            addAndMakeVisible(l);
        };
        label(device_label_, "Device");
        label(profile_label_, "Profile");
        label(step_label_, "Jog step");
        label(sens_label_, "Detents per step");
        label(dead_label_, "Shuttle dead zone");
        label(curve_label_, "Shuttle curve");
        label(speed_label_, "Shuttle top speed");
        status_.setColour(juce::Label::textColourId, hw::tok::text_bright);
        addAndMakeVisible(status_);
        note_.setColour(juce::Label::textColourId, hw::tok::text.withAlpha(0.7f));
        note_.setFont(hw::fonts::ui(11.0f, false));
        note_.setText("Any MIDI controller can be mapped (Learn). HID shuttles that type keys with their vendor's "
                      "software: assign Jog Forward / Jog Back in Keyboard Shortcuts. Native HID support depends on "
                      "the device models chosen for testing.",
                      juce::dontSendNotification);
        addAndMakeVisible(note_);

        device_.setTitle("MIDI device");
        device_.onChange = [this] {
            const int i = device_.getSelectedItemIndex();
            if (i <= 0) surface_.set_device({}, {});
            else if (i - 1 < devices_.size()) surface_.set_device(devices_[i - 1].identifier, devices_[i - 1].name);
        };
        addAndMakeVisible(device_);
        profile_.setTitle("Profile");
        for (const auto& p : sf::builtin_profiles()) profile_.addItem(p.name, profile_.getNumItems() + 1);
        profile_.onChange = [this] { surface_.use_profile(profile_.getText()); };
        addAndMakeVisible(profile_);

        step_.setTitle("Jog step");
        for (auto s : {sf::Step::Frame, sf::Step::Sixteenth, sf::Step::Beat, sf::Step::Bar})
            step_.addItem(s == sf::Step::Frame ? "Frame" : s == sf::Step::Sixteenth ? "Sixteenth" : s == sf::Step::Beat ? "Beat" : "Bar",
                          static_cast<int>(s) + 1);
        step_.onChange = [this] {
            surface_.settings().step = static_cast<sf::Step>(step_.getSelectedId() - 1);
            surface_.save();
        };
        addAndMakeVisible(step_);
        sens_.setTitle("Jog sensitivity");
        sens_.setRange(1, 8, 1);
        sens_.onValueChange = [this] {
            surface_.settings().detents_per_step = static_cast<int>(sens_.getValue());
            surface_.save();
        };
        dead_.setTitle("Shuttle dead zone");
        dead_.setRange(0.0, 0.5, 0.01);
        dead_.onValueChange = [this] {
            surface_.settings().dead_zone = dead_.getValue();
            surface_.save();
        };
        speed_.setTitle("Shuttle top speed");
        speed_.setRange(0.5, 32.0, 0.5);
        speed_.setTextValueSuffix("x");
        speed_.onValueChange = [this] {
            surface_.settings().max_speed = speed_.getValue();
            surface_.save();
        };
        for (auto* s : {&sens_, &dead_, &speed_}) addAndMakeVisible(*s);
        curve_.setTitle("Shuttle curve");
        curve_.addItem("Exponential", 1);
        curve_.addItem("Linear", 2);
        curve_.onChange = [this] {
            surface_.settings().curve = curve_.getSelectedId() == 2 ? sf::Curve::Linear : sf::Curve::Exponential;
            surface_.save();
        };
        addAndMakeVisible(curve_);
        reverse_.onClick = [this] {
            surface_.settings().reverse = reverse_.getToggleState();
            surface_.save();
        };
        accel_.onClick = [this] {
            surface_.settings().acceleration = accel_.getToggleState();
            surface_.save();
        };
        addAndMakeVisible(reverse_);
        addAndMakeVisible(accel_);
        audible_.setTooltip("Hear the scrub: the shuttle forward up to 4x plays at its speed; the jog and reverse play "
                            "short grains. Silent while recording, armed, or writing automation.");
        audible_.onClick = [this] {
            surface_.settings().audible = audible_.getToggleState();
            surface_.save();
        };
        addAndMakeVisible(audible_);

        for (auto a : sf::all_actions()) {
            auto row = std::make_unique<Row>();
            row->action = a;
            row->name.setText(sf::display_name(a), juce::dontSendNotification);
            row->name.setColour(juce::Label::textColourId, hw::tok::text_bright);
            row->source.setColour(juce::Label::textColourId, hw::tok::text);
            row->learn.setComponentID(juce::String("surface_learn_") + sf::to_string(a));
            row->learn.onClick = [this, a] {
                if (surface_.learning() && surface_.learning_action() == a) surface_.cancel_learn();
                else surface_.learn(a);
            };
            row->clear.onClick = [this, a] { surface_.unbind(a); };
            for (juce::Component* c : std::initializer_list<juce::Component*>{&row->name, &row->source, &row->learn, &row->clear})
                addAndMakeVisible(*c);
            rows_.push_back(std::move(row));
        }
        surface_.on_changed = [safe = juce::Component::SafePointer<Panel>(this)] {
            if (safe) safe->load();
        };
        load();
        startTimerHz(20);
    }

    ~Panel() override
    {
        stopTimer();
        if (panel_slot() == this) panel_slot() = nullptr;
    }

    void load()
    {
        devices_ = juce::MidiInput::getAvailableDevices();
        device_.clear(juce::dontSendNotification);
        device_.addItem("None", 1);
        int selected = 1;
        for (int i = 0; i < devices_.size(); ++i) {
            device_.addItem(devices_[i].name, i + 2);
            if (devices_[i].identifier == surface_.device_id()) selected = i + 2;
        }
        if (selected == 1 && surface_.device_name().isNotEmpty()) {
            // Remembered but unplugged: shown, so the choice is not lost.
            device_.addItem(surface_.device_name() + " (not connected)", 1000);
            selected = 1000;
        }
        device_.setSelectedId(selected, juce::dontSendNotification);
        for (int i = 0; i < profile_.getNumItems(); ++i)
            if (profile_.getItemText(i) == juce::String(surface_.profile().name))
                profile_.setSelectedItemIndex(i, juce::dontSendNotification);
        const auto& st = surface_.settings();
        step_.setSelectedId(static_cast<int>(st.step) + 1, juce::dontSendNotification);
        sens_.setValue(st.detents_per_step, juce::dontSendNotification);
        dead_.setValue(st.dead_zone, juce::dontSendNotification);
        speed_.setValue(st.max_speed, juce::dontSendNotification);
        curve_.setSelectedId(st.curve == sf::Curve::Linear ? 2 : 1, juce::dontSendNotification);
        reverse_.setToggleState(st.reverse, juce::dontSendNotification);
        accel_.setToggleState(st.acceleration, juce::dontSendNotification);
        audible_.setToggleState(st.audible, juce::dontSendNotification);
        for (auto& r : rows_) {
            const auto* b = surface_.profile().find(r->action);
            juce::String text = b ? juce::String(sf::describe(b->source)) : juce::String("-");
            if (b && r->action == sf::Action::Jog)
                text << "  (" << (b->encoding == sf::Encoding::SignMagnitude ? "Mackie" : b->encoding == sf::Encoding::BinaryOffset ? "offset 64" : "two's complement") << ")";
            const bool listening = surface_.learning() && surface_.learning_action() == r->action;
            r->source.setText(listening ? juce::String("Move the control...") : text, juce::dontSendNotification);
            r->learn.setButtonText(listening ? "Cancel" : "Learn");
        }
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(hw::tok::panel_bottom);
        // The shuttle: where the ring is, and the speed.
        auto r = meter_area_.toFloat();
        g.setColour(juce::Colour(0xff111315));
        g.fillRoundedRectangle(r, 3.0f);
        const float mid = r.getCentreX();
        const auto& st = surface_.state();
        const float dz = static_cast<float>(st.settings.dead_zone) * r.getWidth() / 2;
        g.setColour(juce::Colour(0xff23282d));
        g.fillRect(juce::Rectangle<float>(mid - dz, r.getY(), dz * 2, r.getHeight()));
        const float x = mid + static_cast<float>(st.shuttle_raw()) * r.getWidth() / 2;
        g.setColour(st.shuttling() ? hw::Theme::get().lcd() : hw::tok::text.withAlpha(0.6f));
        g.fillRect(juce::Rectangle<float>(std::min(mid, x), r.getY() + 3, std::abs(x - mid) + 2, r.getHeight() - 6));
        g.setColour(hw::tok::text_bright);
        g.setFont(hw::fonts::ui(11.0f, true));
        g.drawText(st.shuttling() ? "SHUTTLE " + juce::String(st.shuttle_speed(), 2) + "x" : juce::String("SHUTTLE AT REST"),
                   r, juce::Justification::centred, false);
        g.setColour(hw::tok::text);
        g.setFont(hw::fonts::ui(12.0f, true));
        g.drawText("Controls", controls_title_, juce::Justification::centredLeft, false);
        g.drawText("Jog and shuttle", settings_title_, juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced(14);
        auto row = [&](juce::Label& l, juce::Component& c, int h = 26) {
            auto r = b.removeFromTop(h);
            l.setBounds(r.removeFromLeft(150));
            c.setBounds(r);
            b.removeFromTop(6);
        };
        row(device_label_, device_);
        status_.setBounds(b.removeFromTop(20));
        b.removeFromTop(4);
        row(profile_label_, profile_);
        controls_title_ = b.removeFromTop(22);
        for (auto& r : rows_) {
            auto line = b.removeFromTop(24);
            r->name.setBounds(line.removeFromLeft(150));
            r->clear.setBounds(line.removeFromRight(58));
            line.removeFromRight(6);
            r->learn.setBounds(line.removeFromRight(70));
            r->source.setBounds(line);
            b.removeFromTop(3);
        }
        b.removeFromTop(8);
        settings_title_ = b.removeFromTop(22);
        row(step_label_, step_);
        row(sens_label_, sens_);
        auto toggles = b.removeFromTop(24);
        reverse_.setBounds(toggles.removeFromLeft(200));
        accel_.setBounds(toggles.removeFromLeft(160));
        audible_.setBounds(toggles);
        b.removeFromTop(6);
        row(dead_label_, dead_);
        row(curve_label_, curve_);
        row(speed_label_, speed_);
        meter_area_ = b.removeFromTop(26);
        b.removeFromTop(8);
        note_.setBounds(b.removeFromTop(48));
    }

private:
    struct Row {
        sf::Action action = sf::Action::Jog;
        juce::Label name, source;
        juce::TextButton learn{"Learn"}, clear{"Clear"};
    };

    void timerCallback() override
    {
        juce::String text = surface_.device_id().isEmpty() && surface_.device_name().isEmpty()
                                ? juce::String("No device chosen")
                                : surface_.connected() ? "Connected  -  " + juce::String(surface_.messages()) + " messages"
                                                       : juce::String("Not connected: plug it in and it is found again");
        if (surface_.last_action().isNotEmpty()) text << "  -  " << surface_.last_action();
        if (status_.getText() != text) status_.setText(text, juce::dontSendNotification);
        repaint(meter_area_);
    }

    ControlSurface& surface_;
    juce::Array<juce::MidiDeviceInfo> devices_;
    juce::Label device_label_, profile_label_, step_label_, sens_label_, dead_label_, curve_label_, speed_label_, status_, note_;
    juce::ComboBox device_, profile_, step_, curve_;
    juce::Slider sens_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight}, dead_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight},
        speed_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::ToggleButton reverse_{"Reverse direction"}, accel_{"Acceleration"}, audible_{"Audible scrub"};
    std::vector<std::unique_ptr<Row>> rows_;
    juce::Rectangle<int> meter_area_, controls_title_, settings_title_;
};

}  // namespace

void show_control_surfaces(AppContext& context, ControlSurface& surface)
{
    juce::ignoreUnused(context);
    dialogs::show_tool(window_slot(), "Control Surfaces", 640, 760, [&surface] { return new Panel(surface); },
                       [&surface] { surface.on_changed = nullptr; });
}

juce::Component* control_surfaces_panel() { return panel_slot(); }

}  // namespace vcc::desktop::surface_ui
