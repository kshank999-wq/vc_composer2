#include "ui/hw/HwControls.h"

#include <cmath>

namespace vcc::desktop::hw {

namespace {
constexpr float kPi = juce::MathConstants<float>::pi;
constexpr float kArc = kPi * 0.75f;  // ±135°

/// Where a (linear) slider's value sits in its range, 0 … 1.
double proportion_of(const juce::Slider& s)
{
    const double span = s.getMaximum() - s.getMinimum();
    return span > 0.0 ? juce::jlimit(0.0, 1.0, (s.getValue() - s.getMinimum()) / span) : 0.0;
}

juce::Colour led_or_theme(juce::Colour c) { return c.isTransparent() ? Theme::get().led() : c; }

/// Draws `path` with a soft glow of `colour` around it.
void glow(juce::Graphics& g, const juce::Path& path, juce::Colour colour, int radius)
{
    juce::DropShadow(colour, radius, {}).drawForPath(g, path);
}
}  // namespace

// --- Paint recipes ------------------------------------------------------------------------------

namespace paint {

void panel(juce::Graphics& g, juce::Rectangle<float> r)
{
    juce::Path body;
    body.addRoundedRectangle(r, tok::r_panel);
    juce::DropShadow(juce::Colours::black.withAlpha(0.55f), 10, {0, 2}).drawForPath(g, body);
    g.setGradientFill(juce::ColourGradient::vertical(tok::panel_top, r.getY(), tok::panel_bottom, r.getBottom()));
    g.fillPath(body);
    // Inset top highlight.
    g.setColour(juce::Colours::white.withAlpha(0.07f));
    g.drawHorizontalLine(juce::roundToInt(r.getY() + 1.0f), r.getX() + tok::r_panel, r.getRight() - tok::r_panel);
    g.setColour(tok::edge);
    g.strokePath(body, juce::PathStrokeType(1.0f));
}

void silk(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> r, float size, juce::Justification just,
          juce::Colour colour)
{
    g.setColour(colour);
    g.setFont(fonts::silk(size));
    g.drawText(text.toUpperCase(), r, just, false);
}

void groove(juce::Graphics& g, float x, float top, float bottom)
{
    g.setColour(juce::Colours::black.withAlpha(0.6f));
    g.drawVerticalLine(juce::roundToInt(x), top, bottom);
    g.setColour(juce::Colours::white.withAlpha(0.06f));
    g.drawVerticalLine(juce::roundToInt(x) + 1, top, bottom);
}

void keycap(juce::Graphics& g, juce::Rectangle<float> r, bool on, juce::Colour solid, juce::Colour ring)
{
    const juce::Colour lit = ring.isTransparent() ? Theme::get().led() : ring;
    // Raised: a 2 px dark drop under the cap. Pressed in: 1 px, cap 1 px lower.
    const float drop = on ? 1.0f : 2.0f;
    auto body_r = r.withTrimmedBottom(2.0f).translated(0.0f, on ? 1.0f : 0.0f);
    juce::Path body;
    body.addRoundedRectangle(body_r, tok::r_key);
    juce::Path base;
    base.addRoundedRectangle(body_r.translated(0.0f, drop), tok::r_key);
    if (!on) juce::DropShadow(juce::Colours::black.withAlpha(0.45f), 6, {0, 3}).drawForPath(g, base);
    g.setColour(tok::edge);
    g.fillPath(base);

    if (on && !solid.isTransparent()) {
        glow(g, body, solid.withAlpha(0.55f), 10);
        g.setGradientFill(juce::ColourGradient::vertical(solid.brighter(0.15f), body_r.getY(), solid.darker(0.12f),
                                                         body_r.getBottom()));
        g.fillPath(body);
    } else if (on) {
        glow(g, body, lit.withAlpha(0.6f), 10);
        g.setGradientFill(juce::ColourGradient::vertical(tok::key_on_top, body_r.getY(), tok::key_on_bottom,
                                                         body_r.getBottom()));
        g.fillPath(body);
        g.setColour(lit);
        g.strokePath(body, juce::PathStrokeType(1.0f));
    } else {
        g.setGradientFill(
            juce::ColourGradient::vertical(tok::key_top, body_r.getY(), tok::key_bottom, body_r.getBottom()));
        g.fillPath(body);
    }
    // Inset top highlight and the edge.
    g.setColour(juce::Colours::white.withAlpha(on ? 0.2f : 0.15f));
    g.drawHorizontalLine(juce::roundToInt(body_r.getY() + 1.0f), body_r.getX() + tok::r_key,
                         body_r.getRight() - tok::r_key);
    if (!(on && solid.isTransparent())) {
        g.setColour(tok::edge);
        g.strokePath(body, juce::PathStrokeType(1.0f));
    }
}

void lcd_window(juce::Graphics& g, juce::Rectangle<float> r)
{
    // Bottom bevel highlight, then the glass.
    g.setColour(juce::Colours::white.withAlpha(0.08f));
    g.drawRoundedRectangle(r.translated(0.0f, 1.0f), tok::r_lcd, 1.0f);
    g.setColour(tok::lcd_bg);
    g.fillRoundedRectangle(r, tok::r_lcd);
    // Inset shadow: dark at the top, fading over 8 px.
    juce::Graphics::ScopedSaveState s(g);
    juce::Path clip;
    clip.addRoundedRectangle(r, tok::r_lcd);
    g.reduceClipRegion(clip);
    g.setGradientFill(juce::ColourGradient::vertical(juce::Colours::black.withAlpha(0.95f), r.getY(),
                                                     juce::Colours::transparentBlack, r.getY() + 8.0f));
    g.fillRect(r.withHeight(8.0f));
    g.setColour(juce::Colours::black);
    g.drawRoundedRectangle(r.reduced(0.5f), tok::r_lcd, 1.0f);
}

void lcd_text(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> r, float size,
              juce::Justification just, float alpha)
{
    const auto& theme = Theme::get();
    g.setFont(fonts::lcd(size));
    // The backlight glow: the text spread a little, faintly.
    g.setColour(theme.lcd_glow().withMultipliedAlpha(0.22f * alpha));
    const float spread = std::max(1.0f, size / 14.0f);
    for (const auto& d : {juce::Point<float>{-spread, 0.0f}, {spread, 0.0f}, {0.0f, -spread}, {0.0f, spread},
                          {-spread * 0.7f, -spread * 0.7f}, {spread * 0.7f, spread * 0.7f},
                          {-spread * 0.7f, spread * 0.7f}, {spread * 0.7f, -spread * 0.7f}})
        g.drawText(text, r.translated(d.x, d.y), just, false);
    g.setColour(theme.lcd().withAlpha(alpha));
    g.drawText(text, r, just, false);
}

void led(juce::Graphics& g, juce::Point<float> centre, bool on, juce::Colour colour, float diameter)
{
    const auto r = juce::Rectangle<float>(diameter, diameter).withCentre(centre);
    if (on) {
        juce::Path p;
        p.addEllipse(r);
        glow(g, p, colour.withAlpha(0.6f), 6);
        g.setColour(colour);
        g.fillEllipse(r);
        g.setColour(juce::Colours::white.withAlpha(0.7f));
        g.fillEllipse(r.reduced(diameter * 0.32f).translated(-diameter * 0.12f, -diameter * 0.12f));
    } else {
        g.setColour(tok::led_off);
        g.fillEllipse(r);
        g.setColour(juce::Colours::black.withAlpha(0.7f));
        g.drawEllipse(r.reduced(0.25f), 0.75f);
    }
}

void screw(juce::Graphics& g, juce::Point<float> centre, float angle, float diameter)
{
    const auto r = juce::Rectangle<float>(diameter, diameter).withCentre(centre);
    juce::ColourGradient grad(juce::Colour(0xffb8bec6), r.getX() + diameter * 0.3f, r.getY() + diameter * 0.3f,
                              juce::Colour(0xff3a3f45), r.getRight(), r.getBottom(), true);
    g.setGradientFill(grad);
    g.fillEllipse(r);
    g.setColour(tok::edge);
    g.drawEllipse(r, 0.8f);
    const float len = diameter * 0.38f;
    const juce::Point<float> d(std::cos(angle) * len, std::sin(angle) * len);
    g.setColour(juce::Colour(0xff22252a));
    g.drawLine({centre - d, centre + d}, 1.4f);
}

void well(juce::Graphics& g, juce::Rectangle<float> r, float corner)
{
    g.setColour(tok::edge);
    g.fillRoundedRectangle(r, corner);
    juce::Graphics::ScopedSaveState s(g);
    g.reduceClipRegion(r.toNearestInt());
    g.setGradientFill(juce::ColourGradient::vertical(juce::Colours::black.withAlpha(0.8f), r.getY(),
                                                     juce::Colours::transparentBlack, r.getY() + 5.0f));
    g.fillRect(r.withHeight(5.0f));
    g.setColour(juce::Colours::white.withAlpha(0.05f));
    g.drawHorizontalLine(juce::roundToInt(r.getBottom()) - 1, r.getX() + corner, r.getRight() - corner);
}

}  // namespace paint

void draw_glyph(juce::Graphics& g, Glyph glyph, juce::Rectangle<float> r, juce::Colour colour)
{
    const auto c = r.getCentre();
    const float s = std::min(r.getWidth(), r.getHeight()) * 0.36f;
    juce::Path p;
    auto tri = [&](float x, float dir) {  // dir +1 points right
        p.addTriangle(x - dir * s * 0.5f, c.y - s * 0.55f, x - dir * s * 0.5f, c.y + s * 0.55f, x + dir * s * 0.5f, c.y);
    };
    g.setColour(colour);
    switch (glyph) {
        case Glyph::None: return;
        case Glyph::ReturnToZero:
            p.addRectangle(c.x - s * 0.62f, c.y - s * 0.55f, s * 0.16f, s * 1.1f);
            tri(c.x + s * 0.08f, -1.0f);
            break;
        case Glyph::Rewind:
            tri(c.x - s * 0.25f, -1.0f);
            tri(c.x + s * 0.25f, -1.0f);
            break;
        case Glyph::FastForward:
            tri(c.x - s * 0.25f, 1.0f);
            tri(c.x + s * 0.25f, 1.0f);
            break;
        case Glyph::Stop: p.addRectangle(c.x - s * 0.42f, c.y - s * 0.42f, s * 0.84f, s * 0.84f); break;
        case Glyph::Play: tri(c.x + s * 0.06f, 1.0f); break;
        case Glyph::Record: p.addEllipse(c.x - s * 0.42f, c.y - s * 0.42f, s * 0.84f, s * 0.84f); break;
        case Glyph::Loop: {
            juce::Path arc;
            const float rad = s * 0.62f;
            arc.addCentredArc(c.x, c.y, rad, rad * 0.78f, 0.0f, 0.35f, kPi - 0.35f, true);
            juce::Path arc2;
            arc2.addCentredArc(c.x, c.y, rad, rad * 0.78f, 0.0f, kPi + 0.35f, 2.0f * kPi - 0.35f, true);
            g.strokePath(arc, juce::PathStrokeType(1.6f));
            g.strokePath(arc2, juce::PathStrokeType(1.6f));
            const auto a = arc.getPointAlongPath(arc.getLength());
            const auto b = arc2.getPointAlongPath(arc2.getLength());
            p.addTriangle(a.x - 3.5f, a.y - 3.0f, a.x + 3.5f, a.y - 3.0f, a.x, a.y + 2.5f);
            p.addTriangle(b.x - 3.5f, b.y + 3.0f, b.x + 3.5f, b.y + 3.0f, b.x, b.y - 2.5f);
            break;
        }
    }
    g.fillPath(p);
}

// --- Panel ---------------------------------------------------------------------------------------

Panel::Panel(juce::String title, bool screws) : title_(std::move(title)), screws_(screws)
{
    // Slots at fixed, different angles (decorative).
    const float angles[4] = {0.4f, 1.9f, 2.7f, 0.9f};
    for (int i = 0; i < 4; ++i) screw_angles_[i] = angles[i];
}

void Panel::set_title(const juce::String& t)
{
    title_ = t;
    repaint();
}

void Panel::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced(1.0f, 1.0f).withTrimmedBottom(2.0f);
    paint::panel(g, r);
    if (title_.isNotEmpty()) paint::silk(g, title_, r.reduced(12.0f, 0.0f).withHeight(24.0f).withY(r.getY() + 4.0f));
    if (screws_) {
        const float in = 9.0f;
        paint::screw(g, {r.getX() + in, r.getY() + in}, screw_angles_[0]);
        paint::screw(g, {r.getRight() - in, r.getY() + in}, screw_angles_[1]);
        paint::screw(g, {r.getX() + in, r.getBottom() - in}, screw_angles_[2]);
        paint::screw(g, {r.getRight() - in, r.getBottom() - in}, screw_angles_[3]);
    }
}

