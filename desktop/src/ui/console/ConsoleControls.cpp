#include "ui/console/ConsoleControls.h"

#include <cmath>
#include <map>
#include <tuple>

namespace vcc::desktop::console {

namespace {

constexpr float kKnobStart = -2.36f;  // radians from 12 o'clock
constexpr float kKnobEnd = 2.36f;

juce::Point<float> polar(juce::Point<float> c, float angle, float radius)
{
    return {c.x + std::sin(angle) * radius, c.y - std::cos(angle) * radius};
}

bool reset_click(const juce::MouseEvent& e) { return e.mods.isAltDown() || e.mods.isCommandDown(); }

}  // namespace

// --- Knob ------------------------------------------------------------------------------------

Knob::Knob(Spec spec) : spec_(std::move(spec)), value_(spec_.def)
{
    setRepaintsOnMouseActivity(false);
    setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
}

double Knob::to_norm(double v) const
{
    v = juce::jlimit(spec_.min, spec_.max, v);
    switch (spec_.law) {
        case Law::Log: return std::log(v / spec_.min) / std::log(spec_.max / spec_.min);
        case Law::Fader: return fader_proportion(v);
        case Law::Linear: break;
    }
    return (v - spec_.min) / (spec_.max - spec_.min);
}

double Knob::from_norm(double n) const
{
    n = juce::jlimit(0.0, 1.0, n);
    double v = 0.0;
    switch (spec_.law) {
        case Law::Log: v = spec_.min * std::pow(spec_.max / spec_.min, n); break;
        case Law::Fader: v = fader_db(n); break;
        case Law::Linear: v = spec_.min + n * (spec_.max - spec_.min); break;
    }
    // A gentle detent at the default of bipolar controls.
    if (spec_.bipolar && std::abs(n - to_norm(spec_.def)) < 0.012) v = spec_.def;
    return juce::jlimit(spec_.min, spec_.max, v);
}

void Knob::set_value(double v)
{
    if (dragging_) return;  // the user's hand wins over a document echo
    if (std::abs(v - value_) < 1e-9) return;
    value_ = v;
    repaint();
}

void Knob::set_dim(bool dim)
{
    if (dim == dim_) return;
    dim_ = dim;
    repaint();
}

void Knob::set_automation(juce::Colour lamp)
{
    if (lamp == automation_) return;
    automation_ = lamp;
    repaint();
}

void Knob::open_gesture()
{
    if (gesture_) return;
    gesture_ = true;
    if (on_start) on_start();
}

void Knob::close_gesture()
{
    if (!gesture_) return;
    gesture_ = false;
    if (on_end) on_end();
}

void Knob::change_to(double v)
{
    if (std::abs(v - value_) < 1e-9) return;
    value_ = v;
    repaint();
    if (on_change) on_change(v);
}

namespace {

/// The knob without its pointer: scale dots, shadow, knurled skirt and cap,
/// rendered once per look and size and shared by every strip.
juce::Image knob_body(juce::Colour cap_colour, float d, bool bipolar, float scale)
{
    using Key = std::tuple<juce::uint32, int, bool, int>;
    static std::map<Key, juce::Image> cache;
    const Key key{cap_colour.getARGB(), juce::roundToInt(d * 10.0f), bipolar, juce::roundToInt(scale * 100.0f)};
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    if (cache.size() > 400) cache.clear();

    const float size = d + 8.0f;
    const int px = juce::roundToInt(size * scale) + 1;
    juce::Image img(juce::Image::ARGB, px, px, true);
    juce::Graphics g(img);
    g.addTransform(juce::AffineTransform::scale(scale));
    const float R = d * 0.5f;
    const juce::Point<float> c(size * 0.5f, size * 0.5f);

    for (int i = 0; i <= 10; ++i) {
        const float a = kKnobStart + static_cast<float>(i) / 10.0f * (kKnobEnd - kKnobStart);
        const bool major = i == 0 || i == 10 || (bipolar && i == 5);
        const auto p = polar(c, a, R + 2.4f);
        const float r = major ? 0.95f : 0.6f;
        g.setColour(paint::print.withAlpha(major ? 0.85f : 0.5f));
        g.fillEllipse(p.x - r, p.y - r, r * 2.0f, r * 2.0f);
    }
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.fillEllipse(c.x - R + 0.4f, c.y - R + 1.6f, R * 2.0f, R * 2.0f);
    juce::ColourGradient skirt(juce::Colour(0xff55585e), c.x - R * 0.45f, c.y - R * 0.65f, juce::Colour(0xff0f1012),
                               c.x + R * 0.7f, c.y + R * 1.1f, true);
    g.setGradientFill(skirt);
    g.fillEllipse(c.x - R, c.y - R, R * 2.0f, R * 2.0f);
    g.setColour(juce::Colours::black.withAlpha(0.35f));
    for (int i = 0; i < 28; ++i) {
        const float a = static_cast<float>(i) * juce::MathConstants<float>::twoPi / 28.0f;
        g.drawLine(juce::Line<float>(polar(c, a, R * 0.80f), polar(c, a, R - 0.3f)), 0.7f);
    }
    g.setColour(juce::Colours::black.withAlpha(0.85f));
    g.drawEllipse(c.x - R, c.y - R, R * 2.0f, R * 2.0f, 0.8f);
    const float r = R * 0.66f;
    juce::ColourGradient cap(cap_colour.brighter(0.6f), c.x - r * 0.5f, c.y - r * 0.6f, cap_colour.darker(0.55f),
                             c.x + r * 0.6f, c.y + r * 1.0f, true);
    cap.addColour(0.45, cap_colour);
    g.setGradientFill(cap);
    g.fillEllipse(c.x - r, c.y - r, r * 2.0f, r * 2.0f);
    g.setColour(juce::Colours::black.withAlpha(0.55f));
    g.drawEllipse(c.x - r, c.y - r, r * 2.0f, r * 2.0f, 0.7f);
    g.setColour(juce::Colours::white.withAlpha(0.22f));
    g.fillEllipse(c.x - r * 0.62f, c.y - r * 0.72f, r * 0.75f, r * 0.45f);
    cache.emplace(key, img);
    return img;
}

}  // namespace

void Knob::paint(juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    float d = juce::jmin(b.getWidth(), b.getHeight()) - 6.0f;
    if (spec_.small) d *= 0.84f;
    d = std::round(d);
    if (d < 6.0f) return;
    const float R = d * 0.5f;
    const auto c = b.getCentre();
    const float angle = kKnobStart + static_cast<float>(to_norm(value_)) * (kKnobEnd - kKnobStart);
    const float scale = pixel_scale(g);
    const float size = d + 8.0f;

    if (dim_) g.setOpacity(0.38f);
    else g.setOpacity(1.0f);
    const auto body = knob_body(spec_.cap, d, spec_.bipolar, scale);
    draw_scaled(g, body, {std::round(c.x - size * 0.5f), std::round(c.y - size * 0.5f)}, scale);
    const auto cc = juce::Point<float>(std::round(c.x - size * 0.5f) + size * 0.5f, std::round(c.y - size * 0.5f) + size * 0.5f);
    g.setColour(juce::Colours::white.withAlpha(dim_ ? 0.4f : 0.95f));
    g.drawLine(juce::Line<float>(polar(cc, angle, R * 0.18f), polar(cc, angle, R - 0.6f)), juce::jmax(1.3f, R * 0.13f));
    if (!automation_.isTransparent()) {
        // Automation lamp in the scale's gap at six o'clock, with a little glow.
        g.setOpacity(1.0f);
        const juce::Point<float> p(cc.x, cc.y + R + 2.2f);
        g.setColour(automation_.withAlpha(0.28f));
        g.fillEllipse(p.x - 3.4f, p.y - 2.6f, 6.8f, 5.2f);
        g.setColour(juce::Colours::black.withAlpha(0.8f));
        g.fillEllipse(p.x - 2.0f, p.y - 1.6f, 4.0f, 3.2f);
        g.setColour(automation_);
        g.fillEllipse(p.x - 1.5f, p.y - 1.15f, 3.0f, 2.3f);
        g.setColour(juce::Colours::white.withAlpha(0.55f));
        g.fillEllipse(p.x - 0.7f, p.y - 0.9f, 1.2f, 0.8f);
    }
}

void Knob::mouseDown(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) {
        if (on_menu) on_menu();
        return;
    }
    begin_gesture();
    if (reset_click(e)) {
        open_gesture();
        change_to(spec_.def);
        close_gesture();
        return;
    }
    dragging_ = true;
    drag_norm_ = to_norm(value_);
    last_ = e.position;
    open_gesture();
    show_tip(*this, spec_.text ? spec_.text(value_) : juce::String(value_, 2));
}

