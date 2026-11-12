// Content and fonts embedded in the application binary, so an installed copy
// can never lose its templates, instruments or music font.
#pragma once

#include <memory>

#include "vcc/engraving/LayoutEngine.h"
#include "vcc/session/Session.h"
#include "vcc/ui/MusicFont.h"

namespace vcc::desktop {

/// Every content pack compiled into the binary, plus the user's own
/// templates folder (custom templates saved from the app).
std::unique_ptr<session::Catalogs> load_embedded_catalogs();

/// Bravura (SIL OFL) and its SMuFL glyph table, loaded once.
struct MusicFontResources {
    ui::MusicFont font;
    engraving::GlyphTable glyphs;
    bool loaded = false;
};
MusicFontResources& music_font();

/// Where the user's own files live: custom templates, autosaves.
juce::File user_data_directory();

}  // namespace vcc::desktop