juce::Rectangle<int> Panel::content() const
{
    auto r = getLocalBounds().reduced(12, 8);
    if (title_.isNotEmpty()) r.removeFromTop(20);
    return r;
}

// --- Keycap --------------------------------------------------------------------------------------

Keycap::Keycap(const juce::String& legend, bool toggles) : juce::Button(legend)
{
    setClickingTogglesState(toggles);
    setTitle(legend);  // the accessible name, also for glyph-only keys
    setButtonText(legend);
}

bool Keycap::lit() const { return getToggleState(); }

void Keycap::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
    const bool on = getToggleState() || down;
    const auto r = getLocalBounds().toFloat().reduced(0.5f);
    paint::keycap(g, r, on, getToggleState() ? solid_ : juce::Colour(), lit_colour_);
    auto face = r.withTrimmedBottom(2.0f).translated(0.0f, on ? 1.0f : 0.0f).reduced(4.0f, 2.0f);
    const bool solid_on = getToggleState() && !solid_.isTransparent();
    const juce::Colour ink = solid_on ? juce::Colour(0xff101214) : on ? juce::Colours::white : tok::text;
    if (highlighted && !on) {
        g.setColour(juce::Colours::white.withAlpha(0.04f));
        g.fillRoundedRectangle(face, tok::r_key);
    }
    if (led_dot_) {
        auto dot = face.removeFromLeft(12.0f);
        paint::led(g, dot.getCentre().translated(2.0f, 0.0f), led_forced_ < 0 ? getToggleState() : led_forced_ == 1,
                   led_or_theme(led_colour_));
    }
    if (glyph_ != Glyph::None) {
        const juce::Colour gc = on ? (glyph_lit_.isTransparent() ? Theme::get().led() : glyph_lit_) : tok::text;
        draw_glyph(g, glyph_, face, solid_on ? ink : gc);
        return;
    }
    g.setColour(ink);
    if (symbol_.isNotEmpty()) {
        // The glyph centred by its own outline (a music font's ascent says
        // nothing about where a note sits), the legend in the corner.
        juce::GlyphArrangement ga;
        ga.addLineOfText(symbol_font_, symbol_, 0.0f, 0.0f);
        const auto bb = ga.getBoundingBox(0, -1, true);
        const auto target = face.withTrimmedRight(face.getWidth() * 0.18f).getCentre();
        ga.moveRangeOfGlyphs(0, -1, target.x - bb.getCentreX(), target.y - bb.getCentreY());
        g.setColour(on && solid_.isTransparent() ? Theme::get().led().brighter(0.4f) : ink);
        ga.draw(g);
        g.setColour(ink.withAlpha(0.62f));
        g.setFont(fonts::lcd(9.0f));
        g.drawText(getButtonText(), face.expanded(2.0f, 1.0f), juce::Justification::bottomRight, false);
        return;
    }
    if (dynamic_) {
        g.setFont(fonts::dynamics(legend_size_));
        g.drawText(getButtonText(), face, juce::Justification::centred, false);
        return;
    }
    g.setFont(fonts::silk(legend_size_, 0.06f));
    if (sublegend_.isEmpty()) {
        g.drawText(getButtonText().toUpperCase(), face, juce::Justification::centred, false);
    } else {
        auto top = face.removeFromTop(face.getHeight() * 0.55f);
        g.drawText(getButtonText().toUpperCase(), top, juce::Justification::centredBottom, false);
        g.setColour(ink.withAlpha(0.6f));
        g.setFont(fonts::ui(9.5f));
        g.drawFittedText(sublegend_, face.toNearestInt(), juce::Justification::centredTop, 2);
    }
}

