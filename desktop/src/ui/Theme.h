// The look of VC Composer 2: a dark studio theme around a light "paper"
// notation surface, with family colours carried from the template content.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace vcc::desktop {

namespace colours {
inline const juce::Colour background{0xff1e2126};
inline const juce::Colour panel{0xff262a31};
inline const juce::Colour panel_raised{0xff2f343c};
inline const juce::Colour outline{0xff3a404a};
inline const juce::Colour text{0xffe6e8eb};
inline const juce::Colour text_dim{0xff9aa3ad};
inline const juce::Colour accent{0xff4f9de8};
inline const juce::Colour accent_warm{0xffe8a33d};
inline const juce::Colour selection{0x334f9de8};
inline const juce::Colour paper{0xfffaf9f6};
inline const juce::Colour playhead{0xffe85d4f};
inline const juce::Colour mute{0xffd9a441};
inline const juce::Colour solo{0xff4fbf73};
inline const juce::Colour exact{0xff4fbf73};
inline const juce::Colour fallback{0xffe8c33d};
inline const juce::Colour placeholder{0xff8c96a3};
inline const juce::Colour missing{0xffe85d4f};
}  // namespace colours

/// A track's colour from its "#rrggbb" string.
juce::Colour colour_from_hex(const std::string& hex, juce::Colour fallback = colours::text_dim);

class Theme final : public juce::LookAndFeel_V4 {
public:
    Theme();
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    juce::Font getTextButtonFont(juce::TextButton&, int height) override;
};

}  // namespace vcc::desktop
