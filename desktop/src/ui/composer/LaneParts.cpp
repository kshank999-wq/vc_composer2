#include "ui/composer/LaneParts.h"
#include "ui/composer/TransposePopover.h"

#include <cmath>

#include "ui/Theme.h"
#include "ui/ScoreTools.h"
#include "ui/TrackPanel.h"
#include "ui/console/ConsoleStyle.h"
#include "vcc/ui/NotationView.h"

namespace vcc::desktop::composer_ui {

namespace {
constexpr int kNewCopy = 1001, kNewEmpty = 1002, kDelete = 1003;
}  // namespace

// --- Track header ---------------------------------------------------------------------------

TrackHeader::TrackHeader(AppContext& context, std::string track_id) : context_(context), track_(std::move(track_id))
{
    for (auto* c : std::initializer_list<juce::Component*>{&number_, &scribble_, &rec_, &solo_, &mute_, &tag_, &trans_, &vol_, &pan_,
                                                           &vol_lcd_, &pan_lcd_, &meter_, &instrument_, &midi_in_, &midi_out_})
        addAndMakeVisible(c);
    // The instrument insert (as on a Pro Tools track): "Instrument" until one
    // is assigned, then its name. Click: built-in, virtual instruments, library.
    instrument_.set_legend_size(9.0f);
    instrument_.setComponentID("track_instrument");
    instrument_.setWantsKeyboardFocus(false);
    instrument_.onClick = [this] {
        context_.session.select_track(track_);
        TrackPanel::instrument_menu(context_, track_)
            .showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&instrument_));
    };
    for (auto* k : {&midi_in_, &midi_out_}) {
        k->set_legend_size(8.5f);
        k->setWantsKeyboardFocus(false);
    }
    midi_in_.setComponentID("track_midi_in");
    midi_out_.setComponentID("track_midi_out");
    // Versions: up to ten takes on the music, to try ideas; switching changes the notes.
    versions_.setComponentID("track_versions");
    versions_.setTitle("Track version");
    versions_.setTooltip("Versions of this track's music (up to 10): switch to compare ideas, or start a new one");
    versions_.setWantsKeyboardFocus(false);
    versions_.onChange = [this] { version_chosen(); };
    addAndMakeVisible(versions_);
    midi_in_.onClick = [this] { show_midi_menu(true); };
    midi_out_.onClick = [this] { show_midi_menu(false); };
    // Trans (Add-On spec §10): the track's transpose, at hand.
    trans_.set_legend_size(9.0f);
    trans_.setWantsKeyboardFocus(false);
    trans_.setComponentID("track_trans");
    trans_.onClick = [this] {
        context_.session.select_track(track_);
        TransposePopover::show(context_, track_, trans_.getScreenBounds());
    };
    number_.set_justification(juce::Justification::centred);
    vol_lcd_.set_justification(juce::Justification::centred);
    pan_lcd_.set_justification(juce::Justification::centred);
    rec_.set_solid(hw::tok::rec);
    solo_.set_solid(hw::tok::solo);
    mute_.set_solid(hw::tok::mute);
    for (auto* k : {&rec_, &solo_, &mute_}) k->set_legend_size(11.0f);
    // Keys and knobs leave the keyboard with the score (A-G keep entering notes).
    for (auto* c : std::initializer_list<juce::Component*>{&rec_, &solo_, &mute_, &tag_, &vol_, &pan_})
        c->setWantsKeyboardFocus(false);
    rec_.setTooltip("Record arm");
    solo_.setTooltip("Solo");
    mute_.setTooltip("Mute");
    tag_.set_legend_size(9.5f);
    tag_.setEnabled(false);
    rec_.onClick = [this] { context_.session.set_record_armed(track_, rec_.getToggleState()); };
    solo_.onClick = [this] { context_.session.set_solo(track_, solo_.getToggleState()); };
    mute_.onClick = [this] { context_.session.set_mute(track_, mute_.getToggleState()); };
    vol_.setRange(-60.0, 6.0, 0.1);
    vol_.setDoubleClickReturnValue(true, 0.0);
    vol_.setTitle("Volume");
    pan_.setRange(-1.0, 1.0, 0.01);
    pan_.setDoubleClickReturnValue(true, 0.0);
    pan_.setTitle("Pan");
    vol_.onDragStart = [] { console::begin_gesture(); };
    pan_.onDragStart = [] { console::begin_gesture(); };
    vol_.onValueChange = [this] {
        const double db = vol_.getValue();
        context_.session.update_track_mix(track_, "Volume " + track_ + console::gesture_suffix(),
                                          [db](project::Track& t) { t.gain_db = db; });
    };
    pan_.onValueChange = [this] {
        const double p = pan_.getValue();
        context_.session.update_track_mix(track_, "Pan " + track_ + console::gesture_suffix(),
                                          [p](project::Track& t) { t.pan = p; });
    };
    refresh();
}

