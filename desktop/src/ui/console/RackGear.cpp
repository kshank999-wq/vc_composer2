#include "ui/console/RackGear.h"

#include <cmath>
#include <map>
#include <optional>

#include "audio/ChannelDsp.h"
#include "ui/console/MixActions.h"
#include "ui/hw/HwControls.h"
#include "vcc/project/Automation.h"

namespace vcc::desktop::console {

namespace {

constexpr float kPi = juce::MathConstants<float>::pi;
constexpr float kArc = kPi * 0.75f;
constexpr int kEar = 24;        // a rack ear, each side
constexpr int kOneU = 96;       // one rack unit, on screen
constexpr int kRackWidth = 860;

struct Spec {
    const char* kind;
    const char* name;
    const char* tagline;
    juce::uint32 plate, ink;
    int units;
    bool vu;
};

const Spec kSpecs[] = {
    {"eq", "VC EQ", "FOUR BAND PARAMETRIC EQUALISER", 0xff46505c, 0xffe9edf1, 2, false},
    {"compressor", "VC Compressor", "PROGRAM COMPRESSOR", 0xff161719, 0xffdccba0, 1, true},
    {"gate", "VC Gate", "NOISE GATE  /  EXPANDER", 0xff22372b, 0xffd5e6d8, 1, false},
    {"limiter", "VC Limiter", "BRICKWALL PEAK LIMITER", 0xff5c1d1d, 0xfff1ddd6, 1, true},
    {"reverb", "VC Reverb", "STEREO PLATE REVERBERATOR", 0xff1e2949, 0xffcfd8f4, 1, false},
    {"delay", "VC Delay", "ANALOGUE-STYLE ECHO", 0xffcbb68b, 0xff2a2214, 1, false},
    {"saturation", "VC Saturator", "VALVE SATURATION", 0xff6a3913, 0xfff4dec2, 1, false},
    {"hardware", "VC Outboard", "HARDWARE INSERT  -  SEND / RETURN", 0xff3a3d42, 0xffd3d9e0, 1, false},
};
const Spec kPlugin{"plugin", "", "PLUG-IN", 0xff2a2c30, 0xffc8ced6, 1, false};

const Spec& spec_for(const std::string& kind)
{
    for (const auto& s : kSpecs)
        if (kind == s.kind) return s;
    return kPlugin;
}

/// A knob's cap colour, by what it turns.
juce::Colour cap_for(const std::string& param)
{
    auto has = [&](const char* w) { return param.find(w) != std::string::npos; };
    if (has("gain") || has("threshold") || has("ceiling") || has("drive") || has("send_db") || has("return_db"))
        return juce::Colour(0xffc8433a);
    if (has("freq") || has("time")) return juce::Colour(0xff3a6fc8);
    if (has("mix") || has("room") || has("width") || has("damping") || has("feedback")) return juce::Colour(0xffe2d8b9);
    if (has("pair")) return juce::Colour(0xff3ea36a);
    return juce::Colour(0xffb9bec4);
}

juce::String value_text(const project::ParamInfo& p, double v)
{
    if (p.unit == "dB") return (v > 0.05 ? "+" : "") + juce::String(v, 1) + " dB";
    if (p.unit == "Hz") return v >= 1000.0 ? juce::String(v / 1000.0, v >= 10000.0 ? 1 : 2) + " kHz" : juce::String(juce::roundToInt(v)) + " Hz";
    if (p.unit == "ms") return v < 10.0 ? juce::String(v, 1) + " ms" : juce::String(juce::roundToInt(v)) + " ms";
    if (p.unit == ":1") return juce::String(v, 1) + " : 1";
    if (p.unit == "pair") {
        const int n = juce::roundToInt(v);
        return juce::String(2 * n - 1) + "-" + juce::String(2 * n);
    }
    if (p.unit == "smp") return juce::String(juce::roundToInt(v)) + " smp";
    return juce::String(juce::roundToInt(v * 100.0)) + "%";
}

/// The knob on a faceplate: a scale printed round it, a knurled skirt, a
/// coloured cap and a white pointer.
class GearKnob final : public hw::Knob {
public:
    GearKnob(juce::Colour cap, juce::Colour ink) : cap_(cap), ink_(ink) {}
    void paint(juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        const float d = std::min(b.getWidth(), b.getHeight()) - 2.0f;
        const auto c = b.getCentre();
        // The scale, printed on the panel.
        g.setColour(ink_.withAlpha(0.75f));
        for (int i = 0; i <= 10; ++i) {
            const float a = -kArc + 2.0f * kArc * static_cast<float>(i) / 10.0f;
            const float r0 = d * 0.5f - (i % 5 == 0 ? 6.0f : 4.0f), r1 = d * 0.5f - 1.0f;
            g.drawLine(c.x + std::sin(a) * r0, c.y - std::cos(a) * r0, c.x + std::sin(a) * r1, c.y - std::cos(a) * r1,
                       i % 5 == 0 ? 1.4f : 1.0f);
        }
        // Shadow, skirt, knurling.
        const float skirt = d * 0.36f;
        g.setColour(juce::Colours::black.withAlpha(0.55f));
        g.fillEllipse(juce::Rectangle<float>(skirt * 2.0f, skirt * 2.0f).withCentre(c.translated(1.5f, 2.5f)));
        juce::ColourGradient sg(juce::Colour(0xff3a3e44), c.x, c.y - skirt, juce::Colour(0xff0e0f11), c.x, c.y + skirt, false);
        g.setGradientFill(sg);
        g.fillEllipse(juce::Rectangle<float>(skirt * 2.0f, skirt * 2.0f).withCentre(c));
        g.setColour(juce::Colours::black.withAlpha(0.5f));
        for (int i = 0; i < 28; ++i) {
            const float a = 2.0f * kPi * static_cast<float>(i) / 28.0f;
            g.drawLine(c.x + std::sin(a) * skirt * 0.82f, c.y - std::cos(a) * skirt * 0.82f, c.x + std::sin(a) * skirt,
                       c.y - std::cos(a) * skirt, 1.0f);
        }
        // The cap.
        const float cap = d * 0.25f;
        juce::ColourGradient cg(cap_.brighter(0.45f), c.x - cap * 0.5f, c.y - cap, cap_.darker(0.55f), c.x + cap * 0.4f,
                                c.y + cap, true);
        g.setGradientFill(cg);
        g.fillEllipse(juce::Rectangle<float>(cap * 2.0f, cap * 2.0f).withCentre(c));
        g.setColour(juce::Colours::white.withAlpha(0.22f));
        g.drawEllipse(juce::Rectangle<float>(cap * 2.0f, cap * 2.0f).withCentre(c).reduced(0.5f), 1.0f);
        // The pointer, across skirt and cap.
        const float a = angle();
        g.setColour(juce::Colours::white);
        g.drawLine(c.x + std::sin(a) * cap * 0.15f, c.y - std::cos(a) * cap * 0.15f, c.x + std::sin(a) * skirt * 0.96f,
                   c.y - std::cos(a) * skirt * 0.96f, 2.2f);
        if (hasKeyboardFocus(false)) {
            g.setColour(ink_.withAlpha(0.6f));
            g.drawEllipse(b.reduced(1.0f), 1.0f);
        }
    }

private:
    juce::Colour cap_, ink_;
};

void paint_ears(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour plate)
{
    for (auto ear : {r.withWidth(static_cast<float>(kEar)), r.withLeft(r.getRight() - static_cast<float>(kEar))}) {
        g.setColour(plate.darker(0.25f));
        g.fillRect(ear);
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.drawVerticalLine(juce::roundToInt(ear.getX() == r.getX() ? ear.getRight() - 1.0f : ear.getX()), ear.getY(), ear.getBottom());
        // Slotted holes with their screws.
        const int holes = std::max(1, juce::roundToInt(r.getHeight() / static_cast<float>(kOneU))) * 2;
        for (int i = 0; i < holes; ++i) {
            const float y = ear.getY() + (static_cast<float>(i) + 0.5f) * ear.getHeight() / static_cast<float>(holes);
            const auto hole = juce::Rectangle<float>(12.0f, 8.0f).withCentre({ear.getCentreX(), y});
            g.setColour(juce::Colours::black.withAlpha(0.8f));
            g.fillRoundedRectangle(hole, 4.0f);
            hw::paint::screw(g, hole.getCentre(), 0.6f + static_cast<float>(i), 8.0f);
        }
    }
}

void paint_plate(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour plate)
{
    juce::ColourGradient grad(plate.brighter(0.1f), r.getX(), r.getY(), plate.darker(0.18f), r.getX(), r.getBottom(), false);
    g.setGradientFill(grad);
    g.fillRect(r);
    // Brushed metal: fine horizontal grain.
    juce::Random grain(static_cast<juce::int64>(plate.getARGB()));
    for (float y = r.getY() + 1.0f; y < r.getBottom(); y += 1.5f) {
        g.setColour((grain.nextBool() ? juce::Colours::white : juce::Colours::black).withAlpha(0.015f + grain.nextFloat() * 0.025f));
        g.drawHorizontalLine(juce::roundToInt(y), r.getX(), r.getRight());
    }
    g.setColour(juce::Colours::white.withAlpha(0.18f));
    g.drawHorizontalLine(juce::roundToInt(r.getY()), r.getX(), r.getRight());
    g.setColour(juce::Colours::black.withAlpha(0.6f));
    g.drawHorizontalLine(juce::roundToInt(r.getBottom() - 1.0f), r.getX(), r.getRight());
}

/// One unit of the rack: the faceplate for one insert slot.
class GearUnit final : public juce::Component, private juce::Timer {
public:
    GearUnit(AppContext& c, std::string owner, int slot, const project::InsertSlot& s)
        : context_(c), owner_(std::move(owner)), slot_(slot), kind_(s.kind), spec_(spec_for(s.kind))
    {
        setComponentID("gear_" + juce::String(slot));
        name_ = gear_name(s);
        setTitle(name_);
        const auto& d = c.session.data();
        std::vector<project::ParamInfo> params;
        if (const auto* t = d.find_track(owner_)) params = project::automatable_params(*t, d);
        else if (const auto* b = d.find_bus(owner_)) params = project::automatable_params(*b, d);
        const std::string prefix = "insert:" + std::to_string(slot) + ":";
        for (const auto& p : params) {
            if (p.id.rfind(prefix, 0) != 0) continue;
            const std::string field = p.id.substr(prefix.size());
            if (field == "latency") {  // measured, not turned: shown on the LCD
                latency_id_ = p.id;
                continue;
            }
            Control ctl;
            ctl.info = p;
            ctl.field = field;
            ctl.label = juce::String(p.name).fromFirstOccurrenceOf(juce::String(slot + 1) + " ", false, false).toUpperCase();
            // The EQ's bands are named over their columns: its knobs say GAIN and FREQ.
            if (kind_ == "eq") ctl.label = ctl.label.fromFirstOccurrenceOf(" ", false, false);
            ctl.knob = std::make_unique<GearKnob>(cap_for(field), ink());
            auto& k = *ctl.knob;
            k.setRange(p.min, p.max, p.toggle || p.unit == "pair" ? 1.0 : 0.0);
            if (p.log_scale && p.min > 0.0) k.setSkewFactorFromMidPoint(std::sqrt(p.min * p.max));
            k.setDoubleClickReturnValue(true, p.default_value);
            k.setTitle(name_ + " " + ctl.label.toLowerCase());
            k.setTooltip(ctl.label.toLowerCase() + " (drag up or down; double-click for the default)");
            const std::string id = p.id;
            const juce::String what = juce::String(p.name);
            k.onDragStart = [this] { ++gesture_; };
            k.onValueChange = [this, id, what, kp = &k] {
                if (refreshing_) return;
                apply(id, what, kp->getValue());
                repaint();
            };
            addAndMakeVisible(k);
            controls_.push_back(std::move(ctl));
        }
        if (kind_ == "plugin") {
            edit_.setTooltip("Open the plug-in's own window");
            edit_.onClick = [this] {
                if (context_.show_insert_window) context_.show_insert_window(owner_, slot_);
            };
            addAndMakeVisible(edit_);
        }
        if (kind_ == "hardware") {
            ping_.setComponentID("hardware_ping");
            ping_.setTooltip("Measure the round trip through the gear: a click goes out and is listened for on the return");
            ping_.onClick = [this] { start_ping(); };
            addAndMakeVisible(ping_);
            info_.setComponentID("hardware_info");
            info_.setInterceptsMouseClicks(false, false);
            addAndMakeVisible(info_);
        }
        refresh();
        setSize(kRackWidth, kOneU * spec_.units);
    }

