// View layouts (View > Layouts): how the main window is arranged, saved by
// name and recalled in one step, and the one it opens with.
//
// A layout records the view (Composer or Producer, Track or Page), what the
// bottom deck holds (keyboard or mixer) and whether it is hidden, the Work
// view (side panels hidden, the edit window as large as it gets), the mixer
// full screen, and the console window with the display it sits on (a touch
// screen beside the main monitor, say). Four come built in - Compose,
// Produce, Mix and Work - and composers save their own. Preferences > Open
// With chooses the layout the app opens with, or the one it was left in.
#pragma once

#include <optional>
#include <vector>

#include <juce_graphics/juce_graphics.h>

namespace vcc::desktop::layouts {

/// A floating window in a layout: open or not, and where (any display).
struct FloatingWindow {
    bool shown = false;
    juce::Rectangle<int> bounds;  ///< Empty: where it opens by default.
};

struct ViewLayout {
    juce::String name;
    bool producer = false;
    bool page_view = false;
    bool bottom_mixer = true;   ///< Producer: the bottom deck holds the mixer (else the keyboard).
    bool dock_hidden = false;   ///< The bottom deck (keyboard or mixer) is hidden.
    bool work_view = false;     ///< The side panels are hidden.
    bool mixer_full = false;    ///< The mixer fills the window.
    bool console_window = false;  ///< The console in a window of its own...
    juce::String console_display;  ///< ...on this display (its name), full screen there; empty: wherever it was.
    /// The floating windows: Window > Inspector, the staff tools (detached)
    /// and the track list, each open or closed and where. A built-in layout
    /// (and one saved before these were recorded) leaves them as they are.
    bool has_windows = false;
    FloatingWindow inspector, staff_tools, track_list;

    bool operator==(const ViewLayout& o) const;
    bool operator!=(const ViewLayout& o) const { return !(*this == o); }
};

juce::var to_var(const ViewLayout& layout);
ViewLayout from_var(const juce::var& v);

/// Compose, Produce, Mix and Work.
std::vector<ViewLayout> built_in();
/// The composer's own, in the order saved.
std::vector<ViewLayout> saved();
/// Saves (or replaces, by name) a layout of the composer's own.
void save(const ViewLayout& layout);
/// Removes one of the composer's own; built-ins stay.
bool remove(const juce::String& name);
/// A built-in or saved layout by name (saved ones win).
std::optional<ViewLayout> find(const juce::String& name);

/// The layout the app opens with; empty means the one it was left in.
juce::String startup();
void set_startup(const juce::String& name);
/// The arrangement the app was left in (recorded on quit).
std::optional<ViewLayout> last();
void set_last(const ViewLayout& layout);

/// What the app opens with: the startup layout, else the last one.
std::optional<ViewLayout> to_open_with();

}  // namespace vcc::desktop::layouts
