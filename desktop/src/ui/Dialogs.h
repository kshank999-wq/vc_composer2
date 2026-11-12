// The composer's tool windows. Each opens non-modally, works on the current
// document through Session, and auditions through the engine's preview
// path, which never writes to the project.
#pragma once

#include "ui/AppContext.h"

namespace vcc::desktop {

/// Template browser (Supplement §3, §11): search, filters, preview, then
/// Create (new project) or Add to Existing (merge).
/// `for_new_project` chooses which action is the default.
void show_template_browser(AppContext& context, bool for_new_project);

/// Chord Explorer / Progression Builder (Supplement §9).
void show_chord_explorer(AppContext& context);

/// Ostinato Creator (Supplement §10).
void show_ostinato_creator(AppContext& context);

}  // namespace vcc::desktop
