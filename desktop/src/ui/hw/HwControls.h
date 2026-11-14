// The Edit window's hardware controls (docs/spec/ui/SPEC.md, "Components"):
// panel, keycap, LED, LCD, small knob, ring knob, meters, fader, wheel and
// screw. Each paints itself from the theme (HwTheme.h) every time, so a
// theme change shows at once. Everything is drawn in code: shading,
// gradients and glows, no bitmaps.
#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "ui/hw/HwTheme.h"

namespace vcc::desktop::hw {

// --- Paint recipes ------------------------------------------------------------------------------

namespace paint {
/// An anodised panel: gradient fill, dark edge, top highlight, drop shadow.
void panel(juce::Graphics& g, juce::Rectangle<float> r);
/// Silk-screen text (upper case, spaced).
void silk(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> r, float size = 11.0f,
          juce::Justification just = juce::Justification::centredLeft, juce::Colour colour = tok::silk);
/// A groove divider between control groups.
void groove(juce::Graphics& g, float x, float top, float bottom);
/// A keycap body. `on`: pressed in, LED ring and glow. `solid`: an on state
/// filled with that colour instead (record, solo, mute).
void keycap(juce::Graphics& g, juce::Rectangle<float> r, bool on, juce::Colour solid = {}, juce::Colour ring = {});
/// A backlit LCD window.
void lcd_window(juce::Graphics& g, juce::Rectangle<float> r);
/// LCD text with its glow.
void lcd_text(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> r, float size,
              juce::Justification just = juce::Justification::centredLeft, float alpha = 1.0f);
/// A round LED (6 px by default).
void led(juce::Graphics& g, juce::Point<float> centre, bool on, juce::Colour colour, float diameter = 6.0f);
/// A decorative screw head with its slot at `angle` radians.
void screw(juce::Graphics& g, juce::Point<float> centre, float angle, float diameter = 9.0f);
/// A recessed dark well (meters, wheels, keyboard frame).
void well(juce::Graphics& g, juce::Rectangle<float> r, float corner = 3.0f);
}  // namespace paint

/// Transport and tool glyphs for keycaps.
enum class Glyph { None, ReturnToZero, Rewind, FastForward, Stop, Play, Record, Loop };
void draw_glyph(juce::Graphics& g, Glyph glyph, juce::Rectangle<float> r, juce::Colour colour);

// --- Panel ---------------------------------------------------------------------------------------

/// A raised panel with an optional silk title and corner screws. Children
/// are laid out by the owner.
class Panel : public juce::Component {
public:
    explicit Panel(juce::String title = {}, bool screws = false);
    void set_title(const juce::String& t);
    void paint(juce::Graphics& g) override;
    /// The area under the title.
    juce::Rectangle<int> content() const;

private:
    juce::String title_;
    bool screws_ = false;
    float screw_angles_[4] = {};
};

// --- Keycap --------------------------------------------------------------------------------------

/// A raised keycap: a real button (toggle or momentary), with an optional
/// LED dot left of the legend and an optional glyph. Lit when its toggle is
/// on or while it is held.
class Keycap : public juce::Button {
public:
    explicit Keycap(const juce::String& legend, bool toggles = false);
    void set_led_dot(bool show) { led_dot_ = show; repaint(); }
    /// The LED dot lit or not regardless of the toggle (a finished stage).
    void set_led_lit(bool lit) { led_forced_ = lit ? 1 : 0; repaint(); }
    /// The LED dot's colour; default follows the theme's LED colour.
    void set_led_colour(juce::Colour c) { led_colour_ = c; repaint(); }
    /// Lit as a solid fill (record red, solo amber, mute blue) with dark text.
    void set_solid(juce::Colour c) { solid_ = c; repaint(); }
    void set_glyph(Glyph g) { glyph_ = g; repaint(); }
    /// A lit glyph colour other than the LED's (play is green).
    void set_glyph_lit(juce::Colour c) { glyph_lit_ = c; repaint(); }
    void set_legend_size(float s) { legend_size_ = s; repaint(); }
    /// The lit ring and glow in this colour instead of the theme's LED (play is green).
    void set_lit_colour(juce::Colour c) { lit_colour_ = c; repaint(); }
    /// The legend as a dynamic marking: bold italic serif, as written (pp, mf).
    void set_dynamic_legend(bool on) { dynamic_ = on; repaint(); }
    /// A small second line under the legend.
    void set_sublegend(const juce::String& s) { sublegend_ = s; repaint(); }
    /// A music symbol (a SMuFL glyph in `font`) as the face, with the legend
    /// small in the bottom-right corner, as a keypad key prints its number.
    void set_music_symbol(const juce::String& symbol, const juce::Font& font)
    {
        symbol_ = symbol;
        symbol_font_ = font;
        repaint();
    }

