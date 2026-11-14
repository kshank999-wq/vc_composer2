#include "ui/shell/ShellPanels.h"

#include <algorithm>
#include <cmath>

#include "AppResources.h"
#include "ui/SequencerView.h"
#include "ui/Theme.h"
#include "ui/TrackPanel.h"
#include "ui/composer/KeyLegend.h"
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
        Group entry{"Entry"};
        auto& note = add(entry, "Note entry", 100, false);
        note.set_led_dot(true);
        note.setComponentID("composer_entry");
        note.setTooltip("Note entry: letters A-G enter notes, Shift+letter adds to a chord");
        note.onClick = [this] {
            if (on_command) on_command(composer::Command::ToggleKeyboardEntryMode);
        };
        command_keys_.emplace_back(&note, composer::Command::ToggleKeyboardEntryMode);
        groups_.push_back(entry);
        Group view{"View"};
        auto& track = add(view, "Track", 58, true);
        track.setToggleState(true, juce::dontSendNotification);
        track.setTooltip("Track view: one staff per track");
        auto& page = add(view, "Page", 52);
        page.setEnabled(false);
        page.setTooltip("Page view (coming with the Score stage)");
        add(view, "Mixer", 62).onClick = invoke(cmd::showMixer);
        groups_.push_back(view);
        Group tools{"Tools"};
        add(tools, "Chords", 66).onClick = invoke(cmd::chordExplorer);
        add(tools, "Ostinato", 74).onClick = invoke(cmd::ostinatoCreator);
        groups_.push_back(tools);
        // The note value keypad, as Finale and Sibelius lay it out: the
        // number rising with the value, 5 the quarter; dot, triplet, rest.
        const auto& mf = music_font();
        const juce::Font note_font = mf.loaded ? mf.font.font_for_staff_space(4.4) : juce::Font(juce::FontOptions(14.0f));
        const juce::Font acc_font = mf.loaded ? mf.font.font_for_staff_space(5.0) : juce::Font(juce::FontOptions(14.0f));
        auto music_key = [&](Group& g, composer::Command c, const char* glyph, const char* tip, const juce::Font& f) -> hw::Keycap& {
            auto& k = add(g, {}, 34, false);
            if (mf.loaded) k.set_music_symbol(juce::String::fromUTF8(glyph), f);
            k.setTooltip(tip);
            k.setComponentID("composer_cmd_" + juce::String(static_cast<int>(c)));
            k.onClick = [this, c] {
                if (on_command) on_command(c);
            };
            command_keys_.emplace_back(&k, c);
            return k;
        };
        Group value{"Note value"};
        using C = composer::Command;
        music_key(value, C::SelectDurationWhole, "\xee\xb2\xa2", "Whole note", note_font);
        music_key(value, C::SelectDurationHalf, "\xee\xb2\xa3", "Half note", note_font);
        music_key(value, C::SelectDurationQuarter, "\xee\xb2\xa5", "Quarter note", note_font);
        music_key(value, C::SelectDurationEighth, "\xee\xb2\xa7", "Eighth note", note_font);
        music_key(value, C::SelectDurationSixteenth, "\xee\xb2\xa9", "Sixteenth note", note_font);
        music_key(value, C::SelectDurationThirtySecond, "\xee\xb2\xab", "Thirty-second note", note_font);
        music_key(value, C::ToggleDot, "\xee\xb2\xb7", "Dot", acc_font);
        music_key(value, C::ToggleTuplet, "\xee\xa2\x83", "Triplet", note_font);
        music_key(value, C::ToggleRest, "\xee\x93\xa5", "Rest", note_font);
        groups_.push_back(value);
        Group acc{"Accidental"};
        music_key(acc, C::AccidentalFlat, "\xee\x89\xa0", "Flat", acc_font);
        music_key(acc, C::AccidentalNatural, "\xee\x89\xa1", "Natural", acc_font);
        music_key(acc, C::AccidentalSharp, "\xee\x89\xa2", "Sharp", acc_font);
        groups_.push_back(acc);
        Group zoom{"Zoom"};
        add(zoom, "-", 34).onClick = [this] {
            if (on_zoom) on_zoom(false);
        };
        add(zoom, "+", 34).onClick = [this] {
            if (on_zoom) on_zoom(true);
        };
        groups_.push_back(zoom);
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
    refresh_legends();
    set_producer(false);
}

