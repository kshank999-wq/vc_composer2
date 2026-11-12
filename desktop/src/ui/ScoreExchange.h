// Moving notation in and out of the app (Main Spec §16): MusicXML import and
// export (plain .musicxml/.xml and compressed .mxl) and printed parts as PDF.
// The menus call these; the headless check calls them directly.
#pragma once

#include "ui/AppContext.h"

namespace vcc::desktop {

/// Reads a MusicXML document from a .musicxml/.xml file or a compressed .mxl
/// (its container's root file). False with a reason in `error`.
bool read_musicxml_file(const juce::File& file, std::string& xml, juce::String& error);

/// Imports a MusicXML file into the open project as one undo step: one track
/// per part. `replace` starts the project from the file instead of adding to
/// it. `message` is a summary for the status bar (warnings included).
bool import_musicxml_file(AppContext& context, const juce::File& file, bool replace, juce::String& message);

/// Writes the project as MusicXML; `.mxl` writes the compressed form.
/// `concert_pitch` writes sounding pitch instead of transposed parts.
bool export_musicxml_file(AppContext& context, const juce::File& file, bool concert_pitch, juce::String& message);

/// Prints every part (instrument tracks with notation) to PDF: one file per
/// part in `target` when it is a folder, or every part in one file with a
/// bookmark per part when `target` is a .pdf file. Returns the pages written.
int export_parts_pdf(AppContext& context, const juce::File& target, juce::String& message);

}  // namespace vcc::desktop
