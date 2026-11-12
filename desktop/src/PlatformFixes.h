// Platform workarounds that must run before JUCE starts.
#pragma once

namespace vcc::desktop {

/// Linux/X11: JUCE looks up window-manager atoms with "only if exists" and
/// then uses them unchecked, so on an X server with no window manager yet
/// (a kiosk session, a CI virtual display) the first window crashes the
/// process with BadAtom. Creating those atoms first makes that impossible.
/// The connection is kept open: an X server forgets atoms when its last
/// client disconnects. No-op elsewhere. Returns true.
bool prepare_display();

}  // namespace vcc::desktop
