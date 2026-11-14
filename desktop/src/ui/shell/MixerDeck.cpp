#include "ui/shell/MixerDeck.h"

#include <cmath>

#include "ui/AudioTrackStyle.h"
#include "ui/Theme.h"
#include "ui/TrackPanel.h"
#include "ui/console/ConsoleStyle.h"

namespace vcc::desktop::shell {

namespace {
constexpr int kHeader = 30;

juce::Colour bus_colour(const project::Bus& b)
{
    const juce::Colour kind = b.kind == "master" ? hw::Theme::get().led() : b.kind == "fx" ? hw::tok::type_aux : juce::Colour(0xff5fbfb2);
    return colour_from_hex(b.color, kind);
}
}  // namespace

// --- Strip -----------------------------------------------------------------------------------

MixerDeck::Strip::Strip(AppContext& context, std::string owner_id, bool master)
    : context_(context), owner_(std::move(owner_id)), master_(master), writer_(context, owner_), slots_(context, true)
{
    for (auto* c : std::initializer_list<juce::Component*>{&slots_, &fader_, &pan_, &meter_l_, &meter_r_, &db_, &solo_, &mute_}) {
        addAndMakeVisible(c);
        c->setWantsKeyboardFocus(false);
    }
    slots_.set_owner(owner_);
    solo_.setVisible(!master_);
    solo_.set_solid(hw::tok::solo);
    mute_.set_solid(hw::tok::mute);
    db_.set_justification(juce::Justification::centred);
    pan_.setRange(-1.0, 1.0);
    pan_.setDoubleClickReturnValue(true, 0.0);
    pan_.setTitle(master_ ? "Balance" : "Pan");
    fader_.setTitle("Volume");
    fader_.onDragStart = [this] { writer_.begin("volume"); };
    fader_.onDragEnd = [this] { writer_.end("volume"); };
    fader_.onValueChange = [this] {
        const double db = std::isfinite(fader_.db()) ? std::max(-96.0, fader_.db()) : -96.0;
        writer_.write("volume", db, "Volume", [db](double& gain, double&, std::vector<project::Send>&) { gain = db; });
        db_.set_text(console::db_text(db, false));
    };
    pan_.onDragStart = [this] { writer_.begin("pan"); };
    pan_.onDragEnd = [this] { writer_.end("pan"); };
    pan_.onValueChange = [this] {
        const double p = pan_.getValue();
        writer_.write("pan", p, master_ ? "Balance" : "Pan", [p](double&, double& pan, std::vector<project::Send>&) { pan = p; });
        pan_.setTooltip(console::pan_text(p));
    };
    auto toggle = [this](bool solo) {
        const bool on = solo ? solo_.getToggleState() : mute_.getToggleState();
        if (context_.session.data().find_track(owner_)) {
            if (solo) context_.session.set_solo(owner_, on);
            else context_.session.set_mute(owner_, on);
        } else {
            context_.session.update_bus(owner_, std::string(solo ? "Solo " : "Mute ") + name_.toStdString(),
                                        [&](project::Bus& b) { (solo ? b.solo : b.mute) = on; });
        }
    };
    solo_.onClick = [toggle] { toggle(true); };
    mute_.onClick = [toggle] { toggle(false); };
    refresh();
}

void MixerDeck::Strip::refresh()
{
    const auto& d = context_.session.data();
    if (const auto* t = d.find_track(owner_)) {
        name_ = juce::String(t->name).upToFirstOccurrenceOf(juce::String::fromUTF8(" \xe2\x80\x94"), false, false);
        colour_ = t->kind == project::TrackKind::Audio ? audio_track_colour(*t) : colour_from_hex(t->color);
        fader_.set_db(t->gain_db, juce::dontSendNotification);
        pan_.setValue(t->pan, juce::dontSendNotification);
        solo_.setToggleState(t->solo, juce::dontSendNotification);
        mute_.setToggleState(t->mute, juce::dontSendNotification);
        db_.set_text(console::db_text(t->gain_db, false));
        selected_ = context_.session.selected_track() == owner_;
    } else if (const auto* b = d.find_bus(owner_)) {
        name_ = b->kind == "master" ? juce::String("Master") : juce::String(b->name);
        colour_ = bus_colour(*b);
        fader_.set_db(b->gain_db, juce::dontSendNotification);
        pan_.setValue(b->pan, juce::dontSendNotification);
        solo_.setToggleState(b->solo, juce::dontSendNotification);
        mute_.setToggleState(b->mute, juce::dontSendNotification);
        db_.set_text(console::db_text(b->gain_db, false));
        selected_ = false;
    }
    setTitle(name_);
    slots_.refresh();
    repaint();
}

void MixerDeck::Strip::follow_automation()
{
    if (!fader_.isMouseButtonDown())
        if (const auto v = context_.engine.automated_value(owner_, "volume")) {
            fader_.set_db(*v, juce::dontSendNotification);
            db_.set_text(console::db_text(*v, false));
        }
    if (!pan_.isMouseButtonDown())
        if (const auto v = context_.engine.automated_value(owner_, "pan")) pan_.setValue(*v, juce::dontSendNotification);
}

void MixerDeck::Strip::push_meter()
{
    const auto m = context_.engine.take_deck_meter(owner_, 2);
    auto db = [](float lin) { return lin > 0.0f ? 20.0f * std::log10(lin) : -100.0f; };
    meter_l_.push(db(m.left));
    meter_r_.push(db(m.right));
}

void MixerDeck::Strip::resized()
{
    auto r = getLocalBounds().reduced(5, 4);
    r.removeFromTop(20);  // the name plate
    r.removeFromTop(3);
    slots_.setBounds(r.removeFromTop(16));
    r.removeFromTop(4);
    pan_.setBounds(r.removeFromTop(28).withSizeKeepingCentre(28, 28));
    r.removeFromTop(4);
    auto keys = r.removeFromBottom(20);
    if (master_) {
        mute_.setBounds(keys.withSizeKeepingCentre(std::min(keys.getWidth(), 40), keys.getHeight()));
    } else {
        solo_.setBounds(keys.removeFromLeft(keys.getWidth() / 2 - 2));
        keys.removeFromLeft(4);
        mute_.setBounds(keys);
    }
    r.removeFromBottom(4);
    db_.setBounds(r.removeFromBottom(16));
    r.removeFromBottom(4);
    // The fader (120 px throw) with the stereo meter beside it.
    auto meters = r.removeFromRight(master_ ? 22 : 15);
    meter_l_.setBounds(meters.getX(), meters.getY() + 6, 5, meters.getHeight() - 12);
    meter_r_.setBounds(meters.getX() + (master_ ? 9 : 7), meters.getY() + 6, 5, meters.getHeight() - 12);
    fader_.setBounds(r);
}

void MixerDeck::Strip::paint(juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced(1.0f);
    g.setGradientFill(juce::ColourGradient::vertical(selected_ ? hw::tok::panel_top.brighter(0.08f) : hw::tok::panel_top, b.getY(),
                                                     hw::tok::panel_bottom, b.getBottom()));
    g.fillRoundedRectangle(b, 4.0f);
    g.setColour(hw::tok::edge);
    g.drawRoundedRectangle(b, 4.0f, 1.0f);
    // The coloured name plate.
    const auto plate = b.reduced(4.0f, 3.0f).withHeight(20.0f);
    g.setColour(colour_);
    g.fillRoundedRectangle(plate, 3.0f);
    g.setColour(juce::Colour(0xff101214));
    g.setFont(hw::fonts::silk(10.0f, 0.06f, true));
    g.drawFittedText(name_.toUpperCase(), plate.reduced(3.0f, 0.0f).toNearestInt(), juce::Justification::centred, 1, 0.8f);
}

void MixerDeck::Strip::paintOverChildren(juce::Graphics& g)
{
    // The master is outlined in the LED colour; the selected track in a softer one.
    if (master_ || selected_) {
        g.setColour(hw::Theme::get().led().withAlpha(master_ ? 0.9f : 0.5f));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 4.0f, master_ ? 1.6f : 1.0f);
    }
}

