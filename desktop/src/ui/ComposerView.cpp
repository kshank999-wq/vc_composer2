#include "ui/ComposerView.h"

#include <limits>

#include <algorithm>

#include <cmath>
#include <set>

#include "AppResources.h"
#include "ui/Theme.h"
#include "ui/TrackPanel.h"
#include "ui/Dialogs.h"
#include "audio/PlaybackEngine.h"
#include "ui/hw/HwTheme.h"
#include "ui/composer/CircleOfFifths.h"
#include "ui/composer/ComposerRuler.h"
#include "ui/composer/LaneParts.h"
#include "ui/hw/HwControls.h"
#include "vcc/interchange/Transposition.h"
#include "vcc/ui/KeyMapping.h"

namespace vcc::desktop {

using composer::Command;


// --- Strip: one track's row: header, lane, phrase blocks ------------------------------------

namespace {
/// Quick playback's key: press and hold to hear, let go to stop.
class HoldPlayButton final : public juce::Component, public juce::SettableTooltipClient {
public:
    std::function<void()> on_press, on_release;
    bool held() const noexcept { return held_; }
    void paint(juce::Graphics& g) override
    {
        // Green, as a play key is, so it is seen at once; brighter while held.
        const juce::Colour green(0xff2fae5a);
        auto r = getLocalBounds().toFloat().reduced(1.0f);
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillRoundedRectangle(r.translated(0.0f, 1.5f), 5.0f);
        g.setColour(held_ ? green.brighter(0.45f) : green);
        g.fillRoundedRectangle(r, 5.0f);
        g.setColour(green.darker(0.5f));
        g.drawRoundedRectangle(r.reduced(0.5f), 5.0f, 1.0f);
        auto inner = r.reduced(5.0f, 3.0f);
        auto icon = inner.removeFromLeft(inner.getHeight());
        g.setColour(juce::Colours::white);
        juce::Path tri;
        const auto c = icon.getCentre();
        const float h = icon.getHeight() * 0.8f;
        tri.addTriangle(c.x - h * 0.4f, c.y - h * 0.5f, c.x - h * 0.4f, c.y + h * 0.5f, c.x + h * 0.5f, c.y);
        g.fillPath(tri);
        if (inner.getWidth() > 20.0f) {
            g.setFont(hw::fonts::silk(9.0f, 0.06f, true));
            g.drawText(held_ ? "PLAYING" : "HOLD", inner, juce::Justification::centred, false);
        }
    }
    void mouseDown(const juce::MouseEvent&) override
    {
        held_ = true;
        repaint();
        if (on_press) on_press();
    }
    void mouseUp(const juce::MouseEvent&) override
    {
        if (!held_) return;
        held_ = false;
        repaint();
        if (on_release) on_release();
    }

private:
    bool held_ = false;
};
}  // namespace

class ComposerView::Strip final : public juce::Component {
public:
    Strip(ComposerView& owner, std::string track_id)
        : owner_(owner),
          track_id_(std::move(track_id)),
          header_(owner.context_, track_id_),
          phrases_(owner.context_, track_id_, view_)
    {
        addAndMakeVisible(header_);
        addAndMakeVisible(view_);
        addAndMakeVisible(phrases_);
        auto& font = music_font();
        if (font.loaded) {
            view_.set_glyph_table(font.glyphs);
            view_.set_music_font(&font.font);
        }
        view_.set_follow_playhead(false);
        view_.setWantsKeyboardFocus(false);
        view_.on_staff_clicked = [this](const engraving::InsertionPoint& p) { owner_.strip_clicked(*this, &p, 0); };
        view_.on_event_clicked = [this](notation::EventId id) { owner_.strip_clicked(*this, nullptr, id); };
        view_.on_event_released = [this](notation::EventId id) { owner_.note_released(*this, id); };
        view_.on_marking_clicked = [this](std::size_t staff, Ticks tick) { owner_.marking_clicked(*this, staff, tick); };
        view_.on_event_menu = [this](notation::EventId id) { owner_.note_menu(*this, id); };
        view_.on_event_dragged = [this](notation::EventId id, const engraving::InsertionPoint& from,
                                        const engraving::InsertionPoint& to) { owner_.note_dragged(*this, id, from, to); };
        view_.on_box_selected = [this](std::vector<notation::EventId> ids, bool add) {
            owner_.box_selected(track_id_, std::move(ids), add);
        };
        view_.on_span_drawn = [this](std::size_t staff, Ticks start, Ticks end) {
            owner_.hairpin_drawn(track_id_, staff, start, end);
        };
        view_.on_zoom_requested = [this](bool larger) { owner_.zoom(larger); };
        view_.on_scrolled = [this](Ticks origin) { owner_.set_origin(origin); };
        // This staff's own room: more space above and below it, for notes
        // far off the staff and the markings around them.
        room_.setSliderStyle(juce::Slider::LinearVertical);
        room_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        room_.setRange(0.0, 12.0, 0.5);
        room_.setValue(owner_.lane_room(track_id_), juce::dontSendNotification);
        room_.setDoubleClickReturnValue(true, 0.0);
        room_.setTooltip("Room above and below this staff (double-click: back to fitted)");
        room_.setComponentID("lane_room");
        room_.setTitle("Staff room");
        room_.onValueChange = [this] { owner_.set_lane_room(track_id_, room_.getValue()); };
        addAndMakeVisible(room_);
        // Quick playback: hold to hear this staff alone, from the start of
        // what is shown; let go and the playhead is back where it was.
        play_.setComponentID("lane_quick_play");
        play_.setTooltip("Quick playback: hold to hear this staff alone from where the view starts; let go to come back");
        play_.on_press = [this] { owner_.quick_play_start(track_id_); };
        play_.on_release = [this] { owner_.quick_play_stop(); };
        addAndMakeVisible(play_);
    }
    juce::Component& quick_play_button() noexcept { return play_; }
    juce::Slider& room_slider() noexcept { return room_; }

    const std::string& track_id() const noexcept { return track_id_; }
    ui::NotationView& view() noexcept { return view_; }
    /// The transposed copy the lane shows while the track has a Trans shift.
    notation::Score& shown() noexcept { return shown_; }
    composer_ui::TrackHeader& header() noexcept { return header_; }
    composer_ui::PhraseLayer& phrases() noexcept { return phrases_; }

    /// The spec's row: 108 px for one staff and 184 px for a grand staff at
    /// 100%, never shorter than the header needs.
    int preferred_height(double pixels_per_space, const engraving::LayoutOptions& o) const
    {
        const auto* score = view_.score();
        const int staves = score ? std::max(1, static_cast<int>(score->staff_count())) : 1;
        const double spaces = o.margin_above + o.margin_below + 4.0 * staves + o.staff_gap * (staves - 1);
        return std::max(composer_ui::TrackHeader::kMinHeight, static_cast<int>(std::lround(pixels_per_space * spaces)));
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(hw::tok::edge);
        g.drawHorizontalLine(getHeight() - 1, 0.0f, static_cast<float>(getWidth()));
    }

    void paintOverChildren(juce::Graphics& g) override
    {
        paint_range(g);
        // The selected track's lane is outlined in the LED colour.
        if (owner_.context_.session.selected_track() != track_id_) return;
        g.setColour(hw::Theme::get().led().withAlpha(0.55f));
        g.drawRect(getLocalBounds().withTrimmedLeft(composer_ui::TrackHeader::kWidth), 1);
    }

