#include "ui/shell/ShellPanels.h"

#include <algorithm>
#include <cmath>

#include "ui/TrackPanel.h"
#include "ui/console/ConsoleStyle.h"
#include "ui/shell/TopDeck.h"

namespace vcc::desktop::shell {

namespace {
juce::String pitch_name(const notation::Pitch& p)
{
    static const char* steps = "CDEFGAB";
    juce::String s = juce::String::charToString(static_cast<juce::juce_wchar>(steps[static_cast<int>(p.step)]));
    for (int i = 0; i < p.alteration; ++i) s << "#";
    for (int i = 0; i > p.alteration; --i) s << "b";
    return s + juce::String(p.octave);
}
}  // namespace

// --- Toolbar -----------------------------------------------------------------------------------

hw::Keycap& Toolbar::add(Group& g, const juce::String& legend, int width, bool toggles)
{
    keys_.push_back(std::make_unique<hw::Keycap>(legend, toggles));
    auto& k = *keys_.back();
    k.set_legend_size(11.5f);
    addChildComponent(k);
    g.keys.push_back(&k);
    g.widths.push_back(width);
    return k;
}

Toolbar::Toolbar(AppContext& context) : context_(context)
{
    auto invoke = [this](juce::CommandID id) { return [this, id] { context_.commands.invokeDirectly(id, true); }; };

    // Composer view.
    {
        Group view{"View"};
        auto& track = add(view, "Track", 64, true);
        track.setToggleState(true, juce::dontSendNotification);
        track.setTooltip("Track view: one staff per track");
        auto& page = add(view, "Page", 58);
        page.setEnabled(false);
        page.setTooltip("Page view (coming with the Score stage)");
        add(view, "Mixer", 62).onClick = invoke(cmd::showMixer);
        groups_.push_back(view);
        Group zoom{"Zoom"};
        add(zoom, "-", 36).onClick = [this] {
            if (on_zoom) on_zoom(false);
        };
        add(zoom, "+", 36).onClick = [this] {
            if (on_zoom) on_zoom(true);
        };
        groups_.push_back(zoom);
        Group tools{"Tools"};
        add(tools, "Chords", 70).onClick = invoke(cmd::chordExplorer);
        add(tools, "Ostinato", 78).onClick = invoke(cmd::ostinatoCreator);
        groups_.push_back(tools);
        Group picture{"Picture"};
        add(picture, "Picture", 70).onClick = invoke(cmd::showVideo);
        add(picture, "Cues", 56).onClick = invoke(cmd::showCueManager);
        groups_.push_back(picture);
    }
    // Producer view.
    {
        Group tracks{"Tracks"};
        tracks.producer = true;
        auto& inst = add(tracks, "+ Instrument", 104);
        inst.onClick = [this, &inst] { TrackPanel::show_add_track_menu(context_, &inst); };
        add(tracks, "+ Audio", 74).onClick = invoke(cmd::addAudioTrack);
        add(tracks, "+ Aux", 62).onClick = invoke(cmd::addAuxBus);
        groups_.push_back(tracks);
        Group record{"Record"};
        record.producer = true;
        auto& punch = add(record, "Punch", 64, false);
        punch.set_led_dot(true);
        punch.setComponentID("transport_punch");
        punch.setTooltip("Punch in/out: record only inside the range dragged on the ruler");
        punch.onClick = invoke(cmd::togglePunch);
        auto& pdc = add(record, "PDC", 104, false);
        pdc.set_led_dot(true);
        pdc.setComponentID("transport_pdc");
        pdc.setTooltip("Plug-in delay compensation: delays everything to line up with the slowest plug-in path");
        pdc.onClick = [this] { context_.engine.set_delay_compensation(!context_.engine.delay_compensation()); };
        groups_.push_back(record);
        Group tools{"Tools"};
        tools.producer = true;
        auto& pencil = add(tools, "Pencil", 66, false);
        pencil.set_led_dot(true);
        pencil.setTooltip("Pencil (P): draw automation freehand");
        pencil.onClick = [this, &pencil] {
            if (on_pencil) on_pencil(!pencil.getToggleState());
        };
        add(tools, "Chords", 70).onClick = invoke(cmd::chordExplorer);
        add(tools, "Ostinato", 78).onClick = invoke(cmd::ostinatoCreator);
        groups_.push_back(tools);
        Group view{"View"};
        view.producer = true;
        add(view, "Mixer", 62).onClick = invoke(cmd::showMixer);
        add(view, "Picture", 70).onClick = invoke(cmd::showVideo);
        add(view, "Cues", 56).onClick = invoke(cmd::showCueManager);
        groups_.push_back(view);
        Group zoom{"Zoom"};
        zoom.producer = true;
        add(zoom, "-", 36).onClick = [this] {
            if (on_zoom) on_zoom(false);
        };
        add(zoom, "+", 36).onClick = [this] {
            if (on_zoom) on_zoom(true);
        };
        groups_.push_back(zoom);
    }
    set_producer(false);
}

hw::Keycap* Toolbar::key(const juce::String& legend)
{
    for (const auto& g : groups_)
        if (g.producer == producer_)
            for (auto* k : g.keys)
                if (k->getButtonText() == legend) return k;
    return nullptr;
}

void Toolbar::set_producer(bool producer)
{
    producer_ = producer;
    for (const auto& g : groups_)
        for (auto* k : g.keys) k->setVisible(g.producer == producer);
    resized();
    repaint();
}

void Toolbar::update(bool punch, bool pencil)
{
    if (auto* p = key("Punch")) p->setToggleState(punch, juce::dontSendNotification);
    if (auto* p = key("Pencil")) p->setToggleState(pencil, juce::dontSendNotification);
    // Delay compensation, and how much it adds.
    for (const auto& g : groups_)
        for (auto* k : g.keys)
            if (k->getComponentID() == "transport_pdc") {
                const bool on = context_.engine.delay_compensation();
                juce::String text = "PDC";
                if (on) {
                    const int samples = context_.engine.compensation_samples();
                    double rate = 48000.0;
                    if (auto* d = context_.engine.devices().getCurrentAudioDevice()) rate = d->getCurrentSampleRate();
                    if (samples > 0 && rate > 0.0) text << " " << juce::String(1000.0 * samples / rate, 1) << " ms";
                } else {
                    text << " off";
                }
                k->setToggleState(on, juce::dontSendNotification);
                if (k->getButtonText() != text) k->setButtonText(text);
            }
}

void Toolbar::resized()
{
    int x = 14;
    for (const auto& g : groups_) {
        if (g.producer != producer_) continue;
        for (std::size_t i = 0; i < g.keys.size(); ++i) {
            g.keys[i]->setBounds(x, 26, g.widths[i], 26);
            x += g.widths[i] + 6;
        }
        x += 22;
    }
}

void Toolbar::paint(juce::Graphics& g) { hw::paint::panel(g, getLocalBounds().toFloat().reduced(1.0f).withTrimmedBottom(2.0f)); }

void Toolbar::paintOverChildren(juce::Graphics& g)
{
    bool first = true;
    for (const auto& grp : groups_) {
        if (grp.producer != producer_ || grp.keys.empty()) continue;
        const float x = static_cast<float>(grp.keys.front()->getX());
        hw::paint::silk(g, grp.label, {x, 8.0f, 200.0f, 14.0f}, 10.5f);
        if (!first) hw::paint::groove(g, x - 13.0f, 10.0f, static_cast<float>(getHeight() - 10));
        first = false;
    }
}

// --- Edit window frame ---------------------------------------------------------------------

EditFrame::EditFrame(AppContext& context) : context_(context)
{
    int i = 0;
    for (const char* name : {"Compose", "Score", "Mix", "Master"}) {
        stages_.push_back(std::make_unique<hw::Keycap>(name, false));
        auto& k = *stages_.back();
        k.set_led_dot(true);
        k.set_legend_size(11.0f);
        k.onClick = [this, i] {
            if (on_stage) on_stage(i);
        };
        addAndMakeVisible(k);
        ++i;
    }
    stages_[0]->setTooltip("Compose: the Composer view");
    stages_[1]->setTooltip("Score: parts and full score (PDF, MusicXML)");
    stages_[2]->setTooltip("Mix: the Producer view and the mixer");
    stages_[3]->setTooltip("Master: bounce the mix and stems");
    listener_ = context_.session.add_listener([this](session::Change c) {
        if (c == session::Change::Document || c == session::Change::File) repaint();
    });
    set_stage(0);
}

EditFrame::~EditFrame() { context_.session.remove_listener(listener_); }

void EditFrame::set_stage(int stage)
{
    stage_ = stage;
    for (int i = 0; i < 4; ++i) {
        auto& k = *stages_[static_cast<std::size_t>(i)];
        k.setToggleState(i == stage, juce::dontSendNotification);
        // A stage behind the current one shows a green LED.
        if (i < stage) {
            k.set_led_colour(hw::tok::meter_green);
            k.set_led_lit(true);
        } else {
            k.set_led_colour({});
            k.set_led_lit(i == stage);
        }
    }
}

juce::Rectangle<int> EditFrame::content_area() const { return getLocalBounds().reduced(8, 6).withTrimmedTop(36); }

void EditFrame::paint(juce::Graphics& g) { hw::paint::panel(g, getLocalBounds().toFloat().reduced(1.0f).withTrimmedBottom(2.0f)); }

void EditFrame::paintOverChildren(juce::Graphics& g)
{
    const auto name = juce::String(context_.session.data().name).toUpperCase();
    hw::paint::silk(g, juce::String::fromUTF8("Edit  \xc2\xb7  ") + name, {14.0f, 8.0f, 360.0f, 22.0f}, 11.5f,
                    juce::Justification::centredLeft, hw::tok::text);
    const auto first = stages_.front()->getBounds().toFloat();
    hw::paint::silk(g, "Stage", {first.getX() - 50.0f, first.getY(), 44.0f, first.getHeight()}, 10.0f);
    for (std::size_t i = 0; i + 1 < stages_.size(); ++i) {
        const auto r = stages_[i]->getBounds().toFloat();
        hw::paint::silk(g, juce::String::fromUTF8("\xe2\x80\xba"), {r.getRight(), r.getY(), 14.0f, r.getHeight()}, 14.0f,
                        juce::Justification::centred);
    }
}

void EditFrame::resized()
{
    int x = 430;
    const int widths[] = {92, 74, 60, 80};
    for (int i = 0; i < 4; ++i) {
        stages_[static_cast<std::size_t>(i)]->setBounds(x, 6, widths[i], 24);
        x += widths[i] + 14;
    }
}

// --- Markers -----------------------------------------------------------------------------------

MarkersPanel::MarkersPanel(AppContext& context) : hw::Panel("Markers"), context_(context)
{
    listener_ = context_.session.add_listener([this](session::Change c) {
        if (c == session::Change::Document) refresh();
    });
    refresh();
}

MarkersPanel::~MarkersPanel() { context_.session.remove_listener(listener_); }

void MarkersPanel::refresh()
{
    const auto& d = context_.session.data();
    items_.clear();
    for (const auto& c : d.cues) {
        const juce::String code = juce::String(c.code).trim();
        items_.push_back({code.isEmpty() ? juce::String(c.title) : code + "  " + juce::String(c.title),
                          juce::String(d.meter.bar_at(c.start)), c.start});
    }
    for (const auto& m : d.markers) {
        const Ticks t = d.tempo.tick_at_seconds(m.seconds);
        items_.push_back({m.label.empty() ? juce::String(m.kind) : juce::String(m.label), juce::String(d.meter.bar_at(t)), t});
    }
    std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) { return a.tick < b.tick; });
    repaint();
}

