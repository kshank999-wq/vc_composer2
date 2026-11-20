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
    bool writing = false;       ///< Write (the quill) or Select (the arrow).
    int accent = 50;            ///< The track's accent strength, 1-100.
    int hairpin = 0;            ///< Drawing a crescendo (1) or diminuendo (2) with the mouse.
    int cresc_level = 20;       ///< Where a new crescendo starts (expression, 0-100).
    int dim_level = 20;         ///< Where a new diminuendo ends.
    juce::String track;         ///< The staff's name, for the header.
};

/// A key whose face is a drawn icon: the selection arrow or the quill.
class IconKey final : public hw::Keycap {
public:
    enum class Icon { Arrow, Quill };
    explicit IconKey(Icon icon) : hw::Keycap("", true), icon_(icon) {}
    /// The icon's outline, fitted to `area` (the checks and the menus use it too).
    static juce::Path path(Icon icon, juce::Rectangle<float> area);
    /// The quill as a mouse pointer, its nib the hot spot.
    static const juce::MouseCursor& quill_cursor();

protected:
    void paintButton(juce::Graphics& g, bool highlighted, bool down) override;

private:
    Icon icon_;
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
    /// Docked: its title strip dragged away - the tools float in their own
    /// window where it is let go (another screen too). The point is on screen.
    std::function<void(juce::Point<int>)> on_drag_out;
    /// Select (false) or Write (true) chosen on the mode keys.
    std::function<void(bool)> on_mode;
    /// The accent dial turned (1-100).
    std::function<void(int)> on_accent;
    /// A hairpin's quiet level changed: (1 crescendo, 2 diminuendo, value).
    std::function<void(int, int)> on_hairpin_level;
    juce::Slider& hairpin_level(int kind) { return kind == 1 ? cresc_level_ : dim_level_; }

    /// A key by its command (for checks), or null when not shown.
    hw::Keycap* key_for(composer::Command command);
    hw::Keycap& detach_key() { return detach_; }
    hw::Keycap& chord_key() { return chord_; }
    hw::Keycap& select_key() { return select_; }
    hw::Keycap& write_key() { return write_; }
    hw::SmallKnob& accent_knob() { return accent_; }
    /// How many keys are shown now.
    int visible_keys() const;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;

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
    bool dragging_out_ = false;
    juce::Point<int> drag_from_;
    StaffToolsState state_;
    std::vector<Key> keys_;
    hw::Keycap chord_{"Chord", true}, circle_{"Circle"}, detach_{"Detach"}, collapse_{"-"};
    IconKey select_{IconKey::Icon::Arrow}, write_{IconKey::Icon::Quill};
    hw::SmallKnob accent_;
    juce::Label accent_label_;
    juce::Slider cresc_level_, dim_level_;  ///< The numbers under < and >.
};

}  // namespace vcc::desktop::composer_ui