void TrackHeader::set_number(int n) { number_.set_text(juce::String(n).paddedLeft('0', 2)); }

void TrackHeader::refresh()
{
    const auto* t = context_.session.data().find_track(track_);
    if (!t) return;
    // The second line: what the linked Producer track plays - its sound, or,
    // for a part expanded to articulation tracks, how many play it; a print
    // that replaces the instrument says so (FROZEN, STALE...).
    juce::String sound = !t->instrument_plugin.empty() ? juce::String(t->instrument_plugin.name)
                                                       : juce::String(t->assignment.patch_display);
    if (t->articulations_expanded)
        sound = "Expanded: " + juce::String(static_cast<int>(context_.session.articulation_tracks(track_).size())) +
                " artic. tracks";
    else if (context_.track_state)
        if (const auto state = context_.track_state(track_); state.isNotEmpty() && state != "LIVE") sound = state;
    scribble_.set_text(juce::String(t->name).toUpperCase() + "\n" + sound.toUpperCase());
    rec_.setToggleState(t->record_armed, juce::dontSendNotification);
    solo_.setToggleState(t->solo, juce::dontSendNotification);
    mute_.setToggleState(t->mute, juce::dontSendNotification);
    vol_.setValue(juce::jlimit(-60.0, 6.0, t->gain_db), juce::dontSendNotification);
    pan_.setValue(t->pan, juce::dontSendNotification);
    vol_lcd_.set_text(juce::String(t->gain_db, 1));
    vol_lcd_.set_text(t->gain_db <= -59.95 ? juce::String("-INF") : juce::String(t->gain_db, 1));
    const int pv = juce::roundToInt(t->pan * 100.0);
    pan_lcd_.set_text(pv == 0 ? juce::String("C") : (pv < 0 ? "L" : "R") + juce::String(std::abs(pv)));
    // Trans: its shift as a badge (+2 st), dimmed while bypassed.
    {
        std::string why;
        const bool can = context_.session.can_transpose(*t, &why);
        const int s = t->transpose;
        trans_.setButtonText(s == 0 ? juce::String("Trans") : (s > 0 ? "+" : "") + juce::String(s) + " st");
        trans_.setToggleState(s != 0 && !t->transpose_bypass, juce::dontSendNotification);
        trans_.setEnabled(can);
        trans_.setTooltip(can ? "Trans: transpose this track without changing its notes (" +
                                    juce::String(session::Session::interval_name(s)) + (t->transpose_bypass ? ", bypassed)" : ")")
                              : juce::String(why));
    }
    // The instrument insert: the plug-in's name once one is assigned.
    {
        juce::String name = !t->instrument_plugin.empty() ? juce::String(t->instrument_plugin.name) : juce::String("Instrument");
        if (!t->midi_output.empty()) name = "External MIDI";
        instrument_.setButtonText(name);
        instrument_.setToggleState(!t->instrument_plugin.empty(), juce::dontSendNotification);
        instrument_.setTooltip(t->instrument_plugin.empty() ? juce::String("Instrument: the built-in sound. Click to assign a virtual instrument")
                                                            : "Instrument: " + juce::String(t->instrument_plugin.vendor) + " " +
                                                                  juce::String(t->instrument_plugin.name) + ". Click to change");
        // The versions list: each version, then what can be done.
        versions_.clear(juce::dontSendNotification);
        int active = 0;
        for (std::size_t i = 0; i < t->versions.size(); ++i) {
            const auto& v = t->versions[i];
            versions_.addItem(juce::String(v.name) + (v.approved ? juce::String::fromUTF8(" \xe2\x9c\x93") : juce::String()),
                              static_cast<int>(i) + 1);
            if (v.id == t->active_version_id) active = static_cast<int>(i) + 1;
        }
        versions_.addSeparator();
        const bool room = static_cast<int>(t->versions.size()) < session::Session::kMaxVersions;
        versions_.addItem("New Version (copy of this)", kNewCopy);
        versions_.addItem("New Empty Version", kNewEmpty);
        versions_.setItemEnabled(kNewCopy, room);
        versions_.setItemEnabled(kNewEmpty, room);
        versions_.addItem("Delete This Version", kDelete);
        versions_.setItemEnabled(kDelete, t->versions.size() > 1);
        versions_.setSelectedId(active, juce::dontSendNotification);
        midi_in_.setButtonText(t->midi_input.empty() ? juce::String("In: All") : "In: " + juce::String(t->midi_input));
        midi_in_.setTooltip("MIDI In: " + (t->midi_input.empty() ? juce::String("every input") : juce::String(t->midi_input)));
        midi_out_.setButtonText(t->midi_output.empty() ? juce::String("Out: None") : "Out: " + juce::String(t->midi_output));
        midi_out_.setTooltip("MIDI Out: " + (t->midi_output.empty() ? juce::String("none (the instrument plays)")
                                                                      : juce::String(t->midi_output) + " (external)"));
    }
    // The staff tag: what the staff reads.
    juce::String tag = "Treble";
    if (const auto* v = t->active_version()) {
        const auto& sc = v->score;
        if (sc.staff_count() > 1) {
            tag = "Grand staff";
        } else if (sc.staff_count() == 1) {
            switch (sc.staff(0).clef) {
                case notation::Clef::Bass: tag = "Bass"; break;
                case notation::Clef::BassOctaveDown: tag = "Bass 8vb"; break;
                case notation::Clef::Alto: tag = "Alto"; break;
                case notation::Clef::Tenor: tag = "Tenor"; break;
                case notation::Clef::TrebleOctaveDown: tag = "Treble 8vb"; break;
                case notation::Clef::TrebleOctaveUp: tag = "Treble 8va"; break;
                case notation::Clef::Percussion: tag = "Perc"; break;
                default: break;
            }
        }
    }
    tag_.setButtonText(tag);
    repaint();
}

