// Help > Credits: who made the open-source sounds, code and fonts inside VC
// Composer, under which licence, and the trademark notices - by category
// (Sounds, Software, Fonts, Trademarks). Notices only: no third-party logos.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace vcc::desktop {

struct CreditEntry {
    juce::String title;    ///< "Salamander Grand Piano"
    juce::String by;       ///< "Alexander Holm"
    juce::String licence;  ///< "Creative Commons Attribution 3.0"
    juce::String text;     ///< what it is used for, changes made, the notice to reproduce
};

struct CreditCategory {
    juce::String name;  ///< "Sounds", "Software", "Fonts", "Trademarks"
    std::vector<CreditEntry> entries;
};

/// Everything Help > Credits shows (the checks read it too).
std::vector<CreditCategory> credits();
/// One category as plain text.
juce::String credits_text(const CreditCategory& category);

/// Help > Credits...
void show_credits_window();
/// The open Credits panel's category tabs (the checks), or null.
juce::TabbedComponent* credits_tabs();

}  // namespace vcc::desktop
