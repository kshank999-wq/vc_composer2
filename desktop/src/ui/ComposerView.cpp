#include "ui/ComposerView.h"

#include <algorithm>

#include "ui/Theme.h"
#include "ui/TrackPanel.h"
#include "vcc/ui/KeyMapping.h"

namespace vcc::desktop {

using composer::Command;

namespace {
constexpr int kHeaderWidth = 150;

juce::String command_label(Command c)
{
    switch (c) {
        case Command::SelectDurationWhole: return "Whole";
        case Command::SelectDurationHalf: return "Half";
        case Command::SelectDurationQuarter: return "Quarter";
        case Command::SelectDurationEighth: return "8th";
        case Command::SelectDurationSixteenth: return "16th";
        case Command::ToggleDot: return "Dot";
        case Command::ToggleRest: return "Rest";
        case Command::ToggleTie: return "Tie";
        case Command::ToggleTuplet: return "3:2";
        case Command::AccidentalSharp: return juce::String::fromUTF8("\xe2\x99\xaf");
        case Command::AccidentalFlat: return juce::String::fromUTF8("\xe2\x99\xad");
        case Command::AccidentalNatural: return juce::String::fromUTF8("\xe2\x99\xae");
        case Command::ArticulationStaccato: return "Stacc.";
        case Command::ArticulationAccent: return "Accent";
        case Command::ArticulationTenuto: return "Ten.";
        case Command::ArticulationMarcato: return "Marc.";
        case Command::DynamicPianissimo: return "pp";
        case Command::DynamicPiano: return "p";
        case Command::DynamicMezzoPiano: return "mp";
        case Command::DynamicMezzoForte: return "mf";
        case Command::DynamicForte: return "f";
        case Command::DynamicFortissimo: return "ff";
        case Command::StartCrescendo: return "cresc.";
        case Command::StartDiminuendo: return "dim.";
        case Command::StartSlur: return "Slur";
        default: return "?";
    }
}
}  // namespace

// --- Strip: one track's staves -------------------------------------------------------------

class ComposerView::Strip final : public juce::Component {
public:
    Strip(ComposerView& owner, std::string track_id) : owner_(owner), track_id_(std::move(track_id))
    {
        addAndMakeVisible(view_);
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

    int preferred_height(double pixels_per_space) const
    {
        const auto* score = view_.score();
        const int staves = score ? static_cast<int>(score->staff_count()) : 1;
        return static_cast<int>(pixels_per_space * (6.0 + 12.0 * staves));
    }

    void paint(juce::Graphics& g) override
    {
        const auto* t = owner_.context_.session.data().find_track(track_id_);
        const bool selected = owner_.context_.session.selected_track() == track_id_;
        auto header = getLocalBounds().removeFromLeft(kHeaderWidth);
        g.setColour(selected ? colours::panel_raised : colours::panel);
        g.fillRect(header);
        if (!t) return;
        g.setColour(colour_from_hex(t->color));
        g.fillRect(header.removeFromLeft(4));
        g.setColour(selected ? colours::text : colours::text_dim);
        g.setFont(juce::FontOptions(13.0f, selected ? juce::Font::bold : juce::Font::plain));
        g.drawFittedText(t->name, header.reduced(8, 6).removeFromTop(36), juce::Justification::topLeft, 2);
        if (const auto* v = t->active_version()) {
            g.setFont(juce::FontOptions(11.0f));
            g.setColour(colours::text_dim);
            g.drawText(v->name, header.reduced(8, 6).removeFromBottom(16), juce::Justification::bottomLeft);
        }
        if (selected) {
            g.setColour(colours::accent);
            g.drawRect(getLocalBounds(), 1);
        }
    }

    void resized() override { view_.setBounds(getLocalBounds().withTrimmedLeft(kHeaderWidth)); }

    void mouseDown(const juce::MouseEvent& e) override
    {
        owner_.context_.session.select_track(track_id_);
        if (e.mods.isPopupMenu()) TrackPanel::show_track_menu(owner_.context_, track_id_);
    }

private:
    ComposerView& owner_;
    std::string track_id_;
    ui::NotationView view_;
};

// --- Palette: note entry with the mouse ------------------------------------------------------

class ComposerView::Palette final : public juce::Component {
public:
    explicit Palette(ComposerView& owner) : owner_(owner)
    {
        entry_.setClickingTogglesState(true);
        entry_.setToggleState(true, juce::dontSendNotification);
        entry_.setTooltip("Note entry: letters A-G enter notes, Shift+letter adds to a chord (N toggles)");
        entry_.onClick = [this] { owner_.perform(Command::ToggleKeyboardEntryMode); };
        addAndMakeVisible(entry_);
        const Command groups[][7] = {
            {Command::SelectDurationWhole, Command::SelectDurationHalf, Command::SelectDurationQuarter,
             Command::SelectDurationEighth, Command::SelectDurationSixteenth, Command::ToggleDot, Command::ToggleTuplet},
            {Command::ToggleRest, Command::ToggleTie, Command::AccidentalSharp, Command::AccidentalFlat,
             Command::AccidentalNatural, Command::StartSlur, Command::None},
            {Command::ArticulationStaccato, Command::ArticulationAccent, Command::ArticulationTenuto,
             Command::ArticulationMarcato, Command::None, Command::None, Command::None},
            {Command::DynamicPianissimo, Command::DynamicPiano, Command::DynamicMezzoPiano, Command::DynamicMezzoForte,
             Command::DynamicForte, Command::DynamicFortissimo, Command::None},
            {Command::StartCrescendo, Command::StartDiminuendo, Command::None, Command::None, Command::None,
             Command::None, Command::None},
        };
        for (const auto& group : groups) {
            for (Command c : group) {
                if (c == Command::None) continue;
                auto b = std::make_unique<juce::TextButton>(command_label(c));
                b->onClick = [this, c] { owner_.perform(c); };
                b->setWantsKeyboardFocus(false);
                addAndMakeVisible(*b);
                buttons_.push_back(std::move(b));
            }
            buttons_.push_back(nullptr);  // group gap
        }
    }