void TrackHeader::push_meter(float left_db, float right_db) { meter_.push(std::max(left_db, right_db)); }

void TrackHeader::paint(juce::Graphics& g)
{
    const auto* t = context_.session.data().find_track(track_);
    const bool selected = context_.session.selected_track() == track_;
    auto r = getLocalBounds().toFloat();
    g.setGradientFill(juce::ColourGradient::vertical(selected ? hw::tok::panel_top.brighter(0.08f) : hw::tok::panel_top, r.getY(),
                                                     hw::tok::panel_bottom, r.getBottom()));
    g.fillRect(r);
    g.setColour(hw::tok::edge);
    g.drawHorizontalLine(getHeight() - 1, 0.0f, r.getWidth());
    g.drawVerticalLine(getWidth() - 1, 0.0f, r.getHeight());
    if (t) {
        // The colour stripe, with a soft glow.
        const auto colour = colour_from_hex(t->color);
        const auto stripe = r.withWidth(5.0f);
        juce::Path p;
        p.addRectangle(stripe);
        juce::DropShadow(colour.withAlpha(0.55f), 6, {}).drawForPath(g, p);
        g.setColour(colour);
        g.fillRect(stripe);
    }
    // The meter's well.
    hw::paint::well(g, meter_.getBounds().toFloat().expanded(4.0f, 3.0f), 3.0f);
    hw::paint::silk(g, "Vol", vol_.getBounds().toFloat().withY(static_cast<float>(vol_.getY() - 11)).withHeight(10.0f), 8.5f,
                    juce::Justification::centred);
    hw::paint::silk(g, "Pan", pan_.getBounds().toFloat().withY(static_cast<float>(pan_.getY() - 11)).withHeight(10.0f), 8.5f,
                    juce::Justification::centred);
    if (selected) {
        g.setColour(hw::Theme::get().led().withAlpha(0.7f));
        g.drawRect(getLocalBounds().withTrimmedLeft(5), 1);
    }
}

