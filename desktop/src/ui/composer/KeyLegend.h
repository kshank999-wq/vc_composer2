// The short legend a key prints for its shortcut ("5", ".", "Alt S"), read
// from the active shortcut profile so the keys follow a profile change.
#pragma once

#include "vcc/composer/ShortcutProfile.h"

#include <juce_core/juce_core.h>

namespace vcc::desktop::composer_ui {

inline juce::String key_legend(const composer::ShortcutProfile& profile, composer::Command command)
{
    const auto stroke = profile.stroke_for(command);
    if (!stroke.is_bound()) return {};
    juce::String text(stroke.to_text());
    text = text.replace("Period", ".").replace("Slash", "/").replace("Minus", "-").replace("Equals", "=");
    text = text.replace("Ctrl+", "^").replace("Cmd+", "Cmd ").replace("Alt+", "Alt ").replace("Shift+", "Sh ");
    return text;
}

}  // namespace vcc::desktop::composer_ui
