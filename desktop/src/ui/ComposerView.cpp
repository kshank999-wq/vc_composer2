#include "ui/ComposerView.h"

#include <algorithm>

#include <cmath>

#include "AppResources.h"
#include "ui/Theme.h"
#include "ui/TrackPanel.h"
#include "ui/composer/ComposerRuler.h"
#include "ui/composer/LaneParts.h"
#include "ui/hw/HwControls.h"
#include "vcc/ui/KeyMapping.h"

namespace vcc::desktop {

using composer::Command;


// --- Strip: one track's row: header, lane, phrase blocks ------------------------------------

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
        view_.on_zoom_requested = [this](bool larger) { owner_.zoom(larger); };
        view_.on_scrolled = [this](Ticks origin) { owner_.set_origin(origin); };
    }

    const std::string& track_id() const noexcept { return track_id_; }
    ui::NotationView& view() noexcept { return view_; }
    composer_ui::TrackHeader& header() noexcept { return header_; }
    composer_ui::PhraseLayer& phrases() noexcept { return phrases_; }

    /// The spec's row: 108 px for one staff and 184 px for a grand staff at
    /// 100%, never shorter than the header needs.
    int preferred_height(double pixels_per_space, const engraving::LayoutOptions& o) const
    {
        const auto* score = view_.score();
        const int staves = score ? std::max(1, static_cast<int>(score->staff_count())) : 1;
        const double spaces = o.margin_above + o.margin_below + 4.0 * staves + o.staff_gap * (staves - 1);
        return std::max(100, static_cast<int>(std::lround(pixels_per_space * spaces)));
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(hw::tok::edge);
        g.drawHorizontalLine(getHeight() - 1, 0.0f, static_cast<float>(getWidth()));
    }

    void paintOverChildren(juce::Graphics& g) override
    {
        // The selected track's lane is outlined in the LED colour.
        if (owner_.context_.session.selected_track() != track_id_) return;
        g.setColour(hw::Theme::get().led().withAlpha(0.55f));
        g.drawRect(getLocalBounds().withTrimmedLeft(composer_ui::TrackHeader::kWidth), 1);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        header_.setBounds(r.removeFromLeft(composer_ui::TrackHeader::kWidth));
        view_.setBounds(r.withTrimmedBottom(1));
        phrases_.setBounds(view_.getBounds());
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
        const double px_per_tick = pixels_per_space_ * kSpacesPerQuarter / static_cast<double>(kTicksPerQuarter);
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
    auto db = [](float lin) { return lin > 0.0f ? 20.0f * std::log10(lin) : -100.0f; };
    for (auto& s : strips_) {
        const auto m = context_.engine.take_deck_meter(s->track_id());
        s->header().push_meter(db(m.left), db(m.right));
    }
}