// --- LED -----------------------------------------------------------------------------------------

void Led::set_on(bool on)
{
    if (on == on_) return;
    on_ = on;
    repaint();
}

void Led::paint(juce::Graphics& g)
{
    const float d = std::min(6.0f, static_cast<float>(std::min(getWidth(), getHeight())) - 2.0f);
    paint::led(g, getLocalBounds().toFloat().getCentre(), on_, led_or_theme(colour_), d);
}

// --- LCD -----------------------------------------------------------------------------------------

Lcd::Lcd(juce::String text, float size) : text_(std::move(text)), size_(size) {}

void Lcd::set_text(const juce::String& t)
{
    if (t == text_) return;
    text_ = t;
    repaint();
}

void Lcd::set_on_click(std::function<void()> f)
{
    on_click_ = std::move(f);
    setMouseCursor(on_click_ ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void Lcd::mouseUp(const juce::MouseEvent& e)
{
    if (on_click_ && getLocalBounds().contains(e.getPosition())) on_click_();
}

void Lcd::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced(0.5f).withTrimmedBottom(1.0f);
    paint::lcd_window(g, r);
    auto inner = r.reduced(std::min(7.0f, r.getWidth() * 0.12f), std::min(3.0f, r.getHeight() * 0.1f));
    if (caption_.isNotEmpty()) {
        paint::lcd_text(g, caption_.toUpperCase(), inner.removeFromTop(12.0f), 9.5f, juce::Justification::topLeft,
                        0.65f);
    }
    juce::StringArray lines;
    lines.addLines(text_);
    if (lines.isEmpty()) return;
    const float line_h = inner.getHeight() / static_cast<float>(lines.size());
    for (int i = 0; i < lines.size(); ++i)
        paint::lcd_text(g, lines[i], inner.withHeight(line_h).translated(0.0f, line_h * static_cast<float>(i)), size_,
                        just_);
}

// --- Knobs ---------------------------------------------------------------------------------------

Knob::Knob() : juce::Slider(juce::Slider::RotaryVerticalDrag, juce::Slider::NoTextBox)
{
    setRotaryParameters(kPi * 1.25f, kPi * 2.75f, true);
    setRange(0.0, 1.0);
}

float Knob::angle() const
{
    return -kArc + 2.0f * kArc * static_cast<float>(proportion_of(*this));
}

void Knob::mouseDown(const juce::MouseEvent& e)
{
    proportion_ = valueToProportionOfLength(getValue());
    last_y_ = e.position.y;
    if (onDragStart) onDragStart();
}

void Knob::mouseDrag(const juce::MouseEvent& e)
{
    const float dy = last_y_ - e.position.y;
    last_y_ = e.position.y;
    proportion_ = juce::jlimit(0.0, 1.0, proportion_ + dy / (e.mods.isShiftDown() ? 800.0 : 160.0));
    setValue(proportionOfLengthToValue(proportion_), juce::sendNotificationSync);
}

void Knob::mouseUp(const juce::MouseEvent&)
{
    if (onDragEnd) onDragEnd();
}

void Knob::mouseDoubleClick(const juce::MouseEvent&)
{
    if (isDoubleClickReturnEnabled()) setValue(getDoubleClickReturnValue(), juce::sendNotificationSync);
}

void Knob::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const double p = valueToProportionOfLength(getValue());
    const double step = (e.mods.isShiftDown() ? 0.01 : 0.04) * (w.deltaY > 0 ? 1.0 : w.deltaY < 0 ? -1.0 : 0.0);
    setValue(proportionOfLengthToValue(juce::jlimit(0.0, 1.0, p + step)), juce::sendNotificationSync);
}

