// The Composer workspace (Main Spec §7; docs/spec/ui/SPEC.md, "Composer
// view"): the overview strip and the four ruler rows, then one row per
// instrument track - the 220 px track header and the lane with its staves
// and phrase blocks - aligned bar for bar, with notation entry on the
// selected track through the notation editor ported from the original VC
// Composer.
//
// The selected track is edited on a working copy of its score; each change is
// committed to the project as one undo step, so Undo, versions, the Producer
// view and playback all see the same notes (Main Spec §8).
#pragma once

#include <map>

#include "ui/Dialogs.h"

#include <memory>
#include <optional>

#include "ui/AppContext.h"
#include "ui/composer/ChordPalette.h"
#include "ui/composer/StaffTools.h"
#include "vcc/editing/ScoreEditor.h"
#include "vcc/ui/NotationView.h"

namespace vcc::desktop {

namespace composer_ui {
class ComposerRuler;
class TrackHeader;
class PhraseLayer;
}  // namespace composer_ui

class ComposerView final : public juce::Component,
                           private juce::ScrollBar::Listener,
                           private juce::ChangeListener,
                           private juce::Timer {
public:
    explicit ComposerView(AppContext& context);
    ~ComposerView() override;

    void set_playhead(Ticks tick, bool playing);
    bool note_entry() const noexcept { return note_entry_; }
    /// Write (the quill: clicks write notes) or Select (the arrow: clicks
    /// select and drag, never write). The staff tools' mode keys.
    void set_note_entry(bool write);
    /// Scrolls the lanes so a track's staff is in view (the Track List).
    void reveal_track(const std::string& track_id);
    /// Zoom: staff size (pixels per staff space) and width (staff spaces per
    /// quarter note) - the H and W sliders at the bottom right.
    void set_zoom(double pixels_per_space, double spaces_per_quarter);
    double zoom_height() const noexcept { return pixels_per_space_; }
    double zoom_width() const noexcept { return spaces_per_quarter_; }
    /// A staff's own room above and below (its slider at the lane's right edge).
    double lane_room(const std::string& track_id) const;
    void set_lane_room(const std::string& track_id, double spaces);
    /// Quick playback (each staff's hold key): this staff alone from where
    /// the view starts while held; on release the playhead comes back.
    void quick_play_start(const std::string& track_id);
    void quick_play_stop();
    bool quick_playing() const noexcept { return quick_playing_; }
    /// Hear each note as it is placed (the Entry group's Hear key); remembered.
    void set_hear_entry(bool on);
    bool hear_entry() const noexcept { return hear_entry_; }
    /// The lanes' scroll position (for checks).
    int lanes_scroll() const { return viewport_.getViewPositionY(); }
    /// The checks: answers a marking click instead of the menu (staff, tick).
    std::function<void(std::size_t, Ticks)> marking_menu_hook;
    /// The checks: receives a note's right-click menu instead of showing it.
    std::function<void(juce::PopupMenu&)> note_menu_hook;
    /// Takes away the dynamic or hairpin nearest a place (what the marking menu's Remove does).
    void remove_marking(std::size_t staff, Ticks tick)
    {
        if (editor_) handle(editor_->remove_marking_near(staff, tick, kTicksPerQuarter / 2));
    }
    /// The notes selected in the score being edited (the shared selection's notes).
    std::vector<notation::EventId> selected_notes() const;
    /// The editor's cursor (value, dots, rest, voice), when a track is being written on.
    std::optional<editing::EditCursor> cursor() const;

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

    /// Performs a shortcut-profile command on the selected track.
    void perform(composer::Command command);
    /// Step input from a key on the deck's keyboard: enters the sounding
    /// pitch `midi` at the cursor with the current value (as written for a
    /// transposing staff, spelt in its key), or adds it to the chord just
    /// entered. False when no instrument track is being written on.
    bool enter_midi(int midi, bool chord);
    // --- Chord Mode (Add-On Features spec §3) -------------------------------
    /// On: the palette floats over the lanes, and a click on a staff enters
    /// its chord there, the bass in the octave clicked.
    bool chord_mode() const noexcept { return chord_mode_; }
    void set_chord_mode(bool on);
    composer_ui::ChordModeState& chord_state() noexcept { return chord_state_; }
    composer_ui::ChordPalette& chord_palette();
    /// Enters the palette's chord at the cursor, or in place of the selected
    /// notes (at the first one's place and value), as one undo step - with
    /// its chord symbol when the palette says so. False with the reason on
    /// the status line.
    bool enter_chord(bool replace_selection);
    /// The palette's chord as the notes it would write on the edited staff
    /// ("B3 D4 F4 G4"), and a warning when any lies outside the instrument.
    std::pair<juce::String, juce::String> chord_preview() const;

    // --- Staff tools (Add-On Features spec §8) --------------------------------
    /// The docked bar beside the selected staff.
    composer_ui::StaffTools& staff_tools();
    /// The floating window's tools while detached, else null.
    composer_ui::StaffTools* detached_staff_tools();
    bool staff_tools_detached() const noexcept { return tools_detached_; }
    void set_staff_tools_detached(bool detached);
    /// Folded to a small tab (by its key, the shortcut, or after a while untouched).
    bool staff_tools_collapsed() const noexcept { return tools_collapsed_; }
    void set_staff_tools_collapsed(bool collapsed);
    /// The folded tools' tab.
    hw::Keycap& staff_tools_tab() { return tools_tab_; }
    /// Places and refreshes the tools now (the timer does it continually).
    void update_staff_tools();

    /// Larger or smaller staves (the toolbar's Zoom keys).
    void zoom(bool larger);
    /// Scrolls so `tick` is the left edge of the lanes.
    void set_origin(Ticks tick);
    /// A jog or shuttle moved the playhead: keep it in view, and (on the
    /// grid) bring the entry cursor there too.
    void follow_jog(Ticks tick, bool move_cursor);
    /// Drops the working copy's uncommitted change (the session refused it).
    void discard_uncommitted();
    Ticks origin() const noexcept { return origin_; }
    /// How many ticks the lanes show.
    Ticks shown_ticks() const { return visible_ticks(); }

    // For the checks.
    composer_ui::ComposerRuler& ruler() { return *ruler_; }
    int strip_count() const noexcept { return static_cast<int>(strips_.size()); }
    composer_ui::TrackHeader& header(int i);
    composer_ui::PhraseLayer& phrases(int i);
    ui::NotationView& lane(int i);
    /// The x of `tick` in this component (the ruler's and lanes' shared timeline).
    float x_of_tick(Ticks tick);

private:
    class Strip;

    void rebuild();
    void reload_working_copy(bool new_editor);
    void apply_view_settings();
    void apply_colours();
    void align_clef_zones();
    void handle(const editing::EditResult& result);
    void ask_for_input(composer::Command command);
    void scrollBarMoved(juce::ScrollBar*, double start) override { set_origin(static_cast<Ticks>(start)); }
    void changeListenerCallback(juce::ChangeBroadcaster*) override { apply_colours(); }
    void timerCallback() override;
    void strip_clicked(Strip& strip, const engraving::InsertionPoint* point, notation::EventId id);
    void note_released(Strip& strip, notation::EventId id);
    void marking_clicked(Strip& strip, std::size_t staff, Ticks tick);
    void note_menu(Strip& strip, notation::EventId id);
    void note_dragged(Strip& strip, notation::EventId id, const engraving::InsertionPoint& from,
                      const engraving::InsertionPoint& to);
    Ticks visible_ticks() const;
    engraving::LayoutOptions lane_layout() const;

    AppContext& context_;
    int listener_ = 0;
    std::unique_ptr<composer_ui::ComposerRuler> ruler_;
    juce::Viewport viewport_;
    juce::Component strips_holder_;
    std::vector<std::unique_ptr<Strip>> strips_;
    juce::ScrollBar hscroll_{false};
    juce::Label hint_;

    notation::Score working_;
    std::unique_ptr<editing::ScoreEditor> editor_;
    std::string editing_track_;
    std::string editing_version_;
    bool note_entry_ = true;
    Ticks origin_ = 0;
    std::uint64_t generation_ = 0;
    /// The spec's 100%: a 7 px staff space and a 120 px bar of 4/4.
    static constexpr double kSpaceAt100 = 7.0;
    static constexpr double kSpacesPerQuarter = 30.0 / 7.0;
    double spaces_per_quarter_ = kSpacesPerQuarter;  ///< Horizontal zoom (the W slider).
    std::map<std::string, double> lane_room_;          ///< Per staff: extra room above and below, in staff spaces.
    bool quick_playing_ = false, quick_was_playing_ = false, hear_entry_ = true;
    Ticks quick_return_ = 0;
    void sound_note(int midi);
    juce::Slider zoom_w_, zoom_h_;
    juce::Label zoom_w_label_, zoom_h_label_;
    double pixels_per_space_ = kSpaceAt100;
    bool committing_ = false;
    bool syncing_ = false;  ///< Exchanging the selection with the session.
    /// The selection is only the note just typed: a duration key then sets
    /// the next note's value (as in Finale and Sibelius) rather than
    /// re-timing that note.
    bool just_entered_ = false;
    bool chord_mode_ = false;
    std::unique_ptr<composer_ui::StaffTools> tools_;
    juce::Component::SafePointer<juce::DocumentWindow> tools_window_;
    composer_ui::StaffTools* tools_floating_ = nullptr;
    hw::Keycap tools_tab_{"Tools"};
    bool tools_detached_ = false, tools_collapsed_ = false, tools_auto_folded_ = false;
    /// Room made above the selected staff for the docked tools, so they never
    /// cover a staff: its height and the track it opens above.
    int tools_gap_ = 0;
    std::string tools_gap_track_;
    juce::uint32 last_activity_ = 0;
    void wire_tools(composer_ui::StaffTools& t);
    composer_ui::StaffToolsState tools_state() const;
    void note_activity();
    composer_ui::ChordModeState chord_state_;
    std::unique_ptr<composer_ui::ChordPalette> palette_;
    /// The palette's own floating window (movable to another screen).
    juce::Component::SafePointer<dialogs::ToolWindow> palette_window_;
    void audition_chord();
    /// The range guardrail before a commit of the working copy: true to
    /// commit now. Strict refuses a single note outside the range and asks
    /// about several (move them into range, keep them flagged, or cancel),
    /// calling `commit` itself when the answer is to go ahead.
    bool range_ok(bool single_entry, std::function<void()> commit);
    juce::String range_message_;  ///< A Warn-policy flag, shown instead of the edit's message.
    /// Publishes the editor's selection to the session's shared selection.
    void publish_selection();
};

}  // namespace vcc::desktop