    /// Instrument range (Add-On spec §4): on the track being written, the
    /// staff above its highest and below its lowest note shaded, with the
    /// limits written in it; on every track, notes outside it ringed.
    void paint_range(juce::Graphics& g)
    {
        auto& session = owner_.context_.session;
        const auto* t = session.data().find_track(track_id_);
        const auto* score = view_.score();
        if (!t || !score || score->staff_count() == 0) return;
        const auto limits = session.range_limits(*t);
        if (limits.policy == session::Session::RangePolicy::Off) return;
        const auto vb = view_.getBounds().toFloat();
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(view_.getBounds());
        const bool strict = limits.policy == session::Session::RangePolicy::Strict;
        // What the lane shows already carries the Trans shift in notation
        // mode; in playback mode the shift is still to come.
        const int unshown = t->transpose_mode == "notation" ? 0 : t->effective_transpose();
        if (session.selected_track() == track_id_) {
            auto position_of = [&](int sounding, std::size_t s) {
                const auto& staff = score->staff(s);
                const auto key = interchange::written_key(score->key_signature_at(view_.origin_tick()), staff.transposition_semitones);
                return notation::staff_position(
                    notation::spell_midi_note(sounding - staff.total_transposition() - unshown, key), staff.clef);
            };
            const std::size_t last = score->staff_count() - 1;
            if (score->staff(0).clef != notation::Clef::Percussion) {
                const float y_hi = vb.getY() + view_.y_of_staff_position(0, position_of(limits.high, 0) + 0.75);
                const float y_lo = vb.getY() + view_.y_of_staff_position(last, position_of(limits.low, last) - 0.75);
                const juce::Colour shade = (strict ? juce::Colour(0xff7a2e2e) : juce::Colour(0xff6e6a5a)).withAlpha(0.22f);
                g.setColour(shade);
                if (y_hi > vb.getY()) g.fillRect(juce::Rectangle<float>(vb.getX(), vb.getY(), vb.getWidth(), y_hi - vb.getY()));
                if (y_lo < vb.getBottom()) g.fillRect(juce::Rectangle<float>(vb.getX(), y_lo, vb.getWidth(), vb.getBottom() - y_lo));
                g.setColour(shade.withAlpha(0.7f));
                g.drawHorizontalLine(juce::roundToInt(y_hi), vb.getX(), vb.getRight());
                g.drawHorizontalLine(juce::roundToInt(y_lo), vb.getX(), vb.getRight());
                // The limits, where the shading starts.
                const juce::String label = juce::String(limits.instrument).toUpperCase() + "  " +
                                           juce::MidiMessage::getMidiNoteName(limits.low, true, true, 4) + "-" +
                                           juce::MidiMessage::getMidiNoteName(limits.high, true, true, 4) + "  " +
                                           juce::String(session::Session::range_policy_name(limits.policy)).toUpperCase();
                g.setFont(hw::fonts::silk(9.5f, 0.08f, true));
                g.setColour(hw::tok::text.withAlpha(0.75f));
                g.drawText(label, juce::Rectangle<float>(vb.getRight() - 260.0f, std::max(vb.getY() + 2.0f, y_hi - 13.0f), 252.0f, 12.0f),
                           juce::Justification::centredRight, false);
            }
        }
        // Issues from the notes as stored (the ids are the lane's).
        const notation::Score& own = track_id_ == owner_.editing_track_ && owner_.editor_ ? owner_.working_ : t->active_version()->score;
        for (const auto& issue : session.range_issues(*t, own)) {
            const auto r = view_.notehead_bounds(issue.id);
            if (r.isEmpty()) continue;
            g.setColour(juce::Colour(strict ? 0xffe0553a : 0xffe0a040));
            g.drawEllipse(r.translated(vb.getX(), vb.getY()).expanded(3.5f), 1.6f);
        }
        // Producer overrides (Memory Management spec §2.3): a note that plays
        // differently from how it is written - kept in Producer instead of
        // applied to the score - wears a small amber tag, so the page never
        // silently disagrees with what is heard.
        for (const auto& [id, ov] : t->active_version()->performance) {
            if (!ov.producer) continue;
            const auto r = view_.notehead_bounds(id);
            if (r.isEmpty()) continue;
            const auto at = r.translated(vb.getX(), vb.getY()).expanded(3.0f);
            juce::Path ring, dashed;
            ring.addRoundedRectangle(at, 3.0f);
            const float dashes[] = {2.5f, 2.0f};
            juce::PathStrokeType(1.3f).createDashedStroke(dashed, ring, dashes, 2);
            g.setColour(juce::Colour(0xffe8a33d));
            g.fillPath(dashed);
            g.setFont(hw::fonts::silk(8.5f, 0.04f, true));
            g.drawText("P", juce::Rectangle<float>(at.getRight() - 1.0f, at.getY() - 9.0f, 9.0f, 9.0f), juce::Justification::centred,
                       false);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        header_.setBounds(r.removeFromLeft(composer_ui::TrackHeader::kWidth));
        {
            auto side = r.removeFromRight(22);
            room_.setBounds(side.reduced(4, 4));
        }
        view_.setBounds(r.withTrimmedBottom(1));
        phrases_.setBounds(view_.getBounds());
        // Hold to hear: top left of the staff, over the music, where it is seen.
        play_.setBounds(view_.getX() + 6, 4, 78, 26);
        play_.toFront(false);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        owner_.context_.session.select_track(track_id_);
        if (e.mods.isPopupMenu()) TrackPanel::show_track_menu(owner_.context_, track_id_);
    }

private:
    ComposerView& owner_;
    std::string track_id_;
    composer_ui::TrackHeader header_;
    ui::NotationView view_;
    composer_ui::PhraseLayer phrases_;
    notation::Score shown_;
    juce::Slider room_;
    HoldPlayButton play_;
};

// --- ComposerView -----------------------------------------------------------------------------

ComposerView::ComposerView(AppContext& context) : context_(context)
{
    setWantsKeyboardFocus(true);
    composer_ui::TimelineAccess access;
    access.x_of = [this](Ticks t) { return x_of_tick(t); };
    access.tick_at = [this](float x) {
        if (strips_.empty()) return origin_;
        return strips_.front()->view().tick_at_x(x - static_cast<float>(composer_ui::TrackHeader::kWidth));
    };
    access.origin = [this] { return origin_; };
    access.visible = [this] { return visible_ticks(); };
    access.scroll_to = [this](Ticks t) { set_origin(t); };
    ruler_ = std::make_unique<composer_ui::ComposerRuler>(context_, std::move(access));
    addAndMakeVisible(*ruler_);
    addAndMakeVisible(viewport_);
    viewport_.setViewedComponent(&strips_holder_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(hscroll_);
    hscroll_.setAutoHide(false);
    hscroll_.addListener(this);
    addAndMakeVisible(hint_);
    hint_.setColour(juce::Label::textColourId, colours::text_dim);
    hint_.setFont(hw::fonts::ui(11.5f));
    hear_entry_ = hw::ui_settings().getBoolValue("composer_hear_entry", true);
    cresc_level_ = juce::jlimit(0, 100, hw::ui_settings().getIntValue("composer_cresc_level", 20));
    dim_level_ = juce::jlimit(0, 100, hw::ui_settings().getIntValue("composer_dim_level", 20));
    for (auto* z : {&zoom_w_, &zoom_h_}) {
        z->setSliderStyle(juce::Slider::LinearHorizontal);
        z->setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        addAndMakeVisible(*z);
    }
    zoom_w_.setComponentID("composer_zoom_width");
    zoom_h_.setComponentID("composer_zoom_height");
    zoom_w_.setTitle("Zoom width");
    zoom_h_.setTitle("Zoom height");
    zoom_w_.setTooltip("Width: more or less music across the window (double-click: 100%)");
    zoom_h_.setTooltip("Height: bigger or smaller staves (double-click: 100%)");
    zoom_w_.setRange(1.5, 14.0);
    zoom_w_.setSkewFactorFromMidPoint(kSpacesPerQuarter);
    zoom_h_.setRange(4.0, 16.0);
    zoom_w_.setDoubleClickReturnValue(true, kSpacesPerQuarter);
    zoom_h_.setDoubleClickReturnValue(true, kSpaceAt100);
    zoom_w_.setValue(spaces_per_quarter_ * pixels_per_space_ / kSpaceAt100, juce::dontSendNotification);
    zoom_h_.setValue(pixels_per_space_, juce::dontSendNotification);
    // W is the music's width on screen (a beat's pixels, at 100% height); H only the
    // staves' size: changing H keeps a beat as wide as it was.
    zoom_w_.onValueChange = [this] { set_zoom(pixels_per_space_, zoom_w_.getValue() * kSpaceAt100 / pixels_per_space_); };
    zoom_h_.onValueChange = [this] {
        const double beat_px = spaces_per_quarter_ * pixels_per_space_;
        const double h = juce::jlimit(4.0, 16.0, zoom_h_.getValue());
        set_zoom(h, beat_px / h);
    };
    for (auto* l : {&zoom_w_label_, &zoom_h_label_}) {
        l->setColour(juce::Label::textColourId, colours::text_dim);
        l->setFont(hw::fonts::ui(10.5f));
        l->setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(*l);
    }
    zoom_w_label_.setText("W", juce::dontSendNotification);
    zoom_h_label_.setText("H", juce::dontSendNotification);
    listener_ = context_.session.add_listener([this](session::Change c) {
        if (committing_ || syncing_) {
            // Our own commit: the headers and blocks still follow it.
            for (auto& s : strips_) s->header().refresh();
            return;
        }
        if (c == session::Change::Document || c == session::Change::Selection) rebuild();
    });
    hw::Theme::get().addChangeListener(this);
    rebuild();
    apply_colours();
    startTimerHz(30);
}

ComposerView::~ComposerView()
{
    stopTimer();
    // The palette's window shows the palette, which goes with this view.
    if (auto* w = palette_window_.getComponent()) {
        w->on_close = nullptr;
        w->clearContentComponent();
        delete w;
    }
    hw::Theme::get().removeChangeListener(this);
    context_.session.remove_listener(listener_);
}

composer_ui::TrackHeader& ComposerView::header(int i) { return strips_.at(static_cast<std::size_t>(i))->header(); }
composer_ui::PhraseLayer& ComposerView::phrases(int i) { return strips_.at(static_cast<std::size_t>(i))->phrases(); }
ui::NotationView& ComposerView::lane(int i) { return strips_.at(static_cast<std::size_t>(i))->view(); }

float ComposerView::x_of_tick(Ticks tick)
{
    const auto header = static_cast<float>(composer_ui::TrackHeader::kWidth);
    if (strips_.empty()) {
        const double px_per_tick = pixels_per_space_ * spaces_per_quarter_ / static_cast<double>(kTicksPerQuarter);
        return header + static_cast<float>(46.0 + static_cast<double>(tick - origin_) * px_per_tick);
    }
    return header + strips_.front()->view().x_of_tick(tick);
}

std::optional<editing::EditCursor> ComposerView::cursor() const
{
    if (!editor_) return std::nullopt;
    return editor_->cursor();
}

engraving::LayoutOptions ComposerView::lane_layout() const
{
    // The spec's lane: the ruler rows above carry bars and beats, so no
    // ruler on each staff; a 46 px clef zone at 100%.
    engraving::LayoutOptions o = strips_.empty() ? engraving::LayoutOptions{} : strips_.front()->view().layout_options();
    o.draw_ruler = false;
    o.margin_above = 6.0;
    o.margin_below = 5.4;
    o.staff_gap = 6.9;
    o.header_width = 46.0 / kSpaceAt100;
    return o;
}

void ComposerView::apply_colours()
{
    const auto& th = hw::Theme::get();
    ui::NotationColours c;
    c.background = th.staff_paper();
    c.staff = th.staff_line();
    c.ink = th.ink();
    c.ruler = th.staff_line();
    c.cursor = th.led();
    c.selection = th.led();
    c.playhead = th.lcd();
    for (auto& s : strips_) s->view().set_colours(c);
    repaint();
}

void ComposerView::timerCallback()
{
    // Each header's meter shows its track's output.
    if (!isShowing()) return;
    update_staff_tools();
    auto db = [](float lin) { return lin > 0.0f ? 20.0f * std::log10(lin) : -100.0f; };
    for (auto& s : strips_) {
        const auto m = context_.engine.take_deck_meter(s->track_id());
        s->header().push_meter(db(m.left), db(m.right));
    }
}

void ComposerView::rebuild()
{
    // The key changed: Chord Mode's scale follows it.
    if (palette_ && chord_mode_) {
        const auto k = dialogs::project_key(context_);
        if (!(k.tonic == chord_state_.key.tonic) || k.mode != chord_state_.key.mode) {
            chord_state_.key = k;
            palette_->refresh();
        }
    }
    const auto tracks = context_.session.composer_tracks();
    std::vector<std::string> ids;
    for (const auto* t : tracks)
        if (t->kind == project::TrackKind::Instrument || t->kind == project::TrackKind::MidiOnly) ids.push_back(t->id);
    bool same = ids.size() == strips_.size();
    for (std::size_t i = 0; same && i < ids.size(); ++i) same = strips_[i]->track_id() == ids[i];
    if (!same) {
        strips_.clear();
        for (const auto& id : ids) {
            strips_.push_back(std::make_unique<Strip>(*this, id));
            strips_holder_.addAndMakeVisible(*strips_.back());
        }
        apply_colours();
    }
    for (std::size_t i = 0; i < strips_.size(); ++i) {
        strips_[i]->header().set_number(static_cast<int>(i) + 1);
        strips_[i]->header().refresh();
    }
    const auto* selected = context_.session.data().find_track(context_.session.selected_track());
    const auto* version = selected ? selected->active_version() : nullptr;
    // Another document starts afresh: cursor at the start, scrolled home.
    const bool new_document = context_.session.document_generation() != generation_;
    if (new_document) {
        generation_ = context_.session.document_generation();
        origin_ = 0;
        just_entered_ = false;
    }
    const bool new_track = new_document || !selected || selected->id != editing_track_ || !version ||
                           version->id != editing_version_;
    reload_working_copy(new_track);
    // Notes selected elsewhere (the piano roll, the inspector) are selected here.
    const auto& shared = context_.session.selection();
    if (editor_ && shared.track == editing_track_) {
        auto mine = editor_->selection();
        std::sort(mine.begin(), mine.end());
        if (mine != shared.notes) {
            editor_->set_selection(shared.notes);
            just_entered_ = false;  // a selection made elsewhere
        }
    }
    apply_view_settings();
    resized();
    repaint();
}

std::vector<notation::EventId> ComposerView::selected_notes() const
{
    if (!editor_) return {};
    auto ids = editor_->selection();
    std::sort(ids.begin(), ids.end());
    return ids;
}

void ComposerView::publish_selection()
{
    if (!editor_ || editing_track_.empty() || syncing_) return;
    auto mine = editor_->selection();
    std::sort(mine.begin(), mine.end());
    const auto& shared = context_.session.selection();
    if (shared.track == editing_track_ && shared.notes == mine) return;
    syncing_ = true;
    if (mine.empty()) {
        auto s = shared;
        s.track = editing_track_;
        s.notes.clear();
        context_.session.set_selection(std::move(s));
    } else {
        context_.session.select_notes(editing_track_, mine);
    }
    syncing_ = false;
}

void ComposerView::reload_working_copy(bool new_editor)
{
    const auto* t = context_.session.data().find_track(context_.session.selected_track());
    const auto* v = t ? t->active_version() : nullptr;
    if (!v || (t->kind != project::TrackKind::Instrument && t->kind != project::TrackKind::MidiOnly)) {
        editor_.reset();
        editing_track_.clear();
        editing_version_.clear();
        hint_.setText("Select an instrument track to write on it.", juce::dontSendNotification);
        return;
    }
    editing::EditCursor kept;
    if (editor_ && !new_editor) kept = editor_->cursor();
    working_ = v->score;
    if (new_editor || !editor_) editor_ = std::make_unique<editing::ScoreEditor>(working_);
    if (kept.staff_index >= working_.staff_count()) kept.staff_index = 0;
    editor_->set_cursor(kept);
    editing_track_ = t->id;
    editing_version_ = v->id;
    hint_.setText("Writing on " + juce::String(t->name) + " (" + juce::String(v->name) + ")" +
                      juce::String("  -  type A-G to enter notes, 5 quarter / 4 eighth / 6 half for durations, Space to play"),
                  juce::dontSendNotification);
}

void ComposerView::apply_view_settings()
{
    const auto options = lane_layout();
    for (auto& s : strips_) {
        const auto* t = context_.session.data().find_track(s->track_id());
        const auto* v = t ? t->active_version() : nullptr;
        auto& view = s->view();
        view.set_layout_options(options);
        // Writing: the quill is the pointer over the staff that writes.
        // Select mode: a drag on empty staff draws a box that selects the notes in it.
        view.set_box_select(!note_entry_ && !chord_mode_);
        // Writing: a click on a rest puts the note there (it is not a rest to select).
        view.set_rests_as_staff(note_entry_ || chord_mode_);
        view.set_idle_cursor(note_entry_ && !chord_mode_ && s->track_id() == editing_track_
                                 ? composer_ui::IconKey::quill_cursor()
                                 : juce::MouseCursor(juce::MouseCursor::NormalCursor));
        // Track Trans in notation mode: the lane shows the notes as heard,
        // on a copy with the same note ids (selection and editing still work).
        const int shown_shift = t && t->transpose_mode == "notation" ? t->effective_transpose() : 0;
        auto shown = [&](const notation::Score* score) -> const notation::Score* {
            if (!score || shown_shift == 0) return score;
            s->shown() = session::Session::transposed_score(*score, shown_shift);
            return &s->shown();
        };
        if (s->track_id() == editing_track_ && editor_) {
            view.set_score(shown(&working_));
            view.set_cursor(editor_->cursor().tick, editor_->cursor().staff_index, note_entry_);
            view.set_selection(editor_->selection());
        } else {
            view.set_score(shown(v ? &v->score : nullptr));
            view.set_cursor(0, 0, false);
            view.set_selection({});
        }
        view.set_vertical_zoom(pixels_per_space_);
        view.set_horizontal_zoom(spaces_per_quarter_);
        view.set_origin_tick(origin_);
        view.score_changed();
    }
    align_clef_zones();
    publish_selection();
}

void ComposerView::align_clef_zones()
{
    // A key signature widens a lane's clef zone past the spec's 46 px; every
    // lane takes the widest, so bars line up from track to track.
    const double base = lane_layout().header_width;
    double widest = base;
    for (auto& s : strips_) {
        auto& v = s->view();
        if (v.getWidth() <= 0 || v.score() == nullptr) continue;
        auto o = v.layout_options();
        if (std::abs(o.header_width - base) > 1e-6) {
            o.header_width = base;
            v.set_layout_options(o);
        }
        widest = std::max(widest, static_cast<double>(v.x_of_tick(v.origin_tick())) / pixels_per_space_);
    }
    for (auto& s : strips_) {
        auto o = s->view().layout_options();
        if (std::abs(o.header_width - widest) > 1e-6) {
            o.header_width = widest;
            s->view().set_layout_options(o);
        }
    }
}

Ticks ComposerView::visible_ticks() const
{
    return strips_.empty() ? kTicksPerWhole * 8 : strips_.front()->view().visible_ticks();
}

void ComposerView::set_origin(Ticks tick)
{
    origin_ = std::max<Ticks>(0, tick);
    for (auto& s : strips_) {
        s->view().set_origin_tick(origin_);
        s->phrases().repaint();
    }
    hscroll_.setCurrentRangeStart(static_cast<double>(origin_), juce::dontSendNotification);
    ruler_->repaint();
}

void ComposerView::follow_jog(Ticks tick, bool move_cursor)
{
    const Ticks visible = visible_ticks();
    if (tick < origin_ || tick > origin_ + visible * 9 / 10) set_origin(tick - visible / 10);
    if (!move_cursor || !editor_) return;
    editing::EditCursor c = editor_->cursor();
    c.tick = tick;
    editor_->set_cursor(c);
    for (auto& s : strips_)
        if (s->track_id() == editing_track_) s->view().set_cursor(editor_->cursor().tick, editor_->cursor().staff_index, note_entry_);
}

void ComposerView::discard_uncommitted()
{
    reload_working_copy(false);
    apply_view_settings();
    repaint();
}

void ComposerView::quick_play_start(const std::string& track_id)
{
    auto& engine = context_.engine;
    if (quick_playing_) return;
    quick_playing_ = true;
    quick_return_ = engine.position();
    quick_was_playing_ = engine.playing();
    if (quick_was_playing_) engine.stop();
    // This staff alone (render-only: the project's solo switches are untouched).
    PlaybackEngine::RenderOverride solo;
    solo.solo = {track_id};
    engine.set_render_override(solo);
    engine.locate(std::max<Ticks>(0, origin_));
    engine.play();
    context_.status("Quick playback: " +
                    juce::String(context_.session.data().find_track(track_id) ? context_.session.data().find_track(track_id)->name
                                                                              : std::string()) +
                    " alone - let go to come back");
}

void ComposerView::quick_play_stop()
{
    if (!quick_playing_) return;
    quick_playing_ = false;
    auto& engine = context_.engine;
    engine.stop();
    engine.set_render_override(std::nullopt);
    engine.locate(quick_return_);
    set_playhead(quick_return_, false);
    context_.status("Back where you were");
}

void ComposerView::set_hear_entry(bool on)
{
    hear_entry_ = on;
    hw::ui_settings().setValue("composer_hear_entry", on);
    hw::ui_settings().saveIfNeeded();
}

void ComposerView::sound_note(int midi)
{
    // The note just placed, on the track's own sound, briefly.
    if (!hear_entry_ || midi < 0 || midi > 127) return;
    context_.engine.set_live_track(editing_track_);
    context_.engine.send_live(juce::MidiMessage::noteOn(1, midi, static_cast<juce::uint8>(90)));
    juce::Component::SafePointer<ComposerView> self(this);
    juce::Timer::callAfterDelay(350, [self, midi] {
        if (self) self->context_.engine.send_live(juce::MidiMessage::noteOff(1, midi));
    });
}

double ComposerView::lane_room(const std::string& track_id) const
{
    const auto it = lane_room_.find(track_id);
    return it == lane_room_.end() ? 0.0 : it->second;
}

void ComposerView::set_lane_room(const std::string& track_id, double spaces)
{
    lane_room_[track_id] = juce::jlimit(0.0, 12.0, spaces);
    resized();
}

void ComposerView::set_zoom(double pixels_per_space, double spaces_per_quarter)
{
    pixels_per_space_ = juce::jlimit(4.0, 16.0, pixels_per_space);
    spaces_per_quarter_ = juce::jlimit(0.5, 40.0, spaces_per_quarter);
    zoom_h_.setValue(pixels_per_space_, juce::dontSendNotification);
    zoom_w_.setValue(spaces_per_quarter_ * pixels_per_space_ / kSpaceAt100, juce::dontSendNotification);
    apply_view_settings();
    resized();
    ruler_->repaint();
}

void ComposerView::zoom(bool larger)
{
    const double beat_px = spaces_per_quarter_ * pixels_per_space_;
    pixels_per_space_ = juce::jlimit(4.0, 16.0, pixels_per_space_ * (larger ? 1.15 : 1.0 / 1.15));
    spaces_per_quarter_ = juce::jlimit(0.5, 40.0, beat_px / pixels_per_space_);  // height only
    zoom_h_.setValue(pixels_per_space_, juce::dontSendNotification);
    apply_view_settings();
    resized();
    ruler_->repaint();
    if (context_.status)
        context_.status("Zoom " + juce::String(juce::roundToInt(100.0 * pixels_per_space_ / kSpaceAt100)) + "%");
}

void ComposerView::strip_clicked(Strip& strip, const engraving::InsertionPoint* point, notation::EventId id)
{
    note_activity();
    grabKeyboardFocus();
    just_entered_ = false;
    // A click on another track selects it, in Select mode: it never writes a note.
    const bool already_editing = strip.track_id() == editing_track_;
    context_.session.select_track(strip.track_id());
    if (!already_editing) {
        if (note_entry_ && !chord_mode_) set_note_entry(false);
        return;
    }
    if (!editor_ || strip.track_id() != editing_track_) return;
    if (id != notation::kInvalidEventId) {
        handle(editor_->select_and_follow(id));
        return;
    }
    if (point && point->valid) {
        editing::EditCursor c = editor_->cursor();
        // Not chord mode's free placement: the hidden grid of the note value.
        c.tick = chord_mode_ ? point->tick : snap_entry_tick(point->tick, point->staff_index);
        c.staff_index = point->staff_index;
        editor_->set_cursor(c);
        if (chord_mode_) {
            // The bass goes in the octave clicked (sounding).
            const auto& staff = working_.staff(std::min<std::size_t>(point->staff_index, working_.staff_count() - 1));
            const int sounding = notation::midi_note_number(point->pitch) + staff.total_transposition();
            chord_state_.octave = std::clamp(sounding / 12 - 1, 0, 8);
            // The note clicked is the root, spelled as the key spells it there
            // (sounding, for a transposing staff).
            const auto concert = staff.total_transposition() == 0
                                     ? point->pitch
                                     : notation::spell_midi_note(sounding, working_.key_signature_at(point->tick));
            chord_state_.letter = static_cast<int>(concert.step);
            chord_state_.accidental = std::clamp(static_cast<int>(concert.alteration), -1, 1);
            chord_state_.key = dialogs::project_key(context_);
            if (chord_state_.type == "custom") chord_state_.type = "triad";
            if (palette_) palette_->refresh();
            enter_chord(false);
            return;
        }
        if (note_entry_) {
            handle(editor_->insert_pitch(point->pitch));
            // Placed with the mouse, as typed: a value key now is for the next notes.
            just_entered_ = true;
        } else {
            apply_view_settings();
        }
    }
}

void ComposerView::note_menu(Strip& strip, notation::EventId id)
{
    // A note right-clicked: what can be done to it, in one place.
    context_.session.select_track(strip.track_id());
    if (!editor_ || strip.track_id() != editing_track_) return;
    const auto* n = working_.find_note(id);
    if (n == nullptr) return;
    // A note of a box selection keeps the selection; another note is selected alone.
    const auto current = editor_->selection();
    const bool in_selection = std::find(current.begin(), current.end(), id) != current.end();
    if (!in_selection || current.size() < 2) handle(editor_->select_and_follow(id));
    const int count = static_cast<int>(editor_->selection().size());
    juce::PopupMenu m;
    m.addSectionHeader(count > 1 ? juce::String(count) + " notes selected" : juce::String(n->is_rest ? "Rest" : "Note"));
    juce::Component::SafePointer<ComposerView> self(this);
    auto act = [self](std::function<editing::EditResult(editing::ScoreEditor&)> f) {
        return [self, f] {
            if (self && self->editor_) self->handle(f(*self->editor_));
        };
    };
    const bool note = !n->is_rest;
    m.addItem("Sharp", note, note && n->pitch.alteration == 1, act([](editing::ScoreEditor& e) { return e.set_accidental(1); }));
    m.addItem("Flat", note, note && n->pitch.alteration == -1, act([](editing::ScoreEditor& e) { return e.set_accidental(-1); }));
    m.addItem("Natural", note, note && n->pitch.alteration == 0, act([](editing::ScoreEditor& e) { return e.set_accidental(0); }));
    m.addItem("Respell (enharmonic)", note, false, act([](editing::ScoreEditor& e) { return e.cycle_enharmonic(); }));
    m.addSeparator();
    m.addItem("Up a step", note, false, act([](editing::ScoreEditor& e) { return e.transpose_selection(1); }));
    m.addItem("Down a step", note, false, act([](editing::ScoreEditor& e) { return e.transpose_selection(-1); }));
    m.addItem("Up an octave", note, false, act([](editing::ScoreEditor& e) { return e.transpose_selection_octaves(1); }));
    m.addItem("Down an octave", note, false, act([](editing::ScoreEditor& e) { return e.transpose_selection_octaves(-1); }));
    m.addSeparator();
    m.addItem(count > 1 ? "Delete " + juce::String(count) + " Notes" : juce::String("Delete"),
              act([](editing::ScoreEditor& e) { return e.delete_selection(); }));
    m.addSeparator();
    m.addItem(count > 1 ? "Send " + juce::String(count) + " Notes to Track..." : juce::String("Send Note to Track..."), [self] {
        if (self) self->ask_send_selected_notes();
    });
    if (note_menu_hook) {
        note_menu_hook(m);  // the checks pick an item
        return;
    }
    m.showMenuAsync(juce::PopupMenu::Options());
}

void ComposerView::marking_clicked(Strip& strip, std::size_t staff, Ticks tick)
{
    // A dynamic or hairpin clicked: offer to take it away (one undo step).
    context_.session.select_track(strip.track_id());
    if (!editor_ || strip.track_id() != editing_track_) return;
    juce::PopupMenu m;
    m.addSectionHeader("Marking");
    juce::Component::SafePointer<ComposerView> self(this);
    m.addItem("Remove", [self, staff, tick] {
        if (self && self->editor_)
            self->handle(self->editor_->remove_marking_near(staff, tick, notation::kTicksPerQuarter / 2));
    });
    if (marking_menu_hook) {
        marking_menu_hook(staff, tick);  // the checks choose without a menu
        return;
    }
    m.showMenuAsync(juce::PopupMenu::Options());
}

void ComposerView::note_released(Strip& strip, notation::EventId id)
{
    // A note clicked (not dragged) stays: it is selected, never erased by a
    // click - Delete, Backspace or the right-click menu take notes away.
    (void)strip;
    (void)id;
}

void ComposerView::note_dragged(Strip& strip, notation::EventId id, const engraving::InsertionPoint& from,
                                const engraving::InsertionPoint& to)
{
    // Dragging a note (or the selection it belongs to) moves it in time and pitch.
    if (!editor_ || strip.track_id() != editing_track_) return;
    note_activity();
    const int steps = notation::diatonic_index(to.pitch) - notation::diatonic_index(from.pitch);
    handle(editor_->move_notes(id, to.tick - from.tick, steps));
}

void ComposerView::reveal_track(const std::string& track_id)
{
    for (auto& s : strips_)
        if (s->track_id() == track_id) {
            viewport_.setViewPosition(viewport_.getViewPositionX(), std::max(0, s->getY() - 8));
            return;
        }
}

void ComposerView::set_playhead(Ticks tick, bool playing)
{
    for (auto& s : strips_) s->view().set_playhead(tick, playing || tick > 0);
    ruler_->set_playhead(tick);
    if (playing && context_.follow) {
        const Ticks visible = visible_ticks();
        if (tick < origin_ || tick > origin_ + visible * 9 / 10) set_origin(tick - visible / 10);
    }
}

bool ComposerView::range_ok(bool single_entry, std::function<void()> commit)
{
    auto& session = context_.session;
    const auto* t = session.data().find_track(editing_track_);
    if (!t) return true;
    const auto limits = session.range_limits(*t);
    if (limits.policy == session::Session::RangePolicy::Off) return true;
    std::set<notation::EventId> before;
    for (const auto& i : session.range_issues(*t)) before.insert(i.id);
    std::vector<session::Session::RangeIssue> added;
    for (const auto& i : session.range_issues(*t, working_))
        if (!before.count(i.id)) added.push_back(i);
    if (added.empty()) return true;
    const juce::String range = juce::MidiMessage::getMidiNoteName(limits.low, true, true, 4) + "-" +
                               juce::MidiMessage::getMidiNoteName(limits.high, true, true, 4);
    const auto& first = added.front();
    const juce::String what = juce::MidiMessage::getMidiNoteName(first.sounding, true, true, 4) +
                              (first.above ? " is above" : " is below") + " the " + juce::String(limits.instrument) +
                              "'s range (" + range + ")";
    if (limits.policy != session::Session::RangePolicy::Strict) {
        range_message_ = "Flagged: " + what + (added.size() > 1 ? " (" + juce::String(static_cast<int>(added.size())) + " notes)" : "");
        return true;
    }
    if (single_entry && added.size() == 1) {
        reload_working_copy(false);
        context_.status("Not entered: " + what + ". The range is Strict for this track (Track menu > Range).");
        return false;
    }
    // Several notes (a chord, a paste, a transposition): ask.
    auto* w = new juce::AlertWindow("Outside the " + juce::String(limits.instrument) + "'s range",
                                    juce::String(static_cast<int>(added.size())) + " of these notes would be outside " + range +
                                        " (" + what + ").",
                                    juce::MessageBoxIconType::WarningIcon);
    w->setComponentID("range_question");
    w->addButton("Move into range", 1);
    w->addButton("Keep, flagged", 2);
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<ComposerView> self(this);
    w->enterModalState(true, juce::ModalCallbackFunction::create([self, added, limits, commit](int r) {
        if (!self) return;
        if (r == 0) {
            self->reload_working_copy(false);
            self->context_.status("Cancelled: nothing entered");
        } else {
            if (r == 1) session::Session::fit_into_range(self->working_, added, limits);
            commit();
        }
        self->apply_view_settings();
        self->repaint();
    }), true);
    return false;
}

void ComposerView::handle(const editing::EditResult& result)
{
    // A note placed (typed or clicked) is heard, when Hear is on.
    if (result.score_changed && result.new_event != notation::kInvalidEventId)
        if (const auto* n = working_.find_note(result.new_event); n && !n->is_rest) {
            const auto staff = working_.staff_index_of(n->id);
            sound_note(notation::midi_note_number(n->pitch) + working_.staff(staff).total_transposition());
        }
    // Entered on a lane showing a Trans shift: the note was entered as
    // shown, so it is stored a shift lower and sounds once transposed.
    if (result.score_changed && result.new_event != notation::kInvalidEventId && !editing_track_.empty())
        if (const auto* t = context_.session.data().find_track(editing_track_);
            t && t->transpose_mode == "notation" && t->effective_transpose() != 0)
            if (const auto* n = working_.find_note(result.new_event); n && !n->is_rest) {
                const auto staff = working_.staff_index_of(n->id);
                const auto& st = working_.staff(staff);
                const int midi = notation::midi_note_number(n->pitch) - t->effective_transpose();
                if (midi >= 0 && midi <= 127) {
                    notation::ScoreNote moved = *n;
                    moved.pitch = notation::spell_midi_note(
                        midi, interchange::written_key(working_.key_signature_at(n->start_tick), st.transposition_semitones));
                    working_.modify_note(n->id, moved);
                }
            }
    if (result.score_changed && !editing_track_.empty()) {
        const std::string label = result.message;
        auto commit = [this, label] {
            committing_ = true;
            context_.session.commit_score(editing_track_, working_, label);
            committing_ = false;
        };
        if (!range_ok(result.notes_changed <= 1, commit)) {
            apply_view_settings();
            repaint();
            return;
        }
        commit();
    }
    if (range_message_.isNotEmpty()) {
        context_.status(range_message_);  // the range's word over the edit's
        range_message_.clear();
    } else if (result.status == editing::EditStatus::Refused || result.status == editing::EditStatus::NotHandled)
        context_.status(result.message);
    else if (*result.message)
        context_.status(result.message);
    apply_view_settings();
    // Keep the cursor in view while typing.
    if (editor_) {
        const Ticks cur = editor_->cursor().tick;
        const Ticks visible = visible_ticks();
        if (cur < origin_ || cur > origin_ + visible * 85 / 100) set_origin(cur - visible / 4);
    }
    repaint();
}

void ComposerView::perform(Command command)
{
    switch (command) {
        case Command::Undo: context_.session.undo(); return;
        case Command::Redo: context_.session.redo(); return;
        case Command::StopPlayback: context_.engine.stop(); return;
        case Command::PlayFromCursor:
            if (editor_) context_.engine.locate(editor_->cursor().tick);
            context_.engine.play();
            return;
        case Command::ToggleKeyboardEntryMode: set_note_entry(!note_entry_); return;
        case Command::ExportMidiFile: context_.commands.invokeDirectly(cmd::exportMidi, true); return;
        case Command::EditKeySignature:
        case Command::EditTimeSignature:
        case Command::EditClef:
        case Command::EditTempo:
        case Command::AddText: ask_for_input(command); return;
        default: break;
    }
    if (!editor_) {
        context_.status("Select an instrument track first");
        return;
    }
    if (command == Command::StartCrescendo || command == Command::StartDiminuendo) {
        const int kind = command == Command::StartCrescendo ? 1 : 2;
        // Notes selected: across them, at once. Otherwise draw it with the
        // mouse: press where it starts, drag to where it ends.
        const auto sel = editor_->selection();
        if (!sel.empty()) {
            Ticks from = std::numeric_limits<Ticks>::max(), to = 0;
            std::size_t staff = editor_->cursor().staff_index;
            for (const auto id : sel)
                if (const auto* n = working_.find_note(id)) {
                    from = std::min(from, n->start_tick);
                    to = std::max(to, n->end_tick());
                    staff = working_.staff_index_of(id);
                }
            if (to > from) {
                handle(editor_->add_hairpin(staff, from, to,
                                            kind == 1 ? notation::HairpinKind::Crescendo : notation::HairpinKind::Diminuendo,
                                kind == 1 ? cresc_level_ : dim_level_));
                return;
            }
        }
        set_hairpin_drawing(hairpin_drawing_ == kind ? 0 : kind);
        return;
    }
    // Right after typing a note, a value, dot, triplet or rest is for the
    // next note, as in Finale and Sibelius; with notes the user selected, it
    // changes them.
    const bool for_next = command == Command::SelectDurationBreve || command == Command::SelectDurationWhole ||
                          command == Command::SelectDurationHalf || command == Command::SelectDurationQuarter ||
                          command == Command::SelectDurationEighth || command == Command::SelectDurationSixteenth ||
                          command == Command::SelectDurationThirtySecond || command == Command::SelectDurationSixtyFourth ||
                          command == Command::ToggleDot || command == Command::ToggleDoubleDot ||
                          command == Command::ToggleTuplet || command == Command::ToggleRest;
    if (for_next && just_entered_) {
        editor_->clear_selection();
        handle(editor_->apply(command));
        return;  // still "just entered": 4 then . chooses a dotted eighth
    }
    just_entered_ = false;
    // A dynamic or hairpin goes where the selected notes start, as in
    // Finale and Sibelius, rather than at the entry cursor.
    const bool at_selection = command == Command::DynamicPianississimo || command == Command::DynamicPianissimo ||
                              command == Command::DynamicPiano || command == Command::DynamicMezzoPiano ||
                              command == Command::DynamicMezzoForte || command == Command::DynamicForte ||
                              command == Command::DynamicFortissimo || command == Command::DynamicFortississimo ||
                              command == Command::StartCrescendo || command == Command::StartDiminuendo;
    if (at_selection && !editor_->selection().empty()) {
        const notation::ScoreNote* first = nullptr;
        for (auto id : editor_->selection())
            if (const auto* n = working_.find_note(id); n && (!first || n->start_tick < first->start_tick)) first = n;
        if (first) {
            auto cur = editor_->cursor();
            cur.tick = first->start_tick;
            cur.staff_index = working_.staff_index_of(first->id);
            editor_->set_cursor(cur);
        }
    }
    handle(editor_->apply(command));
}

// --- Staff tools ---------------------------------------------------------------------------

namespace {
constexpr juce::uint32 kToolsIdleMs = 15000;  // fold after this long untouched

juce::Rectangle<int> on_a_display(juce::Rectangle<int> r)
{
    // Kept where the person put it, unless that display is gone.
    for (const auto& d : juce::Desktop::getInstance().getDisplays().displays)
        if (d.userArea.intersects(r.reduced(20))) return r;
    const auto* main = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    return main ? r.withCentre(main->userArea.getCentre()) : r.withPosition(80, 80);
}
}  // namespace

void ComposerView::set_note_entry(bool write)
{
    if (note_entry_ != write) {
        note_entry_ = write;
        just_entered_ = false;
        reload_working_copy(false);
        apply_view_settings();
    }
    context_.status(note_entry_ ? "Write: click the staff to write notes" : "Select: click notes to select them, drag to move them");
    update_staff_tools();
}

void ComposerView::set_hairpin_drawing(int kind)
{
    hairpin_drawing_ = kind;
    for (auto& s : strips_) s->view().set_span_drawing(kind);
    context_.status(kind == 0   ? juce::String("Hairpin drawing off")
                    : kind == 1 ? juce::String("Crescendo: press on the staff where it starts and drag to where it ends (Esc cancels)")
                                : juce::String("Diminuendo: press on the staff where it starts and drag to where it ends (Esc cancels)"));
    update_staff_tools();
}

void ComposerView::hairpin_drawn(const std::string& track_id, std::size_t staff, Ticks start, Ticks end)
{
    const int kind = hairpin_drawing_;
    set_hairpin_drawing(0);
    if (kind == 0) return;
    context_.session.select_track(track_id);
    if (!editor_ || editing_track_ != track_id) return;
    handle(editor_->add_hairpin(staff, start, end,
                                kind == 1 ? notation::HairpinKind::Crescendo : notation::HairpinKind::Diminuendo,
                                kind == 1 ? cresc_level_ : dim_level_));
}

void ComposerView::wire_tools(composer_ui::StaffTools& t)
{
    t.on_mode = [this](bool write) {
        note_activity();
        set_note_entry(write);
        grabKeyboardFocus();
    };
    t.on_hairpin_level = [this](int kind, int value) {
        note_activity();
        (kind == 1 ? cresc_level_ : dim_level_) = juce::jlimit(0, 100, value);
        hw::ui_settings().setValue(kind == 1 ? "composer_cresc_level" : "composer_dim_level", value);
        context_.status(juce::String(kind == 1 ? "Crescendos now start at " : "Diminuendos now end at ") + juce::String(value));
    };
    t.on_accent = [this](int strength) {
        note_activity();
        const auto track = context_.session.selected_track();
        if (!track.empty()) context_.session.set_accent_strength(track, strength);
    };
    t.on_command = [this](Command c) {
        note_activity();
        perform(c);
        grabKeyboardFocus();
        update_staff_tools();
    };
    t.on_chord_mode = [this] {
        note_activity();
        set_chord_mode(!chord_mode_);
    };
    t.on_circle = [this] { composer_ui::show_circle_of_fifths(context_); };
    t.on_detach = [this] { set_staff_tools_detached(!tools_detached_); };
    t.on_drag_out = [this](juce::Point<int> top_left) {
        // Floats where it was let go - on whichever screen that is.
        set_staff_tools_detached(true);
        if (auto* w = staff_tools_window()) {
            w->setTopLeftPosition(top_left);
            const auto b = w->getBounds();
            hw::ui_settings().setValue("staff_tools_bounds", juce::String(b.getX()) + " " + juce::String(b.getY()) + " " +
                                                                 juce::String(b.getWidth()) + " " + juce::String(b.getHeight()));
        }
    };
    t.on_collapse = [this] { set_staff_tools_collapsed(true); };
}

composer_ui::StaffTools& ComposerView::staff_tools()
{
    if (!tools_) {
        tools_ = std::make_unique<composer_ui::StaffTools>(false);
        wire_tools(*tools_);
        addChildComponent(*tools_);
        tools_tab_.setComponentID("staff_tools_tab");
        tools_tab_.setTooltip("Staff tools (Cmd/Ctrl+Alt+T)");
        tools_tab_.set_legend_size(9.5f);
        tools_tab_.setWantsKeyboardFocus(false);
        tools_tab_.onClick = [this] { set_staff_tools_collapsed(false); };
        addChildComponent(tools_tab_);
        auto& settings = hw::ui_settings();
        tools_collapsed_ = settings.getBoolValue("staff_tools_collapsed", false);
        if (settings.getBoolValue("staff_tools_detached", false)) set_staff_tools_detached(true);
        last_activity_ = juce::Time::getMillisecondCounter();
    }
    return *tools_;
}

composer_ui::StaffTools* ComposerView::detached_staff_tools() { return tools_window_ ? tools_floating_ : nullptr; }

void ComposerView::note_activity()
{
    last_activity_ = juce::Time::getMillisecondCounter();
    if (tools_collapsed_ && tools_auto_folded_) {
        tools_collapsed_ = false;
        tools_auto_folded_ = false;
    }
}

void ComposerView::set_staff_tools_collapsed(bool collapsed)
{
    staff_tools();
    tools_collapsed_ = collapsed;
    tools_auto_folded_ = false;
    hw::ui_settings().setValue("staff_tools_collapsed", collapsed);
    last_activity_ = juce::Time::getMillisecondCounter();
    update_staff_tools();
}

void ComposerView::set_staff_tools_detached(bool detached)
{
    staff_tools();
    auto& settings = hw::ui_settings();
    if (detached && !tools_window_) {
        auto content = std::make_unique<composer_ui::StaffTools>(true);
        tools_floating_ = content.get();
        wire_tools(*content);
        juce::Rectangle<int> r(120, 120, 560, 120);
        const auto saved = juce::StringArray::fromTokens(settings.getValue("staff_tools_bounds"), " ", "");
        if (saved.size() == 4) r = {saved[0].getIntValue(), saved[1].getIntValue(), saved[2].getIntValue(), saved[3].getIntValue()};
        r.setHeight(std::max(r.getHeight(), content->height_for(r.getWidth()) + 30));
        auto* w = new dialogs::ToolWindow("Staff Tools", content.release(), r.getWidth(), r.getHeight());
        w->setBounds(on_a_display(r));
        w->setAlwaysOnTop(true);
        // Shown at once (it was made but never shown, so Detach seemed to lose the tools).
        w->setVisible(true);
        w->toFront(false);
        juce::Component::SafePointer<ComposerView> self(this);
        w->on_close = [self] {
            if (self) {
                self->tools_floating_ = nullptr;
                self->tools_detached_ = false;
                hw::ui_settings().setValue("staff_tools_detached", false);
            }
        };
        tools_window_ = w;
    } else if (!detached && tools_window_) {
        const auto b = tools_window_->getBounds();
        settings.setValue("staff_tools_bounds", juce::String(b.getX()) + " " + juce::String(b.getY()) + " " +
                                                    juce::String(b.getWidth()) + " " + juce::String(b.getHeight()));
        tools_floating_ = nullptr;
        delete tools_window_.getComponent();
    }
    tools_detached_ = detached;
    settings.setValue("staff_tools_detached", detached);
    update_staff_tools();
}

composer_ui::StaffToolsState ComposerView::tools_state() const
{
    composer_ui::StaffToolsState s;
    s.editable = editor_ != nullptr;
    s.chord_mode = chord_mode_;
    s.writing = note_entry_;
    s.hairpin = hairpin_drawing_;
    s.cresc_level = cresc_level_;
    s.dim_level = dim_level_;
    if (const auto* t = context_.session.data().find_track(context_.session.selected_track())) {
        s.track = t->name;
        s.accent = t->accent_strength;
    }
    if (editor_) {
        const auto& cur = editor_->cursor();
        s.has_selection = !editor_->selection().empty();
        s.value = static_cast<int>(cur.duration.value) - 1;
        s.dot = cur.duration.dots > 0;
        s.triplet = cur.duration.tuplet.is_tuplet();
        s.rest = cur.rest_mode;
        s.voice = cur.voice;
        if (working_.staff_count() > 0)
            s.pitched = working_.staff(std::min(cur.staff_index, working_.staff_count() - 1)).clef != notation::Clef::Percussion;
    }
    return s;
}

void ComposerView::update_staff_tools()
{
    auto& tools = staff_tools();
    // Folded after a while untouched (the next edit unfolds it).
    if (!tools_collapsed_ && !tools_detached_ && juce::Time::getMillisecondCounter() - last_activity_ > kToolsIdleMs) {
        tools_collapsed_ = true;
        tools_auto_folded_ = true;
    }
    const auto s = tools_state();
    auto same = [](const composer_ui::StaffToolsState& a, const composer_ui::StaffToolsState& b) {
        return a.editable == b.editable && a.has_selection == b.has_selection && a.pitched == b.pitched && a.value == b.value &&
               a.dot == b.dot && a.triplet == b.triplet && a.rest == b.rest && a.voice == b.voice &&
               a.chord_mode == b.chord_mode && a.track == b.track && a.writing == b.writing && a.accent == b.accent && a.hairpin == b.hairpin &&
               a.cresc_level == b.cresc_level && a.dim_level == b.dim_level;
    };
    // The dial is turned by hand without the state knowing: compare what it shows too
    // (undo puts the strength back; the dial must follow).
    auto dial_off = [&](composer_ui::StaffTools& t) { return juce::roundToInt(t.accent_knob().getValue()) != s.accent; };
    if (!same(s, tools.state()) || dial_off(tools)) tools.set_state(s);
    if (tools_floating_ && (!same(s, tools_floating_->state()) || dial_off(*tools_floating_))) tools_floating_->set_state(s);

    // Docked: beside the selected staff, above it when the view has room,
    // otherwise below; hidden when the staff is scrolled away.
    Strip* strip = nullptr;
    for (auto& st : strips_)
        if (st->track_id() == context_.session.selected_track()) strip = st.get();
    const bool docked = !tools_detached_ && strip != nullptr && s.editable;
    // The lanes open a gap above the selected staff for the bar (a smaller
    // one for the folded tab), so no staff is ever under the tools.
    {
        int gap = 0;
        std::string at;
        if (docked) {
            const int lane_w = strip->getWidth() - composer_ui::TrackHeader::kWidth;
            gap = tools_collapsed_ ? 24 : tools.height_for(std::min(lane_w - 16, 780)) + 6;
            at = strip->track_id();
        }
        if (gap != tools_gap_ || at != tools_gap_track_) {
            tools_gap_ = gap;
            tools_gap_track_ = at;
            resized();
        }
    }
    juce::Rectangle<int> area;
    if (docked) {
        const auto sb = getLocalArea(strip, strip->getLocalBounds());
        const auto vb = viewport_.getBounds();
        const int lane_x = sb.getX() + composer_ui::TrackHeader::kWidth;
        const int width = std::min(sb.getRight() - lane_x - 16, 780);
        const int h = tools.height_for(width);
        int y = sb.getY() - h - 3;  // in the gap above the staff
        if (sb.getBottom() < vb.getY() || y > vb.getBottom()) y = -10000;  // scrolled away
        y = std::max(y, y == -10000 ? y : vb.getY() - h + 18);
        // Centred over the music, whatever the window's width (no reaching to the right edge).
        area = {lane_x + (sb.getRight() - lane_x - width) / 2, y, width, h};
    }
    const bool show_bar = docked && !tools_collapsed_ && area.getY() > -10000;
    const bool show_tab = docked && tools_collapsed_ && area.getY() > -10000;
    if (show_bar && tools.getBounds() != area) tools.setBounds(area);
    if (tools.isVisible() != show_bar) tools.setVisible(show_bar);
    if (show_bar) tools.toFront(false);
    const auto tab = juce::Rectangle<int>(area.getCentreX() - 29, area.getBottom() - 21, 58, 20);
    if (show_tab && tools_tab_.getBounds() != tab) tools_tab_.setBounds(tab);
    if (tools_tab_.isVisible() != show_tab) tools_tab_.setVisible(show_tab);
}

// --- Chord Mode -------------------------------------------------------------------------

namespace {

/// Letter steps for `semitones` (a transposition), as an interval spells them.
juce::String note_names(const std::vector<theory::VoicedTone>& voiced)
{
    juce::String s;
    for (const auto& v : voiced) s << (s.isEmpty() ? "" : " ") << juce::String::fromUTF8(v.pitch.display_name().c_str()) << v.octave();
    return s;
}

}  // namespace

composer_ui::ChordPalette& ComposerView::chord_palette()
{
    if (!palette_) {
        palette_ = std::make_unique<composer_ui::ChordPalette>(chord_state_);
        palette_->on_insert = [this](bool replace) { enter_chord(replace); };
        palette_->on_audition = [this] { audition_chord(); };
        palette_->on_close = [this] { set_chord_mode(false); };
        palette_->on_changed = [this] {
            const auto [notes, warning] = chord_preview();
            palette_->set_preview(notes, warning);
        };
    }
    return *palette_;
}

void ComposerView::set_chord_mode(bool on)
{
    chord_mode_ = on;
    auto& p = chord_palette();
    // Always the project's key: the root keys show its scale, the borrowed row its borrowings.
    chord_state_.key = dialogs::project_key(context_);
    p.refresh();
    // Its own floating window: kept above VC Composer, movable to another screen.
    if (on) {
        if (palette_window_ == nullptr) {
            p.setVisible(true);
            auto* w = new dialogs::ToolWindow("Chord Mode", &p, composer_ui::ChordPalette::kWidth,
                                              composer_ui::ChordPalette::kHeight, false);
            w->setResizable(false, false);
            w->on_close = [self = juce::Component::SafePointer<ComposerView>(this)] {
                if (self) self->set_chord_mode(false);
            };
            w->setVisible(true);
            palette_window_ = w;
        }
        palette_window_->toFront(true);
    } else if (palette_window_ != nullptr) {
        p.setVisible(false);
        auto* w = palette_window_.getComponent();
        palette_window_ = nullptr;
        w->on_close = nullptr;
        w->clearContentComponent();
        w->closeButtonPressed();
    }
    if (on) {
        p.refresh();
        const auto [notes, warning] = chord_preview();
        p.set_preview(notes, warning);
    }
    context_.status(on ? "Chord mode: choose a chord in the palette, then click a staff (the bass goes in the octave you click)"
                       : "Chord mode off");
}

std::pair<juce::String, juce::String> ComposerView::chord_preview() const
{
    std::string problem;
    const auto chord = composer_ui::chord_for(chord_state_, &problem);
    if (!chord) return {{}, juce::String(problem)};
    auto voiced = composer_ui::voice_chord_for_staves(*chord, chord_state_, false).upper;
    juce::String warning;
    if (const auto* t = context_.session.data().find_track(editing_track_)) {
        const auto [low, high] = context_.session.range_for(*t);
        int outside = 0;
        for (const auto& v : voiced) outside += (v.midi < low || v.midi > high) ? 1 : 0;
        if (outside > 0)
            warning << outside << (outside == 1 ? " note is" : " notes are") << " outside the " << juce::String(t->name)
                    << "'s range (" << juce::MidiMessage::getMidiNoteName(low, true, true, 4) << "-"
                    << juce::MidiMessage::getMidiNoteName(high, true, true, 4) << ")";
    }
    return {note_names(voiced), warning};
}

bool ComposerView::enter_chord(bool replace_selection)
{
    if (!editor_) {
        context_.status("Select an instrument track to write the chord on");
        return false;
    }
    std::string problem;
    const auto chord = composer_ui::chord_for(chord_state_, &problem);
    if (!chord) {
        context_.status(juce::String(problem));
        return false;
    }
    // Send to a section: the chord spread over its players, a note each, at the cursor.
    if (chord_state_.send_to > 0) {
        using S = session::Session::Section;
        const S section = chord_state_.send_to == 1   ? S::Strings
                          : chord_state_.send_to == 2 ? S::Woodwinds
                          : chord_state_.send_to == 3 ? S::Brass
                                                      : S::Choir;
        const auto targets = context_.session.section_tracks(section);
        const juce::String what = section == S::Strings     ? "strings"
                                  : section == S::Woodwinds ? "woodwinds"
                                  : section == S::Brass     ? "brass"
                                                            : "choir";
        if (std::all_of(targets.begin(), targets.end(), [](const std::string& t) { return t.empty(); })) {
            context_.status("No " + what + " in this project to send the chord to");
            return false;
        }
        const auto hands = composer_ui::voice_chord_for_staves(*chord, chord_state_, true);
        std::vector<int> sounding;
        for (const auto* part : {&hands.lower, &hands.upper})
            for (const auto& v : *part) sounding.push_back(v.midi);
        const auto cur = editor_->cursor();
        const auto duration = cur.duration;
        if (!context_.session.spread_chord_at(cur.tick, duration, sounding, targets, section == S::Strings)) return false;
        auto c = editor_->cursor();
        c.tick = cur.tick + duration.ticks();
        editor_->set_cursor(c);
        apply_view_settings();
        context_.status(juce::String::fromUTF8(chord->display_name().c_str()) + " spread over the " + what + ", a note each" +
                        (section == S::Strings ? " (open)" : ""));
        return true;
    }
    // A single-line instrument (violins, flute...) never gets a chord.
    if (const auto* t = context_.session.data().find_track(editing_track_);
        t && !session::Session::plays_chords(t->instrument_standard_id)) {
        context_.status(juce::String(t->name) + " plays one line: write the chord on the piano, or choose Send to: Strings, "
                                                "Woodwinds or Brass in the chord box");
        return false;
    }
    if (replace_selection) {
        const auto sel = editor_->selection();
        const notation::ScoreNote* first = nullptr;
        for (auto id : sel)
            if (const auto* n = working_.find_note(id); n && (!first || n->start_tick < first->start_tick)) first = n;
        if (!first) {
            context_.status("Select the notes the chord replaces");
            return false;
        }
        editing::EditCursor c = editor_->cursor();
        c.tick = first->start_tick;
        c.staff_index = working_.staff_index_of(first->id);
        c.duration = first->duration;
        editor_->delete_selection();
        editor_->set_cursor(c);
    }
    const auto cur = editor_->cursor();
    auto staff_index = std::min(cur.staff_index, working_.staff_count() - 1);
    const auto* track = context_.session.data().find_track(editing_track_);
    // A grand staff (piano): the right hand on the treble staff, the left
    // hand on the bass staff, at the same place.
    const bool grand = working_.staff_count() >= 2 && working_.staff(0).clef == notation::Clef::Treble &&
                       working_.staff(1).clef == notation::Clef::Bass;
    const auto hands = composer_ui::voice_chord_for_staves(*chord, chord_state_, grand);
    const int trans = track ? track->effective_transpose() : 0;
    std::vector<std::pair<std::size_t, std::vector<notation::Pitch>>> parts;
    if (grand) {
        if (!hands.upper.empty()) parts.push_back({0, composer_ui::written_chord(hands.upper, working_.staff(0), trans)});
        if (!hands.lower.empty()) parts.push_back({1, composer_ui::written_chord(hands.lower, working_.staff(1), trans)});
    } else {
        parts.push_back({staff_index, composer_ui::written_chord(hands.upper, working_.staff(staff_index), trans)});
    }
    if (parts.empty() || parts.front().second.empty()) return false;
    const notation::Score before = working_;
    editing::EditResult r;
    bool ok = true;
    for (const auto& [staff, pitches] : parts) {
        if (pitches.empty()) continue;
        auto c = editor_->cursor();
        c.tick = cur.tick;
        c.staff_index = staff;
        editor_->set_cursor(c);
        r = editor_->insert_pitch(pitches.front());
        ok = ok && r.succeeded();
        for (std::size_t i = 1; ok && i < pitches.size(); ++i) ok = editor_->add_to_chord(pitches[i]).succeeded();
    }
    staff_index = parts.front().first;
    {
        // Entry carries on from the staff written on first (the right hand).
        auto c = editor_->cursor();
        c.staff_index = staff_index;
        editor_->set_cursor(c);
    }
    if (!ok) {
        working_ = before;  // nothing half-entered
        reload_working_copy(false);
        context_.status(juce::String("The chord could not be entered: ") + r.message);
        return false;
    }
    // Then: past the chord (as entry does), or on to the next beat or bar.
    if (chord_state_.then != 0) {
        const auto& meter = context_.session.data().meter;
        const int bar = meter.bar_at(cur.tick);
        Ticks next = meter.bar_start(bar + 1);
        if (chord_state_.then == 1) {
            const Ticks beat = std::max<Ticks>(1, meter.at_bar(bar).beat_ticks());
            const Ticks bs = meter.bar_start(bar);
            next = bs + ((cur.tick - bs) / beat + 1) * beat;
        }
        auto c = editor_->cursor();
        c.tick = next;
        editor_->set_cursor(c);
    }
    const std::string label = "Enter chord " + chord->name();
    const bool symbol = chord_state_.write_symbol;
    const std::string name = chord->name();
    const Ticks at = cur.tick;
    auto commit = [this, label, symbol, name, at] {
        committing_ = true;
        if (symbol) context_.session.commit_score_with_chord(editing_track_, working_, label, at, name);
        else context_.session.commit_score(editing_track_, working_, label);
        committing_ = false;
    };
    if (!range_ok(false, commit)) {
        apply_view_settings();
        repaint();
        return false;
    }
    commit();
    just_entered_ = true;
    apply_view_settings();
    repaint();
    const auto [notes, warning] = chord_preview();
    context_.status("Entered " + juce::String::fromUTF8(chord->display_name().c_str()) + ": " + notes +
                    (warning.isNotEmpty() ? " - " + warning : juce::String()));
    return true;
}

void ComposerView::audition_chord()
{
    const auto chord = composer_ui::chord_for(chord_state_);
    if (!chord) return;
    context_.session.select_track(editing_track_);
    const auto voiced = theory::voice_for_entry(*chord, chord_state_.octave, chord_state_.style);
    std::vector<int> notes;
    for (const auto& v : voiced) {
        context_.engine.send_live(juce::MidiMessage::noteOn(1, v.midi, static_cast<juce::uint8>(90)));
        notes.push_back(v.midi);
    }
    juce::Component::SafePointer<ComposerView> self(this);
    juce::Timer::callAfterDelay(900, [self, notes] {
        if (!self) return;
        for (int n : notes) self->context_.engine.send_live(juce::MidiMessage::noteOff(1, n));
    });
}

bool ComposerView::enter_midi(int midi, bool chord)
{
    if (!editor_) return false;
    const auto cur = editor_->cursor();
    const auto staff_index = std::min(cur.staff_index, working_.staff_count() - 1);
    const auto& staff = working_.staff(staff_index);
    const int written = midi - staff.total_transposition();
    if (written < 0 || written > 127) return false;
    const auto key = interchange::written_key(working_.key_signature_at(cur.tick), staff.transposition_semitones);
    const auto pitch = notation::spell_midi_note(written, key);
    handle(chord ? editor_->add_to_chord(pitch) : editor_->insert_pitch(pitch));
    just_entered_ = true;
    return true;
}

void ComposerView::ask_for_input(Command command)
{
    juce::PopupMenu m;
    if (command == Command::EditKeySignature) {
        // The key belongs to the project, so every staff and the Chord
        // Explorer agree (Main Spec 8; decision D12).
        for (const char* mode : {"major", "minor"})
            for (const char* tonic : {"C", "G", "D", "A", "E", "B", "F#", "F", "Bb", "Eb", "Ab", "Db"}) {
                juce::String key = juce::String(tonic) + " " + mode;
                m.addItem(key, [this, key] { context_.session.set_key(key.toStdString()); });
            }
    } else if (command == Command::EditTimeSignature) {
        for (const char* meter : {"2/4", "3/4", "4/4", "5/4", "6/8", "7/8", "9/8", "12/8", "2/2"})
            m.addItem(meter, [this, meter] {
                if (auto sig = TimeSignature::parse(meter)) context_.session.set_meter(*sig);
            });
    } else if (command == Command::EditClef && editor_) {
        const std::pair<const char*, notation::Clef> clefs[] = {
            {"Treble", notation::Clef::Treble}, {"Bass", notation::Clef::Bass}, {"Alto", notation::Clef::Alto},
            {"Tenor", notation::Clef::Tenor}, {"Treble 8vb", notation::Clef::TrebleOctaveDown},
            {"Percussion", notation::Clef::Percussion}};
        for (const auto& c : clefs) {
            const notation::Clef clef = c.second;
            m.addItem(c.first, [this, clef] {
                if (editor_) handle(editor_->set_clef(editor_->cursor().staff_index, clef));
            });
        }
    } else if (command == Command::EditTempo || command == Command::AddText) {
        const bool tempo = command == Command::EditTempo;
        auto* w = new juce::AlertWindow(tempo ? "Tempo" : "Text", tempo ? "Beats per minute" : "Text at the cursor",
                                        juce::MessageBoxIconType::NoIcon);
        w->addTextEditor("value", tempo ? juce::String(context_.session.data().tempo.bpm_at(0)) : juce::String());
        w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        w->enterModalState(true, juce::ModalCallbackFunction::create([this, w, tempo](int r) {
            if (r == 1) {
                const auto value = w->getTextEditorContents("value");
                if (tempo) context_.session.set_tempo(value.getDoubleValue());
                else if (editor_) handle(editor_->add_text(editor_->cursor().tick, value.toStdString()));
            }
        }), true);
        return;
    }
    m.showMenuAsync({});
}

bool ComposerView::keyPressed(const juce::KeyPress& key)
{
    note_activity();
    if (key == juce::KeyPress::escapeKey && hairpin_drawing_ != 0) {
        set_hairpin_drawing(0);
        return true;
    }
    if (key == juce::KeyPress::spaceKey) {
        context_.engine.toggle_play();
        return true;
    }
    // Delete (or Backspace) with notes selected - a box, a click, Select All: they go.
    if ((key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) && editor_ && !editor_->selection().empty()) {
        handle(editor_->delete_selection());
        return true;
    }
    composer::KeyStroke stroke;
    if (!ui::key_stroke_from(key, stroke)) return false;
    notation::Step step = notation::Step::C;
    // Typing writes in either mode (Select governs only what a click does).
    if (editor_ && stroke.modifiers == 0 && ui::step_from_key(stroke, step)) {
        handle(editor_->insert_step(step));
        just_entered_ = true;
        return true;
    }
    if (editor_ && stroke.modifiers == composer::to_mask(composer::Modifier::Shift) &&
        ui::step_from_key(stroke, step)) {
        handle(editor_->add_step_to_chord(step));
        just_entered_ = true;
        return true;
    }
    const Command command = context_.shortcuts.command_for(stroke);
    if (command == Command::None) return false;
    perform(command);
    return true;
}

void ComposerView::paint(juce::Graphics& g) { g.fillAll(hw::tok::chassis); }

void ComposerView::resized()
{
    auto r = getLocalBounds();
    ruler_->setBounds(r.removeFromTop(composer_ui::ComposerRuler::kHeight));
    {
        // The bottom row: the hint, then the zoom sliders at the right -
        // width (how much music across) and height (how big the staves are).
        auto row = r.removeFromBottom(18);
        zoom_h_.setBounds(row.removeFromRight(110).reduced(2, 1));
        zoom_h_label_.setBounds(row.removeFromRight(16));
        row.removeFromRight(8);
        zoom_w_.setBounds(row.removeFromRight(110).reduced(2, 1));
        zoom_w_label_.setBounds(row.removeFromRight(16));
        hint_.setBounds(row.withTrimmedLeft(8));
    }
    hscroll_.setBounds(r.removeFromBottom(12).withTrimmedLeft(composer_ui::TrackHeader::kWidth));
    viewport_.setBounds(r);
    int y = 0;
    const int w = viewport_.getMaximumVisibleWidth();
    const auto options = lane_layout();
    for (auto& s : strips_) {
        if (tools_gap_ > 0 && s->track_id() == tools_gap_track_) y += tools_gap_;  // the docked tools' place
        // Laid out at the lane's width first, the lane then grows to what is
        // drawn on it (ledger notes, dynamics, articulations): nothing cut off.
        if (s->getWidth() != w) s->setSize(w, s->preferred_height(pixels_per_space_, options));
        const double room = lane_room(s->track_id());
        s->view().fit_margins_to_content(options.margin_above + room, options.margin_below + room);
        const int h = s->preferred_height(pixels_per_space_, s->view().layout_options());
        s->setBounds(0, y, w, h);
        y += h;
    }
    strips_holder_.setSize(w, std::max(y, viewport_.getHeight()));
    align_clef_zones();

    const double end = static_cast<double>(context_.session.content_end());
    hscroll_.setRangeLimits(0.0, end + static_cast<double>(kTicksPerWhole) * 4, juce::dontSendNotification);
    hscroll_.setCurrentRange(static_cast<double>(origin_), static_cast<double>(visible_ticks()), juce::dontSendNotification);
}

// --- Box selection and Send to Track -------------------------------------------------------------

void ComposerView::box_selected(const std::string& track_id, std::vector<notation::EventId> ids, bool add)
{
    if (track_id != editing_track_) context_.session.select_track(track_id);
    if (!editor_ || track_id != editing_track_) return;
    // Rests stay out: a box selects the music.
    ids.erase(std::remove_if(ids.begin(), ids.end(),
                             [&](notation::EventId id) {
                                 const auto* n = working_.find_note(id);
                                 return n == nullptr || n->is_rest;
                             }),
              ids.end());
    if (add) {
        auto mine = editor_->selection();
        for (auto id : ids)
            if (std::find(mine.begin(), mine.end(), id) == mine.end()) mine.push_back(id);
        ids = std::move(mine);
    }
    editor_->set_selection(ids);
    publish_selection();
    for (auto& s : strips_)
        if (s->track_id() == editing_track_) s->view().set_selection(editor_->selection());
    repaint();
    context_.status(ids.empty() ? juce::String("No notes in the box")
                                : juce::String(static_cast<int>(ids.size())) +
                                      " notes selected: Delete removes them; right-click one to send them to another track");
}

int ComposerView::send_selected_notes(const std::string& target_track)
{
    if (!editor_ || target_track.empty() || target_track == editing_track_) return 0;
    auto& session = context_.session;
    const auto* source = session.data().find_track(editing_track_);
    if (!source) return 0;
    // Written pitches back to what sounds (a B-flat clarinet's written D sounds C).
    int shift = 0;
    if (const auto* def = session.instrument_for(*source)) shift = def->transposition;
    std::vector<NoteEvent> events;
    Ticks from = std::numeric_limits<Ticks>::max(), to = 0;
    for (auto id : editor_->selection()) {
        const auto* n = working_.find_note(id);
        if (!n || n->is_rest) continue;
        const Ticks end = n->end_tick();
        events.push_back({n->start_tick, std::max<Ticks>(1, end - n->start_tick),
                          juce::jlimit(0, 127, notation::midi_note_number(n->pitch) + shift), 90, 0, {}});
        from = std::min(from, n->start_tick);
        to = std::max(to, end);
    }
    if (events.empty()) return 0;
    std::sort(events.begin(), events.end(), [](const NoteEvent& a, const NoteEvent& b) {
        return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
    });
    const auto result =
        session.insert_notes(target_track, from, to, events, project::CollisionPolicy::Merge, "Sent from " + source->name);
    const auto* target = session.data().find_track(target_track);
    if (!result.ok) {
        context_.status("Could not send the notes: " + juce::String(result.error));
        return 0;
    }
    context_.status(juce::String(static_cast<int>(events.size())) + " notes sent to " +
                    juce::String(target ? target->name : std::string()) + " (Undo takes them back)");
    return static_cast<int>(events.size());
}

namespace {

/// The tracks to send notes to, in a list, with Send and Cancel.
class SendNotesPanel final : public juce::Component, private juce::ListBoxModel {
public:
    SendNotesPanel(std::vector<std::pair<std::string, juce::String>> tracks, int count, std::function<void(const std::string&)> send)
        : tracks_(std::move(tracks)), send_(std::move(send))
    {
        setComponentID("send_notes_dialog");
        caption_.setText("Send " + juce::String(count) + (count == 1 ? " note" : " notes") +
                             " to (the same bars and pitches, written as notation there):",
                         juce::dontSendNotification);
        caption_.setColour(juce::Label::textColourId, colours::text);
        addAndMakeVisible(caption_);
        list_.setModel(this);
        list_.setRowHeight(28);
        list_.setColour(juce::ListBox::backgroundColourId, colours::panel);
        list_.setColour(juce::ListBox::outlineColourId, colours::outline);
        list_.setOutlineThickness(1);
        addAndMakeVisible(list_);
        send_key_.setColour(juce::TextButton::buttonColourId, colours::accent);
        send_key_.onClick = [this] { go(); };
        cancel_.onClick = [this] { close(); };
        addAndMakeVisible(send_key_);
        addAndMakeVisible(cancel_);
        if (!tracks_.empty()) list_.selectRow(0);
        setSize(420, 440);
    }
    ~SendNotesPanel() override { list_.setModel(nullptr); }
    void paint(juce::Graphics& g) override { g.fillAll(colours::background); }
    void resized() override
    {
        auto r = getLocalBounds().reduced(12);
        caption_.setBounds(r.removeFromTop(36));
        auto b = r.removeFromBottom(30);
        send_key_.setBounds(b.removeFromRight(100));
        b.removeFromRight(8);
        cancel_.setBounds(b.removeFromRight(90));
        r.removeFromBottom(8);
        list_.setBounds(r);
    }

private:
    int getNumRows() override { return static_cast<int>(tracks_.size()); }
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= getNumRows()) return;
        g.fillAll(selected ? colours::accent.withAlpha(0.3f) : colours::panel);
        g.setColour(colours::text);
        g.setFont(juce::FontOptions(14.0f));
        g.drawText(tracks_[static_cast<std::size_t>(row)].second, 10, 0, w - 16, h, juce::Justification::centredLeft, true);
    }
    void listBoxItemDoubleClicked(int, const juce::MouseEvent&) override { go(); }
    void go()
    {
        const int r = list_.getSelectedRow();
        if (r < 0 || r >= getNumRows()) return;
        const auto id = tracks_[static_cast<std::size_t>(r)].first;
        auto send = send_;
        close();
        if (send) send(id);
    }
    void close()
    {
        if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState(0);
    }

    std::vector<std::pair<std::string, juce::String>> tracks_;
    std::function<void(const std::string&)> send_;
    juce::Label caption_;
    juce::ListBox list_;
    juce::TextButton send_key_{"Send"}, cancel_{"Cancel"};
};

}  // namespace

void ComposerView::ask_send_selected_notes()
{
    if (!editor_) return;
    int count = 0;
    for (auto id : editor_->selection())
        if (const auto* n = working_.find_note(id); n && !n->is_rest) ++count;
    if (count == 0) {
        context_.status("Select notes first: drag a box over them, or click one");
        return;
    }
    if (send_target_hook) {  // the checks
        send_selected_notes(send_target_hook());
        return;
    }
    std::vector<std::pair<std::string, juce::String>> tracks;
    for (const auto* t : context_.session.composer_tracks())
        if (t->id != editing_track_ && t->kind == project::TrackKind::Instrument)
            tracks.push_back({t->id, juce::String::fromUTF8(t->name.c_str())});
    juce::Component::SafePointer<ComposerView> self(this);
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(new SendNotesPanel(std::move(tracks), count, [self](const std::string& id) {
        if (self) self->send_selected_notes(id);
    }));
    o.dialogTitle = "Send Notes to Track";
    o.dialogBackgroundColour = colours::background;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = false;
    o.resizable = false;
    o.launchAsync();
}

// --- The hidden entry grid ------------------------------------------------------------------------

Ticks ComposerView::snap_entry_tick(Ticks raw, std::size_t staff) const
{
    if (!editor_ || staff >= working_.staff_count()) return raw;
    const auto& cur = editor_->cursor();
    Ticks grid = cur.duration.is_valid() ? cur.duration.ticks() : kTicksPerQuarter;
    if (grid <= 0) grid = kTicksPerQuarter;
    const auto bar = working_.measure_at(std::max<Ticks>(0, raw));
    const Ticks start = bar.start_tick, length = bar.length_ticks > 0 ? bar.length_ticks : 4 * kTicksPerQuarter;
    grid = std::min(grid, length);
    const int slots = std::max<int>(1, static_cast<int>(length / grid));
    // What is written in this bar (rests are room to write in).
    bool blank = true;
    auto taken = [&](int slot) {
        const Ticks a = start + slot * grid, b = a + grid;
        for (const auto& n : working_.notes(staff))
            if (!n.is_rest && !n.grace && n.voice == cur.voice && n.start_tick < b && n.end_tick() > a) return true;
        return false;
    };
    for (const auto& n : working_.notes(staff))
        if (!n.is_rest && n.start_tick < start + length && n.end_tick() > start) blank = false;
    if (blank) return start;  // a blank bar: beat 1
    const int under = std::clamp(static_cast<int>((raw - start) / grid), 0, slots - 1);
    if (!taken(under)) return start + under * grid;
    // The nearest empty slot (later first on a tie); none empty: the one clicked.
    for (int d = 1; d < slots; ++d) {
        if (under + d < slots && !taken(under + d)) return start + (under + d) * grid;
        if (under - d >= 0 && !taken(under - d)) return start + (under - d) * grid;
    }
    return start + under * grid;
}

}  // namespace vcc::desktop
