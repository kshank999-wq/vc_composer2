#include "ui/Theme.h"

namespace vcc::desktop {

juce::Colour colour_from_hex(const std::string& hex, juce::Colour fallback)
{
    if (hex.size() != 7 || hex[0] != '#') return fallback;
    return juce::Colour::fromString("ff" + juce::String(hex.substr(1)));
}

Theme::Theme()
{
    using namespace colours;
    auto scheme = juce::LookAndFeel_V4::getDarkColourScheme();
    scheme.setUIColour(ColourScheme::windowBackground, background);
    scheme.setUIColour(ColourScheme::widgetBackground, panel_raised);
    scheme.setUIColour(ColourScheme::menuBackground, panel);
    scheme.setUIColour(ColourScheme::outline, outline);
    scheme.setUIColour(ColourScheme::defaultText, text);
    scheme.setUIColour(ColourScheme::defaultFill, accent);
    scheme.setUIColour(ColourScheme::highlightedText, juce::Colours::white);
    scheme.setUIColour(ColourScheme::highlightedFill, accent);
    scheme.setUIColour(ColourScheme::menuText, text);
    setColourScheme(scheme);

    setColour(juce::ResizableWindow::backgroundColourId, background);
    setColour(juce::DocumentWindow::backgroundColourId, background);
    setColour(juce::TextButton::buttonColourId, panel_raised);
    setColour(juce::TextButton::buttonOnColourId, accent);
    setColour(juce::TextButton::textColourOffId, text);
    setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    setColour(juce::ComboBox::backgroundColourId, panel_raised);
    setColour(juce::ComboBox::outlineColourId, outline);
    setColour(juce::TextEditor::backgroundColourId, panel_raised);
    setColour(juce::TextEditor::outlineColourId, outline);
    setColour(juce::ListBox::backgroundColourId, panel);
    setColour(juce::Label::textColourId, text);
    setColour(juce::Slider::thumbColourId, accent);
    setColour(juce::Slider::trackColourId, outline);
    setColour(juce::Slider::backgroundColourId, panel_raised);
    setColour(juce::TabbedButtonBar::tabOutlineColourId, outline);
    setColour(juce::ScrollBar::thumbColourId, outline.brighter(0.2f));
    setColour(juce::PopupMenu::backgroundColourId, panel);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, accent);
}

void Theme::drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour& base, bool highlighted, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced(0.5f);
    auto c = base;
    if (down) c = c.brighter(0.15f);
    else if (highlighted) c = c.brighter(0.08f);
    g.setColour(c);
    g.fillRoundedRectangle(r, 4.0f);
    g.setColour(colours::outline);
    g.drawRoundedRectangle(r, 4.0f, 1.0f);
}

juce::Font Theme::getTextButtonFont(juce::TextButton&, int height)
{
    return juce::Font(juce::FontOptions(juce::jmin(14.0f, static_cast<float>(height) * 0.55f)));
}

}  // namespace vcc::desktop