    bool lit() const;

protected:
    void paintButton(juce::Graphics& g, bool highlighted, bool down) override;
    /// A key that cannot be pressed now is dimmed.
    void enablementChanged() override
    {
        setAlpha(isEnabled() ? 1.0f : 0.42f);
        juce::Button::enablementChanged();
    }

private:
    bool led_dot_ = false;
    int led_forced_ = -1;  ///< -1: follows the toggle.
    juce::Colour led_colour_;
    juce::Colour solid_;
    juce::Colour glyph_lit_;
    juce::Colour lit_colour_;
    bool dynamic_ = false;
    Glyph glyph_ = Glyph::None;
    float legend_size_ = 12.0f;
    juce::String sublegend_;
    juce::String symbol_;
    juce::Font symbol_font_{juce::FontOptions{}};
};

// --- LED -----------------------------------------------------------------------------------------

class Led : public juce::Component {
public:
    explicit Led(juce::Colour colour = {}) : colour_(colour) {}
    void set_on(bool on);
    bool on() const noexcept { return on_; }
    /// Transparent follows the theme's LED colour.
    void set_colour(juce::Colour c) { colour_ = c; repaint(); }
    void paint(juce::Graphics& g) override;

private:
    juce::Colour colour_;
    bool on_ = false;
};

// --- LCD -----------------------------------------------------------------------------------------

/// A backlit readout. `caption` is a small label in the top-left corner at
/// 65%; the text can run to several lines.
class Lcd : public juce::Component, public juce::SettableTooltipClient {
public:
    explicit Lcd(juce::String text = {}, float size = 13.0f);
    void set_text(const juce::String& t);
    const juce::String& text() const noexcept { return text_; }
    void set_caption(const juce::String& c) { caption_ = c; repaint(); }
    void set_size(float s) { size_ = s; repaint(); }
    void set_justification(juce::Justification j) { just_ = j; repaint(); }
    /// Makes the readout clickable (tempo, meter and key open an editor).
    void set_on_click(std::function<void()> f);
    void paint(juce::Graphics& g) override;
    void mouseUp(const juce::MouseEvent& e) override;

private:
    std::function<void()> on_click_;
    juce::String text_, caption_;
    float size_ = 13.0f;
    juce::Justification just_ = juce::Justification::centredLeft;
};

// --- Knobs ---------------------------------------------------------------------------------------

/// A knob: drag vertically to change (Shift for fine), double-click resets.
/// A slider underneath, so values, ranges and accessibility come with it.
class Knob : public juce::Slider {
public:
    Knob();
    /// Rotation for the current value, −135° … +135°.
    float angle() const;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;

private:
    double drag_start_ = 0.0;
    float last_y_ = 0.0f;
    double proportion_ = 0.0;
};

/// The 26–30 px knob: radial cap, white pointer, collar ring.
class SmallKnob final : public Knob {
public:
    void paint(juce::Graphics& g) override;
};

/// The 84 px ring knob: 15 LEDs on a 270° arc, knurled 50 px cap, LED pointer.
class RingKnob final : public Knob {
public:
    static constexpr int kLeds = 15;
    /// LEDs lit for the current value.
    int lit_leds() const;
    void paint(juce::Graphics& g) override;
};

// --- Meters --------------------------------------------------------------------------------------

/// Meter ballistics: the shown level falls at 20 dB a second, and the peak
/// is held for 1.5 s before it falls too.
struct MeterBallistics {
    static constexpr float kFloor = -60.0f;
    static constexpr float kFallDbPerSecond = 20.0f;
    static constexpr float kHoldSeconds = 1.5f;
    float level = kFloor;  ///< dBFS shown.
    float peak = kFloor;   ///< dBFS of the held peak.
    float hold = 0.0f;     ///< Seconds of hold left.
    /// A new reading: jumps up at once, never down (falling is `advance`'s).
    void push(float dbfs);
    void advance(float seconds);
    bool idle() const noexcept { return level <= kFloor && peak <= kFloor; }
};

/// An LED ladder. Vertical: `segments` (12, or 20 on a grand staff), green
/// to n−4, amber to n−1, red at the top. Horizontal: 30, green to 22,
/// amber to 28, red 29–30.
class Meter final : public juce::Component, private juce::Timer {
public:
    enum class Orientation { Vertical, Horizontal };
    explicit Meter(Orientation o = Orientation::Vertical, int segments = 12);
    /// The spec's colour split for a vertical ladder of `n`, or a horizontal one.
    void set_segments(int n);
    /// A reading in dBFS (−inf … 0); ballistics are applied.
    void push(float dbfs);
    /// Shown level and held peak as lit segment counts (for tests).
    int lit_segments() const;
    int peak_segment() const;
    const MeterBallistics& ballistics() const noexcept { return b_; }
    /// Advances the ballistics by `seconds` (the timer does this at 30 Hz).
    void advance(float seconds);
    void paint(juce::Graphics& g) override;

private:
    void timerCallback() override;
    juce::Colour segment_colour(int index) const;
    int segments_for(float dbfs) const;
    Orientation orientation_;
    int segments_ = 12;
    int green_ = 8, amber_ = 11;
    MeterBallistics b_;
};

// --- Fader ---------------------------------------------------------------------------------------

/// A long-throw fader: 6 px slot, metal cap 28 × 16 with a centre line, dB
/// scale (+6 … −∞) on the left. The slider's value is the fader position
/// (0 … 1); `db()` reads it through the fader law.
class Fader final : public juce::Slider {
public:
    Fader();
    static double proportion_for(double db);
    static double db_for(double proportion);
    double db() const { return db_for(getValue()); }
    void set_db(double db, juce::NotificationType n = juce::sendNotificationSync);
    void paint(juce::Graphics& g) override;
    void mouseDoubleClick(const juce::MouseEvent&) override { set_db(0.0); }

private:
    juce::Rectangle<float> slot() const;
};

// --- Wheel ---------------------------------------------------------------------------------------

/// A ribbed pitch or modulation wheel in a recessed 28 × 84 slot. The pitch
/// wheel springs back to centre when let go; the mod wheel stays put.
class Wheel final : public juce::Slider {
public:
    explicit Wheel(bool springs_back);
    void paint(juce::Graphics& g) override;
    void mouseUp(const juce::MouseEvent& e) override;

private:
    bool springs_ = false;
};

}  // namespace vcc::desktop::hw
