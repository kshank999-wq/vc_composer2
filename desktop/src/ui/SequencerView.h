// Sequencer workspace (Main Spec §9): the same tracks as the Composer view
// on a timeline - bar ruler, one lane per track with its rendered notes
// (score-linked notes and MIDI-region notes told apart), playhead, and a
// piano roll for editing the selected track's MIDI regions.
#pragma once

#include "ui/AppContext.h"

namespace vcc::desktop {

class SequencerView final : public juce::Component {
public:
    explicit SequencerView(AppContext& context);
    ~SequencerView() override;

    /// Called by the main window ~30 times a second with the transport position.
    void set_playhead(Ticks tick, bool playing);

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
    /// Pencil: dragging in an automation lane draws freehand (key P).
    bool pencil_mode() const;
    void set_pencil_mode(bool on);
    /// The sequencer showing this context's document (for menus elsewhere), or null.
    static SequencerView* find(const AppContext& context);

    /// The selected audio clip: {track id, clip id}, empty when none.
    std::pair<std::string, std::string> selected_clip() const;

    /// Swaps a take (a clip on a lane > 0) onto the main lane as one undo step;
    /// the clips it overlaps there move to the take's lane.
    static bool promote_take(session::Session& session, const std::string& track_id, const std::string& clip_id);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    AppContext& context_;
};

}  // namespace vcc::desktop
