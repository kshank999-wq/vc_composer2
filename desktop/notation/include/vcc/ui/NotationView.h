// Copyright (c) VC Composer. All rights reserved.
//
// The notation strip (spec 02 sections 4.3, 4.4, 5.2).
//
// A scrollable, zoomable view of a score whose horizontal axis is the host
// timeline, so that "measures, bars and musical positions ... remain
// synchronized with the corresponding Pro Tools track" (spec 4.3).
//
// It draws and it reports what was clicked. It does not edit: an edit is a
// change to the score, and the score is owned elsewhere, so entry and
// editing commands arrive through the callbacks and come back as a new
// revision. That keeps the view free of a second copy of the score to keep
// in step.
#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "vcc/engraving/HitTest.h"
#include "vcc/engraving/LayoutEngine.h"
#include "vcc/ui/NotationRenderer.h"

namespace vcc::ui {

class NotationView : public juce::Component {
public:
    NotationView();
    ~NotationView() override;

    /// The score to show. Not owned, and may be null. The view watches its
    /// revision counter, so a caller that edits the score only has to
    /// trigger a repaint.
    void set_score(const notation::Score* score);
    const notation::Score* score() const noexcept { return score_; }

    /// Horizontal zoom: staff spaces per quarter note (spec 4.4).
    /// What the two axes are when the notation is at its default size.
    ///
    /// One zoom for the composer, two axes underneath: `set_zoom` scales
    /// both from here, so "zoom in" means "bigger" rather than "less time
    /// across the width" (app/ViewCommand.h).
    static constexpr double kDefaultSpacesPerQuarter =
        engraving::TimelineMap::kDefaultSpacesPerQuarter;
    static constexpr double kDefaultPixelsPerStaffSpace = ui::kDefaultPixelsPerStaffSpace;

    /// Draws the notation at `factor` times its default size.
    void set_zoom(double factor);

    /// What that factor is now, read back from the staff size - which is
    /// what a composer means by how big the notation is. The axes can still
    /// be set apart from each other below, and a caller that does so is
    /// asking for something this cannot describe in one number.
    double zoom() const noexcept
    {
        return vertical_zoom() / kDefaultPixelsPerStaffSpace;
    }

    /// The composer turned the wheel with the zoom modifier held.
    ///
    /// Reported rather than acted on: which rungs a zoom steps through is
    /// the application's to decide, and a view that stepped its own would
    /// be a second answer to a question that already has one. A view with
    /// nobody listening does not zoom on the wheel, which is right - there
    /// is nothing to remember it.
    std::function<void(bool larger)> on_zoom_requested;

    void set_horizontal_zoom(double staff_spaces_per_quarter);
    double horizontal_zoom() const noexcept { return timeline_.staff_spaces_per_quarter(); }

    /// Vertical zoom: pixels per staff space.
    void set_vertical_zoom(double pixels_per_staff_space);
    double vertical_zoom() const noexcept { return settings_.pixels_per_staff_space; }

    /// Scrolls so that `tick` is at the left edge of the music area.
    void set_origin_tick(notation::Ticks tick);
    notation::Ticks origin_tick() const noexcept { return timeline_.origin_tick(); }

    /// The tick range currently visible, which is what gets laid out.
    notation::Ticks visible_ticks() const;

    /// Shows the host playhead, and scrolls to keep it in view (spec 4.4).
    void set_playhead(notation::Ticks tick, bool visible);
    void set_follow_playhead(bool follow);
    bool follow_playhead() const noexcept { return follow_playhead_; }

    /// Scrolls so the playhead sits where playback would have put it.
    ///
    /// Unconditional, even when the playhead is already on screen: a
    /// composer who asked to go there asked for a known position, not for
    /// nothing to happen. Does nothing when the host has not reported one,
    /// which is what `playhead_visible` says.
    void scroll_to_playhead();

    /// The edit cursor, which is where keyboard entry would insert.
    void set_cursor(notation::Ticks tick, std::size_t staff_index, bool visible);

    void set_selection(std::vector<notation::EventId> selection);
    const std::vector<notation::EventId>& selection() const noexcept { return selection_; }

    /// The glyph table and font the renderer uses. Both optional: with no
    /// font the view draws stand-in shapes rather than going blank.
    void set_glyph_table(engraving::GlyphTable glyphs);
    void set_music_font(const MusicFont* font);

    void set_colours(const NotationColours& colours);
    void set_layout_options(const engraving::LayoutOptions& options);
    const engraving::LayoutOptions& layout_options() const noexcept;

    /// Reports that the score changed, so the next paint lays out again.
    /// Also called implicitly when the score's revision has moved on.
    void score_changed();

    /// A note or rest was clicked.
    std::function<void(notation::EventId)> on_event_clicked;

    /// Empty staff was clicked: where a note would be entered (spec 5.2).
    std::function<void(const engraving::InsertionPoint&)> on_staff_clicked;

    /// The view scrolled or zoomed, so a host-side follower can keep up.
    std::function<void(notation::Ticks origin_tick)> on_scrolled;

    /// Entry grid for `on_staff_clicked`, in ticks. 0 means no quantization.
    void set_entry_grid(notation::Ticks ticks) { entry_grid_ = ticks; }

    // --- Component ---------------------------------------------------------
    void paint(juce::Graphics& graphics) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event,
                        const juce::MouseWheelDetails& wheel) override;

    /// The layout currently on screen. Exposed for testing, and for a
    /// container that needs the extent to size a scrollbar.
    const engraving::LayoutResult& layout() const noexcept { return layout_; }

private:
    void relayout_if_needed();
    void relayout();
    engraving::Point to_staff_spaces(juce::Point<int> position) const;

    /// Where the origin goes to put the playhead at the follow margin. One
    /// answer, so that following during playback and going there on demand
    /// land in the same place.
    notation::Ticks origin_for_playhead() const;

    const notation::Score* score_ = nullptr;

    engraving::LayoutEngine engine_;
    engraving::TimelineMap timeline_{kDefaultSpacesPerQuarter};
    engraving::LayoutResult layout_;

    NotationRenderSettings settings_{};
    const MusicFont* music_font_ = nullptr;

    std::vector<notation::EventId> selection_;

    notation::Ticks playhead_tick_ = 0;
    bool playhead_visible_ = false;
    bool follow_playhead_ = true;

    notation::Ticks entry_grid_ = notation::kTicksPerQuarter / 4;  // A sixteenth.

    /// What the last layout was made from, so a paint that changes nothing
    /// does not redo it.
    std::uint64_t laid_out_revision_ = 0;
    bool layout_valid_ = false;
    int laid_out_width_ = 0;
    int laid_out_height_ = 0;
    notation::Ticks laid_out_origin_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NotationView)
};

}  // namespace vcc::ui
