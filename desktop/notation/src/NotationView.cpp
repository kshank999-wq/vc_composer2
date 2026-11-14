#include "vcc/ui/NotationView.h"

#include <algorithm>

namespace vcc::ui {
namespace {

/// Room kept to the right of the playhead when following it, as a fraction
/// of the view width. Scrolling so the playhead sits at the left edge means
/// the music ahead of it is never visible; a third back gives context.
constexpr double kPlayheadFollowMargin = 0.35;

}  // namespace

NotationView::NotationView()
{
    setOpaque(true);
    settings_.pixels_per_staff_space = 10.0;
}

NotationView::~NotationView() = default;

void NotationView::set_score(const notation::Score* score)
{
    if (score_ == score)
        return;
    score_ = score;
    score_changed();
}

void NotationView::set_zoom(double factor)
{
    // Both axes from one number, so the music keeps its shape as it grows.
    // Each setter clamps its own axis, which is why this does not: the
    // staff size saturates before the time scale does, and saturating is
    // what a zoom control should do.
    set_horizontal_zoom(kDefaultSpacesPerQuarter * factor);
    set_vertical_zoom(kDefaultPixelsPerStaffSpace * factor);
}

void NotationView::set_horizontal_zoom(double staff_spaces_per_quarter)
{
    timeline_.set_staff_spaces_per_quarter(staff_spaces_per_quarter);
    score_changed();
}

void NotationView::set_vertical_zoom(double pixels_per_staff_space)
{
    // Clamped for the same reason the horizontal zoom is: a zoom control
    // should saturate rather than produce a staff too small to read.
    settings_.pixels_per_staff_space = std::max(2.0, std::min(80.0, pixels_per_staff_space));
    score_changed();
}

void NotationView::set_origin_tick(notation::Ticks tick)
{
    const notation::Ticks clamped = std::max<notation::Ticks>(0, tick);
    if (timeline_.origin_tick() == clamped)
        return;

    timeline_.set_origin_tick(clamped);
    score_changed();

    if (on_scrolled)
        on_scrolled(clamped);
}

notation::Ticks NotationView::visible_ticks() const
{
    const double width_in_spaces =
        static_cast<double>(getWidth()) / settings_.pixels_per_staff_space;
    return timeline_.ticks_for_width(std::max(1.0, width_in_spaces));
}

void NotationView::set_playhead(notation::Ticks tick, bool visible)
{
    if (playhead_tick_ == tick && playhead_visible_ == visible)
        return;

    playhead_tick_ = tick;
    playhead_visible_ = visible;

    if (follow_playhead_ && visible) {
        const notation::Ticks span = visible_ticks();
        const notation::Ticks origin = timeline_.origin_tick();
        // Scroll only when the playhead leaves the view, so playback does
        // not drag the score under the reader continuously.
        if (tick < origin || tick > origin + span)
            set_origin_tick(origin_for_playhead());
    }

    repaint();
}

void NotationView::set_follow_playhead(bool follow)
{
    follow_playhead_ = follow;
}

notation::Ticks NotationView::origin_for_playhead() const
{
    const notation::Ticks span = visible_ticks();
    const auto margin = static_cast<notation::Ticks>(static_cast<double>(span)
                                                     * kPlayheadFollowMargin);
    return std::max<notation::Ticks>(0, playhead_tick_ - margin);
}

void NotationView::scroll_to_playhead()
{
    if (!playhead_visible_)
        return;
    set_origin_tick(origin_for_playhead());
}

void NotationView::set_cursor(notation::Ticks tick, std::size_t staff_index, bool visible)
{
    engraving::LayoutOptions options = engine_.options();
    options.draw_cursor = visible;
    options.cursor_tick = tick;
    options.cursor_staff = staff_index;
    engine_.set_options(options);
    score_changed();
}

void NotationView::set_selection(std::vector<notation::EventId> selection)
{
    selection_ = std::move(selection);
    repaint();  // Selection is drawn from the existing layout.
}

void NotationView::set_glyph_table(engraving::GlyphTable glyphs)
{
    engine_.set_glyph_table(std::move(glyphs));
    score_changed();
}

void NotationView::set_music_font(const MusicFont* font)
{
    music_font_ = font;
    repaint();
}

void NotationView::set_colours(const NotationColours& colours)
{
    settings_.colours = colours;
    repaint();
}

void NotationView::set_layout_options(const engraving::LayoutOptions& options)
{
    engine_.set_options(options);
    score_changed();
}

const engraving::LayoutOptions& NotationView::layout_options() const noexcept
{
    return engine_.options();
}

void NotationView::score_changed()
{
    layout_valid_ = false;
    repaint();
}

void NotationView::resized()
{
    layout_valid_ = false;
}

void NotationView::relayout_if_needed()
{
    const std::uint64_t revision = score_ != nullptr ? score_->revision() : 0;

    if (layout_valid_ && revision == laid_out_revision_ && getWidth() == laid_out_width_
        && getHeight() == laid_out_height_ && timeline_.origin_tick() == laid_out_origin_)
        return;

    relayout();

    laid_out_revision_ = revision;
    laid_out_width_ = getWidth();
    laid_out_height_ = getHeight();
    laid_out_origin_ = timeline_.origin_tick();
    layout_valid_ = true;
}

void NotationView::relayout()
{
    layout_.clear();
    if (score_ == nullptr || getWidth() <= 0)
        return;

    const notation::Ticks from = timeline_.origin_tick();
    // One extra staff space of ticks, so a note straddling the right edge is
    // laid out rather than popping into view on the next scroll step.
    const notation::Ticks to = from + visible_ticks() + timeline_.ticks_for_width(1.0);

    layout_ = engine_.layout(*score_, timeline_, from, to);

    // The layout is in staff spaces with the header at negative x; the view
    // puts the header at its left edge, so the origin shifts by it.
    settings_.origin = juce::Point<float>(
        static_cast<float>(-layout_.extent.x * settings_.pixels_per_staff_space), 0.0f);
}

float NotationView::x_of_tick(notation::Ticks tick)
{
    relayout_if_needed();
    return static_cast<float>(timeline_.x_for_tick(tick) * settings_.pixels_per_staff_space) + settings_.origin.x;
}

notation::Ticks NotationView::tick_at_x(float x)
{
    relayout_if_needed();
    return timeline_.tick_for_x((static_cast<double>(x) - settings_.origin.x) / settings_.pixels_per_staff_space);
}

engraving::Point NotationView::to_staff_spaces(juce::Point<int> position) const
{
    const double scale = settings_.pixels_per_staff_space;
    return engraving::Point{(static_cast<double>(position.x) - settings_.origin.x) / scale,
                            (static_cast<double>(position.y) - settings_.origin.y) / scale};
}

void NotationView::paint(juce::Graphics& graphics)
{
    relayout_if_needed();

    graphics.fillAll(settings_.colours.background);
    if (score_ == nullptr)
        return;

    draw_notation(graphics, layout_, engine_.glyph_table(), settings_, music_font_,
                  &selection_);

    if (playhead_visible_) {
        // Drawn here rather than in the layout: the playhead moves many
        // times a second and must never cost a relayout.
        const double x = timeline_.x_for_tick(playhead_tick_) * settings_.pixels_per_staff_space
                       + static_cast<double>(settings_.origin.x);
        graphics.setColour(settings_.colours.playhead);
        graphics.fillRect(static_cast<float>(x - 1.0), 0.0f, 2.0f,
                          static_cast<float>(getHeight()));
    }
}

void NotationView::mouseDown(const juce::MouseEvent& event)
{
    if (score_ == nullptr)
        return;

    relayout_if_needed();
    const engraving::Point point = to_staff_spaces(event.getPosition());

    const engraving::HitResult hit = engraving::hit_test(layout_, point);
    if (hit.id != notation::kInvalidEventId) {
        if (on_event_clicked)
            on_event_clicked(hit.id);
        return;
    }

    const engraving::InsertionPoint insertion =
        engraving::insertion_point_at(layout_, *score_, timeline_, point, entry_grid_);
    if (insertion.valid && on_staff_clicked)
        on_staff_clicked(insertion);
}

void NotationView::mouseWheelMove(const juce::MouseEvent& event,
                                  const juce::MouseWheelDetails& wheel)
{
    // Modifier plus wheel zooms, plain wheel scrolls - the convention in
    // every DAW timeline. What a step of the wheel comes to is the
    // application's to say, so that the wheel, the buttons and the
    // shortcuts all land on the same sizes.
    if (event.mods.isCommandDown() || event.mods.isCtrlDown()) {
        if (wheel.deltaY != 0.0f && on_zoom_requested)
            on_zoom_requested(wheel.deltaY > 0.0f);
        return;
    }

    const double spaces = static_cast<double>(-wheel.deltaX + -wheel.deltaY) * 8.0;
    const notation::Ticks step = timeline_.ticks_for_width(spaces);
    if (step != 0)
        set_origin_tick(timeline_.origin_tick() + step);
}

}  // namespace vcc::ui