void TrackHeader::resized()
{
    const int h = getHeight();
    number_.setBounds(11, 6, 24, 32);
    scribble_.setBounds(39, 6, kWidth - 39 - 24, 32);
    rec_.setBounds(11, 44, 24, 22);
    solo_.setBounds(38, 44, 24, 22);
    mute_.setBounds(65, 44, 24, 22);
    tag_.setBounds(11, 70, 50, 20);
    trans_.setBounds(63, 70, 42, 20);
    // Under Treble and Trans, as wide as both: the instrument; then MIDI in and out.
    instrument_.setBounds(11, 94, 94, 20);
    versions_.setBounds(108, 96, 88, 18);
    midi_in_.setBounds(11, 118, 84, 18);
    midi_out_.setBounds(98, 118, 84, 18);
    vol_.setBounds(112, 52, 28, 28);
    pan_.setBounds(150, 52, 28, 28);
    vol_lcd_.setBounds(106, 82, 40, 15);
    pan_lcd_.setBounds(146, 82, 36, 15);
    meter_.setBounds(kWidth - 14, 8, 6, std::max(20, h - 16));
}

void TrackHeader::mouseDown(const juce::MouseEvent& e)
{
    context_.session.select_track(track_);
    if (e.mods.isPopupMenu()) show_menu();
}

void TrackHeader::version_chosen()
{
    const int id = versions_.getSelectedId();
    const auto* t = context_.session.data().find_track(track_);
    if (!t || id == 0) return;
    context_.session.select_track(track_);
    if (id == kNewCopy || id == kNewEmpty) {
        const auto made = context_.session.new_version(track_, id == kNewCopy);
        context_.status(made ? (id == kNewCopy ? juce::String("A new version, a copy to try the idea on")
                                               : juce::String("A new, empty version: the others keep their notes"))
                             : "This track has " + juce::String(session::Session::kMaxVersions) +
                                   " versions already: delete one to make room");
    } else if (id == kDelete) {
        const std::string current = t->active_version_id;
        const juce::String name(t->active_version() ? t->active_version()->name : std::string());
        juce::Component::SafePointer<TrackHeader> self(this);
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, "Delete " + name + "?",
                                           "Its notes go with it (Undo brings them back).", "Delete", "Cancel", this,
                                           juce::ModalCallbackFunction::create([self, current](int r) {
                                               if (self == nullptr) return;
                                               if (r == 1) self->context_.session.delete_version(self->track_, current);
                                               self->refresh();
                                           }));
    } else if (id > 0 && id <= static_cast<int>(t->versions.size())) {
        const auto& v = t->versions[static_cast<std::size_t>(id - 1)];
        if (v.id != t->active_version_id) {
            context_.session.set_active_version(track_, v.id);
            context_.status(juce::String(t->name) + ": " + juce::String(v.name));
        }
    }
    refresh();
}

void TrackHeader::show_midi_menu(bool input)
{
    context_.session.select_track(track_);
    const auto* t = context_.session.data().find_track(track_);
    if (!t) return;
    juce::PopupMenu m;
    const std::string current = input ? t->midi_input : t->midi_output;
    m.addSectionHeader(input ? "MIDI In" : "MIDI Out");
    m.addItem(input ? "All inputs" : "None (the instrument plays)", true, current.empty(), [this, input] {
        if (input) context_.session.set_midi_input(track_, {});
        else context_.session.set_midi_output(track_, {});
    });
    const auto devices = input ? juce::MidiInput::getAvailableDevices() : juce::MidiOutput::getAvailableDevices();
    if (devices.isEmpty()) m.addItem(input ? "No MIDI inputs connected" : "No MIDI outputs connected", false, false, [] {});
    for (const auto& d : devices) {
        const std::string name = d.name.toStdString();
        m.addItem(d.name, true, name == current, [this, input, name] {
            if (input) context_.session.set_midi_input(track_, name);
            else context_.session.set_midi_output(track_, name);
        });
    }
    // One saved with the project but not connected now.
    if (!current.empty() && std::none_of(devices.begin(), devices.end(), [&](const auto& d) { return d.name.toStdString() == current; }))
        m.addItem(juce::String(current) + " (not connected)", false, true, [] {});
    m.addSeparator();
    m.addItem("MIDI Setup...", [this] { context_.commands.invokeDirectly(cmd::midiSetup, true); });
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(input ? &midi_in_ : &midi_out_));
}