void Knob::mouseDrag(const juce::MouseEvent& e)
{
    if (!dragging_) return;
    const float sens = e.mods.isShiftDown() ? 1000.0f : 200.0f;
    const auto delta = e.position - last_;
    last_ = e.position;
    drag_norm_ = juce::jlimit(0.0, 1.0, drag_norm_ + static_cast<double>((delta.x - delta.y) / sens));
    change_to(from_norm(drag_norm_));
    show_tip(*this, spec_.text ? spec_.text(value_) : juce::String(value_, 2));
}

void Knob::mouseUp(const juce::MouseEvent&)
{
    dragging_ = false;
    hide_tip(*this);
    close_gesture();
}

void Knob::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    dragging_ = false;
    // Inside a click (down ... up) the gesture is already open and the
    // mouse-up closes it; called on its own, it is a gesture by itself.
    const bool open = gesture_;
    if (!open) {
        begin_gesture();
        open_gesture();
    }
    change_to(spec_.def);
    if (!open) close_gesture();
    hide_tip(*this);
}

// --- LedButton -------------------------------------------------------------------------------

LedButton::LedButton(const juce::String& label, juce::Colour lamp, const juce::String& tooltip)
    : juce::Button(label), lamp_(lamp)
{
    setTooltip(tooltip.isEmpty() ? label : tooltip);
    setClickingTogglesState(false);
}

void LedButton::set_lit(bool on) { setToggleState(on, juce::dontSendNotification); }

void LedButton::set_automation(juce::Colour lamp)
{
    if (lamp == automation_) return;
    automation_ = lamp;
    repaint();
}

void LedButton::mouseDown(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() && on_menu) {
        menu_press_ = true;
        on_menu();
        return;
    }
    menu_press_ = false;
    juce::Button::mouseDown(e);
}

void LedButton::mouseDrag(const juce::MouseEvent& e)
{
    if (!menu_press_) juce::Button::mouseDrag(e);
}

void LedButton::mouseUp(const juce::MouseEvent& e)
{
    if (menu_press_) {
        menu_press_ = false;
        return;
    }
    juce::Button::mouseUp(e);
}

void LedButton::set_lamp(juce::Colour lamp)
{
    if (lamp == lamp_) return;
    lamp_ = lamp;
    repaint();
}

