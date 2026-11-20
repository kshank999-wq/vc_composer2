// The built-in inserts as rack-mount outboard gear: VC EQ, VC Compressor,
// VC Gate, VC Limiter, VC Reverb, VC Delay, VC Saturator and VC Outboard.
//
// In a mixer strip each insert slot is drawn as a small faceplate (paint_
// mini_unit). Clicking one opens the strip's rack: every insert stacked
// between rack rails as a full faceplate, with rack ears and screws, a
// power switch (bypass), knobs to turn like the hardware's, and VU meters
// on the dynamics units. The knobs set the same parameters automation lanes
// use; each turn is one undo step. Plug-in inserts sit in the rack as a
// plain unit whose Edit key opens the plug-in's own window.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ui/AppContext.h"

namespace vcc::desktop::console {

/// The unit's brand name: "VC EQ", "VC Compressor"... (plug-ins: their name).
juce::String gear_name(const project::InsertSlot& slot);
/// The unit's faceplate colour and the colour of its legends.
juce::Colour gear_plate(const std::string& kind);
juce::Colour gear_ink(const std::string& kind);

/// One slot drawn as a small rack unit: ears, screw dots, the power LED at
/// the left (in `led`), and the unit's name. `slot` null: an empty bay.
void paint_mini_unit(juce::Graphics& g, juce::Rectangle<float> r, const project::InsertSlot* slot,
                     juce::Rectangle<float>* led = nullptr);

/// Opens (or brings forward) the rack for a track or bus, showing `slot`'s unit.
void show_rack(AppContext& context, const std::string& owner_id, int slot);
/// The document changed: open racks follow it (and close when their owner goes).
void refresh_racks(AppContext& context);
/// Closes every rack (the window is going away).
void close_racks();
/// The open rack window for an owner, or null (the checks).
juce::DocumentWindow* rack_window(const std::string& owner_id);

}  // namespace vcc::desktop::console