int MarkersPanel::row_at(juce::Point<int> p) const
{
    const auto c = content();
    if (!c.contains(p)) return -1;
    const int i = (p.y - c.getY()) / 22;
    return i < static_cast<int>(items_.size()) ? i : -1;
}

void MarkersPanel::locate(int i)
{
    if (i < 0 || i >= static_cast<int>(items_.size())) return;
    context_.engine.locate(items_[static_cast<std::size_t>(i)].tick);
}

void MarkersPanel::mouseUp(const juce::MouseEvent& e) { locate(row_at(e.getPosition())); }

void MarkersPanel::mouseMove(const juce::MouseEvent& e)
{
    const int h = row_at(e.getPosition());
    if (h != hover_) {
        hover_ = h;
        setMouseCursor(h >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void MarkersPanel::paintOverChildren(juce::Graphics& g)
{
    const auto c = content();
    if (items_.empty()) {
        g.setColour(hw::tok::silk);
        g.setFont(hw::fonts::ui(12.0f));
        g.drawText("No cues or hit points yet", c.withHeight(22), juce::Justification::centredLeft, true);
        return;
    }
    const auto here = context_.engine.position();
    int current = -1;
    for (std::size_t i = 0; i < items_.size(); ++i)
        if (items_[i].tick <= here) current = static_cast<int>(i);
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const auto row = juce::Rectangle<int>(c.getX(), c.getY() + i * 22, c.getWidth(), 22);
        if (row.getBottom() > c.getBottom()) break;
        if (i == hover_ || i == current) {
            g.setColour(juce::Colours::white.withAlpha(i == current ? 0.06f : 0.04f));
            g.fillRoundedRectangle(row.toFloat(), 3.0f);
        }
        juce::Path tri;
        const float cy = static_cast<float>(row.getCentreY());
        tri.addTriangle(static_cast<float>(row.getX()) + 4.0f, cy - 4.0f, static_cast<float>(row.getX()) + 4.0f, cy + 4.0f,
                        static_cast<float>(row.getX()) + 10.0f, cy);
        g.setColour(i == current ? hw::Theme::get().led() : hw::tok::silk);
        g.fillPath(tri);
        g.setColour(hw::tok::text);
        g.setFont(hw::fonts::ui(12.5f));
        g.drawText(items_[static_cast<std::size_t>(i)].name, row.withTrimmedLeft(18).withTrimmedRight(30),
                   juce::Justification::centredLeft, true);
        g.setColour(hw::tok::silk);
        g.setFont(hw::fonts::lcd(11.0f));
        g.drawText(items_[static_cast<std::size_t>(i)].where, row.withTrimmedRight(4), juce::Justification::centredRight,
                   false);
    }
}

// --- New track ------------------------------------------------------------------------------

NewTrackPanel::NewTrackPanel(AppContext& context) : hw::Panel("New track"), context_(context)
{
    const char* names[] = {"MIDI", "Instrument", "Audio", "Aux / Bus"};
    const char* subs[] = {"Out to hardware / plug-in", "Virtual instrument", "Record / import WAV", "Submix, FX return"};
    for (int i = 0; i < 4; ++i) {
        types_.push_back(std::make_unique<hw::Keycap>(names[i], true));
        auto& k = *types_.back();
        k.setRadioGroupId(31);
        k.set_led_dot(true);
        k.set_legend_size(11.0f);
        k.set_sublegend(subs[i]);
        const juce::Colour tags[] = {hw::tok::type_midi, hw::tok::type_instrument, hw::tok::type_audio, hw::tok::type_aux};
        k.set_led_colour(tags[i]);
        addAndMakeVisible(k);
    }
    types_[0]->setEnabled(false);
    types_[0]->setTooltip("MIDI tracks to external hardware arrive with the Producer view tracks");
    types_[1]->setToggleState(true, juce::dontSendNotification);
    count_.set_justification(juce::Justification::centred);
    for (auto* c : std::initializer_list<juce::Component*>{&minus_, &plus_, &stereo_, &create_, &count_})
        addAndMakeVisible(c);
    stereo_.setToggleState(true, juce::dontSendNotification);
    minus_.onClick = [this] { set_count(count_value_ - 1); };
    plus_.onClick = [this] { set_count(count_value_ + 1); };
    create_.onClick = [this] { create(); };
    create_.setTooltip("Instrument opens the instrument list; Audio and Aux make as many as the count");
}

void NewTrackPanel::set_count(int n)
{
    count_value_ = juce::jlimit(1, 32, n);
    count_.set_text(juce::String(count_value_));
}

int NewTrackPanel::create()
{
    auto& s = context_.session;
    if (types_[1]->getToggleState()) {
        TrackPanel::show_add_track_menu(context_, &create_);
        return 0;
    }
    int made = 0;
    for (int i = 0; i < count_value_; ++i) {
        if (types_[2]->getToggleState()) {
            const auto n = static_cast<int>(std::count_if(s.data().tracks.begin(), s.data().tracks.end(),
                                                          [](const project::Track& t) {
                                                              return t.kind == project::TrackKind::Audio;
                                                          }));
            if (!s.add_audio_track("Audio " + std::to_string(n + 1), stereo_.getToggleState()).empty()) ++made;
        } else if (types_[3]->getToggleState()) {
            if (!s.add_bus("Aux " + std::to_string(s.data().buses.size()), "fx").empty()) ++made;
        }
    }
    if (made > 0 && context_.status)
        context_.status("Created " + juce::String(made) + (types_[2]->getToggleState() ? " audio track" : " aux bus") +
                        (made == 1 ? "" : "s"));
    return made;
}

void NewTrackPanel::resized()
{
    const auto c = content();
    const int kw = (c.getWidth() - 6) / 2;
    for (int i = 0; i < 4; ++i)
        types_[static_cast<std::size_t>(i)]->setBounds(c.getX() + (i % 2) * (kw + 6), c.getY() + (i / 2) * 44, kw, 40);
    const int y = c.getY() + 94;
    minus_.setBounds(c.getX() + 44, y, 24, 24);
    count_.setBounds(c.getX() + 70, y, 34, 24);
    plus_.setBounds(c.getX() + 106, y, 24, 24);
    stereo_.setBounds(c.getX(), y + 32, kw, 24);
    create_.setBounds(c.getX() + kw + 6, y + 32, kw, 24);
}

void NewTrackPanel::paintOverChildren(juce::Graphics& g)
{
    hw::paint::silk(g, "Count", minus_.getBounds().toFloat().withX(static_cast<float>(content().getX())).withWidth(40.0f),
                    10.0f);
}

// --- Selection ------------------------------------------------------------------------------

SelectionPanel::SelectionPanel(AppContext& context) : hw::Panel("Selection"), context_(context)
{
    addAndMakeVisible(lcd_);
    lcd_.setComponentID("selection_lcd");
    listener_ = context_.session.add_listener([this](session::Change c) {
        if (c == session::Change::Document || c == session::Change::Selection) refresh();
    });
    refresh();
}

SelectionPanel::~SelectionPanel() { context_.session.remove_listener(listener_); }

void SelectionPanel::resized() { lcd_.setBounds(content()); }

void SelectionPanel::refresh()
{
    const auto* t = context_.session.data().find_track(context_.session.selected_track());
    if (!t) {
        lcd_.set_text("No track selected");
        return;
    }
    juce::String text;
    text << "TRACK    " << juce::String(t->name).toUpperCase() << "\n";
    if (const auto* v = t->active_version()) {
        text << "VERSION  " << juce::String(v->name).toUpperCase() << "\n";
        const auto sound = !t->instrument_plugin.empty() ? t->instrument_plugin.name : t->assignment.patch_display;
        text << "SOUND    " << juce::String(sound).toUpperCase() << "\n";
        int notes = 0;
        const notation::ScoreNote *lo = nullptr, *hi = nullptr;
        for (std::size_t s = 0; s < v->score.staff_count(); ++s)
            for (const auto& n : v->score.notes(s)) {
                if (n.is_rest) continue;
                ++notes;
                if (!lo || notation::midi_note_number(n.pitch) < notation::midi_note_number(lo->pitch)) lo = &n;
                if (!hi || notation::midi_note_number(n.pitch) > notation::midi_note_number(hi->pitch)) hi = &n;
            }
        text << "NOTES    " << notes;
        if (lo && hi) text << "  " << pitch_name(lo->pitch) << juce::String::fromUTF8(" \xe2\x86\x92 ") << pitch_name(hi->pitch);
        text << "\nOVERRIDES " << static_cast<int>(v->performance.size());
    }
    lcd_.set_text(text);
}

// --- Channel ------------------------------------------------------------------------------------

ChannelPanel::ChannelPanel(AppContext& context) : hw::Panel("Channel"), context_(context)
{
    for (auto* c : std::initializer_list<juce::Component*>{&fader_, &pan_, &meter_l_, &meter_r_, &db_, &pan_lcd_, &out_,
                                                           &inserts_, &mute_, &solo_})
        addAndMakeVisible(c);
    mute_.set_solid(hw::tok::mute);
    solo_.set_solid(hw::tok::solo);
    db_.set_justification(juce::Justification::centred);
    pan_lcd_.set_justification(juce::Justification::centred);
    pan_.setRange(-1.0, 1.0);
    pan_.setDoubleClickReturnValue(true, 0.0);
    pan_.setTitle("Pan");
    fader_.setTitle("Volume");
    fader_.onDragStart = [] { console::begin_gesture(); };
    pan_.onDragStart = [] { console::begin_gesture(); };
    fader_.onValueChange = [this] {
        if (track_.empty()) return;
        const double db = std::isfinite(fader_.db()) ? std::max(-96.0, fader_.db()) : -96.0;
        context_.session.update_track_mix(track_, "Volume " + track_ + console::gesture_suffix(),
                                          [db](project::Track& t) { t.gain_db = db; });
    };
    pan_.onValueChange = [this] {
        if (track_.empty()) return;
        const double p = pan_.getValue();
        context_.session.update_track_mix(track_, "Pan " + track_ + console::gesture_suffix(),
                                          [p](project::Track& t) { t.pan = p; });
    };
    mute_.onClick = [this] {
        if (!track_.empty()) context_.session.set_mute(track_, mute_.getToggleState());
    };
    solo_.onClick = [this] {
        if (!track_.empty()) context_.session.set_solo(track_, solo_.getToggleState());
    };
    int i = 0;
    for (const char* mode : {"Read", "Touch", "Latch", "Write"}) {
        modes_.push_back(std::make_unique<hw::Keycap>(mode, true));
        auto& k = *modes_.back();
        k.setRadioGroupId(41);
        k.set_led_dot(true);
        k.set_legend_size(11.0f);
        k.set_led_colour(console::automation_colour(juce::String(mode).toLowerCase().toStdString()));
        k.onClick = [this, i] {
            const char* names[] = {"read", "touch", "latch", "write"};
            if (!track_.empty()) context_.session.set_automation_mode(track_, names[i]);
        };
        addAndMakeVisible(k);
        ++i;
    }
    listener_ = context_.session.add_listener([this](session::Change c) {
        if (c == session::Change::Document || c == session::Change::Selection) refresh();
    });
    refresh();
    startTimerHz(30);
}

ChannelPanel::~ChannelPanel() { context_.session.remove_listener(listener_); }

void ChannelPanel::refresh()
{
    const auto& d = context_.session.data();
    track_ = context_.session.selected_track();
    const auto* t = d.find_track(track_);
    if (!t) track_.clear();
    set_title(t ? "Channel  " + juce::String::fromUTF8("\xc2\xb7  ") + juce::String(t->name) : juce::String("Channel"));
    for (auto* c : std::initializer_list<juce::Component*>{&fader_, &pan_, &mute_, &solo_})
        c->setEnabled(t != nullptr);
    for (auto& k : modes_) k->setEnabled(t != nullptr);
    if (!t) {
        db_.set_text("-");
        out_.set_text("No track selected");
        inserts_.set_text({});
        return;
    }
    fader_.set_db(t->gain_db, juce::dontSendNotification);
    pan_.setValue(t->pan, juce::dontSendNotification);
    mute_.setToggleState(t->mute, juce::dontSendNotification);
    solo_.setToggleState(t->solo, juce::dontSendNotification);
    db_.set_text(juce::String(t->gain_db, 1) + " dB");
    const int pv = juce::roundToInt(t->pan * 100.0);
    pan_lcd_.set_text(pv == 0 ? juce::String("C") : (pv < 0 ? "L" : "R") + juce::String(std::abs(pv)));
    const char* names[] = {"read", "touch", "latch", "write"};
    for (std::size_t i = 0; i < 4; ++i)
        modes_[i]->setToggleState(t->automation_mode == names[i], juce::dontSendNotification);
    const auto* bus = d.find_bus(t->output_bus_id);
    out_.set_text("OUT  " + (bus ? juce::String(bus->name).toUpperCase() : juce::String("MASTER")));
    juce::String ins;
    for (std::size_t i = 0; i < 5; ++i) {
        ins << juce::String::charToString(static_cast<juce::juce_wchar>('A' + i)) << "  ";
        if (i < t->inserts.size()) {
            const auto& s = t->inserts[i];
            juce::String name = s.kind == "plugin" ? juce::String(s.plugin.name) : juce::String(s.kind);
            if (s.suggestion) name = "(" + name + ")";
            ins << name.toUpperCase() << (s.bypass ? "  OFF" : "");
        } else {
            ins << "-";
        }
        if (i < 4) ins << "\n";
    }
    inserts_.set_text(ins);
}

void ChannelPanel::timerCallback()
{
    if (track_.empty() || !isShowing()) return;
    const auto m = context_.engine.take_deck_meter(track_);
    auto db = [](float lin) { return lin > 0.0f ? 20.0f * std::log10(lin) : -100.0f; };
    meter_l_.push(db(m.left));
    meter_r_.push(db(m.right));
}

void ChannelPanel::resized()
{
    const auto c = content();
    inserts_.setBounds(c.getX(), c.getY() + 18, c.getWidth(), 92);
    const int top = c.getY() + 126;
    pan_.setBounds(c.getX() + 4, top + 14, 30, 30);
    pan_lcd_.setBounds(c.getX(), top + 48, 40, 16);
    fader_.setBounds(c.getX() + 44, top, 80, 210);
    meter_l_.setBounds(c.getX() + 130, top + 10, 6, 190);
    meter_r_.setBounds(c.getX() + 140, top + 10, 6, 190);
    const int rx = c.getX() + 156;
    const int rw = c.getRight() - rx;
    db_.setBounds(rx, top + 6, rw, 24);
    for (int i = 0; i < 4; ++i) modes_[static_cast<std::size_t>(i)]->setBounds(rx, top + 38 + i * 30, rw, 26);
    mute_.setBounds(rx, top + 160, rw / 2 - 2, 24);
    solo_.setBounds(rx + rw / 2 + 2, top + 160, rw / 2 - 2, 24);
    out_.setBounds(c.getX(), top + 220, c.getWidth(), 22);
}

void ChannelPanel::paintOverChildren(juce::Graphics& g)
{
    hw::paint::silk(g, "Inserts", inserts_.getBounds().toFloat().withY(static_cast<float>(inserts_.getY() - 14)).withHeight(12.0f),
                    9.5f);
    hw::paint::silk(g, "Pan", pan_.getBounds().toFloat().withY(static_cast<float>(pan_.getY() - 14)).withHeight(12.0f), 9.5f,
                    juce::Justification::centred);
}

// --- Status strip -------------------------------------------------------------------------------

StatusStrip::StatusStrip(AppContext& context) : context_(context) { startTimer(1000); }

void StatusStrip::set_message(const juce::String& text)
{
    message_ = text;
    repaint();
}

juce::String StatusStrip::readout() const
{
    const auto& d = context_.session.data();
    juce::String s;
    auto& dev = context_.engine.devices();
    if (auto* device = dev.getCurrentAudioDevice()) {
        s << juce::String(device->getCurrentSampleRate() / 1000.0, 1) << " kHz  " << device->getCurrentBufferSizeSamples()
          << " smp   CPU " << juce::roundToInt(dev.getCpuUsage() * 100.0) << "%   ";
    } else {
        s << "NO AUDIO DEVICE   ";
    }
    auto words = juce::StringArray::fromTokens(juce::String(d.key), " ", "");
    s << (words.isEmpty() ? juce::String("C") : words[0]) << " "
      << (words.size() > 1 ? words[1].substring(0, 3).toUpperCase() : juce::String()) << "  "
      << d.meter.at_bar(1).to_string() << "   ";
    s << (context_.session.dirty() ? "UNSAVED" : "SAVED");
    if (autosaved_.toMilliseconds() > 0) s << "   AUTOSAVE " << autosaved_.formatted("%H:%M");
    return s;
}

void StatusStrip::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour(hw::tok::chassis.brighter(0.04f));
    g.fillRect(r);
    g.setColour(hw::tok::edge);
    g.drawHorizontalLine(0, r.getX(), r.getRight());
    r = r.reduced(10.0f, 0.0f);
    g.setFont(hw::fonts::lcd(11.0f));
    const auto right = readout();
    const float rw = juce::GlyphArrangement::getStringWidth(hw::fonts::lcd(11.0f), right) + 8.0f;
    g.setColour(hw::tok::silk);
    g.drawText(right, r.removeFromRight(rw), juce::Justification::centredRight, false);
    g.setFont(hw::fonts::ui(12.0f));
    g.setColour(hw::tok::text);
    g.drawText(message_, r, juce::Justification::centredLeft, true);
}

}  // namespace vcc::desktop::shell
