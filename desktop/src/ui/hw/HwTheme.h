// The hardware look of the Edit window (docs/spec/ui/SPEC.md, "Design
// tokens"): anodised panels, raised keycaps with LEDs, backlit LCDs.
//
// Fixed colours are constants. The LED colour, the LCD colour and the staff
// paper (Dark or Paper) are themeable at run time: every control reads them
// when it paints, and changing one repaints every window, with no reload.
// The choice is remembered between runs.
#pragma once

#include <array>

#include <juce_gui_basics/juce_gui_basics.h>

namespace vcc::desktop::hw {

// --- Fixed tokens -------------------------------------------------------------------------------

namespace tok {
inline const juce::Colour chassis{0xff0c0d0f};
inline const juce::Colour panel_top{0xff23272c};
inline const juce::Colour panel_bottom{0xff1a1d21};
inline const juce::Colour edge{0xff08090a};
inline const juce::Colour silk{0xff8e98a3};  ///< Silk-screen labels.
inline const juce::Colour text{0xffd3d9e0};
inline const juce::Colour text_bright{0xffe7ebf0};
inline const juce::Colour lcd_bg{0xff06100d};
inline const juce::Colour led_off{0xff2b2e32};
inline const juce::Colour meter_off{0xff24282d};
inline const juce::Colour meter_green{0xff3ddc84};
inline const juce::Colour meter_amber{0xffffc24a};
inline const juce::Colour meter_red{0xffff5a4e};
inline const juce::Colour marker{0xfff5d547};
inline const juce::Colour rec{0xffff5a4e};
inline const juce::Colour solo{0xffffc24a};
inline const juce::Colour mute{0xff4fd1ff};
inline const juce::Colour input_monitor{0xff3ddc84};
inline const juce::Colour key_top{0xff3c4148};
inline const juce::Colour key_bottom{0xff2a2e34};
inline const juce::Colour key_on_top{0xff4a4f57};
inline const juce::Colour key_on_bottom{0xff363b42};
// Track-type tags (Producer view).
inline const juce::Colour type_instrument{0xffc9a2ff};
inline const juce::Colour type_midi{0xff4fd1ff};
inline const juce::Colour type_audio{0xff3ddc84};
inline const juce::Colour type_aux{0xfff5d547};

// Shapes and spacing, in px.
constexpr float r_panel = 8.0f;
constexpr float r_key = 5.0f;
constexpr float r_lcd = 4.0f;
constexpr int gap_panel = 10;
constexpr int pad_page = 14;
}  // namespace tok

// --- Runtime theme ------------------------------------------------------------------------------

/// The themeable colours, with the spec's choices.
class Theme final : public juce::ChangeBroadcaster {
public:
    static Theme& get();

    struct Choice {
        const char* name;
        juce::Colour colour;
    };
    static const std::array<Choice, 4>& led_choices();  ///< Amber (default), red, cyan, green.
    static const std::array<Choice, 4>& lcd_choices();  ///< Cyan (default), green, amber, blue.

    juce::Colour led() const noexcept { return led_; }
    juce::Colour led_glow() const noexcept { return led_.withAlpha(0.6f); }
    juce::Colour lcd() const noexcept { return lcd_; }
    juce::Colour lcd_glow() const noexcept { return lcd_.withAlpha(0.55f); }
    bool paper() const noexcept { return paper_; }
    juce::Colour staff_paper() const noexcept;  ///< Lane background.
    juce::Colour ink() const noexcept;           ///< Note heads, stems, clefs, dynamics.
    juce::Colour staff_line() const noexcept;    ///< Staff lines and barlines.
    juce::Colour grid() const noexcept;          ///< Beat and bar grid in lanes.

    /// Each change repaints every window and is remembered.
    void set_led(juce::Colour c);
    void set_lcd(juce::Colour c);
    void set_paper(bool paper);

    /// Where the choice is remembered (the app's UI settings file). Loads it.
    void attach(juce::PropertiesFile* settings);

private:
    Theme() = default;
    void changed();

    juce::Colour led_{0xffffb547};
    juce::Colour lcd_{0xff6ee7d2};
    bool paper_ = false;
    juce::PropertiesFile* settings_ = nullptr;
};

/// The UI settings file: theme and other per-user display choices.
juce::PropertiesFile& ui_settings();

// --- Fonts --------------------------------------------------------------------------------------

namespace fonts {
/// Barlow Condensed: silk-screen labels and keycap legends, upper case,
/// spaced (`tracking` em). Semibold, or bold for the brand.
juce::Font silk(float size, float tracking = 0.14f, bool bold = false);
/// Share Tech Mono: LCD readouts.
juce::Font lcd(float size);
/// Inter: body text. Regular or semibold.
juce::Font ui(float size, bool semibold = false);
/// Dynamics: a bold italic serif.
juce::Font dynamics(float size);
/// True when the embedded faces loaded.
bool loaded();
}  // namespace fonts

}  // namespace vcc::desktop::hw
