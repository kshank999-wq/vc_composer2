// The Edit window's frames (docs/spec/ui/SPEC.md, "Layout"): the toolbar
// band, the edit window with its stage bar, the left rack's panels, the
// right panel and the status strip. Built from the hardware controls
// (ui/hw). The views inside them are the app's existing Composer and
// Sequencer (now Producer) views, restyled in later steps.
#pragma once

#include <memory>
#include <vector>

#include "ui/AppContext.h"
#include "ui/hw/HwControls.h"
#include "vcc/composer/ShortcutProfile.h"
#include "vcc/notation/MusicalTime.h"
#include "vcc/project/RenderCache.h"
#include "ui/console/MixActions.h"
#include "ui/shell/MixControls.h"

namespace vcc::desktop::shell {

// --- Toolbar -----------------------------------------------------------------------------------

/// Groups of keys under silk labels, divided by grooves. One set per view.
class Toolbar final : public juce::Component {
public:
    explicit Toolbar(AppContext& context);
    void set_producer(bool producer);
    /// Refreshes the keys that mirror state (PDC, punch, pencil).
    void update(bool punch, bool pencil);
    std::function<void(bool larger)> on_zoom;
    std::function<void(bool on)> on_pencil;
    /// A Composer key (note entry, value, dot, accidental) ran a notation command.
    std::function<void(composer::Command)> on_command;
    /// Lights the Composer keys from the editor: entry on, the value it
    /// enters, dot, triplet, rest.
    void set_entry_state(bool entry, const notation::Duration& value, bool rest);
    /// Re-reads the keys' number legends from the active shortcut profile.
    void refresh_legends();
    /// The Composer key for a command, or null.
    hw::Keycap* command_key(composer::Command c);

    void resized() override;
    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;