void LedButton::paintButton(juce::Graphics& g, bool over, bool down)
{
    const bool on = getToggleState();
    auto r = getLocalBounds().toFloat().reduced(1.5f);
    // Bezel.
    g.setColour(juce::Colours::black.withAlpha(0.75f));
    g.fillRoundedRectangle(r.expanded(1.0f), 3.0f);
    if (on) {
        g.setColour(lamp_.withAlpha(0.30f));
        g.fillRoundedRectangle(r.expanded(1.5f), 3.5f);
    }
    auto lens = r.reduced(0.8f);
    if (on) {
        juce::ColourGradient grad(lamp_.brighter(0.45f), lens.getX(), lens.getY(), lamp_.darker(0.25f), lens.getX(),
                                  lens.getBottom(), false);
        g.setGradientFill(grad);
    } else {
        const auto top = juce::Colour(0xff3a3c41).interpolatedWith(lamp_, 0.10f);
        const auto bottom = juce::Colour(0xff1b1c1f).interpolatedWith(lamp_, 0.07f);
        juce::ColourGradient grad(top, lens.getX(), lens.getY(), bottom, lens.getX(), lens.getBottom(), false);
        g.setGradientFill(grad);
    }
    g.fillRoundedRectangle(lens, 2.0f);
    if (on) {
        juce::ColourGradient hot(juce::Colours::white.withAlpha(0.35f), lens.getCentreX(), lens.getCentreY(),
                                 juce::Colours::white.withAlpha(0.0f), lens.getRight(), lens.getBottom(), true);
        g.setGradientFill(hot);
        g.fillRoundedRectangle(lens, 2.0f);
    }
    g.setColour(juce::Colours::white.withAlpha(on ? 0.5f : 0.13f));
    g.fillRect(lens.getX() + 2.0f, lens.getY() + 0.8f, lens.getWidth() - 4.0f, 0.8f);
    if (down || over) {
        g.setColour(down ? juce::Colours::black.withAlpha(0.2f) : juce::Colours::white.withAlpha(0.05f));
        g.fillRoundedRectangle(lens, 2.0f);
    }
    const float size = juce::jmin(9.0f, lens.getHeight() * 0.62f);
    juce::Colour ink = paint::print_dim;
    if (on) ink = lamp_.getPerceivedBrightness() > 0.55f ? juce::Colour(0xff17181a) : juce::Colours::white;
    print_text(g, getButtonText(), lens.reduced(1.0f, 0.0f), size, juce::Justification::centred, ink);
    if (!automation_.isTransparent()) {
        // A pin-head lamp in the lens' top-right corner.
        const juce::Point<float> p(lens.getRight() - 3.2f, lens.getY() + 3.0f);
        g.setColour(juce::Colours::black.withAlpha(0.85f));
        g.fillEllipse(p.x - 2.1f, p.y - 2.1f, 4.2f, 4.2f);
        g.setColour(automation_);
        g.fillEllipse(p.x - 1.5f, p.y - 1.5f, 3.0f, 3.0f);
        g.setColour(juce::Colours::white.withAlpha(0.5f));
        g.fillEllipse(p.x - 0.8f, p.y - 1.1f, 1.1f, 0.8f);
    }
}

// --- ModeSelector --------------------------------------------------------------------------

ModeSelector::ModeSelector(const juce::String& tooltip) : juce::Button("READ")
{
    setTooltip(tooltip);
    setClickingTogglesState(false);
}

void ModeSelector::set_mode(const std::string& mode)
{
    if (mode == mode_) return;
    mode_ = mode;
    setButtonText(automation_label(mode_));
    repaint();
}

void ModeSelector::mouseDown(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() && on_menu) {
        menu_press_ = true;
        on_menu();
        return;
    }
    menu_press_ = false;
    juce::Button::mouseDown(e);
}

void ModeSelector::mouseDrag(const juce::MouseEvent& e)
{
    if (!menu_press_) juce::Button::mouseDrag(e);
}

void ModeSelector::mouseUp(const juce::MouseEvent& e)
{
    if (menu_press_) {
        menu_press_ = false;
        return;
    }
    juce::Button::mouseUp(e);
}

void ModeSelector::paintButton(juce::Graphics& g, bool over, bool down)
{
    const auto lamp = automation_colour(mode_);
    const bool off = lamp.isTransparent();
    const bool writing = mode_ == "touch" || mode_ == "latch" || mode_ == "write";
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    draw_well(g, r, 2.5f);
    auto glass = r.reduced(1.5f);
    const auto base = writing ? juce::Colour(0xff15100a).interpolatedWith(lamp, 0.30f) : paint::lcd_back;
    juce::ColourGradient grad(base.brighter(0.25f), 0.0f, glass.getY(), base, 0.0f, glass.getBottom(), false);
    g.setGradientFill(grad);
    g.fillRoundedRectangle(glass, 1.5f);
    if (writing) {
        g.setColour(lamp.withAlpha(0.55f));
        g.drawRoundedRectangle(glass, 1.5f, 0.8f);
    }
    // Status lamp.
    auto inner = glass.reduced(3.0f, 0.0f);
    const auto dot = inner.removeFromLeft(7.0f).withSizeKeepingCentre(5.0f, 5.0f);
    if (!off) {
        g.setColour(lamp.withAlpha(0.3f));
        g.fillEllipse(dot.expanded(1.6f));
    }
    g.setColour(off ? juce::Colour(0xff2a2c30) : lamp);
    g.fillEllipse(dot);
    g.setColour(juce::Colours::white.withAlpha(off ? 0.08f : 0.5f));
    g.fillEllipse(dot.getX() + 1.0f, dot.getY() + 0.8f, 2.0f, 1.4f);
    // Menu arrow.
    {
        auto a = inner.removeFromRight(6.0f);
        juce::Path p;
        const float cx = a.getCentreX(), cy = a.getCentreY();
        p.addTriangle(cx - 2.2f, cy - 1.0f, cx + 2.2f, cy - 1.0f, cx, cy + 1.8f);
        g.setColour((off ? paint::print_dim : lamp).withAlpha(0.6f));
        g.fillPath(p);
    }
    const auto ink = off ? paint::print_dim : (writing ? lamp.brighter(0.5f) : lamp.brighter(0.15f));
    g.setColour(ink);
    g.setFont(print_font(juce::jmin(9.0f, glass.getHeight() * 0.62f)));
    g.drawFittedText(getButtonText(), inner.toNearestInt(), juce::Justification::centred, 1, 0.7f);
    g.setColour(juce::Colours::white.withAlpha(0.05f));
    g.fillRect(glass.withHeight(glass.getHeight() * 0.45f));
    if (down || over) {
        g.setColour(down ? juce::Colours::black.withAlpha(0.2f) : juce::Colours::white.withAlpha(0.04f));
        g.fillRoundedRectangle(glass, 1.5f);
    }
}