    int slot() const noexcept { return slot_; }
    const std::string& kind() const noexcept { return kind_; }
    bool has_vu() const noexcept { return spec_.vu; }

    /// Lit for a moment: the unit the click opened.
    void flash()
    {
        flash_ = 20;
        startTimerHz(20);
    }

    void refresh()
    {
        const auto& d = context_.session.data();
        const auto* list = inserts_of(context_, owner_);
        if (list && slot_ < static_cast<int>(list->size())) {
            const auto& s = (*list)[static_cast<std::size_t>(slot_)];
            bypass_ = s.bypass;
            suggestion_ = s.suggestion;
        }
        refreshing_ = true;
        for (auto& c : controls_) {
            if (c.knob->isMouseButtonDown()) continue;
            std::optional<double> v;
            if (const auto* t = d.find_track(owner_)) v = project::static_value(*t, c.info.id);
            else if (const auto* b = d.find_bus(owner_)) v = project::static_value(*b, c.info.id);
            c.knob->setValue(v ? *v : c.info.default_value, juce::dontSendNotification);
        }
        refreshing_ = false;
        if (kind_ == "hardware") refresh_hardware();
        repaint();
    }

    /// The output level for the VU (dB), pushed by the rack at 30 Hz.
    void push_level(float db)
    {
        // VU ballistics: about 300 ms to get there.
        vu_ += (std::max(-40.0f, db) - vu_) * 0.25f;
        repaint(vu_area_);
    }