    hw::Keycap* key(const juce::String& legend);

private:
    struct Group {
        explicit Group(juce::String l) : label(std::move(l)) {}
        juce::String label;
        std::vector<hw::Keycap*> keys;
        std::vector<int> widths;
        bool producer = false;
    };
    hw::Keycap& add(Group& g, const juce::String& legend, int width, bool toggles = false);
    AppContext& context_;
    bool producer_ = false;
    std::vector<std::unique_ptr<hw::Keycap>> keys_;
    std::vector<Group> groups_;
    std::vector<std::pair<hw::Keycap*, composer::Command>> command_keys_;
};

// --- Edit window frame ---------------------------------------------------------------------

/// The edit window: a title bar with the session name and the stage bar
/// (Compose › Score › Mix › Master), around the current view.
class EditFrame final : public juce::Component {
public:
    explicit EditFrame(AppContext& context);
    ~EditFrame() override;
    /// The stage lit: 0 Compose, 1 Score, 2 Mix, 3 Master.
    void set_stage(int stage);
    std::function<void(int stage)> on_stage;
    /// The area inside the frame for the view.
    juce::Rectangle<int> content_area() const;

    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    void resized() override;
    hw::Keycap& stage_key(int i) { return *stages_[static_cast<std::size_t>(i)]; }

private:
    AppContext& context_;
    int listener_ = 0;
    std::vector<std::unique_ptr<hw::Keycap>> stages_;
    int stage_ = 0;
};

// --- Left rack ----------------------------------------------------------------------------------

/// Markers: the project's cues and hit points, in time order; click to
/// locate the playhead there.
class MarkersPanel final : public hw::Panel {
public:
    explicit MarkersPanel(AppContext& context);
    ~MarkersPanel() override;
    void paintOverChildren(juce::Graphics& g) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    int rows() const noexcept { return static_cast<int>(items_.size()); }
    /// Locates to row `i` (as a click does).
    void locate(int i);

private:
    struct Item {
        juce::String name, where;
        Ticks tick = 0;
    };
    void refresh();
    int row_at(juce::Point<int> p) const;
    AppContext& context_;
    int listener_ = 0;
    std::vector<Item> items_;
    int hover_ = -1;
};

/// Clips (Producer view): every audio clip in the project, on audio tracks
/// and on instrument tracks' own audio lanes; click to select one and go to it.
class ClipsPanel final : public hw::Panel {
public:
    explicit ClipsPanel(AppContext& context);
    ~ClipsPanel() override;
    void paintOverChildren(juce::Graphics& g) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    int rows() const noexcept { return static_cast<int>(items_.size()); }
    /// Selects row `i` (as a click does).
    void choose(int i);

private:
    struct Item {
        juce::String name, track, where;
        std::string track_id, clip_id;
        juce::Colour colour;
        Ticks tick = 0;
    };
    void refresh();
    int row_at(juce::Point<int> p) const;
    AppContext& context_;
    int listener_ = 0;
    std::vector<Item> items_;
    int hover_ = -1;
};

/// New track (Producer view): a type, a count, mono or stereo, Create.
class NewTrackPanel final : public hw::Panel {
public:
    explicit NewTrackPanel(AppContext& context);
    void resized() override;
    void paintOverChildren(juce::Graphics& g) override;
    /// Creates what the panel is set to (as Create does). Returns the count made.
    int create();
    hw::Keycap& type_key(int i) { return *types_[static_cast<std::size_t>(i)]; }
    void set_count(int n);

private:
    AppContext& context_;
    std::vector<std::unique_ptr<hw::Keycap>> types_;
    hw::Keycap minus_{"-"}, plus_{"+"}, stereo_{"Stereo", true}, create_{"Create"};
    hw::Lcd count_{"1", 14.0f};
    int count_value_ = 1;
};

// --- Right panel --------------------------------------------------------------------------------

/// Composer view: what is selected, live.
class SelectionPanel final : public hw::Panel {
public:
    explicit SelectionPanel(AppContext& context);
    ~SelectionPanel() override;
    void resized() override;
    void refresh();
    const hw::Lcd& readout() const noexcept { return lcd_; }

private:
    AppContext& context_;
    int listener_ = 0;
    hw::Lcd lcd_{{}, 12.0f};
    project::RenderCache render_;
};

/// Producer view: the selected track's channel strip (docs/spec/ui, "Mix and
/// master"): inserts A-E with bypass LEDs, a send knob per FX bus, pan, a
/// long-throw fader with its dB scale, the stereo meter, the automation
/// mode, and the input and output (click to change). Fader, pan and sends
/// write automation while a pass is being written for them.
class ChannelPanel final : public hw::Panel, private juce::Timer {
public:
    explicit ChannelPanel(AppContext& context);
    ~ChannelPanel() override;
    void resized() override;
    void paintOverChildren(juce::Graphics& g) override;
    void refresh();
    hw::Fader& fader() { return fader_; }
    hw::SmallKnob& pan() { return pan_; }
    hw::Keycap& mode_key(int i) { return *modes_[static_cast<std::size_t>(i)]; }
    InsertSlots& inserts() { return *slots_; }
    int send_count() const noexcept { return static_cast<int>(sends_.size()); }
    hw::SmallKnob& send(int i) { return *sends_[static_cast<std::size_t>(i)]; }
    const std::string& send_bus(int i) const { return send_bus_[static_cast<std::size_t>(i)]; }
    hw::Lcd& input_lcd() { return in_; }
    hw::Lcd& output_lcd() { return out_; }

private:
    void timerCallback() override;
    AppContext& context_;
    int listener_ = 0;
    std::string track_;
    console::MixWriter writer_;
    std::unique_ptr<InsertSlots> slots_;
    std::vector<std::unique_ptr<hw::SmallKnob>> sends_;
    std::vector<std::string> send_bus_;
    std::vector<juce::String> send_names_;
    hw::Fader fader_;
    hw::SmallKnob pan_;
    hw::Meter meter_l_{hw::Meter::Orientation::Vertical, 20}, meter_r_{hw::Meter::Orientation::Vertical, 20};
    hw::Lcd db_{{}, 13.0f}, pan_lcd_{{}, 11.0f}, in_{{}, 10.5f}, out_{{}, 10.5f};
    hw::Keycap mute_{"M", true}, solo_{"S", true};
    std::vector<std::unique_ptr<hw::Keycap>> modes_;
    bool compact_ = false;
};

// --- Status strip -------------------------------------------------------------------------------

/// The bottom strip: the latest message on the left; sample rate, buffer,
/// CPU, key, meter and the last autosave on the right.
class StatusStrip final : public juce::Component, private juce::Timer {
public:
    explicit StatusStrip(AppContext& context);
    void set_message(const juce::String& text);
    const juce::String& message() const noexcept { return message_; }
    void set_autosaved(juce::Time when) { autosaved_ = when; }
    juce::String readout() const;
    void paint(juce::Graphics& g) override;

private:
    void timerCallback() override { repaint(); }
    AppContext& context_;
    juce::String message_;
    juce::Time autosaved_;
};

}  // namespace vcc::desktop::shell