// --- Fader -----------------------------------------------------------------------------------

Fader::Fader() { setMouseCursor(juce::MouseCursor::UpDownResizeCursor); }

void Fader::set_automation(juce::Colour lamp)
{
    if (lamp == automation_) return;
    automation_ = lamp;
    repaint();
}

void Fader::open_gesture()
{
    if (gesture_) return;
    gesture_ = true;
    if (on_start) on_start();
}

void Fader::close_gesture()
{
    if (!gesture_) return;
    gesture_ = false;
    if (on_end) on_end();
}

void Fader::set_cap_colour(juce::Colour c)
{
    if (c == cap_) return;
    cap_ = c;
    repaint();
}

float Fader::cap_height() const { return juce::jlimit(26.0f, 46.0f, static_cast<float>(getWidth()) * 1.45f); }

float Fader::y_for(double db) const
{
    const float ch = cap_height();
    const float top = ch * 0.5f + 2.0f, bottom = static_cast<float>(getHeight()) - ch * 0.5f - 2.0f;
    return bottom - static_cast<float>(fader_proportion(db)) * (bottom - top);
}

void Fader::set_value(double db)
{
    if (dragging_ || std::abs(db - value_) < 1e-9) return;
    value_ = db;
    repaint();
}

void Fader::change_to(double db)
{
    db = std::round(db * 10.0) / 10.0;
    if (std::abs(db) < 0.05) db = 0.0;
    if (db <= kMinDb + 0.5) db = kMinDb;
    if (std::abs(db - value_) < 1e-9) return;
    value_ = db;
    repaint();
    if (on_change) on_change(db);
}

void Fader::paint(juce::Graphics& g)
{
    const float w = static_cast<float>(getWidth());
    const float cx = w * 0.5f;
    const float ch = cap_height();
    const float top = y_for(kMaxDb), bottom = y_for(kMinDb);

    // The slot.
    draw_well(g, {cx - 3.0f, top - 6.0f, 6.0f, bottom - top + 12.0f}, 2.5f);
    g.setColour(juce::Colour(0xff26282c));
    g.fillRect(cx - 0.5f, top - 3.0f, 1.0f, bottom - top + 6.0f);

    // The cap.
    const float y = y_for(value_);
    const float cw = juce::jmin(w - 2.0f, 30.0f);
    const juce::Rectangle<float> cap(cx - cw * 0.5f, y - ch * 0.5f, cw, ch);
    for (int i = 1; i <= 4; ++i) {
        g.setColour(juce::Colours::black.withAlpha(0.13f));
        g.fillRoundedRectangle(cap.translated(0.0f, static_cast<float>(i) * 1.3f).expanded(static_cast<float>(i) * 0.6f),
                               3.0f + static_cast<float>(i));
    }
    juce::ColourGradient body(cap_.brighter(0.7f), 0.0f, cap.getY(), cap_.darker(0.6f), 0.0f, cap.getBottom(), false);
    body.addColour(0.12, cap_.brighter(0.35f));
    body.addColour(0.44, cap_.darker(0.15f));
    body.addColour(0.50, cap_.darker(0.7f));
    body.addColour(0.56, cap_.brighter(0.1f));
    body.addColour(0.88, cap_.darker(0.3f));
    g.setGradientFill(body);
    g.fillRoundedRectangle(cap, 3.0f);
    // Finger ridges, upper and lower halves.
    for (float yy = cap.getY() + 4.0f; yy < y - 5.0f; yy += 3.0f) {
        g.setColour(juce::Colours::black.withAlpha(0.32f));
        g.fillRect(cap.getX() + 2.5f, yy, cw - 5.0f, 1.0f);
        g.setColour(juce::Colours::white.withAlpha(0.10f));
        g.fillRect(cap.getX() + 2.5f, yy + 1.0f, cw - 5.0f, 1.0f);
    }
    for (float yy = y + 5.0f; yy < cap.getBottom() - 4.0f; yy += 3.0f) {
        g.setColour(juce::Colours::black.withAlpha(0.32f));
        g.fillRect(cap.getX() + 2.5f, yy, cw - 5.0f, 1.0f);
        g.setColour(juce::Colours::white.withAlpha(0.08f));
        g.fillRect(cap.getX() + 2.5f, yy + 1.0f, cw - 5.0f, 1.0f);
    }
    // Side bevels.
    g.setColour(juce::Colours::white.withAlpha(0.12f));
    g.fillRect(cap.getX() + 1.0f, cap.getY() + 2.0f, 1.0f, ch - 4.0f);
    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.fillRect(cap.getRight() - 2.0f, cap.getY() + 2.0f, 1.0f, ch - 4.0f);
    g.setColour(juce::Colours::black.withAlpha(0.9f));
    g.drawRoundedRectangle(cap, 3.0f, 1.0f);
    // Indicator line: lit in the automation mode's colour while a lane drives the fader.
    if (!dragging_ && !automation_.isTransparent()) {
        g.setColour(automation_.withAlpha(0.35f));
        g.fillRect(cap.getX() + 1.0f, y - 2.0f, cw - 2.0f, 4.0f);
        g.setColour(automation_.brighter(0.25f));
    } else {
        g.setColour(dragging_ ? paint::selected : juce::Colours::white);
    }
    g.fillRect(cap.getX() + 1.5f, y - 1.0f, cw - 3.0f, 2.0f);
}