void SmallKnob::paint(juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const float d = std::min(b.getWidth(), b.getHeight()) - 2.0f;
    const auto r = juce::Rectangle<float>(d, d).withCentre(b.getCentre());
    // Collar ring.
    g.setColour(tok::edge);
    g.fillEllipse(r);
    g.setColour(juce::Colour(0xff3a3f46));
    g.drawEllipse(r.reduced(1.0f), 1.0f);
    // Cap.
    const auto cap = r.reduced(d * 0.12f);
    juce::Path cp;
    cp.addEllipse(cap);
    juce::DropShadow(juce::Colours::black.withAlpha(0.6f), 3, {0, 1}).drawForPath(g, cp);
    juce::ColourGradient grad(juce::Colour(0xff7a828c), cap.getX() + cap.getWidth() * 0.3f,
                              cap.getY() + cap.getHeight() * 0.25f, juce::Colour(0xff23272c), cap.getRight(),
                              cap.getBottom(), true);
    g.setGradientFill(grad);
    g.fillPath(cp);
    g.setColour(juce::Colours::black.withAlpha(0.7f));
    g.drawEllipse(cap, 0.8f);
    // Pointer.
    const float a = angle();
    const auto c = cap.getCentre();
    const float len = cap.getWidth() * 0.46f;
    const juce::Point<float> tip(c.x + std::sin(a) * len, c.y - std::cos(a) * len);
    const juce::Point<float> from(c.x + std::sin(a) * len * 0.25f, c.y - std::cos(a) * len * 0.25f);
    g.setColour(juce::Colours::white.withAlpha(isEnabled() ? 0.95f : 0.4f));
    g.drawLine({from, tip}, 1.8f);
}

