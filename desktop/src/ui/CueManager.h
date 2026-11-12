// Cue Manager (Main Spec §14): the production's cues as a dashboard - status,
// reels and scenes, spotting against picture, deadlines, director notes,
// deliverables - with cue sheet / spotting-note exports and render-by-cue.
//
// A non-modal tool window (single instance, titled "Cue Manager"). It reads
// the document through Session and changes it only through Session, one undo
// step per commit, and follows every document change while it is open.
#pragma once

#include "ui/AppContext.h"

namespace vcc::desktop {

/// Opens the Cue Manager, or brings the open one to the front.
void show_cue_manager(AppContext& context);

}  // namespace vcc::desktop
