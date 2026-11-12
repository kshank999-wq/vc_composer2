// The console's hardware: knobs, illuminated push-buttons, long-throw faders,
// LED ladder meters, the gain-reduction meter, displays, the EQ curve window,
// the insert rack and the scribble strip. All drawn in code.
#pragma once

#include <array>
#include <functional>
#include <vector>

#include "ui/console/ConsoleStyle.h"

namespace vcc::desktop::console {

// --- Knob ------------------------------------------------------------------------------------

class Knob final : public juce::Component, public juce::SettableTooltipClient {
public:
    enum class Law { Linear, Log, Fader };
    struct Spec {
        double min = 0.0, max = 1.0, def = 0.0;
        Law law = Law::Linear;
        bool bipolar = false;
        juce::Colour cap = paint::cap_grey;
        std::function<juce::String(double)> text;
        bool small = false;  ///< Smaller cap (Q, freq).
    };

    explicit Knob(Spec spec);

    /// Called while the user turns the knob (and on reset).
    std::function<void(double)> on_change;
    /// Called once at the start of a turn (before the first on_change).
    std::function<void()> on_start;
    /// Right-click.
    std::function<void()> on_menu;

    void set_value(double v);
    double value() const noexcept { return value_; }
    void set_dim(bool dim);
    const Spec& spec() const noexcept { return spec_; }
    double to_norm(double v) const;
    double from_norm(double n) const;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;

private:
    void change_to(double v);
    Spec spec_;
    double value_ = 0.0;
    double drag_norm_ = 0.0;
    juce::Point<float> last_;
    bool dragging_ = false;
    bool dim_ = false;
};

// --- Illuminated push-button -------------------------------------------------------------------

class LedButton final : public juce::Button {
public:
    LedButton(const juce::String& label, juce::Colour lamp, const juce::String& tooltip = {});
    void set_lit(bool on);
    void set_lamp(juce::Colour lamp);
    void paintButton(juce::Graphics& g, bool over, bool down) override;

private:
    juce::Colour lamp_;
};

// --- Fader -------------------------------------------------------------------------------------

class Fader final : public juce::Component, public juce::SettableTooltipClient {
public:
    Fader();
    std::function<void(double)> on_change;
    void set_value(double db);
    double value() const noexcept { return value_; }
    void set_cap_colour(juce::Colour c);
    /// The y (in this component) of a dB value on the travel.
    float y_for(double db) const;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;

private:
    float cap_height() const;
    void change_to(double db);
    double value_ = 0.0;
    double drag_prop_ = 0.0;
    float last_y_ = 0.0f;
    bool dragging_ = false;
    juce::Colour cap_ = juce::Colour(0xff3a3d42);
};

// --- LED ladder meter --------------------------------------------------------------------------

class LedMeter final : public juce::Component, public juce::SettableTooltipClient {
public:
    explicit LedMeter(int channels, bool large = false);
    /// One meter tick: linear peaks since the last tick.
    void push(float left, float right, bool clipped);
    void reset();
    /// The y (in this component) of a dBFS level on the ladder, for printed scales.
    float y_for(double dbfs) const;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent&) override;
    void resized() override;

private:
    struct Channel {
        double level = -200.0, hold = -200.0;
        int hold_ticks = 0;
        int lit = 0, hold_seg = -1;
    };
    int segments() const;
    int channels_;
    bool large_;
    std::array<Channel, 2> ch_{};
    int clip_ticks_ = 0;
};

// --- Gain-reduction meter --------------------------------------------------------------------

class GrMeter final : public juce::Component, public juce::SettableTooltipClient {
public:
    GrMeter();
    void set(float gr_db, bool gate_closed);
    void paint(juce::Graphics& g) override;

private:
    int lit_ = 0;
    bool gate_ = false;
};

// --- Display / selector ------------------------------------------------------------------------

/// A backlit display window; clickable when `on_click` is set.
class Lcd final : public juce::Component, public juce::SettableTooltipClient {
public:
    std::function<void()> on_click;
    void set(const juce::String& text, juce::Colour text_colour = paint::lcd_text, bool menu_arrow = true);
    const juce::String& text() const noexcept { return text_; }
    void set_dot(juce::Colour c) { dot_ = c; repaint(); }
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;

private:
    juce::String text_;
    juce::Colour colour_ = paint::lcd_text;
    juce::Colour dot_ = juce::Colours::transparentBlack;
    bool arrow_ = true;
};

// --- EQ curve window -----------------------------------------------------------------------------

struct EqCurveParams {
    bool eq_on = false, hpf_on = false, lpf_on = false;
    double hpf = 80.0, lpf = 12000.0;
    struct Band { double f = 1000.0, g = 0.0, q = 1.0; bool bell = true; };
    std::array<Band, 4> bands{};  ///< LF, LMF, HMF, HF
    bool operator==(const EqCurveParams& o) const;
};

class EqCurve final : public juce::Component {
public:
    void set(const EqCurveParams& p);
    void paint(juce::Graphics& g) override;

private:
    double response_db(double hz) const;
    EqCurveParams p_;
    bool valid_ = false;
};

// --- Insert rack -------------------------------------------------------------------------------

class InsertRack final : public juce::Component, public juce::SettableTooltipClient {
public:
    struct Slot {
        juce::String name;
        bool bypass = false;
        bool suggestion = false;
        bool used = false;
        bool operator==(const Slot& o) const
        {
            return name == o.name && bypass == o.bypass && suggestion == o.suggestion && used == o.used;
        }
    };
    static constexpr int kSlots = 5;
    /// Clicked slot: index, whether on the bypass lamp.
    std::function<void(int, bool)> on_click;
    void set(std::vector<Slot> slots);
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    juce::Rectangle<float> slot_bounds(int i) const;

private:
    std::vector<Slot> slots_;
};

// --- Scribble strip ----------------------------------------------------------------------------

class Scribble final : public juce::Component, public juce::SettableTooltipClient {
public:
    std::function<void(const juce::MouseEvent&)> on_mouse_down;
    void set(const juce::String& name, juce::Colour colour, const juce::String& number, const juce::String& badge,
             bool dim);
    void set_selected(bool s);
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;

private:
    juce::String name_, number_, badge_;
    juce::Colour colour_;
    bool dim_ = false, selected_ = false;
};

}  // namespace vcc::desktop::console