int RingKnob::lit_leds() const
{
    return juce::roundToInt(proportion_of(*this) * kLeds);
}

void RingKnob::paint(juce::Graphics& g)
{
    const auto& theme = Theme::get();
    const auto b = getLocalBounds().toFloat();
    const float d = std::min(b.getWidth(), b.getHeight());
    const auto c = b.getCentre();
    // The LED ring: 15 on a 270° arc.
    const float ring = d * 0.44f;
    const int lit = lit_leds();
    for (int i = 0; i < kLeds; ++i) {
        const float a = -kArc + 2.0f * kArc * static_cast<float>(i) / static_cast<float>(kLeds - 1);
        const juce::Point<float> p(c.x + std::sin(a) * ring, c.y - std::cos(a) * ring);
        paint::led(g, p, i < lit, theme.led(), std::max(3.5f, d * 0.055f));
    }
    // Knurled cap (50 px at 84).
    const float cap_d = d * 50.0f / 84.0f;
    const auto cap = juce::Rectangle<float>(cap_d, cap_d).withCentre(c);
    juce::Path cp;
    cp.addEllipse(cap);
    juce::DropShadow(juce::Colours::black.withAlpha(0.7f), 6, {0, 2}).drawForPath(g, cp);
    g.setColour(tok::edge);
    g.fillEllipse(cap.expanded(2.0f));
    juce::ColourGradient grad(juce::Colour(0xff6a717a), cap.getX() + cap_d * 0.3f, cap.getY() + cap_d * 0.25f,
                              juce::Colour(0xff1c1f23), cap.getRight(), cap.getBottom(), true);
    g.setGradientFill(grad);
    g.fillPath(cp);
    // Knurling: fine ridges round the rim.
    g.setColour(juce::Colours::black.withAlpha(0.45f));
    for (int i = 0; i < 48; ++i) {
        const float a = 2.0f * kPi * static_cast<float>(i) / 48.0f;
        const juce::Point<float> o(c.x + std::sin(a) * cap_d * 0.5f, c.y - std::cos(a) * cap_d * 0.5f);
        const juce::Point<float> in(c.x + std::sin(a) * cap_d * 0.42f, c.y - std::cos(a) * cap_d * 0.42f);
        g.drawLine({in, o}, 0.8f);
    }
    // Top face and the LED pointer.
    const auto face = cap.reduced(cap_d * 0.09f);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff4a5058), face.getX(), face.getY(), juce::Colour(0xff2a2e34),
                                           face.getRight(), face.getBottom(), false));
    g.fillEllipse(face);
    const float a = angle();
    const float len = face.getWidth() * 0.42f;
    const juce::Point<float> tip(c.x + std::sin(a) * len, c.y - std::cos(a) * len);
    const juce::Point<float> from(c.x + std::sin(a) * len * 0.45f, c.y - std::cos(a) * len * 0.45f);
    juce::Path pointer;
    pointer.addLineSegment({from, tip}, 2.6f);
    juce::DropShadow(theme.led_glow(), 6, {}).drawForPath(g, pointer);
    g.setColour(theme.led());
    g.fillPath(pointer);
}

