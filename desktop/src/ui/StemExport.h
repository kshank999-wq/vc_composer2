// The stem export window (Main Spec §16): define the stems (which tracks and
// buses each prints, wet or dry), check that every track is in exactly one,
// and print the mix and stems for the whole project or every cue.
#pragma once

#include "ui/AppContext.h"

namespace vcc::desktop {

/// Opens (or brings forward) the window titled "Export Stems".
void show_stem_export(AppContext& context);

}  // namespace vcc::desktop