void Fader::mouseDown(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) {
        if (on_menu) on_menu();
        return;
    }
    begin_gesture();
    if (reset_click(e)) {
        open_gesture();
        change_to(0.0);
        close_gesture();
        return;
    }
    open_gesture();
    dragging_ = true;
    drag_prop_ = fader_proportion(value_);
    last_y_ = e.position.y;
    repaint();
    show_tip(*this, db_text(value_));
}

void Fader::mouseDrag(const juce::MouseEvent& e)
{
    if (!dragging_) return;
    const float travel = juce::jmax(20.0f, y_for(kMinDb) - y_for(kMaxDb));
    const float dy = e.position.y - last_y_;
    last_y_ = e.position.y;
    drag_prop_ = juce::jlimit(0.0, 1.0, drag_prop_ - static_cast<double>(dy / travel) * (e.mods.isShiftDown() ? 0.15 : 1.0));
    change_to(fader_db(drag_prop_));
    show_tip(*this, db_text(value_));
}

void Fader::mouseUp(const juce::MouseEvent&)
{
    dragging_ = false;
    repaint();
    hide_tip(*this);
    close_gesture();
}

void Fader::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    dragging_ = false;
    const bool open = gesture_;  // see Knob::mouseDoubleClick
    if (!open) {
        begin_gesture();
        open_gesture();
    }
    change_to(0.0);
    if (!open) close_gesture();
    hide_tip(*this);
}

// --- LedMeter --------------------------------------------------------------------------------

LedMeter::LedMeter(int channels, bool large) : channels_(juce::jlimit(1, 2, channels)), large_(large)
{
    setOpaque(true);
    setTooltip("Peak meter - click to reset the clip light and peak hold");
}

int LedMeter::segments() const
{
    const int clip = large_ ? 9 : 6;
    const int pitch = large_ ? 4 : 3;
    return juce::jmax(4, (getHeight() - clip - 2) / pitch);
}

float LedMeter::y_for(double dbfs) const
{
    const float pitch = large_ ? 4.0f : 3.0f;
    const float ladder = static_cast<float>(segments()) * pitch;
    return static_cast<float>(getHeight()) - 1.0f - static_cast<float>(meter_proportion(dbfs)) * ladder;
}

void LedMeter::resized()
{
    for (auto& c : ch_) c.lit = c.hold_seg = -2;  // force a recount
}

void LedMeter::reset()
{
    for (auto& c : ch_) c = Channel{};
    clip_ticks_ = 0;
    repaint();
}

void LedMeter::push(float left, float right, bool clipped)
{
    const int n = segments();
    bool changed = false;
    const std::array<float, 2> in{left, channels_ == 2 ? right : juce::jmax(left, right)};
    for (int i = 0; i < channels_; ++i) {
        auto& c = ch_[static_cast<std::size_t>(i)];
        const float peak = in[static_cast<std::size_t>(i)];
        const double db = peak > 1e-7f ? 20.0 * std::log10(static_cast<double>(peak)) : -200.0;
        c.level = db > c.level ? db : juce::jmax(-200.0, c.level - 26.0 / 30.0);
        if (db >= c.hold) {
            c.hold = db;
            c.hold_ticks = 45;
        } else if (c.hold_ticks > 0) {
            --c.hold_ticks;
        } else {
            c.hold = juce::jmax(-200.0, c.hold - 20.0 / 30.0);
        }
        if (peak >= 1.0f) clipped = true;
        const int lit = c.level > -60.0 ? juce::roundToInt(meter_proportion(c.level) * n) : 0;
        const int hold = c.hold > -60.0 ? juce::jlimit(0, n - 1, static_cast<int>(meter_proportion(c.hold) * n)) : -1;
        if (lit != c.lit || hold != c.hold_seg) changed = true;
        c.lit = lit;
        c.hold_seg = hold;
    }
    const bool was = clip_ticks_ > 0;
    if (clipped) clip_ticks_ = 90;
    else if (clip_ticks_ > 0) --clip_ticks_;
    if (changed || was != (clip_ticks_ > 0)) repaint();
}

void LedMeter::mouseDown(const juce::MouseEvent&) { reset(); }