// --- Meters --------------------------------------------------------------------------------------

void MeterBallistics::push(float dbfs)
{
    const float v = std::max(kFloor, std::isfinite(dbfs) ? std::min(dbfs, 0.0f) : kFloor);
    level = std::max(level, v);
    if (v >= peak) {
        peak = v;
        hold = kHoldSeconds;
    }
}

void MeterBallistics::advance(float seconds)
{
    level = std::max(kFloor, level - kFallDbPerSecond * seconds);
    if (hold > 0.0f) {
        const float used = std::min(hold, seconds);
        hold -= used;
        seconds -= used;
    }
    if (hold <= 0.0f) peak = std::max(kFloor, std::max(level, peak - kFallDbPerSecond * seconds));
}

Meter::Meter(Orientation o, int segments) : orientation_(o) { set_segments(segments); }

void Meter::set_segments(int n)
{
    segments_ = std::max(4, n);
    if (orientation_ == Orientation::Horizontal) {
        // 30: green to 22, amber to 28, red 29–30 (scaled for other counts).
        green_ = juce::roundToInt(segments_ * 22.0 / 30.0);
        amber_ = juce::roundToInt(segments_ * 28.0 / 30.0);
    } else {
        green_ = segments_ - 4;
        amber_ = segments_ - 1;
    }
    repaint();
}