void Toolbar::refresh_legends()
{
    for (auto& [k, c] : command_keys_) {
        if (c == composer::Command::ToggleKeyboardEntryMode) {
            k->setTooltip("Note entry (" + composer_ui::key_legend(context_.shortcuts, c) +
                          "): letters A-G enter notes, Shift+letter adds to a chord");
            continue;
        }
        k->setButtonText(composer_ui::key_legend(context_.shortcuts, c));
    }
}

hw::Keycap* Toolbar::command_key(composer::Command c)
{
    for (auto& [k, cmd] : command_keys_)
        if (cmd == c) return k;
    return nullptr;
}

void Toolbar::set_entry_state(bool entry, const notation::Duration& value, bool rest)
{
    using C = composer::Command;
    auto lit = [this](C c, bool on) {
        if (auto* k = command_key(c); k && k->getToggleState() != on) k->setToggleState(on, juce::dontSendNotification);
    };
    lit(C::ToggleKeyboardEntryMode, entry);
    const C values[] = {C::None, C::SelectDurationWhole, C::SelectDurationHalf, C::SelectDurationQuarter,
                        C::SelectDurationEighth, C::SelectDurationSixteenth, C::SelectDurationThirtySecond};
    const int v = static_cast<int>(value.value);
    for (int i = 1; i < 7; ++i) lit(values[i], i == v);
    lit(C::ToggleDot, value.dots > 0);
    lit(C::ToggleTuplet, value.tuplet.is_tuplet());
    lit(C::ToggleRest, rest);
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
    stages_[3]->setTooltip("Master: loudness, the master chain and Export master");
    // Follow: the views scroll with the playhead while it plays.
    follow_.set_led_dot(true);
    follow_.set_legend_size(11.0f);
    follow_.setComponentID("edit_follow");
    follow_.setTooltip("Follow (Cmd/Ctrl+Shift+F): the edit window scrolls with the playhead, and the keyboard with the notes "
                       "that light up");
    follow_.onClick = [this] { context_.commands.invokeDirectly(cmd::toggleFollow, true); };
    follow_.setToggleState(context_.follow, juce::dontSendNotification);
    addAndMakeVisible(follow_);
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

void EditFrame::refresh_follow() { follow_.setToggleState(context_.follow, juce::dontSendNotification); }

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
    // Follow sits at the right of the title bar.
    follow_.setBounds(std::max(x, getWidth() - 14 - 86), 6, 86, 24);
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

// --- Clips ------------------------------------------------------------------------------------

ClipsPanel::ClipsPanel(AppContext& context) : hw::Panel("Clips"), context_(context)
{
    listener_ = context_.session.add_listener([this](session::Change c) {
        if (c == session::Change::Document) refresh();
    });
    refresh();
}

ClipsPanel::~ClipsPanel() { context_.session.remove_listener(listener_); }

void ClipsPanel::refresh()
{
    const auto& d = context_.session.data();
    items_.clear();
    for (const auto& t : d.tracks) {
        const auto* v = t.active_version();
        if (!v) continue;
        for (const auto& c : v->audio_clips) {
            if (c.lane != 0) continue;
            const Ticks tick = d.tempo.tick_at_seconds(c.start_seconds);
            const juce::String name = c.name.empty() ? juce::File(juce::String(c.file)).getFileNameWithoutExtension()
                                                     : juce::String(c.name);
            items_.push_back({name, juce::String(t.name), juce::String(d.meter.bar_at(tick)) + "  " + juce::String(c.length, 1) + "s",
                              t.id, c.id, colour_from_hex(t.color), tick});
        }
    }
    std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) { return a.tick < b.tick; });
    repaint();
}

int ClipsPanel::row_at(juce::Point<int> p) const
{
    const auto c = content();
    if (!c.contains(p)) return -1;
    const int i = (p.y - c.getY()) / 30;
    return i < static_cast<int>(items_.size()) ? i : -1;
}

void ClipsPanel::mouseMove(const juce::MouseEvent& e)
{
    const int h = row_at(e.getPosition());
    if (h != hover_) {
        hover_ = h;
        repaint();
    }
}

void ClipsPanel::mouseUp(const juce::MouseEvent& e)
{
    if (const int i = row_at(e.getPosition()); i >= 0) choose(i);
}