void TrackHeader::show_menu()
{
    juce::PopupMenu m;
    const auto& sel = context_.session.selection();
    const auto& d = context_.session.data();
    const Ticks here = context_.engine.position();
    const int bar = d.meter.bar_at(here);
    m.addItem("New phrase from the selection", sel.has_range(), false, [this, sel] {
        context_.session.add_phrase(track_, "Phrase", sel.from, sel.to);
    });
    m.addItem("New phrase at the playhead (one bar)", true, false, [this, bar] {
        const auto& meter = context_.session.data().meter;
        context_.session.add_phrase(track_, "Phrase", meter.bar_start(bar), meter.bar_start(bar + 1));
    });
    m.addSeparator();
    // Its Producer track(s): consolidated (one track switches articulations)
    // or expanded (a track per sound).
    const auto* t = d.find_track(track_);
    const auto* part = t && !t->articulation_parent.empty() ? d.find_track(t->articulation_parent) : t;
    const bool expanded = part && part->articulations_expanded;
    m.addItem("Show in Producer", [this] {
        context_.session.select_track(track_);
        context_.commands.invokeDirectly(cmd::showSequencer, true);
    });
    m.addItem(expanded ? "Consolidate to One Track" : "Expand Articulations to Tracks...", [this, expanded] {
        context_.session.select_track(track_);
        context_.commands.invokeDirectly(expanded ? cmd::consolidateArticulations : cmd::expandArticulations, true);
    });
    m.addSeparator();
    // The sketch (the Piano, normally): its notes to other tracks, its chords over the strings.
    m.addSubMenu("Send / Spread (selected range, or all)", sketch_menu(context_, track_));
    m.addSubMenu("Add to the Sketch Area", TrackPanel::sketch_area_menu(context_));
    m.addSeparator();
    m.addItem("Track...", [this] { TrackPanel::show_track_menu(context_, track_); });
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
}

// --- Phrase blocks ----------------------------------------------------------------------------

PhraseLayer::PhraseLayer(AppContext& context, std::string track_id, ui::NotationView& view)
    : context_(context), track_(std::move(track_id)), view_(view)
{
    setInterceptsMouseClicks(true, false);
}

juce::Rectangle<float> PhraseLayer::block_rect(Ticks from, Ticks to) const
{
    const float x0 = view_.x_of_tick(from) + 2.0f, x1 = view_.x_of_tick(to) - 2.0f;
    return {x0, 2.0f, std::max(6.0f, x1 - x0), static_cast<float>(getHeight()) - 4.0f};
}

PhraseLayer::Hit PhraseLayer::hit(juce::Point<int> p) const
{
    const auto* t = context_.session.data().find_track(track_);
    const auto* v = t ? t->active_version() : nullptr;
    if (!v) return {};
    const auto pf = p.toFloat();
    if (pf.x < music_left()) return {};  // the clef zone stays the staff's
    for (int i = static_cast<int>(v->phrases.size()); --i >= 0;) {
        const auto& ph = v->phrases[static_cast<std::size_t>(i)];
        const auto r = block_rect(ph.start, ph.end);
        if (!r.expanded(4.0f, 0.0f).contains(pf)) continue;
        if (std::abs(pf.x - r.getX()) <= 4.0f) return {i, Part::Left};
        if (std::abs(pf.x - r.getRight()) <= 4.0f) return {i, Part::Right};
        if (pf.y <= r.getY() + kNameStrip) return {i, Part::Name};
    }
    return {};
}