void MixerDeck::Strip::mouseUp(const juce::MouseEvent& e)
{
    // Click the plate to select the track; right-click for its menu.
    if (!context_.session.data().find_track(owner_)) {
        if (e.mods.isPopupMenu()) console::show_output_menu(context_, owner_, *this);
        return;
    }
    context_.session.select_track(owner_);
    if (e.mods.isPopupMenu()) TrackPanel::show_track_menu(context_, owner_);
}

// --- Deck ------------------------------------------------------------------------------------

MixerDeck::MixerDeck(AppContext& context) : hw::Panel({}, true), context_(context)
{
    addAndMakeVisible(viewport_);
    viewport_.setViewedComponent(&holder_, false);
    viewport_.setScrollBarsShown(false, true);
    viewport_.setScrollBarThickness(6);
    for (auto* k : {&keyboard_, &console_}) {
        addAndMakeVisible(*k);
        k->set_legend_size(10.0f);
        k->setWantsKeyboardFocus(false);
    }
    keyboard_.setTooltip("Show the keyboard in the bottom deck");
    console_.setTooltip("Open the full console (every strip's EQ, dynamics and sends)");
    keyboard_.onClick = [this] {
        if (on_keyboard) on_keyboard();
    };
    console_.onClick = [this] { context_.commands.invokeDirectly(cmd::showMixer, true); };
    listener_ = context_.session.add_listener([this](session::Change c) {
        if (c == session::Change::Document || c == session::Change::Selection) rebuild();
    });
    rebuild();
    startTimerHz(30);
}