void LedMeter::paint(juce::Graphics& g)
{
    g.fillAll(paint::well);
    const auto b = getLocalBounds().toFloat();
    const float clip_h = large_ ? 7.0f : 4.0f;
    const float pitch = large_ ? 4.0f : 3.0f;
    const float led_h = pitch - 1.0f;
    const int n = segments();
    const float gap = 1.0f;
    const float col_w = (b.getWidth() - 2.0f - gap * static_cast<float>(channels_ - 1)) / static_cast<float>(channels_);
    const double yellow = meter_proportion(-12.0), red = meter_proportion(-3.0);

    // Clip light across the top.
    {
        const auto lamp = juce::Rectangle<float>(1.0f, 1.0f, b.getWidth() - 2.0f, clip_h);
        g.setColour(clip_ticks_ > 0 ? paint::led_red : paint::led_red.withAlpha(0.16f));
        g.fillRect(lamp);
        if (clip_ticks_ > 0) {
            g.setColour(juce::Colours::white.withAlpha(0.35f));
            g.fillRect(lamp.withHeight(1.0f));
        }
    }
    const float base = b.getBottom() - 1.0f;
    for (int c = 0; c < channels_; ++c) {
        const auto& ch = ch_[static_cast<std::size_t>(c)];
        const float x = 1.0f + static_cast<float>(c) * (col_w + gap);
        for (int i = 0; i < n; ++i) {
            const double p = (static_cast<double>(i) + 0.5) / static_cast<double>(n);
            const juce::Colour colour = p >= red ? paint::led_red : (p >= yellow ? paint::led_yellow : paint::led_green);
            const bool lit = i < ch.lit || i == ch.hold_seg;
            g.setColour(lit ? colour : colour.withAlpha(0.11f));
            g.fillRect(x, base - static_cast<float>(i + 1) * pitch + 1.0f, col_w, led_h);
        }
    }
}

// --- GrMeter ---------------------------------------------------------------------------------

namespace {
constexpr std::array<float, 8> kGrSteps{1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 9.0f, 12.0f, 18.0f};
}

GrMeter::GrMeter()
{
    setOpaque(true);
    setTooltip("Gain reduction: 1 2 3 4 6 9 12 18 dB.  G: the gate is closed");
}

void GrMeter::set(float gr_db, bool gate_closed)
{
    int lit = 0;
    for (float s : kGrSteps)
        if (-gr_db >= s - 0.01f) ++lit;
    if (lit == lit_ && gate_closed == gate_) return;
    lit_ = lit;
    gate_ = gate_closed;
    repaint();
}

void GrMeter::paint(juce::Graphics& g)
{
    g.fillAll(paint::well);
    const auto b = getLocalBounds().toFloat().reduced(1.5f);
    const float gate_w = b.getHeight() + 2.0f;
    const float w = (b.getWidth() - gate_w - 2.0f) / static_cast<float>(kGrSteps.size());
    for (std::size_t i = 0; i < kGrSteps.size(); ++i) {
        const auto c = i < 4 ? paint::led_yellow : (i < 6 ? paint::led_amber : paint::led_red);
        g.setColour(static_cast<int>(i) < lit_ ? c : c.withAlpha(0.13f));
        g.fillRect(b.getX() + static_cast<float>(i) * w, b.getY(), w - 1.0f, b.getHeight());
    }
    const auto lamp = juce::Rectangle<float>(b.getRight() - gate_w + 2.0f, b.getY(), gate_w - 2.0f, b.getHeight());
    g.setColour(gate_ ? paint::led_green : paint::led_green.withAlpha(0.13f));
    g.fillEllipse(lamp.withSizeKeepingCentre(b.getHeight(), b.getHeight()));
}

// --- Lcd -------------------------------------------------------------------------------------

void Lcd::set(const juce::String& text, juce::Colour text_colour, bool menu_arrow)
{
    if (text == text_ && text_colour == colour_ && menu_arrow == arrow_) return;
    text_ = text;
    colour_ = text_colour;
    arrow_ = menu_arrow;
    repaint();
}

void Lcd::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    draw_well(g, r, 2.5f);
    auto glass = r.reduced(1.5f);
    juce::ColourGradient grad(paint::lcd_back.brighter(0.25f), 0.0f, glass.getY(), paint::lcd_back, 0.0f,
                              glass.getBottom(), false);
    g.setGradientFill(grad);
    g.fillRoundedRectangle(glass, 1.5f);
    auto text = glass.reduced(3.0f, 0.0f);
    if (!dot_.isTransparent()) {
        auto d = text.removeFromLeft(6.0f);
        g.setColour(dot_);
        g.fillEllipse(d.withSizeKeepingCentre(4.5f, 4.5f));
        text.removeFromLeft(1.0f);
    }
    if (arrow_ && on_click) {
        auto a = text.removeFromRight(6.0f);
        juce::Path p;
        const float cx = a.getCentreX(), cy = a.getCentreY();
        p.addTriangle(cx - 2.5f, cy - 1.2f, cx + 2.5f, cy - 1.2f, cx, cy + 2.0f);
        g.setColour(colour_.withAlpha(0.6f));
        g.fillPath(p);
    }
    g.setColour(colour_);
    g.setFont(juce::Font(juce::FontOptions(juce::jmin(10.5f, glass.getHeight() * 0.72f), juce::Font::bold)));
    g.drawFittedText(text_, text.toNearestInt(), juce::Justification::centred, 1, 0.7f);
    // Glass reflection.
    g.setColour(juce::Colours::white.withAlpha(0.05f));
    g.fillRect(glass.withHeight(glass.getHeight() * 0.45f));
}

void Lcd::mouseDown(const juce::MouseEvent& e)
{
    if (on_click && !e.mods.isPopupMenu()) on_click();
    else if (auto* p = getParentComponent()) p->mouseDown(e.getEventRelativeTo(p));
}