bool PhraseLayer::hitTest(int x, int y) { return dragging_ || hit({x, y}).part != Part::None; }

void PhraseLayer::paint(juce::Graphics& g)
{
    const auto* t = context_.session.data().find_track(track_);
    const auto* v = t ? t->active_version() : nullptr;
    if (!v) return;
    const auto colour = colour_from_hex(t->color);
    // Blocks scroll under the sticky clef zone.
    g.reduceClipRegion(juce::Rectangle<int>(static_cast<int>(music_left()), 0, getWidth(), getHeight()));
    for (std::size_t i = 0; i < v->phrases.size(); ++i) {
        const auto& ph = v->phrases[i];
        const bool moving = dragging_ && drag_.index == static_cast<int>(i);
        auto draw = [&](Ticks from, Ticks to, float alpha) {
            const auto r = block_rect(from, to);
            if (r.getRight() < 0.0f || r.getX() > static_cast<float>(getWidth())) return;
            g.setColour(colour.withAlpha(0.09f * alpha));
            g.fillRoundedRectangle(r, 5.0f);
            g.setColour(colour.withAlpha(0.55f * alpha));
            g.drawRoundedRectangle(r, 5.0f, 1.0f);
            // The name strip, solid, with the name in dark capitals.
            juce::Path strip;
            strip.addRoundedRectangle(r.getX(), r.getY(), r.getWidth(), static_cast<float>(kNameStrip), 5.0f, 5.0f, true, true,
                                      false, false);
            g.setColour(colour.withAlpha(alpha));
            g.fillPath(strip);
            g.setColour(juce::Colour(0xff121417).withAlpha(alpha));
            g.setFont(hw::fonts::silk(10.0f, 0.06f, true));
            g.drawText((juce::String(t->name).upToFirstOccurrenceOf(juce::String::fromUTF8(" \xe2\x80\x94"), false, false).toUpperCase() +
                        juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(ph.name).toUpperCase()),
                       r.withHeight(static_cast<float>(kNameStrip)).reduced(6.0f, 0.0f), juce::Justification::centredLeft, true);
        };
        draw(ph.start, ph.end, moving ? 0.35f : 1.0f);
        if (moving) draw(preview_from_, preview_to_, 1.0f);
    }
}

float PhraseLayer::music_left() const { return view_.x_of_tick(view_.origin_tick()) - 1.0f; }

Ticks PhraseLayer::snap(Ticks t, bool to_bar) const
{
    const auto& m = context_.session.data().meter;
    t = std::max<Ticks>(0, t);
    const int bar = m.bar_at(t);
    if (to_bar) {
        const Ticks a = m.bar_start(bar), b = m.bar_start(bar + 1);
        return t - a < b - t ? a : b;
    }
    const Ticks beat = std::max<Ticks>(1, m.at_bar(bar).beat_ticks());
    const Ticks start = m.bar_start(bar);
    return start + (t - start + beat / 2) / beat * beat;
}

void PhraseLayer::mouseMove(const juce::MouseEvent& e)
{
    const auto h = hit(e.getPosition());
    setMouseCursor(h.part == Part::Name                               ? juce::MouseCursor::DraggingHandCursor
                   : h.part == Part::Left || h.part == Part::Right   ? juce::MouseCursor::LeftRightResizeCursor
                                                                      : juce::MouseCursor::NormalCursor);
}

void PhraseLayer::mouseDown(const juce::MouseEvent& e)
{
    context_.session.select_track(track_);
    drag_ = hit(e.getPosition());
    if (drag_.part == Part::None) return;
    if (e.mods.isPopupMenu()) {
        show_menu(drag_.index);
        drag_ = {};
        return;
    }
    const auto* v = context_.session.data().find_track(track_)->active_version();
    const auto& ph = v->phrases[static_cast<std::size_t>(drag_.index)];
    down_tick_ = view_.tick_at_x(e.position.x);
    preview_from_ = ph.start;
    preview_to_ = ph.end;
    dragging_ = true;
    ++gesture_;
}