MixerDeck::~MixerDeck()
{
    stopTimer();
    context_.session.remove_listener(listener_);
}

MixerDeck::Strip* MixerDeck::strip_for(const std::string& owner_id)
{
    for (auto& s : strips_)
        if (s->owner() == owner_id) return s.get();
    if (master_ && master_->owner() == owner_id) return master_.get();
    return nullptr;
}

void MixerDeck::rebuild()
{
    // Tracks in order, then group and FX buses, then the master.
    const auto& d = context_.session.data();
    std::vector<std::string> ids;
    for (const auto* t : context_.session.music_tracks()) ids.push_back(t->id);
    for (const char* kind : {"group", "fx"})
        for (const auto& b : d.buses)
            if (b.kind == kind) ids.push_back(b.id);
    for (const auto& b : d.buses)
        if (b.kind == "master") ids.push_back(b.id);
    if (ids != ids_) {
        ids_ = ids;
        strips_.clear();
        master_.reset();
        for (const auto& id : ids) {
            const auto* b = d.find_bus(id);
            if (b && b->kind == "master") {
                // The master stays in view, at the right.
                master_ = std::make_unique<Strip>(context_, id, true);
                addAndMakeVisible(*master_);
                continue;
            }
            strips_.push_back(std::make_unique<Strip>(context_, id, false));
            holder_.addAndMakeVisible(*strips_.back());
        }
        resized();
    } else {
        for (auto& s : strips_) s->refresh();
        if (master_) master_->refresh();
    }
}

void MixerDeck::timerCallback()
{
    if (!isShowing()) return;
    for (auto& s : strips_) {
        s->push_meter();
        s->follow_automation();
    }
    if (master_) {
        master_->push_meter();
        master_->follow_automation();
    }
}

void MixerDeck::resized()
{
    auto r = getLocalBounds().reduced(16, 0);
    auto header = r.removeFromTop(kHeader).withTrimmedTop(5).withHeight(22);
    keyboard_.setBounds(header.removeFromRight(92));
    header.removeFromRight(6);
    console_.setBounds(header.removeFromRight(84));
    r.removeFromBottom(8);
    const int h = r.getHeight() - 8;
    if (master_) {
        master_->setBounds(r.removeFromRight(kMasterStrip).withHeight(h));
        r.removeFromRight(8);
    }
    viewport_.setBounds(r);
    int x = 0;
    for (auto& s : strips_) {
        s->setBounds(x, 0, kStrip, h);
        x += kStrip + 4;
    }
    holder_.setSize(std::max(x, r.getWidth()), h);
}

void MixerDeck::paintOverChildren(juce::Graphics& g)
{
    hw::paint::silk(g, "Mixer", {30.0f, 5.0f, 80.0f, 22.0f}, 11.0f);
    hw::paint::silk(g, juce::String(strips_.size()) + " strips + master", {100.0f, 5.0f, 160.0f, 22.0f}, 9.5f);
}

}  // namespace vcc::desktop::shell