// --- DelayReadout ----------------------------------------------------------------------------

void DelayReadout::set(int samples, double sample_rate)
{
    if (sample_rate <= 0.0) sample_rate = 48000.0;
    if (samples == samples_ && std::abs(sample_rate - rate_) < 1e-6) return;
    samples_ = samples;
    rate_ = sample_rate;
    setTooltip("Processing delay (instrument and inserts): " + juce::String(samples_) + " samples / " +
               juce::String(1000.0 * samples_ / rate_, 2) + " ms. Compensated when PDC is on");
    repaint();
}

void DelayReadout::mouseEnter(const juce::MouseEvent&)
{
    hover_ = true;
    repaint();
}

void DelayReadout::mouseExit(const juce::MouseEvent&)
{
    hover_ = false;
    repaint();
}

void DelayReadout::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    const bool active = samples_ > 0;
    auto label = r.removeFromLeft(juce::jmin(22.0f, r.getWidth() * 0.3f));
    print_text(g, "DLY", label, 7.0f, juce::Justification::centredLeft, active ? paint::led_amber : paint::print_dim);
    auto window = r.reduced(0.0f, 0.5f);
    draw_well(g, window, 2.0f);
    auto glass = window.reduced(1.5f);
    g.setColour(active ? juce::Colour(0xff1d1404) : paint::lcd_back);
    g.fillRoundedRectangle(glass, 1.5f);
    juce::String text;
    if (hover_) text = juce::String(1000.0 * samples_ / rate_, samples_ > 0 && 1000.0 * samples_ / rate_ < 10.0 ? 2 : 1) + " ms";
    else text = juce::String(samples_) + (glass.getWidth() > 44.0f ? " smp" : "");
    g.setColour(active ? paint::led_amber : paint::lcd_text.withAlpha(0.45f));
    g.setFont(juce::Font(juce::FontOptions(juce::jmin(9.5f, glass.getHeight() * 0.8f), juce::Font::bold)));
    g.drawFittedText(text, glass.reduced(2.0f, 0.0f).toNearestInt(), juce::Justification::centredRight, 1, 0.7f);
    g.setColour(juce::Colours::white.withAlpha(0.05f));
    g.fillRect(glass.withHeight(glass.getHeight() * 0.45f));
}

// --- EqCurve ---------------------------------------------------------------------------------

bool EqCurveParams::operator==(const EqCurveParams& o) const
{
    auto same = [](double a, double b) { return std::abs(a - b) < 1e-9; };
    if (eq_on != o.eq_on || hpf_on != o.hpf_on || lpf_on != o.lpf_on || !same(hpf, o.hpf) || !same(lpf, o.lpf)) return false;
    for (std::size_t i = 0; i < bands.size(); ++i)
        if (!same(bands[i].f, o.bands[i].f) || !same(bands[i].g, o.bands[i].g) || !same(bands[i].q, o.bands[i].q) ||
            bands[i].bell != o.bands[i].bell)
            return false;
    return true;
}

void EqCurve::set(const EqCurveParams& p)
{
    if (valid_ && p == p_) return;
    p_ = p;
    valid_ = true;
    repaint();
}

double EqCurve::response_db(double hz) const
{
    double db = 0.0;
    if (p_.eq_on)
        for (std::size_t i = 0; i < p_.bands.size(); ++i) {
            const auto& b = p_.bands[i];
            const double ratio = hz / b.f;
            if (b.bell) {
                const double x = b.q * (ratio - 1.0 / ratio);
                db += b.g / (1.0 + x * x);
            } else if (i == 0) {
                db += b.g / (1.0 + ratio * ratio);
            } else {
                db += b.g / (1.0 + 1.0 / (ratio * ratio));
            }
        }
    if (p_.hpf_on) db -= 10.0 * std::log10(1.0 + std::pow(p_.hpf / hz, 4.0));
    if (p_.lpf_on) db -= 10.0 * std::log10(1.0 + std::pow(hz / p_.lpf, 4.0));
    return db;
}

void EqCurve::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    draw_well(g, r, 2.5f);
    auto screen = r.reduced(2.0f);
    g.setColour(juce::Colour(0xff0a1418));
    g.fillRoundedRectangle(screen, 1.5f);
    const auto x_of = [&](double hz) {
        return screen.getX() + static_cast<float>(std::log(hz / 20.0) / std::log(1000.0)) * screen.getWidth();
    };
    const auto y_of = [&](double db) {
        return screen.getCentreY() - static_cast<float>(juce::jlimit(-20.0, 20.0, db) / 20.0) * screen.getHeight() * 0.5f;
    };
    g.setColour(juce::Colours::white.withAlpha(0.07f));
    for (double f : {100.0, 1000.0, 10000.0}) g.fillRect(x_of(f), screen.getY(), 1.0f, screen.getHeight());
    g.fillRect(screen.getX(), y_of(0.0), screen.getWidth(), 1.0f);
    const bool active = p_.eq_on || p_.hpf_on || p_.lpf_on;
    juce::Path curve, fill;
    const int steps = juce::jmax(16, static_cast<int>(screen.getWidth() / 2.0f));
    for (int i = 0; i <= steps; ++i) {
        const double hz = 20.0 * std::pow(1000.0, static_cast<double>(i) / steps);
        const float x = x_of(hz), y = y_of(response_db(hz));
        if (i == 0) {
            curve.startNewSubPath(x, y);
            fill.startNewSubPath(x, y_of(0.0));
        }
        curve.lineTo(x, y);
        fill.lineTo(x, y);
    }
    fill.lineTo(screen.getRight(), y_of(0.0));
    fill.closeSubPath();
    const auto colour = active ? juce::Colour(0xff7fe6ff) : paint::print_dim.withAlpha(0.5f);
    g.saveState();
    g.reduceClipRegion(screen.toNearestInt());
    g.setColour(colour.withAlpha(0.18f));
    g.fillPath(fill);
    g.setColour(colour);
    g.strokePath(curve, juce::PathStrokeType(1.2f));
    g.restoreState();
}