void PhraseLayer::mouseDrag(const juce::MouseEvent& e)
{
    if (!dragging_) return;
    const auto* v = context_.session.data().find_track(track_)->active_version();
    if (drag_.index >= static_cast<int>(v->phrases.size())) return;
    const auto& ph = v->phrases[static_cast<std::size_t>(drag_.index)];
    const Ticks delta = view_.tick_at_x(e.position.x) - down_tick_;
    if (drag_.part == Part::Name) {
        // Moves by whole bars.
        const Ticks from = snap(ph.start + delta, true);
        preview_from_ = from;
        preview_to_ = from + (ph.end - ph.start);
    } else if (drag_.part == Part::Left) {
        preview_from_ = std::min(snap(ph.start + delta, false), ph.end - 1);
        preview_to_ = ph.end;
    } else {
        preview_from_ = ph.start;
        preview_to_ = std::max(snap(ph.end + delta, false), ph.start + 1);
    }
    repaint();
}

void PhraseLayer::mouseUp(const juce::MouseEvent&)
{
    if (!dragging_) return;
    dragging_ = false;
    const auto* v = context_.session.data().find_track(track_)->active_version();
    if (drag_.index < static_cast<int>(v->phrases.size())) {
        const auto ph = v->phrases[static_cast<std::size_t>(drag_.index)];
        if (drag_.part == Part::Name && preview_from_ != ph.start) {
            std::string why;
            if (!context_.session.move_phrase(track_, ph.id, preview_from_ - ph.start, &why) && context_.status)
                context_.status(why.empty() ? juce::String("The phrase could not move there") : juce::String(why));
        } else if ((drag_.part == Part::Left || drag_.part == Part::Right) &&
                   (preview_from_ != ph.start || preview_to_ != ph.end)) {
            context_.session.set_phrase_span(track_, ph.id, preview_from_, preview_to_,
                                             "Trim phrase" + std::string(gesture_ % 2 ? " " : ""));
        }
    }
    drag_ = {};
    repaint();
}

void PhraseLayer::show_menu(int index)
{
    const auto* v = context_.session.data().find_track(track_)->active_version();
    if (!v || index < 0 || index >= static_cast<int>(v->phrases.size())) return;
    const auto ph = v->phrases[static_cast<std::size_t>(index)];
    const std::string track = track_;
    auto& ctx = context_;
    juce::PopupMenu m;
    m.addSectionHeader(juce::String(ph.name));
    m.addItem("Rename...", [&ctx, track, ph] {
        auto* w = new juce::AlertWindow("Rename phrase", {}, juce::MessageBoxIconType::NoIcon);
        w->addTextEditor("name", juce::String(ph.name));
        w->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        w->enterModalState(true, juce::ModalCallbackFunction::create([&ctx, track, ph, w](int r) {
                               if (r == 1) ctx.session.rename_phrase(track, ph.id, w->getTextEditorContents("name").toStdString());
                           }),
                           true);
    });
    const Ticks here = ctx.engine.position();
    m.addItem("Split at the playhead", here > ph.start && here < ph.end, false,
              [&ctx, track, ph, here] { ctx.session.split_phrase(track, ph.id, here); });
    m.addItem("Duplicate", [&ctx, track, ph] {
        std::string why;
        if (ctx.session.duplicate_phrase(track, ph.id, &why).empty() && ctx.status) ctx.status(juce::String(why));
    });
    m.addItem("Select its notes", [&ctx, track, ph] {
        const auto* t = ctx.session.data().find_track(track);
        std::vector<notation::EventId> ids;
        if (const auto* ver = t ? t->active_version() : nullptr)
            for (std::size_t s = 0; s < ver->score.staff_count(); ++s)
                for (const auto& n : ver->score.notes(s))
                    if (!n.is_rest && n.start_tick >= ph.start && n.start_tick < ph.end) ids.push_back(n.id);
        ctx.session.select_notes(track, ids);
    });
    m.addSeparator();
    m.addItem("Remove the block (keep the notes)", [&ctx, track, ph] { ctx.session.remove_phrase(track, ph.id); });
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
}

}  // namespace vcc::desktop::composer_ui
