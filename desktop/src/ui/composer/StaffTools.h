// Track-contextual and detachable composition tools (Add-On Features spec
// §8). Docked, a compact bar sits beside the selected staff - above it when
// there is room in the view, otherwise below - and follows the selection;
// it collapses to a tab after a while untouched (or on its key) and comes
// back on the next edit. Detached, the same tools live in a floating window
// whose place is remembered and kept on a connected display.
//
// Every key runs a Composer command on the track being written, so the
// shortcuts, undo and the selection behave exactly as they do from the
// main toolbar. Which keys show adapts to what is selected.
#pragma once

#include <memory>
#include <vector>

#include "ui/AppContext.h"
#include "ui/hw/HwControls.h"
#include "vcc/composer/ShortcutProfile.h"

namespace vcc::desktop::composer_ui {

/// What the tools reflect.
struct StaffToolsState {
    bool editable = false;      ///< A track is being written on.
    bool has_selection = false; ///< Notes are selected (articulations, dynamics...).
    bool pitched = true;        ///< Not a percussion staff (accidentals hide).
    int value = -1;             ///< The note value lit: index into the value keys, -1 none.
    bool dot = false, triplet = false, rest = false;
    int voice = 0;
    bool chord_mode = false;
    juce::String track;         ///< The staff's name, for the header.
};

class StaffTools final : public juce::Component {
public:
    explicit StaffTools(bool detached);

    void set_state(const StaffToolsState& state);
    const StaffToolsState& state() const noexcept { return state_; }
    /// The height the keys need at `width` (they wrap into rows).
    int height_for(int width) const;

    std::function<void(composer::Command)> on_command;
    std::function<void()> on_chord_mode, on_circle, on_detach, on_collapse;

    /// A key by its command (for checks), or null when not shown.
    hw::Keycap* key_for(composer::Command command);
    hw::Keycap& detach_key() { return detach_; }
    hw::Keycap& chord_key() { return chord_; }
    /// How many keys are shown now.
    int visible_keys() const;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    struct Key {
        std::unique_ptr<hw::Keycap> cap;
        composer::Command command = composer::Command::None;
        enum class When { Always, Entry, Selection, Pitched } when = When::Always;
        bool group_start = false;
    };
    hw::Keycap& add(composer::Command c, const juce::String& legend, const char* glyph, const juce::String& tip, Key::When when,
                    bool group_start = false);
    void layout(int width, bool apply, int* height) const;

    bool detached_ = false;
    StaffToolsState state_;
    std::vector<Key> keys_;
    hw::Keycap chord_{"Chord", true}, circle_{"Circle"}, detach_{"Detach"}, collapse_{"-"};
};

}  // namespace vcc::desktop::composer_ui