// --- InsertRack ------------------------------------------------------------------------------

void InsertRack::set(std::vector<Slot> slots)
{
    if (slots == slots_) return;
    slots_ = std::move(slots);
    repaint();
}

juce::Rectangle<float> InsertRack::slot_bounds(int i) const
{
    const float h = static_cast<float>(getHeight()) / static_cast<float>(kSlots);
    return {0.0f, static_cast<float>(i) * h, static_cast<float>(getWidth()), h};
}

void InsertRack::paint(juce::Graphics& g)
{
    for (int i = 0; i < kSlots; ++i) {
        auto r = slot_bounds(i).reduced(0.5f, 1.0f);
        draw_well(g, r, 2.0f);
        const Slot* s = i < static_cast<int>(slots_.size()) ? &slots_[static_cast<std::size_t>(i)] : nullptr;
        const bool used = s && s->used;
        auto lamp = r.removeFromLeft(r.getHeight()).reduced(3.2f);
        const auto lamp_colour = used ? (s->suggestion ? paint::led_amber : paint::led_green) : paint::led_green;
        g.setColour(used && !s->bypass ? lamp_colour : lamp_colour.withAlpha(0.12f));
        g.fillRect(lamp);
        g.setFont(juce::Font(juce::FontOptions(juce::jmin(9.5f, r.getHeight() * 0.75f))));
        if (!used) {
            g.setColour(paint::print_dim.withAlpha(0.35f));
            g.drawText(juce::String(juce::CharPointer_UTF8("\xe2\x80\x94")), r, juce::Justification::centred, false);
            continue;
        }
        g.setColour(s->suggestion ? paint::led_amber.withAlpha(0.8f)
                                  : (s->bypass ? paint::print_dim.withAlpha(0.6f) : paint::lcd_text));
        g.drawFittedText(s->name, r.reduced(1.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.65f);
    }
}

void InsertRack::mouseDown(const juce::MouseEvent& e)
{
    const int i = juce::jlimit(0, kSlots - 1, static_cast<int>(e.position.y / slot_bounds(0).getHeight()));
    const bool lamp = e.position.x < slot_bounds(i).getHeight() + 1.0f;
    if (on_click) on_click(i, lamp && !e.mods.isPopupMenu());
}

// --- Scribble --------------------------------------------------------------------------------

void Scribble::set(const juce::String& name, juce::Colour colour, const juce::String& number, const juce::String& badge,
                   bool dim)
{
    if (name == name_ && colour == colour_ && number == number_ && badge == badge_ && dim == dim_) return;
    name_ = name;
    colour_ = colour;
    number_ = number;
    badge_ = badge;
    dim_ = dim;
    setTooltip(name);
    repaint();
}

void Scribble::set_selected(bool s)
{
    if (s == selected_) return;
    selected_ = s;
    repaint();
}

void Scribble::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced(1.0f);
    if (selected_) {
        g.setColour(paint::selected.withAlpha(0.35f));
        g.fillRoundedRectangle(r.expanded(1.0f), 3.5f);
    }
    draw_well(g, r.reduced(0.5f), 2.5f);
    auto glass = r.reduced(2.0f);
    // Backlit in the track colour.
    const auto lit = colour_.withMultipliedSaturation(0.9f);
    juce::ColourGradient grad(lit.withMultipliedBrightness(0.62f), 0.0f, glass.getY(), lit.withMultipliedBrightness(0.30f),
                              0.0f, glass.getBottom(), false);
    g.setGradientFill(grad);
    g.fillRoundedRectangle(glass, 1.5f);
    g.setColour(lit.brighter(0.4f));
    g.fillRect(glass.withHeight(2.0f));
    auto info = glass.reduced(3.0f, 2.0f);
    auto top = info.removeFromTop(10.0f).translated(0.0f, 1.0f);
    g.setFont(print_font(8.0f));
    g.setColour(juce::Colours::white.withAlpha(0.8f));
    g.drawText(number_, top, juce::Justification::centredLeft, false);
    if (badge_.isNotEmpty()) {
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.drawText(badge_, top, juce::Justification::centredRight, false);
    }
    g.setColour(dim_ ? juce::Colours::white.withAlpha(0.45f) : juce::Colours::white);
    g.setFont(juce::Font(juce::FontOptions(juce::jmin(12.0f, info.getHeight() * 0.62f), juce::Font::bold)));
    g.drawFittedText(name_, info.toNearestInt(), juce::Justification::centred, 2, 0.75f);
    g.setColour(juce::Colours::white.withAlpha(0.06f));
    g.fillRect(glass.withHeight(glass.getHeight() * 0.4f));
    if (selected_) {
        g.setColour(paint::selected);
        g.drawRoundedRectangle(r, 3.0f, 1.5f);
    }
}

void Scribble::mouseDown(const juce::MouseEvent& e)
{
    if (on_mouse_down) on_mouse_down(e);
}

}  // namespace vcc::desktop::console
