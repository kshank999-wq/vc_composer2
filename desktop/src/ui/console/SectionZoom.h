// A console strip's section blown up in a floating window: click a section's
// title (INPUT, FILTERS, EQUALISER, DYNAMICS, SENDS, OUTPUT) on a strip of
// the full-screen mixer and that section opens three times the size, with the
// same live controls, to work on comfortably; click the title again (on the
// strip or in the window) and it goes back into the mixer. INSERTS opens the
// strip's rack of gear instead (RackGear.h).
#pragma once

#include <string>
#include <vector>

#include "ui/console/ConsoleStrip.h"

namespace vcc::desktop::console {

/// Opens the section's window, or closes it when it is open.
void toggle_section_zoom(AppContext& context, Strip::Kind kind, const std::string& id, Section section,
                         const std::vector<Strip::FxBus>& fx_buses, int number);
/// The open window for a strip's section, or null (the checks).
juce::DocumentWindow* section_zoom_window(const std::string& id, Section section);
/// Closes every section window (the console is going away).
void close_section_zooms();

}  // namespace vcc::desktop::console
