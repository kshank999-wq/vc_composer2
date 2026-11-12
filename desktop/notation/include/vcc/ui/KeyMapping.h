// Copyright (c) VC Composer. All rights reserved.
//
// JUCE key presses to the shortcut model (spec 02 sections 5.2, 11).
//
// The shortcut profiles are framework-independent by design: they are user
// data, they are written to a settings file, and they have to mean the same
// thing whichever UI toolkit is in front of them. So they carry their own
// key model, and this is the one place that translates a toolkit's key
// press into it.
//
// Keeping the translation in a function of its own, rather than inside a
// component's keyPressed, is what makes it testable: a key press can be
// constructed and converted with no window and no keyboard.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "vcc/composer/ShortcutProfile.h"
#include "vcc/notation/Pitch.h"

namespace vcc::ui {

/// Converts a JUCE key press.
///
/// Returns false for a key the shortcut model has no name for - a media key,
/// a dead key - which is not an error: an unmapped key simply triggers
/// nothing.
bool key_stroke_from(const juce::KeyPress& press, composer::KeyStroke& out);

/// The modifiers alone, for a caller that has them without a key press.
composer::ModifierMask modifiers_from(const juce::ModifierKeys& modifiers);

/// The step a letter key names, for pitch entry: A through G.
/// Returns false for anything else.
bool step_from_key(const composer::KeyStroke& stroke, notation::Step& out);

}  // namespace vcc::ui