int Meter::segments_for(float dbfs) const
{
    const float p = (dbfs - MeterBallistics::kFloor) / -MeterBallistics::kFloor;
    return juce::jlimit(0, segments_, juce::roundToInt(p * static_cast<float>(segments_)));
}

int Meter::lit_segments() const { return segments_for(b_.level); }
int Meter::peak_segment() const { return segments_for(b_.peak); }

void Meter::push(float dbfs)
{
    b_.push(dbfs);
    repaint();
    if (!isTimerRunning()) startTimerHz(30);
}

void Meter::advance(float seconds)
{
    b_.advance(seconds);
    repaint();
}

void Meter::timerCallback()
{
    advance(1.0f / 30.0f);
    if (b_.idle()) stopTimer();
}

juce::Colour Meter::segment_colour(int index) const
{
    return index < green_ ? tok::meter_green : index < amber_ ? tok::meter_amber : tok::meter_red;
}

void Meter::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    const int lit = lit_segments();
    const int peak = peak_segment();
    const bool vertical = orientation_ == Orientation::Vertical;
    const float length = vertical ? r.getHeight() : r.getWidth();
    const float gap = 1.0f;
    const float seg = (length - gap * static_cast<float>(segments_ - 1)) / static_cast<float>(segments_);
    for (int i = 0; i < segments_; ++i) {
        juce::Rectangle<float> s;
        if (vertical)
            s = {r.getX(), r.getBottom() - (seg + gap) * static_cast<float>(i) - seg, r.getWidth(), seg};
        else
            s = {r.getX() + (seg + gap) * static_cast<float>(i), r.getY(), seg, r.getHeight()};
        const bool on = i < lit || (peak > 0 && i == peak - 1);
        if (on) {
            const auto col = segment_colour(i);
            juce::Path p;
            p.addRectangle(s);
            juce::DropShadow(col.withAlpha(0.45f), 3, {}).drawForPath(g, p);
            g.setColour(col);
        } else {
            g.setColour(tok::meter_off);
        }
        g.fillRect(s);
    }
}

// --- Fader ---------------------------------------------------------------------------------------

namespace {
// The fader law: printed scale positions (dB, proportion of the throw).
constexpr double kLaw[][2] = {{6.0, 1.0}, {0.0, 0.82}, {-6.0, 0.66}, {-12.0, 0.52},
                              {-24.0, 0.34}, {-48.0, 0.14}, {-96.0, 0.0}};
constexpr int kLawPoints = 7;
}  // namespace

Fader::Fader() : juce::Slider(juce::Slider::LinearVertical, juce::Slider::NoTextBox)
{
    setRange(0.0, 1.0);
    setValue(proportion_for(0.0), juce::dontSendNotification);
    setDoubleClickReturnValue(true, proportion_for(0.0));
    setSliderSnapsToMousePosition(false);
}

double Fader::proportion_for(double db)
{
    if (!(db > kLaw[kLawPoints - 1][0])) return 0.0;
    if (db >= kLaw[0][0]) return 1.0;
    for (int i = 0; i + 1 < kLawPoints; ++i)
        if (db <= kLaw[i][0] && db >= kLaw[i + 1][0]) {
            const double t = (db - kLaw[i + 1][0]) / (kLaw[i][0] - kLaw[i + 1][0]);
            return kLaw[i + 1][1] + t * (kLaw[i][1] - kLaw[i + 1][1]);
        }
    return 0.0;
}

double Fader::db_for(double proportion)
{
    if (proportion <= 0.0) return -std::numeric_limits<double>::infinity();
    if (proportion >= 1.0) return kLaw[0][0];
    for (int i = 0; i + 1 < kLawPoints; ++i)
        if (proportion <= kLaw[i][1] && proportion >= kLaw[i + 1][1]) {
            const double t = (proportion - kLaw[i + 1][1]) / (kLaw[i][1] - kLaw[i + 1][1]);
            return kLaw[i + 1][0] + t * (kLaw[i][0] - kLaw[i + 1][0]);
        }
    return kLaw[0][0];
}

void Fader::set_db(double db, juce::NotificationType n) { setValue(proportion_for(db), n); }

juce::Rectangle<float> Fader::slot() const
{
    const auto r = getLocalBounds().toFloat();
    return {r.getCentreX() + 6.0f - 3.0f, r.getY() + 10.0f, 6.0f, r.getHeight() - 20.0f};
}

