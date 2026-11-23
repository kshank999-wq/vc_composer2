#include "vcc/ui/NotationView.h"

#include <cmath>

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
    // Notes sit a little after their beat, so a bar's first note does not
    // touch its barline (the cursor and clicks follow the same mapping).
    timeline_.set_lead(1.2);
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

void NotationView::set_lead(double staff_spaces)
{
    if (std::abs(timeline_.lead() - staff_spaces) < 1e-9) return;
    timeline_.set_lead(staff_spaces);
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

bool NotationView::fit_margins_to_content(double min_above, double min_below)
{
    if (score_ == nullptr)
        return false;
    auto options = engine_.options();
    const double before_above = options.margin_above, before_below = options.margin_below;
    // Start from the minimum, then let what is drawn push the margins out.
    options.margin_above = min_above;
    options.margin_below = min_below;
    engine_.set_options(options);
    relayout();
    const auto& extent = layout_.extent;
    const auto& drawn = layout_.drawn_bounds;
    if (drawn.width > 0.0 && drawn.height > 0.0) {
        const double over_above = extent.y - drawn.y;
        const double over_below = drawn.bottom() - extent.bottom();
        if (over_above > 0.0)
            options.margin_above = min_above + over_above + 1.2;
        if (over_below > 0.0)
            options.margin_below = min_below + over_below + 1.2;
        if (over_above > 0.0 || over_below > 0.0) {
            engine_.set_options(options);
            relayout();
        }
    }
    repaint();
    return std::abs(options.margin_above - before_above) > 1e-6 || std::abs(options.margin_below - before_below) > 1e-6;
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

float NotationView::y_of_staff_position(std::size_t staff_index, double position)
{
    relayout_if_needed();
    if (staff_index >= layout_.staff_top_y.size()) return 0.0f;
    const double spaces = layout_.staff_top_y[staff_index] + (8.0 - position) * 0.5;
    return static_cast<float>(settings_.origin.y + spaces * settings_.pixels_per_staff_space);
}

juce::Rectangle<float> NotationView::notehead_bounds(notation::EventId id)
{
    relayout_if_needed();
    const double scale = settings_.pixels_per_staff_space;
    for (const auto& n : layout_.notes)
        if (n.id == id)
            return {static_cast<float>(settings_.origin.x + n.notehead_bounds.x * scale),
                    static_cast<float>(settings_.origin.y + n.notehead_bounds.y * scale),
                    static_cast<float>(n.notehead_bounds.width * scale), static_cast<float>(n.notehead_bounds.height * scale)};
    return {};
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

    if (spanning_) {
        // The hairpin being drawn, from where the press was to the pointer.
        const float space = static_cast<float>(settings_.pixels_per_staff_space);
        const float x0 = static_cast<float>(x_of_tick(span_start_));
        const float x1 = static_cast<float>(drag_at_.x);
        const float y = static_cast<float>(press_at_.y);
        const float open = space * 0.9f;
        const bool cresc = (span_kind_ == 1) == (x1 >= x0);
        const float narrow_x = cresc ? x0 : x1, wide_x = cresc ? x1 : x0;
        juce::Path wedge;
        wedge.startNewSubPath(wide_x, y - open);
        wedge.lineTo(narrow_x, y);
        wedge.lineTo(wide_x, y + open);
        graphics.setColour(settings_.colours.playhead.withAlpha(0.85f));
        graphics.strokePath(wedge, juce::PathStrokeType(1.6f));
        graphics.setColour(settings_.colours.playhead.withAlpha(0.12f));
        graphics.fillRect(juce::Rectangle<float>(std::min(x0, x1), y - open - 2.0f, std::abs(x1 - x0), 2.0f * open + 4.0f));
    }

    if (boxing_ && drag_at_.getDistanceFrom(press_at_) > 4) {
        // The selection box.
        const auto box = juce::Rectangle<int>(press_at_, drag_at_).toFloat();
        graphics.setColour(settings_.colours.playhead.withAlpha(0.12f));
        graphics.fillRect(box);
        graphics.setColour(settings_.colours.playhead.withAlpha(0.8f));
        graphics.drawRect(box, 1.0f);
    }
    if (dragging_) {
        // Where the dragged note will land: a ghost head under the pointer.
        const float space = static_cast<float>(settings_.pixels_per_staff_space);
        const auto c = drag_at_.toFloat();
        graphics.setColour(settings_.colours.playhead.withAlpha(0.55f));
        graphics.fillEllipse(c.x - space * 0.65f, c.y - space * 0.5f, space * 1.3f, space);
    }
}

void NotationView::set_span_drawing(int kind)
{
    span_kind_ = kind;
    spanning_ = false;
    setMouseCursor(kind != 0 ? juce::MouseCursor(juce::MouseCursor::CrosshairCursor) : idle_cursor_);
    repaint();
}

void NotationView::set_idle_cursor(const juce::MouseCursor& cursor)
{
    idle_cursor_ = cursor;
    if (span_kind_ == 0 && !dragging_) setMouseCursor(idle_cursor_);
}

void NotationView::mouseDown(const juce::MouseEvent& event)
{
    if (score_ == nullptr)
        return;

    relayout_if_needed();
    const engraving::Point point = to_staff_spaces(event.getPosition());

    if (span_kind_ != 0 && !event.mods.isPopupMenu()) {
        // Drawing a hairpin: the press is where it starts, on any staff.
        const auto at = engraving::insertion_point_at(layout_, *score_, timeline_, point, entry_grid_);
        if (!at.valid) return;
        spanning_ = true;
        span_staff_ = at.staff_index;
        span_start_ = at.tick;
        press_at_ = drag_at_ = event.getPosition();
        repaint();
        return;
    }

    engraving::HitResult hit = engraving::hit_test(layout_, point);
    if (rests_as_staff_ && hit.id != notation::kInvalidEventId && !event.mods.isPopupMenu())
        if (const auto* n = score_->find_note(hit.id); n && n->is_rest) hit = engraving::HitResult{};  // write over it
    // A slur or tie clicked: its menu (Remove), even when it carries its note's id.
    if (hit.hit && (hit.role == engraving::ItemRole::Slur || hit.role == engraving::ItemRole::Tie) && on_marking_clicked) {
        marking_role_ = hit.role;
        pressed_ = notation::kInvalidEventId;
        const auto at = engraving::insertion_point_at(layout_, *score_, timeline_, point, 0);
        on_marking_clicked(at.valid ? at.staff_index : 0, tick_at_x(static_cast<float>(event.x)));
        return;
    }
    if (hit.id != notation::kInvalidEventId && event.mods.isPopupMenu() && on_event_menu) {
        pressed_ = notation::kInvalidEventId;
        on_event_menu(hit.id);
        return;
    }
    // Select mode: only a notehead (or what hangs on it) is grabbed to drag;
    // a press on a rest, stem, beam or anything else may draw a box, so notes
    // can be boxed even where the staff is full of them.
    if (box_select_ && hit.id != notation::kInvalidEventId && !event.mods.isPopupMenu() &&
        hit.role != engraving::ItemRole::Notehead && hit.role != engraving::ItemRole::Accidental &&
        hit.role != engraving::ItemRole::AugmentationDot) {
        boxing_ = true;
        box_event_ = hit.id;
        box_click_ = engraving::insertion_point_at(layout_, *score_, timeline_, point, entry_grid_);
        press_at_ = drag_at_ = event.getPosition();
        return;
    }
    if (hit.id != notation::kInvalidEventId) {
        // Pressed: selected now; let go in place, or dragged (mouseUp).
        pressed_ = hit.id;
        press_at_ = drag_at_ = event.getPosition();
        dragging_ = false;
        if (on_event_clicked)
            on_event_clicked(hit.id);
        return;
    }
    pressed_ = notation::kInvalidEventId;
    if (hit.hit &&
        (hit.role == engraving::ItemRole::Dynamic || hit.role == engraving::ItemRole::Hairpin ||
         hit.role == engraving::ItemRole::Slur || hit.role == engraving::ItemRole::Tie) &&
        on_marking_clicked) {
        marking_role_ = hit.role;
        const auto at = engraving::insertion_point_at(layout_, *score_, timeline_, point, 0);
        on_marking_clicked(at.valid ? at.staff_index : 0, tick_at_x(static_cast<float>(event.x)));
        return;
    }

    const engraving::InsertionPoint insertion =
        engraving::insertion_point_at(layout_, *score_, timeline_, point, entry_grid_);
    if (box_select_ && !event.mods.isPopupMenu()) {
        // Maybe a box: decided when the pointer moves (a plain click stays a click).
        boxing_ = true;
        box_event_ = notation::kInvalidEventId;
        box_click_ = insertion;
        press_at_ = drag_at_ = event.getPosition();
        return;
    }
    if (insertion.valid && on_staff_clicked)
        on_staff_clicked(insertion);
}

std::vector<notation::EventId> NotationView::notes_in(juce::Rectangle<float> area)
{
    relayout_if_needed();
    std::vector<notation::EventId> ids;
    for (const auto& n : layout_.notes)
        if (notehead_bounds(n.id).intersects(area) && std::find(ids.begin(), ids.end(), n.id) == ids.end()) ids.push_back(n.id);
    return ids;
}

void NotationView::mouseDrag(const juce::MouseEvent& event)
{
    if (boxing_) {
        drag_at_ = event.getPosition();
        repaint();
        return;
    }
    if (spanning_) {
        drag_at_ = event.getPosition();
        repaint();
        return;
    }
    if (pressed_ == notation::kInvalidEventId)
        return;
    drag_at_ = event.getPosition();
    if (!dragging_ && drag_at_.getDistanceFrom(press_at_) > 4) {
        dragging_ = true;
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    }
    if (dragging_)
        repaint();
}

void NotationView::mouseUp(const juce::MouseEvent& event)
{
    if (boxing_) {
        boxing_ = false;
        repaint();
        if (event.getPosition().getDistanceFrom(press_at_) <= 4) {
            if (box_event_ != notation::kInvalidEventId) {
                if (on_event_clicked) on_event_clicked(box_event_);
            } else if (box_click_.valid && on_staff_clicked) {
                on_staff_clicked(box_click_);
            }
            return;
        }
        const auto box = juce::Rectangle<int>(press_at_, event.getPosition()).toFloat();
        if (on_box_selected) on_box_selected(notes_in(box), event.mods.isShiftDown());
        return;
    }
    if (spanning_) {
        spanning_ = false;
        repaint();
        // Where it ends: the tick under the pointer, on the entry grid.
        notation::Ticks end = tick_at_x(static_cast<float>(event.x));
        if (entry_grid_ > 0) end = ((end + entry_grid_ / 2) / entry_grid_) * entry_grid_;
        if (on_span_drawn && end != span_start_)
            on_span_drawn(span_staff_, std::min(span_start_, end), std::max(span_start_, end));
        return;
    }
    const notation::EventId id = pressed_;
    const bool dragged = dragging_;
    pressed_ = notation::kInvalidEventId;
    dragging_ = false;
    setMouseCursor(idle_cursor_);
    if (id == notation::kInvalidEventId || score_ == nullptr)
        return;
    if (!dragged) {
        if (on_event_released)
            on_event_released(id);
        return;
    }
    repaint();
    relayout_if_needed();
    // Measured from the note's own start, not from where on its head it was
    // grabbed: a drag of two beats moves it exactly two beats.
    juce::Point<int> pick = press_at_, drop = event.getPosition();
    if (const auto* n = score_->find_note(id)) {
        const int start_x = juce::roundToInt(x_of_tick(n->start_tick));
        drop.x -= pick.x - start_x;
        pick.x = start_x;
    }
    const auto from = engraving::insertion_point_at(layout_, *score_, timeline_, to_staff_spaces(pick), entry_grid_);
    const auto to = engraving::insertion_point_at(layout_, *score_, timeline_, to_staff_spaces(drop), entry_grid_);
    if (from.valid && to.valid && on_event_dragged)
        on_event_dragged(id, from, to);
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
