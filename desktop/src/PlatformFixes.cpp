#include "PlatformFixes.h"

#if defined(__linux__)
#include <X11/Xlib.h>
#endif

namespace vcc::desktop {

bool prepare_display()
{
#if defined(__linux__)
    static Display* display = XOpenDisplay(nullptr);
    if (display != nullptr) {
        const char* names[] = {"WM_PROTOCOLS", "WM_TAKE_FOCUS", "WM_DELETE_WINDOW", "_NET_WM_PING", "WM_CHANGE_STATE",
                               "WM_STATE", "_NET_WM_WINDOW_TYPE", "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN",
                               "_NET_WM_STATE_FULLSCREEN", "_NET_WM_MOVERESIZE", "_NET_FRAME_EXTENTS", "_NET_WORKAREA",
                               "_MOTIF_WM_HINTS", "_NET_WM_ALLOWED_ACTIONS", "_NET_WM_WINDOW_TYPE_NORMAL",
                               "_NET_WM_WINDOW_TYPE_COMBO"};
        for (const char* n : names) XInternAtom(display, n, False);
        XSync(display, False);
    }
#endif
    return true;
}

}  // namespace vcc::desktop
