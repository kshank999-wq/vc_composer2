// Content and fonts embedded in the application binary, so an installed copy
// can never lose its templates, instruments or music font.
#pragma once

#include <memory>

#include <juce_graphics/juce_graphics.h>

#include "vcc/engraving/LayoutEngine.h"
#include "vcc/session/Session.h"
#include "vcc/ui/MusicFont.h"

namespace vcc::desktop {

/// Every content pack compiled into the binary, plus the user's own
/// templates folder (custom templates saved from the app).
std::unique_ptr<session::Catalogs> load_embedded_catalogs();
/// content/library/providers.json, the library browser's catalog (Main Spec §10).
std::string embedded_library_catalog();

/// Bravura (SIL OFL) and its SMuFL glyph table, loaded once.
struct MusicFontResources {
    ui::MusicFont font;
    engraving::GlyphTable glyphs;
    bool loaded = false;
};
MusicFontResources& music_font();

/// Where the user's own files live: custom templates, autosaves.
juce::File user_data_directory();

/// The VC Composer logo (brand/logo, cut by brand/make-assets.py): the
/// horizontal one for the window's corner, the stacked one for the about box.
juce::Image brand_logo_horizontal();
juce::Image brand_logo_stacked();

}  // namespace vcc::desktop
