// Sequencer workspace (Main Spec §9): the same tracks as the Composer view
// on a timeline - bar ruler, one lane per track with its rendered notes
// (score-linked notes and MIDI-region notes told apart), playhead, and a
// piano roll for editing the selected track's MIDI regions.
#pragma once

#include <optional>

#include "vcc/project/MidiTools.h"

#include "ui/AppContext.h"

namespace vcc::desktop {

class SequencerView final : public juce::Component {
public:
    explicit SequencerView(AppContext& context);
    ~SequencerView() override;

    /// Called by the main window ~30 times a second with the transport position.
    void set_playhead(Ticks tick, bool playing);
    /// Takes in document changes made while it was hidden (it defers them).
    void bring_up_to_date();
    void visibilityChanged() override;
    /// Scrolls so `tick` is in view (a jog or shuttle moved the playhead).
    void reveal(Ticks tick);
    /// Scrolls the track lanes so a track is in view.
    void reveal_track(const std::string& track_id);

    /// The range selected by dragging on the ruler (the punch range); empty
    /// when first == second.
    std::pair<Ticks, Ticks> range() const;
    void set_range(Ticks start, Ticks end);
    /// Draws the range as the punch range (red).
    void set_punch_display(bool on);
    /// Called when the user changes the range.
    std::function<void()> on_range_changed;

    // --- Automation lanes (Main Spec §12) --------------------------------------------
    /// Opens or closes a track's or bus's automation lanes under it ("bus-master": the master row).
    void show_automation(const std::string& owner_id, bool show);
    bool automation_shown(const std::string& owner_id) const;
    void hide_all_automation();
    /// An expanded part's articulation tracks folded under it, or shown (Memory
    /// Management spec §4.2; a view setting, they still play).
    void set_part_folded(const std::string& part_id, bool folded);
    bool part_folded(const std::string& part_id) const;
    /// How many track rows show (folded articulation tracks do not).
    int shown_track_rows() const;
    /// Pencil: dragging in an automation lane draws freehand (key P).
    bool pencil_mode() const;
    void set_pencil_mode(bool on);
    /// The tick at the left edge of the timeline (the horizontal scroll).
    double view_start() const;
    /// Zooms the timeline in or out about its middle (the toolbar's Zoom keys).
    void zoom(bool in);
    /// Opens Notate Region for a MIDI region of the selected track.
    void notate_region(const std::string& region_id);
    /// The sequencer showing this context's document (for menus elsewhere), or null.
    static SequencerView* find(const AppContext& context);

    /// The selected audio clip: {track id, clip id}, empty when none.
    std::pair<std::string, std::string> selected_clip() const;
    /// How many clips are selected (Shift-click and box selection add to it).
    int selected_clip_count() const;
    /// Every selected clip, {track id, clip id}, the primary one first.
    std::vector<std::pair<std::string, std::string>> selected_clips() const;
    /// Selects a clip (the Clips list): its track, the clip, scrolled into view.
    void select_clip(const std::string& track_id, const std::string& clip_id);
    /// The track whose piano roll is open inline (empty: none).
    std::string expanded_track() const;
    void set_expanded_track(const std::string& track_id);
    /// A track's row height in the lanes (0 when it has none).
    int row_height(const std::string& track_id) const;
    void set_row_height(const std::string& track_id, int height);

    /// Swaps a take (a clip on a lane > 0) onto the main lane as one undo step;
    /// the clips it overlaps there move to the take's lane.
    static bool promote_take(session::Session& session, const std::string& track_id, const std::string& clip_id);
    /// Opens or closes a track's take lanes under it (where a drag across a
    /// take comps it onto the main lane, and a double-click promotes it).
    void show_take_lanes(const std::string& track_id, bool show);

    // --- Edit modes and the Smart tool (the Producer toolbar's Edit keys, F1-F4, F6) -----
    /// Shuffle: clips on a lane stay butted together (moving, trimming and
    /// deleting close or open the gaps). Slip: clips move freely. Spot: a
    /// click on a clip asks for its start time. Grid: moves and trims snap to
    /// the grid. Cmd/Ctrl while dragging inverts snapping in every mode.
    enum class EditMode { Shuffle, Slip, Spot, Grid };
    EditMode edit_mode() const;
    void set_edit_mode(EditMode mode);
    /// The Smart tool: on a clip, the top half drags a range on its track
    /// (Delete removes it, Cmd/Ctrl+T splits at its edges), the bottom half
    /// moves the clip; edges trim and the top corners fade, as always.
    bool smart_tool() const;
    void set_smart_tool(bool on);
    /// Selects [from, to) on these tracks' clip rows, as the Smart tool does.
    void select_track_range(const std::vector<std::string>& track_ids, Ticks from, Ticks to);
    /// What Delete does in the lanes: the Smart range, else the selected
    /// clips (closing the gaps in Shuffle). False when nothing was deleted.
    bool delete_selection();
    /// A main-lane clip's rectangle in the lanes component ("sequencer_lanes").
    juce::Rectangle<float> clip_bounds(const std::string& track_id, const std::string& clip_id) const;
    /// Spot: puts the selected clip's start at `seconds` (the Spot dialog's OK).
    bool spot_selected(double seconds);
    /// The Spot dialog's time scales: 0 Bars|Beats, 1 Min:Sec, 2 Timecode.
    static juce::String spot_text(const project::ProjectData& d, double seconds, int scale);
    static std::optional<double> parse_spot_text(const project::ProjectData& d, const juce::String& text, int scale);

    // --- Clip editing on the selected clip (also its menu and keys) ----------------------
    /// Splits the selected clip's track at the playhead (Cmd/Ctrl+T).
    bool split_at_playhead();
    /// Crossfades the selected clip into the clip that meets its end.
    bool crossfade_selected(double seconds);
    /// Renders the selected clip and the clips touching it into one file.
    bool consolidate_selected();
    /// Moves the selected clip (Alt+Left/Right: 10 ms; with Shift a grid step).
    bool nudge_selected(double seconds);

    // --- MIDI tools on the selected track's played-in notes --------------------------------
    /// Quantizes the piano roll's selected notes, or all of the track's (Q).
    int quantize_notes(const project::QuantizeSettings& q);
    /// Changes the velocity of the selected notes, or all of the track's.
    int change_velocities(const project::VelocityChange& v);
    /// What the piano roll's bottom lane shows: -1 velocity, else a CC number.
    void set_cc_lane(int cc);
    int cc_lane() const;
    /// Draws a straight ramp of controller `cc` from `v0` to `v1` over [from, to).
    bool draw_cc(int cc, Ticks from, Ticks to, int v0, int v1);

    /// A track header's Print (freeze false) or Freeze (true) key.
    std::function<void(const std::string& track_id, bool freeze)> on_print_track;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    AppContext& context_;
};

}  // namespace vcc::desktop