void ClipsPanel::choose(int i)
{
    if (i < 0 || i >= static_cast<int>(items_.size())) return;
    const auto item = items_[static_cast<std::size_t>(i)];
    context_.engine.locate(item.tick);
    if (auto* seq = SequencerView::find(context_)) seq->select_clip(item.track_id, item.clip_id);
    else context_.session.select_track(item.track_id);
    repaint();
}

void ClipsPanel::paintOverChildren(juce::Graphics& g)
{
    const auto c = content();
    if (items_.empty()) {
        g.setColour(hw::tok::silk);
        g.setFont(hw::fonts::ui(12.0f));
        g.drawFittedText("No audio clips yet - record, import WAV, or print the score to audio", c.withHeight(36),
                         juce::Justification::topLeft, 2);
        return;
    }
    std::pair<std::string, std::string> selected;
    if (auto* seq = SequencerView::find(context_)) selected = seq->selected_clip();
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const auto row = juce::Rectangle<int>(c.getX(), c.getY() + i * 30, c.getWidth(), 28);
        if (row.getBottom() > c.getBottom()) break;
        const auto& it = items_[static_cast<std::size_t>(i)];
        const bool sel = selected.first == it.track_id && selected.second == it.clip_id;
        if (i == hover_ || sel) {
            g.setColour(juce::Colours::white.withAlpha(sel ? 0.07f : 0.04f));
            g.fillRoundedRectangle(row.toFloat(), 3.0f);
        }
        if (sel) {
            g.setColour(hw::Theme::get().led().withAlpha(0.7f));
            g.drawRoundedRectangle(row.toFloat().reduced(0.5f), 3.0f, 1.0f);
        }
        g.setColour(it.colour);
        g.fillRect(row.withWidth(3).reduced(0, 4));
        auto text = row.withTrimmedLeft(10).withTrimmedRight(4);
        g.setColour(hw::tok::text);
        g.setFont(hw::fonts::ui(12.0f));
        g.drawText(it.name, text.removeFromTop(15), juce::Justification::centredLeft, true);
        g.setColour(hw::tok::silk);
        g.setFont(hw::fonts::ui(10.5f));
        g.drawText(it.track, text.withTrimmedRight(70), juce::Justification::centredLeft, true);
        g.setFont(hw::fonts::lcd(10.5f));
        g.drawText(it.where, text, juce::Justification::centredRight, false);
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
    // The shared selection (docs/spec/ui, "Panel behavior"): track, range,
    // note count and pitch span, value, velocity range.
    const auto& session = context_.session;
    const auto& sel = session.selection();
    const auto& d = session.data();
    const auto* t = d.find_track(sel.track);
    if (!t) {
        lcd_.set_text("No track selected");
        return;
    }
    const auto* v = t->active_version();
    auto bar_beat = [&](Ticks tick) {
        const auto bbt = bars_beats_ticks(d.meter, tick);
        return bbt.upToLastOccurrenceOf("|", false, false);
    };
    juce::String text;
    text << "TRACK     " << juce::String(t->name).toUpperCase() << "\n";
    text << "RANGE     "
         << (sel.has_range() ? "BAR " + bar_beat(sel.from) + juce::String::fromUTF8(" \xe2\x86\x92 ") + bar_beat(sel.to - 1)
                             : juce::String("-"))
         << "\n";
    if (!v) {
        lcd_.set_text(text);
        return;
    }
    // The notes: the selected ones, or the whole track when none are.
    std::vector<const notation::ScoreNote*> notes;
    if (!sel.notes.empty()) {
        for (auto id : sel.notes)
            if (const auto* n = v->score.find_note(id); n && !n->is_rest) notes.push_back(n);
    } else {
        for (std::size_t s = 0; s < v->score.staff_count(); ++s)
            for (const auto& n : v->score.notes(s))
                if (!n.is_rest) notes.push_back(&n);
    }
    const notation::ScoreNote *lo = nullptr, *hi = nullptr;
    for (const auto* n : notes) {
        if (!lo || notation::midi_note_number(n->pitch) < notation::midi_note_number(lo->pitch)) lo = n;
        if (!hi || notation::midi_note_number(n->pitch) > notation::midi_note_number(hi->pitch)) hi = n;
    }
    text << "NOTES     " << static_cast<int>(notes.size());
    if (lo && hi) text << "  " << pitch_name(lo->pitch) << juce::String::fromUTF8(" \xe2\x86\x92 ") << pitch_name(hi->pitch);
    text << (sel.notes.empty() ? "  (TRACK)" : "") << "\n";
    // Value: the selected notes' written value when they share one.
    juce::String value = "-";
    if (!sel.notes.empty() && !notes.empty()) {
        const auto& d0 = notes.front()->duration;
        const bool same = std::all_of(notes.begin(), notes.end(), [&](const notation::ScoreNote* n) { return n->duration == d0; });
        if (same) {
            const char* names[] = {"2/1", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64"};
            value = names[static_cast<int>(d0.value)];
            for (int i = 0; i < d0.dots; ++i) value << ".";
            if (d0.tuplet.is_tuplet()) value << " " << juce::String(d0.tuplet.actual) << ":" << juce::String(d0.tuplet.normal);
        } else {
            value = "MIXED";
        }
    }
    text << "VALUE     " << value << "\n";
    // Velocity: as performed (overrides included), selected notes only.
    juce::String velocity = "-";
    if (!sel.notes.empty()) {
        const auto& r = render_.render(*t);
        int vmin = 128, vmax = -1;
        for (std::size_t i = 0; i < r.notes.size(); ++i)
            if (std::binary_search(sel.notes.begin(), sel.notes.end(), r.note_ids[i])) {
                vmin = std::min(vmin, r.notes[i].velocity);
                vmax = std::max(vmax, r.notes[i].velocity);
            }
        if (vmax >= 0)
            velocity = vmin == vmax ? juce::String(vmin)
                                    : juce::String(vmin) + juce::String::fromUTF8(" \xe2\x86\x92 ") + juce::String(vmax);
    }
    text << "VELOCITY  " << velocity;
    lcd_.set_text(text);
}

// --- Channel ------------------------------------------------------------------------------------

ChannelPanel::ChannelPanel(AppContext& context)
    : hw::Panel("Channel"), context_(context), writer_(context, {}), slots_(std::make_unique<InsertSlots>(context, false))
{
    for (auto* c : std::initializer_list<juce::Component*>{slots_.get(), &fader_, &pan_, &meter_l_, &meter_r_, &db_, &pan_lcd_,
                                                           &in_, &out_, &mute_, &solo_})
        addAndMakeVisible(c);
    mute_.set_solid(hw::tok::mute);
    solo_.set_solid(hw::tok::solo);
    for (auto* l : {&db_, &pan_lcd_}) l->set_justification(juce::Justification::centred);
    pan_.setRange(-1.0, 1.0);
    pan_.setDoubleClickReturnValue(true, 0.0);
    pan_.setTitle("Pan");
    fader_.setTitle("Volume");
    in_.set_on_click([this] {
        if (!track_.empty()) console::show_input_menu(context_, track_, in_);
    });
    out_.set_on_click([this] {
        if (!track_.empty()) console::show_output_menu(context_, track_, out_);
    });
    in_.setTooltip("Input: what this track records and monitors (an instrument track records onto its audio lane)");
    out_.setTooltip("Output: the master, or a group or FX bus");
    // Fader and pan: an edit per gesture, or automation while a pass writes them.
    fader_.onDragStart = [this] { writer_.begin("volume"); };
    fader_.onDragEnd = [this] { writer_.end("volume"); };
    fader_.onValueChange = [this] {
        const double db = std::isfinite(fader_.db()) ? std::max(-96.0, fader_.db()) : -96.0;
        writer_.write("volume", db, "Volume", [db](double& gain, double&, std::vector<project::Send>&) { gain = db; });
        db_.set_text(juce::String(db, 1) + " dB");
    };
    pan_.onDragStart = [this] { writer_.begin("pan"); };
    pan_.onDragEnd = [this] { writer_.end("pan"); };
    pan_.onValueChange = [this] {
        const double p = pan_.getValue();
        writer_.write("pan", p, "Pan", [p](double&, double& pan, std::vector<project::Send>&) { pan = p; });
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
        k.set_legend_size(10.5f);
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

ChannelPanel::~ChannelPanel()
{
    stopTimer();
    context_.session.remove_listener(listener_);
}

void ChannelPanel::refresh()
{
    const auto& d = context_.session.data();
    track_ = context_.session.selected_track();
    const auto* t = d.find_track(track_);
    if (!t) track_.clear();
    writer_.set_owner(track_);
    slots_->set_owner(track_);
    set_title(t ? "Channel  " + juce::String::fromUTF8("\xc2\xb7  ") + juce::String(t->name) : juce::String("Channel"));
    // A send knob per FX bus (rebuilt when the buses change).
    std::vector<std::string> fx;
    std::vector<juce::String> fx_names;
    for (const auto& b : d.buses)
        if (b.kind == "fx") {
            fx.push_back(b.id);
            fx_names.push_back(juce::String(b.name));
        }
    if (fx != send_bus_) {
        sends_.clear();
        send_bus_ = fx;
        send_names_ = fx_names;
        for (const auto& id : fx) {
            auto k = std::make_unique<hw::SmallKnob>();
            k->setRange(-60.0, 6.0, 0.1);
            k->setDoubleClickReturnValue(true, 0.0);
            k->setWantsKeyboardFocus(false);
            const std::string param = "send:" + id;
            auto* knob = k.get();
            k->onDragStart = [this, param] { writer_.begin(param); };
            k->onDragEnd = [this, param] { writer_.end(param); };
            k->onValueChange = [this, knob, id, param] {
                const double v = knob->getValue();
                writer_.write(param, v, "Send", [&](double&, double&, std::vector<project::Send>& sends) {
                    auto it = std::find_if(sends.begin(), sends.end(), [&](const project::Send& s) { return s.bus_id == id; });
                    if (it == sends.end()) sends.emplace_back(id, v);
                    else it->level_db = v;
                });
                knob->setTooltip(juce::String(v, 1) + " dB");
            };
            addAndMakeVisible(*k);
            sends_.push_back(std::move(k));
        }
        resized();
    }
    for (auto* c : std::initializer_list<juce::Component*>{&fader_, &pan_, &mute_, &solo_, slots_.get()}) c->setEnabled(t != nullptr);
    for (auto& k : modes_) k->setEnabled(t != nullptr);
    for (auto& k : sends_) k->setEnabled(t != nullptr);
    if (!t) {
        db_.set_text("-");
        in_.set_text({});
        out_.set_text("No track selected");
        repaint();
        return;
    }
    fader_.set_db(t->gain_db, juce::dontSendNotification);
    pan_.setValue(t->pan, juce::dontSendNotification);
    mute_.setToggleState(t->mute, juce::dontSendNotification);
    solo_.setToggleState(t->solo, juce::dontSendNotification);
    db_.set_text(juce::String(t->gain_db, 1) + " dB");
    pan_lcd_.set_text(console::pan_text(t->pan).toUpperCase());
    const char* names[] = {"read", "touch", "latch", "write"};
    for (std::size_t i = 0; i < 4; ++i) modes_[i]->setToggleState(t->automation_mode == names[i], juce::dontSendNotification);
    for (std::size_t i = 0; i < sends_.size(); ++i) {
        double level = -60.0;
        for (const auto& s : t->sends)
            if (s.bus_id == send_bus_[i]) level = s.level_db;
        sends_[i]->setValue(level, juce::dontSendNotification);
        sends_[i]->setTooltip(send_names_[i] + "  " + juce::String(level, 1) + " dB");
    }
    in_.set_text(console::input_text(context_, track_));
    out_.set_text(console::output_text(context_, track_));
    repaint();
}

void ChannelPanel::timerCallback()
{
    if (track_.empty() || !isShowing()) return;
    const auto m = context_.engine.take_deck_meter(track_, 1);
    auto db = [](float lin) { return lin > 0.0f ? 20.0f * std::log10(lin) : -100.0f; };
    meter_l_.push(db(m.left));
    meter_r_.push(db(m.right));
    // Fader and pan follow automation as it plays.
    if (!fader_.isMouseButtonDown())
        if (const auto v = context_.engine.automated_value(track_, "volume")) {
            fader_.set_db(*v, juce::dontSendNotification);
            db_.set_text(juce::String(*v, 1) + " dB");
        }
    if (!pan_.isMouseButtonDown())
        if (const auto v = context_.engine.automated_value(track_, "pan")) pan_.setValue(*v, juce::dontSendNotification);
}

void ChannelPanel::resized()
{
    auto c = content();
    // Short (a tall keyboard deck, the print panel below): inserts as a row of lights, sends hidden.
    compact_ = c.getHeight() < 330;
    slots_->set_compact(compact_);
    if (compact_) {
        slots_->setBounds(c.removeFromTop(24).withTrimmedTop(2));
        for (auto& k : sends_) k->setVisible(false);
    } else {
        c.removeFromTop(14);
        slots_->setBounds(c.removeFromTop(80));
        c.removeFromTop(16);
        auto row = c.removeFromTop(30);
        for (std::size_t i = 0; i < sends_.size(); ++i) {
            const bool fits = static_cast<int>(i) < row.getWidth() / 44;
            sends_[i]->setVisible(fits);
            if (fits) sends_[i]->setBounds(row.getX() + static_cast<int>(i) * 44 + 7, row.getY(), 30, 30);
        }
        c.removeFromTop(sends_.empty() ? 0 : 12);
    }
    c.removeFromTop(6);
    auto io = c.removeFromBottom(20);
    in_.setBounds(io.removeFromLeft(io.getWidth() / 2 - 3));
    io.removeFromLeft(6);
    out_.setBounds(io);
    c.removeFromBottom(6);
    const int top = c.getY();
    const int h = c.getHeight();
    pan_.setBounds(c.getX() + 4, top + 14, 30, 30);
    pan_lcd_.setBounds(c.getX(), top + 48, 40, 16);
    fader_.setBounds(c.getX() + 44, top, 80, h);
    meter_l_.setBounds(c.getX() + 130, top + 8, 6, h - 16);
    meter_r_.setBounds(c.getX() + 140, top + 8, 6, h - 16);
    const int rx = c.getX() + 156;
    const int rw = c.getRight() - rx;
    db_.setBounds(rx, top + 2, rw, 22);
    // Read, Touch, Latch, Write: a column when there is room, else two by two.
    const bool column = h >= 28 + 5 * 24;
    const int key_h = column ? juce::jlimit(20, 26, (h - 28) / 5 - 3) : 20;
    for (int i = 0; i < 4; ++i) {
        auto& k = *modes_[static_cast<std::size_t>(i)];
        k.set_legend_size(column ? 10.5f : 8.5f);
        k.set_led_dot(column);  // two by two, the legend needs the room
        if (column) k.setBounds(rx, top + 28 + i * (key_h + 3), rw, key_h);
        else k.setBounds(rx + (i % 2) * (rw / 2 + 2), top + 28 + (i / 2) * (key_h + 3), rw / 2 - 2, key_h);
    }
    const int ms_y = top + 28 + (column ? 4 : 2) * (key_h + 3) + 2;
    mute_.setBounds(rx, ms_y, rw / 2 - 2, key_h);
    solo_.setBounds(rx + rw / 2 + 2, ms_y, rw / 2 - 2, key_h);
}

void ChannelPanel::paintOverChildren(juce::Graphics& g)
{
    if (!compact_) {
        hw::paint::silk(g, "Inserts", slots_->getBounds().toFloat().withY(static_cast<float>(slots_->getY() - 13)).withHeight(12.0f), 9.5f);
        if (!sends_.empty()) {
            hw::paint::silk(g, "Sends", {static_cast<float>(slots_->getX()), static_cast<float>(sends_.front()->getY() - 14), 80.0f, 12.0f},
                            9.5f);
            for (std::size_t i = 0; i < sends_.size(); ++i)
                if (sends_[i]->isVisible())
                    hw::paint::silk(g, send_names_[i], sends_[i]->getBounds().toFloat().translated(0.0f, 31.0f).withHeight(10.0f).expanded(7.0f, 0.0f),
                                    7.5f, juce::Justification::centred);
        } else {
            hw::paint::silk(g, "Sends  -  no FX buses", {static_cast<float>(slots_->getX()), static_cast<float>(slots_->getBottom() + 4), 200.0f, 12.0f},
                            9.0f);
        }
    }
    hw::paint::silk(g, "Pan", pan_.getBounds().toFloat().withY(static_cast<float>(pan_.getY() - 13)).withHeight(12.0f), 9.5f,
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