    juce::Rectangle<int> power_area() const { return power_; }

    void paint(juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        const auto plate = juce::Colour(spec_.plate);
        paint_plate(g, r, plate);
        paint_ears(g, r, plate);
        if (flash_ > 0) {
            g.setColour(hw::Theme::get().led().withAlpha(0.06f * static_cast<float>(flash_) / 4.0f));
            g.drawRect(r.reduced(static_cast<float>(kEar), 1.0f), 2.0f);
        }
        // The badge and the model.
        auto face = getLocalBounds().reduced(kEar + 14, 8);
        auto brand = face.removeFromLeft(196).toFloat();
        const auto badge = brand.removeFromTop(18.0f).withWidth(30.0f);
        g.setColour(ink());
        g.drawRoundedRectangle(badge.reduced(0.5f), 3.0f, 1.2f);
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.drawText("VC", badge, juce::Justification::centred);
        g.setFont(juce::FontOptions(spec_.units > 1 ? 24.0f : 20.0f, juce::Font::bold));
        const juce::String model = kind_ == "plugin" ? name_ : juce::String(spec_.name).fromFirstOccurrenceOf("VC ", false, false);
        g.drawFittedText(model.toUpperCase(), brand.removeFromTop(spec_.units > 1 ? 34.0f : 28.0f).toNearestInt(),
                         juce::Justification::centredLeft, 1, 0.6f);
        g.setColour(ink().withAlpha(0.7f));
        g.setFont(juce::FontOptions(8.5f));
        g.drawFittedText(spec_.tagline, brand.removeFromTop(14.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
        if (suggestion_) {
            g.setColour(hw::tok::meter_amber);
            g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
            g.drawText("SUGGESTED - NOT YET IN", brand.removeFromTop(14.0f), juce::Justification::centredLeft);
        }
        // The power switch (bypass), with its lamp.
        paint_power(g);
        // Group lines and the legends: label over each knob, value under it.
        for (auto& c : controls_) {
            const auto kb = c.knob->getBounds().toFloat();
            g.setColour(ink().withAlpha(0.9f));
            g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
            g.drawFittedText(c.label, juce::Rectangle<float>(kb.getX() - 16.0f, kb.getY() - 13.0f, kb.getWidth() + 32.0f, 12.0f)
                                          .toNearestInt(),
                             juce::Justification::centred, 1, 0.7f);
            const auto lcd = juce::Rectangle<float>(kb.getCentreX() - 30.0f, kb.getBottom() + 1.0f, 60.0f, 14.0f);
            hw::paint::lcd_window(g, lcd);
            hw::paint::lcd_text(g, value_text(c.info, c.knob->getValue()), lcd, 9.5f, juce::Justification::centred,
                                bypass_ || suggestion_ ? 0.45f : 1.0f);
        }
        if (kind_ == "eq") {
            // Band names over the columns, and the grooves between them.
            static const char* bands[] = {"LOW", "LOW MID", "HIGH MID", "HIGH"};
            for (int b = 0; b < 4 && b < static_cast<int>(columns_.size()); ++b) {
                const auto col = columns_[static_cast<std::size_t>(b)].toFloat();
                g.setColour(ink());
                g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
                g.drawText(bands[b], col.withHeight(14.0f), juce::Justification::centred);
                if (b > 0) {
                    g.setColour(juce::Colours::black.withAlpha(0.35f));
                    g.drawVerticalLine(juce::roundToInt(col.getX()), col.getY() + 4.0f, col.getBottom() - 4.0f);
                    g.setColour(juce::Colours::white.withAlpha(0.08f));
                    g.drawVerticalLine(juce::roundToInt(col.getX()) + 1, col.getY() + 4.0f, col.getBottom() - 4.0f);
                }
            }
        }
        if (spec_.vu) paint_vu(g);
        if (kind_ == "plugin") {
            g.setColour(ink().withAlpha(0.6f));
            g.setFont(juce::FontOptions(10.0f));
            g.drawText("Its controls are in the plug-in's own window", edit_.getBounds().toFloat().translated(-300.0f, 0.0f).withWidth(290.0f),
                       juce::Justification::centredRight);
        }
        if (kind_ == "hardware") hw::paint::lcd_window(g, info_.getBounds().toFloat().expanded(3.0f, 2.0f));
    }

    void resized() override
    {
        auto face = getLocalBounds().reduced(kEar + 14, 8);
        face.removeFromLeft(196);
        power_ = face.removeFromLeft(56);
        face.removeFromLeft(10);
        if (spec_.vu) {
            vu_area_ = face.removeFromRight(150).reduced(4, 6);
            face.removeFromRight(8);
        }
        if (kind_ == "plugin") {
            edit_.setBounds(face.removeFromRight(110).withSizeKeepingCentre(100, 28));
            return;
        }
        if (kind_ == "hardware") {
            auto right = face.removeFromRight(250);
            ping_.setBounds(right.removeFromBottom(26).removeFromRight(110));
            right.removeFromBottom(6);
            info_.setBounds(right.reduced(4, 3));
            face.removeFromRight(10);
        }
        const int knob = 46;
        if (kind_ == "eq") {
            // Four columns: gain over frequency.
            columns_.clear();
            const int w = face.getWidth() / 4;
            for (int b = 0; b < 4; ++b) columns_.push_back(face.withX(face.getX() + b * w).withWidth(w));
            for (std::size_t i = 0; i < controls_.size(); ++i) {
                const int band = static_cast<int>(i / 2), row = static_cast<int>(i % 2);
                if (band >= 4) break;
                const auto col = columns_[static_cast<std::size_t>(band)];
                const int cy = col.getY() + 52 + row * 80;
                controls_[i].knob->setBounds(juce::Rectangle<int>(knob, knob).withCentre({col.getCentreX(), cy}));
            }
            return;
        }
        const int n = std::max(1, static_cast<int>(controls_.size()));
        const int w = std::min(96, face.getWidth() / n);
        for (int i = 0; i < static_cast<int>(controls_.size()); ++i)
            controls_[static_cast<std::size_t>(i)].knob->setBounds(
                juce::Rectangle<int>(knob, knob).withCentre({face.getX() + w * i + w / 2, face.getCentreY() - 2}));
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (e.mouseWasDraggedSinceMouseDown()) return;
        if (e.mods.isPopupMenu()) {
            show_insert_menu(context_, owner_, slot_, *this);
            return;
        }
        if (power_.contains(e.getPosition())) toggle_insert_bypass(context_, owner_, slot_);
    }

private:
    struct Control {
        project::ParamInfo info;
        std::string field;
        juce::String label;
        std::unique_ptr<GearKnob> knob;
    };

    juce::Colour ink() const { return juce::Colour(spec_.ink); }

    void paint_power(juce::Graphics& g)
    {
        const auto p = power_.toFloat();
        const bool on = !bypass_ && !suggestion_;
        g.setColour(ink().withAlpha(0.85f));
        g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
        g.drawText("POWER", p.withHeight(12.0f), juce::Justification::centred);
        hw::paint::led(g, {p.getCentreX(), p.getY() + 20.0f}, on, juce::Colour(0xffff3b30), 8.0f);
        // A bat-handle toggle: up is in, down is bypassed.
        const auto plate = juce::Rectangle<float>(20.0f, 34.0f).withCentre({p.getCentreX(), p.getY() + 50.0f});
        g.setColour(juce::Colours::black.withAlpha(0.55f));
        g.fillRoundedRectangle(plate, 4.0f);
        g.setColour(juce::Colour(0xffa9aeb4));
        g.fillEllipse(juce::Rectangle<float>(12.0f, 12.0f).withCentre(plate.getCentre()));
        const float tip = on ? plate.getY() - 2.0f : plate.getBottom() + 2.0f;
        juce::Path bat;
        bat.startNewSubPath(plate.getCentreX(), plate.getCentreY());
        bat.lineTo(plate.getCentreX(), tip);
        g.setColour(juce::Colour(0xffe4e7ea));
        g.strokePath(bat, juce::PathStrokeType(4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.fillEllipse(juce::Rectangle<float>(8.0f, 8.0f).withCentre({plate.getCentreX(), tip}));
        g.setColour(ink().withAlpha(0.7f));
        g.setFont(juce::FontOptions(8.0f));
        g.drawText(on ? "IN" : "BYPASS", juce::Rectangle<float>(p.getX(), plate.getBottom() + 6.0f, p.getWidth(), 10.0f),
                   juce::Justification::centred);
    }

    void paint_vu(juce::Graphics& g)
    {
        const auto r = vu_area_.toFloat();
        g.setColour(juce::Colours::black);
        g.fillRoundedRectangle(r, 3.0f);
        const auto face = r.reduced(3.0f);
        juce::ColourGradient lamp(juce::Colour(0xfff6e7b4), face.getCentreX(), face.getBottom(), juce::Colour(0xffd9c487),
                                  face.getX(), face.getY(), true);
        g.setGradientFill(lamp);
        g.fillRoundedRectangle(face, 2.0f);
        // The scale: -20 .. +3 VU on an arc, red from 0.
        const juce::Point<float> pivot{face.getCentreX(), face.getBottom() + face.getHeight() * 0.55f};
        const float radius = face.getHeight() * 1.15f;
        auto angle_for = [](float vu) { return juce::jmap(juce::jlimit(-20.0f, 3.0f, vu), -20.0f, 3.0f, -0.75f, 0.75f); };
        for (float m : {-20.0f, -10.0f, -7.0f, -5.0f, -3.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 3.0f}) {
            const float a = angle_for(m);
            g.setColour(m > 0.0f ? juce::Colour(0xffc0302a) : juce::Colour(0xff2a2418));
            g.drawLine(pivot.x + std::sin(a) * radius * 0.9f, pivot.y - std::cos(a) * radius * 0.9f, pivot.x + std::sin(a) * radius,
                       pivot.y - std::cos(a) * radius, 1.2f);
        }
        g.setColour(juce::Colour(0xff2a2418));
        g.setFont(juce::FontOptions(8.0f, juce::Font::bold));
        g.drawText("VU", face.withTrimmedTop(face.getHeight() * 0.55f), juce::Justification::centred);
        g.setFont(juce::FontOptions(7.0f));
        g.drawText("-20", face.reduced(4.0f, 3.0f), juce::Justification::topLeft);
        g.setColour(juce::Colour(0xffc0302a));
        g.drawText("+3", face.reduced(4.0f, 3.0f), juce::Justification::topRight);
        // The needle: the strip's output, about -18 dBFS = 0 VU.
        const float a = angle_for(vu_ + 18.0f);
        juce::Graphics::ScopedSaveState clip(g);
        g.reduceClipRegion(face.toNearestInt());
        g.setColour(juce::Colour(0xff111111));
        g.drawLine(pivot.x, pivot.y, pivot.x + std::sin(a) * radius * 1.02f, pivot.y - std::cos(a) * radius * 1.02f, 1.3f);
    }

    void apply(const std::string& id, const juce::String& what, double value)
    {
        auto& session = context_.session;
        // One undo step per turn: the label alternates between gestures.
        const std::string label = (what + (gesture_ % 2 ? " " : "")).toStdString();
        if (session.data().find_track(owner_))
            session.update_track_mix(owner_, label, [&](project::Track& t) { project::set_static_value(t, id, value); });
        else
            session.update_bus(owner_, label, [&](project::Bus& b) { project::set_static_value(b, id, value); });
    }

    // --- Outboard ---------------------------------------------------------------------------
    int pair_value(const char* field) const
    {
        for (const auto& c : controls_)
            if (c.field == field) return juce::roundToInt(c.knob->getValue());
        return 2;
    }

    void refresh_hardware()
    {
        auto* device = context_.engine.devices().getCurrentAudioDevice();
        auto names = [](const juce::StringArray& all, int pair) {
            const int a = 2 * (pair - 1);
            return a + 1 < all.size() ? all[a] + " / " + all[a + 1]
                                      : "channels " + juce::String(a + 1) + "-" + juce::String(a + 2) + " (not on this interface)";
        };
        juce::String text = device == nullptr ? juce::String("No audio interface open (Studio > Audio Setup)")
                                              : "OUT  " + names(device->getOutputChannelNames(), pair_value("out_pair")) +
                                                    "\nIN   " + names(device->getInputChannelNames(), pair_value("in_pair"));
        double latency = 0.0;
        if (!latency_id_.empty()) {
            const auto& d = context_.session.data();
            std::optional<double> v;
            if (const auto* t = d.find_track(owner_)) v = project::static_value(*t, latency_id_);
            else if (const auto* b = d.find_bus(owner_)) v = project::static_value(*b, latency_id_);
            latency = v.value_or(0.0);
        }
        text << "\nROUND TRIP  " << juce::String(juce::roundToInt(latency)) << " samples";
        if (ping_message_.isNotEmpty()) text << "\n" << ping_message_;
        info_.text = text;
        info_.repaint();
    }

    void start_ping()
    {
        hardware_io().request_ping(pair_value("out_pair"), pair_value("in_pair"));
        ping_message_ = "Listening for the click...";
        pinging_ = true;
        ping_ticks_ = 0;
        startTimer(50);
        refresh_hardware();
    }

    void timerCallback() override
    {
        if (flash_ > 0 && --flash_ == 0 && !pinging_) stopTimer();
        if (flash_ > 0) repaint();
        if (!pinging_) return;
        const int r = hardware_io().ping_result();
        if (hardware_io().pinging() || r == -1) {
            if (++ping_ticks_ > 60) {  // 3 s: the audio is not running
                pinging_ = false;
                ping_message_ = "No answer: is the audio interface running?";
                refresh_hardware();
            }
            return;
        }
        pinging_ = false;
        if (r >= 0 && !latency_id_.empty()) {
            apply(latency_id_, "Latency", static_cast<double>(r));
            ping_message_ = "Measured: the rest of the mix waits for it";
        } else {
            ping_message_ = "Nothing came back: check cables, levels and pairs";
        }
        refresh_hardware();
    }

    /// The outboard unit's readout.
    struct Readout final : public juce::Component {
        juce::String text;
        void paint(juce::Graphics& g) override
        {
            juce::StringArray lines;
            lines.addLines(text);
            const float h = static_cast<float>(getHeight()) / static_cast<float>(std::max(4, lines.size()));
            for (int i = 0; i < lines.size(); ++i)
                hw::paint::lcd_text(g, lines[i], {0.0f, static_cast<float>(i) * h, static_cast<float>(getWidth()), h}, 9.0f);
        }
    };

    AppContext& context_;
    std::string owner_;
    int slot_;
    std::string kind_;
    const Spec& spec_;
    juce::String name_;
    std::vector<Control> controls_;
    std::vector<juce::Rectangle<int>> columns_;
    juce::Rectangle<int> power_, vu_area_;
    hw::Keycap edit_{"Edit"}, ping_{"Ping"};
    Readout info_;
    std::string latency_id_;
    juce::String ping_message_;
    bool pinging_ = false;
    int ping_ticks_ = 0;
    bool bypass_ = false, suggestion_ = false, refreshing_ = false;
    int gesture_ = 0;
    int flash_ = 0;
    float vu_ = -40.0f;
};

/// The rack: rails down both sides, the units bolted in, and an empty bay
/// to add another.
class RackContent final : public juce::Component, private juce::Timer {
public:
    RackContent(AppContext& c, std::string owner) : context_(c), owner_(std::move(owner))
    {
        setComponentID("rack_content");
        rebuild();
        startTimerHz(30);
    }

    GearUnit* unit(int slot)
    {
        for (auto& u : units_)
            if (u->slot() == slot) return u.get();
        return nullptr;
    }

    /// Follows the document: new units when the inserts changed, else new values.
    void refresh()
    {
        if (signature() != signature_) rebuild();
        else
            for (auto& u : units_) u->refresh();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff060708));
        // The rails, with their square holes.
        for (auto rail : {getLocalBounds().removeFromLeft(kEar).toFloat(), getLocalBounds().removeFromRight(kEar).toFloat()}) {
            juce::ColourGradient grad(juce::Colour(0xff5b6067), rail.getX(), 0.0f, juce::Colour(0xff2b2e33), rail.getRight(), 0.0f, false);
            g.setGradientFill(grad);
            g.fillRect(rail.reduced(3.0f, 0.0f));
            g.setColour(juce::Colours::black.withAlpha(0.85f));
            for (float y = 6.0f; y < rail.getBottom(); y += static_cast<float>(kOneU) / 3.0f)
                g.fillRect(juce::Rectangle<float>(7.0f, 7.0f).withCentre({rail.getCentreX(), y}));
        }
        // The empty bay.
        if (!bay_.isEmpty()) {
            const auto r = bay_.toFloat();
            paint_plate(g, r, juce::Colour(0xff1b1d20));
            paint_ears(g, r, juce::Colour(0xff1b1d20));
            g.setColour(juce::Colours::black.withAlpha(0.6f));
            for (float x = r.getX() + 60.0f; x < r.getRight() - 160.0f; x += 9.0f)
                g.fillRoundedRectangle(juce::Rectangle<float>(4.0f, r.getHeight() * 0.5f).withCentre({x, r.getCentreY()}), 2.0f);
            hw::paint::silk(g, "+ ADD UNIT", r.withTrimmedRight(static_cast<float>(kEar) + 12.0f).withTrimmedLeft(r.getWidth() - 160.0f),
                            11.0f, juce::Justification::centredRight, hw::tok::text_bright);
        }
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (!bay_.contains(e.getPosition()) || e.mouseWasDraggedSinceMouseDown()) return;
        const auto* list = inserts_of(context_, owner_);
        show_insert_menu(context_, owner_, list ? static_cast<int>(list->size()) : 0, *this);
    }

    void resized() override
    {
        int y = 0;
        for (auto& u : units_) {
            u->setBounds(0, y, getWidth(), u->getHeight());
            y += u->getHeight();
        }
        bay_ = has_bay_ ? juce::Rectangle<int>(0, y + 4, getWidth(), kOneU / 2) : juce::Rectangle<int>();
    }

    int content_height() const
    {
        int h = 0;
        for (auto& u : units_) h += u->getHeight();
        return h + (has_bay_ ? kOneU / 2 + 8 : 4);
    }

private:
    std::string signature() const
    {
        std::string s;
        if (const auto* list = inserts_of(context_, owner_))
            for (const auto& slot : *list) s += slot.kind + "/" + slot.plugin.name + "|";
        return s;
    }

    void rebuild()
    {
        signature_ = signature();
        units_.clear();
        const auto* list = inserts_of(context_, owner_);
        const int n = list ? static_cast<int>(list->size()) : 0;
        for (int i = 0; i < n; ++i) {
            units_.push_back(std::make_unique<GearUnit>(context_, owner_, i, (*list)[static_cast<std::size_t>(i)]));
            addAndMakeVisible(*units_.back());
        }
        has_bay_ = n < 5;
        setSize(kRackWidth, content_height());
        resized();
        repaint();
    }

    void timerCallback() override
    {
        if (!isShowing()) return;
        bool any = false;
        for (auto& u : units_) any = any || u->has_vu();
        if (!any) return;
        const auto m = context_.engine.take_deck_meter(owner_, 5);
        const float peak = std::max(m.left, m.right);
        const float db = peak > 0.0f ? 20.0f * std::log10(peak) : -60.0f;
        for (auto& u : units_)
            if (u->has_vu()) u->push_level(db);
    }

    AppContext& context_;
    std::string owner_;
    std::vector<std::unique_ptr<GearUnit>> units_;
    std::string signature_;
    bool has_bay_ = false;
    juce::Rectangle<int> bay_;
};

class RackWindow final : public juce::DocumentWindow {
public:
    RackWindow(AppContext& c, std::string owner)
        : juce::DocumentWindow("Rack", juce::Colour(0xff060708), juce::DocumentWindow::closeButton), context_(c),
          owner_(std::move(owner)), content_(c, owner_)
    {
        setUsingNativeTitleBar(true);
        setResizable(true, false);
        viewport_.setViewedComponent(&content_, false);
        viewport_.setScrollBarsShown(true, false);
        viewport_.setScrollBarThickness(10);
        setContentNonOwned(&viewport_, false);
        retitle();
        fit();
        setVisible(true);
    }
    ~RackWindow() override { clearContentComponent(); }

    void closeButtonPressed() override { setVisible(false); }
    const std::string& owner() const noexcept { return owner_; }
    RackContent& content() noexcept { return content_; }

    void retitle()
    {
        const auto& d = context_.session.data();
        juce::String name = owner_;
        if (const auto* t = d.find_track(owner_)) name = t->name;
        else if (const auto* b = d.find_bus(owner_)) name = b->name;
        setName("Rack - " + name);
    }

    /// As tall as the units, up to most of the screen.
    void fit()
    {
        const int want = content_.content_height() + getTitleBarHeight() + 2;
        auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea;
        const int h = std::min(want, area.getHeight() * 85 / 100);
        if (!placed_) {
            centreWithSize(kRackWidth + 12, h);
            placed_ = true;
        } else {
            setSize(kRackWidth + 12, h);
        }
        setResizeLimits(kRackWidth + 12, kOneU, kRackWidth + 12, 4000);
    }

    void show_slot(int slot)
    {
        setVisible(true);
        toFront(true);
        if (auto* u = content_.unit(slot)) {
            viewport_.setViewPosition(0, u->getY());
            u->flash();
        }
    }

    void refresh()
    {
        const int before = content_.content_height();
        retitle();
        content_.refresh();
        if (content_.content_height() != before) fit();
    }

private:
    AppContext& context_;
    std::string owner_;
    RackContent content_;
    juce::Viewport viewport_;
    bool placed_ = false;
};

std::map<std::string, std::unique_ptr<RackWindow>>& racks()
{
    static std::map<std::string, std::unique_ptr<RackWindow>> r;
    return r;
}

}  // namespace

juce::String gear_name(const project::InsertSlot& slot)
{
    if (slot.kind == "plugin") return slot.plugin.name.empty() ? juce::String(slot.plugin_ref) : juce::String(slot.plugin.name);
    const auto& s = spec_for(slot.kind);
    return s.name[0] != '\0' ? juce::String(s.name) : juce::String(slot.kind).toUpperCase();
}

juce::Colour gear_plate(const std::string& kind) { return juce::Colour(spec_for(kind).plate); }
juce::Colour gear_ink(const std::string& kind) { return juce::Colour(spec_for(kind).ink); }

void paint_mini_unit(juce::Graphics& g, juce::Rectangle<float> r, const project::InsertSlot* slot, juce::Rectangle<float>* led)
{
    const float ear = std::min(7.0f, r.getWidth() * 0.08f);
    if (slot == nullptr) {
        // An empty bay: the rails and a dark gap.
        g.setColour(juce::Colour(0xff060708));
        g.fillRect(r);
        g.setColour(juce::Colour(0xff3c4046));
        g.fillRect(r.withWidth(ear * 0.6f));
        g.fillRect(r.withLeft(r.getRight() - ear * 0.6f));
        if (led) *led = {};
        return;
    }
    const auto plate = gear_plate(slot->kind);
    const auto ink = gear_ink(slot->kind);
    const bool on = !slot->bypass && !slot->suggestion;
    juce::ColourGradient grad(plate.brighter(0.12f), r.getX(), r.getY(), plate.darker(0.25f), r.getX(), r.getBottom(), false);
    g.setGradientFill(grad);
    g.fillRect(r);
    g.setColour(plate.darker(0.35f));
    g.fillRect(r.withWidth(ear));
    g.fillRect(r.withLeft(r.getRight() - ear));
    // Screw dots in the ears.
    g.setColour(juce::Colour(0xffb5bac0).withAlpha(0.8f));
    const float dot = std::max(1.5f, std::min(3.0f, r.getHeight() * 0.22f));
    for (float x : {r.getX() + ear * 0.5f, r.getRight() - ear * 0.5f})
        g.fillEllipse(juce::Rectangle<float>(dot, dot).withCentre({x, r.getCentreY()}));
    g.setColour(juce::Colours::black.withAlpha(0.55f));
    g.drawHorizontalLine(juce::roundToInt(r.getBottom() - 1.0f), r.getX(), r.getRight());
    // The power lamp, then the name.
    const float lamp_d = std::max(3.0f, std::min(6.0f, r.getHeight() * 0.42f));
    const juce::Point<float> lamp{r.getX() + ear + lamp_d * 0.5f + 3.0f, r.getCentreY()};
    hw::paint::led(g, lamp, on, slot->suggestion ? hw::tok::meter_amber : juce::Colour(0xffff3b30), lamp_d);
    if (led) *led = juce::Rectangle<float>(ear + lamp_d + 6.0f, r.getHeight()).withPosition(r.getX(), r.getY());
    auto text = r.withTrimmedLeft(ear + lamp_d + 6.0f).withTrimmedRight(ear + 1.0f);
    juce::String name = gear_name(*slot);
    if (slot->kind != "plugin" && text.getWidth() < 46.0f) name = name.fromFirstOccurrenceOf("VC ", false, false);
    g.setColour(ink.withAlpha(on ? 1.0f : 0.5f));
    g.setFont(juce::FontOptions(std::max(6.5f, std::min(10.5f, r.getHeight() * 0.68f)), juce::Font::bold));
    g.drawFittedText(name.toUpperCase(), text.toNearestInt(), juce::Justification::centredLeft, 1, 0.6f);
    if (slot->bypass && text.getWidth() > 70.0f) {
        g.setColour(hw::tok::meter_amber);
        g.setFont(juce::FontOptions(std::max(6.0f, r.getHeight() * 0.5f)));
        g.drawText("BYP", text, juce::Justification::centredRight);
    }
}

void show_rack(AppContext& context, const std::string& owner_id, int slot)
{
    auto& w = racks()[owner_id];
    if (!w) w = std::make_unique<RackWindow>(context, owner_id);
    else w->refresh();
    w->show_slot(slot);
}

void refresh_racks(AppContext& context)
{
    auto& all = racks();
    for (auto it = all.begin(); it != all.end();) {
        const auto& d = context.session.data();
        if (!d.find_track(it->first) && !d.find_bus(it->first)) {
            it = all.erase(it);
            continue;
        }
        if (it->second->isVisible()) it->second->refresh();
        ++it;
    }
}

void close_racks() { racks().clear(); }

juce::DocumentWindow* rack_window(const std::string& owner_id)
{
    auto& all = racks();
    const auto it = all.find(owner_id);
    return it == all.end() ? nullptr : it->second.get();
}

}  // namespace vcc::desktop::console