void ComposerView::rebuild()
{
    const auto tracks = context_.session.music_tracks();
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
                      (note_entry_ ? "  -  type A-G to enter notes, 5 quarter / 4 eighth / 6 half for durations, Space to play" : ""),
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
        if (s->track_id() == editing_track_ && editor_) {
            view.set_score(&working_);
            view.set_cursor(editor_->cursor().tick, editor_->cursor().staff_index, note_entry_);
            view.set_selection(editor_->selection());
        } else {
            view.set_score(v ? &v->score : nullptr);
            view.set_cursor(0, 0, false);
            view.set_selection({});
        }
        view.set_vertical_zoom(pixels_per_space_);
        view.set_horizontal_zoom(kSpacesPerQuarter);
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

void ComposerView::zoom(bool larger)
{
    pixels_per_space_ = juce::jlimit(4.0, 16.0, pixels_per_space_ * (larger ? 1.15 : 1.0 / 1.15));
    apply_view_settings();
    resized();
    ruler_->repaint();
    if (context_.status)
        context_.status("Zoom " + juce::String(juce::roundToInt(100.0 * pixels_per_space_ / kSpaceAt100)) + "%");
}

void ComposerView::strip_clicked(Strip& strip, const engraving::InsertionPoint* point, notation::EventId id)
{
    grabKeyboardFocus();
    just_entered_ = false;
    context_.session.select_track(strip.track_id());
    if (!editor_ || strip.track_id() != editing_track_) return;
    if (id != notation::kInvalidEventId) {
        handle(editor_->select_and_follow(id));
        return;
    }
    if (point && point->valid) {
        editing::EditCursor c = editor_->cursor();
        c.tick = point->tick;
        c.staff_index = point->staff_index;
        editor_->set_cursor(c);
        if (note_entry_) handle(editor_->insert_pitch(point->pitch));
        else apply_view_settings();
    }
}

void ComposerView::set_playhead(Ticks tick, bool playing)
{
    for (auto& s : strips_) s->view().set_playhead(tick, playing || tick > 0);
    ruler_->set_playhead(tick);
    if (playing) {
        const Ticks visible = visible_ticks();
        if (tick < origin_ || tick > origin_ + visible * 9 / 10) set_origin(tick - visible / 10);
    }
}

void ComposerView::handle(const editing::EditResult& result)
{
    if (result.score_changed && !editing_track_.empty()) {
        committing_ = true;
        context_.session.commit_score(editing_track_, working_, result.message);
        committing_ = false;
    }
    if (result.status == editing::EditStatus::Refused || result.status == editing::EditStatus::NotHandled)
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
        case Command::ToggleKeyboardEntryMode:
            note_entry_ = !note_entry_;
            reload_working_copy(false);
            apply_view_settings();
            context_.status(note_entry_ ? "Note entry on" : "Note entry off");
            return;
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
    // Right after typing a note, a value, dot, triplet or rest is for the
    // next note, as in Finale and Sibelius; with notes the user selected, it
    // changes them.
    const bool for_next = command == Command::SelectDurationBreve || command == Command::SelectDurationWhole ||
                          command == Command::SelectDurationHalf || command == Command::SelectDurationQuarter ||
                          command == Command::SelectDurationEighth || command == Command::SelectDurationSixteenth ||
                          command == Command::SelectDurationThirtySecond || command == Command::SelectDurationSixtyFourth ||
                          command == Command::ToggleDot || command == Command::ToggleDoubleDot ||
                          command == Command::ToggleTuplet || command == Command::ToggleRest;
    if (for_next && just_entered_ && note_entry_) {
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
    if (key == juce::KeyPress::spaceKey) {
        context_.engine.toggle_play();
        return true;
    }
    composer::KeyStroke stroke;
    if (!ui::key_stroke_from(key, stroke)) return false;
    notation::Step step = notation::Step::C;
    if (editor_ && note_entry_ && stroke.modifiers == 0 && ui::step_from_key(stroke, step)) {
        handle(editor_->insert_step(step));
        just_entered_ = true;
        return true;
    }
    if (editor_ && note_entry_ && stroke.modifiers == composer::to_mask(composer::Modifier::Shift) &&
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
    hint_.setBounds(r.removeFromBottom(18).withTrimmedLeft(8));
    hscroll_.setBounds(r.removeFromBottom(12).withTrimmedLeft(composer_ui::TrackHeader::kWidth));
    viewport_.setBounds(r);
    int y = 0;
    const int w = viewport_.getMaximumVisibleWidth();
    const auto options = lane_layout();
    for (auto& s : strips_) {
        const int h = s->preferred_height(pixels_per_space_, options);
        s->setBounds(0, y, w, h);
        y += h;
    }
    strips_holder_.setSize(w, std::max(y, viewport_.getHeight()));
    align_clef_zones();

    const double end = static_cast<double>(context_.session.content_end());
    hscroll_.setRangeLimits(0.0, end + static_cast<double>(kTicksPerWhole) * 4, juce::dontSendNotification);
    hscroll_.setCurrentRange(static_cast<double>(origin_), static_cast<double>(visible_ticks()), juce::dontSendNotification);
}

}  // namespace vcc::desktop