    void set_entry(bool on) { entry_.setToggleState(on, juce::dontSendNotification); }

    void paint(juce::Graphics& g) override { g.fillAll(colours::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced(6, 4);
        entry_.setBounds(r.removeFromLeft(96));
        r.removeFromLeft(12);
        for (auto& b : buttons_) {
            if (!b) {
                r.removeFromLeft(10);
                continue;
            }
            const int w = juce::jmax(30, b->getBestWidthForHeight(r.getHeight()) - 8);
            b->setBounds(r.removeFromLeft(w));
            r.removeFromLeft(3);
        }
    }

private:
    ComposerView& owner_;
    juce::TextButton entry_{"Note entry"};
    std::vector<std::unique_ptr<juce::TextButton>> buttons_;
};

// --- ComposerView -----------------------------------------------------------------------------

ComposerView::ComposerView(AppContext& context) : context_(context)
{
    setWantsKeyboardFocus(true);
    palette_ = std::make_unique<Palette>(*this);
    addAndMakeVisible(*palette_);
    addAndMakeVisible(viewport_);
    viewport_.setViewedComponent(&strips_holder_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(hscroll_);
    hscroll_.setAutoHide(false);
    hscroll_.addListener(this);
    addAndMakeVisible(hint_);
    hint_.setColour(juce::Label::textColourId, colours::text_dim);
    hint_.setFont(juce::FontOptions(12.0f));
    listener_ = context_.session.add_listener([this](session::Change c) {
        if (committing_) return;
        if (c == session::Change::Document || c == session::Change::Selection) rebuild();
    });
    rebuild();
}

ComposerView::~ComposerView() { context_.session.remove_listener(listener_); }

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
    }
    const auto* selected = context_.session.data().find_track(context_.session.selected_track());
    const auto* version = selected ? selected->active_version() : nullptr;
    const bool new_track = !selected || selected->id != editing_track_ || !version || version->id != editing_version_;
    reload_working_copy(new_track);
    apply_view_settings();
    resized();
    repaint();
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
                      (note_entry_ ? "  -  type A-G to enter notes, 1-7 for durations, Space to play" : ""),
                  juce::dontSendNotification);
}

void ComposerView::apply_view_settings()
{
    for (auto& s : strips_) {
        const auto* t = context_.session.data().find_track(s->track_id());
        const auto* v = t ? t->active_version() : nullptr;
        auto& view = s->view();
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
        view.set_horizontal_zoom(spaces_per_quarter_);
        view.set_origin_tick(origin_);
        view.score_changed();
    }
}

Ticks ComposerView::visible_ticks() const
{
    return strips_.empty() ? kTicksPerWhole * 8 : strips_.front()->view().visible_ticks();
}

void ComposerView::set_origin(Ticks tick)
{
    origin_ = std::max<Ticks>(0, tick);
    for (auto& s : strips_) s->view().set_origin_tick(origin_);
    hscroll_.setCurrentRangeStart(static_cast<double>(origin_), juce::dontSendNotification);
}

void ComposerView::zoom(bool larger)
{
    pixels_per_space_ = juce::jlimit(4.0, 16.0, pixels_per_space_ * (larger ? 1.15 : 1.0 / 1.15));
    apply_view_settings();
    resized();
}

void ComposerView::strip_clicked(Strip& strip, const engraving::InsertionPoint* point, notation::EventId id)
{
    grabKeyboardFocus();
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
            palette_->set_entry(note_entry_);
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
        return true;
    }
    if (editor_ && note_entry_ && stroke.modifiers == composer::to_mask(composer::Modifier::Shift) &&
        ui::step_from_key(stroke, step)) {
        handle(editor_->add_step_to_chord(step));
        return true;
    }
    const Command command = context_.shortcuts.command_for(stroke);
    if (command == Command::None) return false;
    perform(command);
    return true;
}

void ComposerView::paint(juce::Graphics& g) { g.fillAll(colours::paper); }

void ComposerView::resized()
{
    auto r = getLocalBounds();
    palette_->setBounds(r.removeFromTop(34));
    hint_.setBounds(r.removeFromBottom(20).withTrimmedLeft(8));
    hscroll_.setBounds(r.removeFromBottom(14).withTrimmedLeft(kHeaderWidth));
    viewport_.setBounds(r);
    int y = 0;
    const int w = viewport_.getMaximumVisibleWidth();
    for (auto& s : strips_) {
        const int h = s->preferred_height(pixels_per_space_);
        s->setBounds(0, y, w, h);
        y += h + 2;
    }
    strips_holder_.setSize(w, std::max(y, viewport_.getHeight()));

    const double end = static_cast<double>(context_.session.content_end());
    hscroll_.setRangeLimits(0.0, end + static_cast<double>(kTicksPerWhole) * 4, juce::dontSendNotification);
    hscroll_.setCurrentRange(static_cast<double>(origin_), static_cast<double>(visible_ticks()), juce::dontSendNotification);
}

}  // namespace vcc::desktop