void Fader::paint(juce::Graphics& g)
{
    const auto s = slot();
    // The dB scale on the left.
    g.setFont(fonts::lcd(9.0f));
    for (const double db : {6.0, 0.0, -6.0, -12.0, -24.0, -48.0, -96.0}) {
        const float y = s.getBottom() - static_cast<float>(proportion_for(db)) * s.getHeight();
        g.setColour(tok::silk.withAlpha(0.8f));
        g.drawHorizontalLine(juce::roundToInt(y), s.getX() - 9.0f, s.getX() - 3.0f);
        const juce::String label = db <= -96.0 ? juce::String(juce::CharPointer_UTF8("-\xe2\x88\x9e"))
                                               : (db > 0 ? "+" : "") + juce::String(static_cast<int>(db));
        g.drawText(label, juce::Rectangle<float>(s.getX() - 40.0f, y - 6.0f, 29.0f, 12.0f),
                   juce::Justification::centredRight, false);
    }
    paint::well(g, s, 3.0f);
    // The cap.
    const float y = s.getBottom() - static_cast<float>(getValue()) * s.getHeight();
    const auto cap = juce::Rectangle<float>(28.0f, 16.0f).withCentre({s.getCentreX(), y});
    juce::Path cp;
    cp.addRoundedRectangle(cap, 2.5f);
    juce::DropShadow(juce::Colours::black.withAlpha(0.7f), 5, {0, 3}).drawForPath(g, cp);
    g.setGradientFill(juce::ColourGradient::vertical(juce::Colour(0xffd6dbe0), cap.getY(), juce::Colour(0xff7d848c),
                                                     cap.getBottom()));
    g.fillPath(cp);
    g.setColour(tok::edge);
    g.strokePath(cp, juce::PathStrokeType(1.0f));
    g.setColour(juce::Colour(0xff15171a));
    g.drawHorizontalLine(juce::roundToInt(cap.getCentreY()), cap.getX() + 3.0f, cap.getRight() - 3.0f);
}

// --- Wheel ---------------------------------------------------------------------------------------

Wheel::Wheel(bool springs_back)
    : juce::Slider(juce::Slider::LinearVertical, juce::Slider::NoTextBox), springs_(springs_back)
{
    setRange(springs_back ? -1.0 : 0.0, 1.0);
    setValue(springs_back ? 0.0 : 0.0, juce::dontSendNotification);
    setSliderSnapsToMousePosition(false);
}

void Wheel::mouseUp(const juce::MouseEvent& e)
{
    juce::Slider::mouseUp(e);
    if (springs_) setValue(0.0, juce::sendNotificationSync);
}

void Wheel::paint(juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const auto slot = juce::Rectangle<float>(std::min(28.0f, b.getWidth()), std::min(84.0f, b.getHeight()))
                          .withCentre(b.getCentre());
    paint::well(g, slot, 4.0f);
    const auto wheel = slot.reduced(5.0f, 4.0f);
    // A cylinder lit from the front: dark at top and bottom.
    juce::ColourGradient body(juce::Colour(0xff15171a), wheel.getX(), wheel.getY(), juce::Colour(0xff15171a),
                              wheel.getX(), wheel.getBottom(), false);
    body.addColour(0.5, juce::Colour(0xff4a5058));
    g.setGradientFill(body);
    g.fillRoundedRectangle(wheel, 3.0f);
    // Ribs, moving with the wheel's turn.
    const double p = proportion_of(*this);
    const float phase = static_cast<float>(p) * 36.0f;
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    for (float y = std::fmod(phase, 6.0f); y < wheel.getHeight(); y += 6.0f) {
        const float t = y / wheel.getHeight();
        const float fade = std::sin(t * kPi);
        g.setColour(juce::Colours::black.withAlpha(0.25f + 0.35f * fade));
        g.drawHorizontalLine(juce::roundToInt(wheel.getY() + y), wheel.getX() + 1.0f, wheel.getRight() - 1.0f);
    }
    // The centre mark (where the pitch wheel rests).
    const float mark = wheel.getY() + wheel.getHeight() * (1.0f - static_cast<float>(p));
    g.setColour(Theme::get().led().withAlpha(0.85f));
    g.fillRect(juce::Rectangle<float>(wheel.getX() + 2.0f, mark - 1.0f, wheel.getWidth() - 4.0f, 2.0f));
}

}  // namespace vcc::desktop::hw
