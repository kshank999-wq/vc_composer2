// The console's look: panel paint, silk-screen printing, knob cap colours,
// LED colours, the fader and meter laws and value formatting. Everything is
// original artwork drawn in code.
#pragma once

#include <string>

#include <juce_gui_basics/juce_gui_basics.h>

namespace vcc::desktop::console {

// --- Palette ---------------------------------------------------------------------------------

namespace paint {
inline const juce::Colour backdrop{0xff0d0e10};      ///< Behind the strips (the console frame).
inline const juce::Colour track_panel{0xff2a2d32};   ///< Graphite channel paint.
inline const juce::Colour group_panel{0xff233040};   ///< Blue-steel group strips.
inline const juce::Colour fx_panel{0xff302638};      ///< Aubergine aux returns.
inline const juce::Colour master_panel{0xff1e1f22};  ///< Black anodised master.
inline const juce::Colour print{0xffd8dbe0};         ///< Silk-screen white.
inline const juce::Colour print_dim{0xff8d939c};
inline const juce::Colour rule{0x30ffffff};          ///< Printed section lines.
inline const juce::Colour well{0xff0b0c0e};          ///< Recessed slots and windows.
inline const juce::Colour lcd_back{0xff081014};
inline const juce::Colour lcd_text{0xff8fe3ff};
inline const juce::Colour selected{0xffffb547};

// Knob caps, by section.
inline const juce::Colour cap_grey{0xff9aa0a8};
inline const juce::Colour cap_filter{0xff6f7782};
inline const juce::Colour cap_hf{0xffd8434f};
inline const juce::Colour cap_hmf{0xff3fae5d};
inline const juce::Colour cap_lmf{0xff3a78d0};
inline const juce::Colour cap_lf{0xff5e4632};
inline const juce::Colour cap_comp{0xffe8bf2e};
inline const juce::Colour cap_gate{0xffe57b2c};
inline const juce::Colour cap_send{0xffa070d8};
inline const juce::Colour cap_pan{0xffe9ebee};

// Lamp colours.
inline const juce::Colour led_green{0xff35e06a};
inline const juce::Colour led_yellow{0xfff2d230};
inline const juce::Colour led_amber{0xffffa12a};
inline const juce::Colour led_red{0xffff3b30};
inline const juce::Colour led_blue{0xff46a8ff};
inline const juce::Colour led_white{0xfff4f4ee};
}  // namespace paint

// --- Laws ------------------------------------------------------------------------------------

constexpr double kMinDb = -96.0;  ///< Fader bottom: off.
constexpr double kMaxDb = 12.0;

/// Console fader law (0 at the bottom, 1 at the top), piecewise like a printed scale.
double fader_proportion(double db);
double fader_db(double proportion);
/// Meter law for dBFS (-60 .. 0), linear in segments like a hardware ladder.
double meter_proportion(double dbfs);

// --- Text -------------------------------------------------------------------------------------

juce::String db_text(double db, bool unit = true);
juce::String freq_text(double hz);
juce::String pan_text(double pan);
juce::String ms_text(double ms);

// --- Drawing ---------------------------------------------------------------------------------

juce::Font print_font(float size, bool bold = true);
/// Paints a brushed-metal panel in `base` over `area`.
void fill_panel(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour base);
/// The same, from a shared cache: strips of one kind and size share one image.
void fill_panel_cached(juce::Graphics& g, juce::Rectangle<int> area, juce::Colour base);
/// Draws `image` (rendered at `scale` x the logical size) at `topLeft`.
void draw_scaled(juce::Graphics& g, const juce::Image& image, juce::Point<float> top_left, float scale);
/// The physical pixel scale of a graphics context (2 on a retina screen).
float pixel_scale(juce::Graphics& g);
/// Silk-screen text.
void print_text(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, float size,
                juce::Justification just = juce::Justification::centred, juce::Colour colour = paint::print);
/// A printed section heading with rules either side.
void print_heading(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, juce::Colour colour);
/// A recessed window (meters, displays).
void draw_well(juce::Graphics& g, juce::Rectangle<float> area, float corner = 2.0f);

// --- Interaction -----------------------------------------------------------------------------

/// Each mouse gesture gets a new parity so two drags of the same control are
/// two undo steps, while one drag stays one (Session coalesces equal labels).
void begin_gesture();
std::string gesture_suffix();

/// Implemented by the console view: a value bubble while dragging a control.
class TipHost {
public:
    virtual ~TipHost() = default;
    virtual void show_tip(juce::Component& source, const juce::String& text) = 0;
    virtual void hide_tip() = 0;
};
void show_tip(juce::Component& source, const juce::String& text);
void hide_tip(juce::Component& source);

}  // namespace vcc::desktop::console
