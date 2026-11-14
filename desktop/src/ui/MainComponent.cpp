#include "ui/MainComponent.h"

#include "ui/hw/HwGallery.h"
#include "ui/hw/HwTheme.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include "ui/CueManager.h"
#include "ui/Dialogs.h"
#include "ui/ScoreExchange.h"
#include "ui/StemExport.h"
#include "ui/VideoWindow.h"
#include "ui/WaveformCache.h"
#include "vcc/picture/Picture.h"
#include "ui/Theme.h"
#include "vcc/core/Files.h"
#include "vcc/engraving/PrintLayout.h"
#include "vcc/engraving/SvgRenderer.h"
#include "vcc/project/Automation.h"
#include "vcc/project/Render.h"

namespace vcc::desktop {

// --- Plug-in windows --------------------------------------------------------------------------

class MainComponent::PluginWindow final : public juce::DocumentWindow {
public:
    /// An instrument's window (`slot` < 0) or an effect insert's.
    PluginWindow(AppContext& c, std::string owner_id, int slot, juce::AudioPluginInstance& instance)
        : juce::DocumentWindow(instance.getName(), colours::background, juce::DocumentWindow::closeButton),
          context_(c), owner_id_(std::move(owner_id)), slot_(slot), instance_(instance)
    {
        setUsingNativeTitleBar(true);
        juce::AudioProcessorEditor* editor = instance.hasEditor() ? instance.createEditorIfNeeded() : nullptr;
        if (editor == nullptr) editor = new juce::GenericAudioProcessorEditor(instance);
        setContentOwned(editor, true);
        setResizable(editor->isResizable(), false);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    ~PluginWindow() override { store(); }

    /// The plug-in's settings go into the project (no undo step).
    void store()
    {
        const auto state = PluginHost::state_of(instance_);
        if (slot_ < 0) context_.session.store_plugin_state(owner_id_, state);
        else context_.session.store_insert_plugin_state(owner_id_, slot_, state);
    }

    void closeButtonPressed() override
    {
        store();
        setVisible(false);
    }

    juce::AudioPluginInstance* instance() noexcept { return &instance_; }
    const std::string& owner() const noexcept { return owner_id_; }
    int slot() const noexcept { return slot_; }

private:
    AppContext& context_;
    std::string owner_id_;
    int slot_;
    juce::AudioPluginInstance& instance_;
};

/// The window for a built-in processor in an insert slot: a control for
/// each of its settings, the same parameters automation lanes use. Each
/// drag is one undo step.
class MainComponent::BuiltinInsertWindow final : public juce::DocumentWindow {
public:
    BuiltinInsertWindow(AppContext& c, std::string owner_id, int slot, const juce::String& title)
        : juce::DocumentWindow(title, colours::background, juce::DocumentWindow::closeButton),
          context_(c), owner_id_(std::move(owner_id)), slot_(slot)
    {
        setUsingNativeTitleBar(true);
        auto* content = new Content(*this);
        setContentOwned(content, true);
        centreWithSize(content->getWidth(), content->getHeight());
        setVisible(true);
    }
    void closeButtonPressed() override { setVisible(false); }
    const std::string& owner() const noexcept { return owner_id_; }
    int slot() const noexcept { return slot_; }
    std::string kind() const noexcept { return kind_; }
    /// The document changed (an undo, automation, another view): follow it.
    void refresh()
    {
        if (auto* content = dynamic_cast<Content*>(getContentComponent())) content->refresh();
    }

private:
    struct Row {
        project::ParamInfo info;
        std::unique_ptr<juce::Label> label;
        std::unique_ptr<juce::Slider> slider;
    };

    class Content final : public juce::Component {
    public:
        explicit Content(BuiltinInsertWindow& w) : window_(w)
        {
            const auto& d = w.context_.session.data();
            std::vector<project::ParamInfo> params;
            if (const auto* t = d.find_track(w.owner_id_)) {
                params = project::automatable_params(*t, d);
                if (w.slot_ < static_cast<int>(t->inserts.size())) w.kind_ = t->inserts[static_cast<size_t>(w.slot_)].kind;
            } else if (const auto* b = d.find_bus(w.owner_id_)) {
                params = project::automatable_params(*b, d);
                if (w.slot_ < static_cast<int>(b->inserts.size())) w.kind_ = b->inserts[static_cast<size_t>(w.slot_)].kind;
            }
            const std::string prefix = "insert:" + std::to_string(w.slot_) + ":";
            for (const auto& p : params) {
                if (p.id.rfind(prefix, 0) != 0) continue;
                Row row;
                row.info = p;
                // "compressor 1 Threshold" -> "Threshold"
                row.label = std::make_unique<juce::Label>(juce::String(), juce::String(p.name).fromFirstOccurrenceOf(
                                                                              juce::String(w.slot_ + 1) + " ", false, false));
                row.label->setColour(juce::Label::textColourId, colours::text_dim);
                addAndMakeVisible(*row.label);
                row.slider = std::make_unique<juce::Slider>(juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
                row.slider->setRange(p.min, p.max, p.toggle ? 1.0 : 0.0);
                if (p.log_scale && p.min > 0.0) row.slider->setSkewFactorFromMidPoint(std::sqrt(p.min * p.max));
                row.slider->setTextValueSuffix(p.unit.empty() ? juce::String() : " " + juce::String(p.unit));
                row.slider->setNumDecimalPlacesToDisplay(p.unit == "Hz" || p.unit == "ms" ? 0 : 2);
                row.slider->setDoubleClickReturnValue(true, p.default_value);
                const std::string id = p.id;
                const juce::String label_text = row.label->getText();
                row.slider->onDragStart = [this] { ++gesture_; };
                row.slider->onValueChange = [this, id, label_text, s = row.slider.get()] {
                    if (refreshing_) return;
                    apply(id, label_text, s->getValue());
                };
                addAndMakeVisible(*row.slider);
                rows_.push_back(std::move(row));
            }
            if (rows_.empty()) {
                empty_.setText("This processor has no settings to show.", juce::dontSendNotification);
                addAndMakeVisible(empty_);
            }
            refresh();
            setSize(460, juce::jmax(80, 20 + static_cast<int>(rows_.size()) * 34));
        }

        void refresh()
        {
            const auto& d = window_.context_.session.data();
            refreshing_ = true;
            for (auto& r : rows_) {
                if (r.slider->isMouseButtonDown()) continue;
                std::optional<double> v;
                if (const auto* t = d.find_track(window_.owner_id_)) v = project::static_value(*t, r.info.id);
                else if (const auto* b = d.find_bus(window_.owner_id_)) v = project::static_value(*b, r.info.id);
                r.slider->setValue(v ? *v : r.info.default_value, juce::dontSendNotification);
            }
            refreshing_ = false;
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(10);
            empty_.setBounds(area);
            for (auto& r : rows_) {
                auto line = area.removeFromTop(34).reduced(0, 4);
                r.label->setBounds(line.removeFromLeft(130));
                r.slider->setBounds(line);
            }
        }
        void paint(juce::Graphics& g) override { g.fillAll(colours::background); }

    private:
        void apply(const std::string& id, const juce::String& what, double value)
        {
            auto& session = window_.context_.session;
            // One undo step per drag: the label alternates between gestures.
            const std::string label = (what + (gesture_ % 2 ? " " : "")).toStdString();
            if (session.data().find_track(window_.owner_id_))
                session.update_track_mix(window_.owner_id_, label,
                                         [&](project::Track& t) { project::set_static_value(t, id, value); });
            else
                session.update_bus(window_.owner_id_, label, [&](project::Bus& b) { project::set_static_value(b, id, value); });
        }

        BuiltinInsertWindow& window_;
        std::vector<Row> rows_;
        juce::Label empty_;
        int gesture_ = 0;
        bool refreshing_ = false;
    };

    AppContext& context_;
    std::string owner_id_;
    int slot_;
    std::string kind_;
};

// --- Main component -----------------------------------------------------------------------------

MainComponent::MainComponent(AppContext& context)
    : context_(context),
      deck_(context),
      toolbar_(context),
      edit_(context),
      markers_(context),
      new_track_(context),
      selection_(context),
      inspector_(context),
      channel_(context),
      status_strip_(context),
      keyboard_(context),
      tracks_(context),
      composer_(context),
      sequencer_(context)
{
    // The UI theme (LED, LCD, staves) the user chose last time.
    hw::Theme::get().attach(&hw::ui_settings());
    // The Edit window (docs/spec/ui): top deck, toolbar, left rack, the
    // edit window around the current view, right panel, status strip.
    for (auto* c : std::initializer_list<juce::Component*>{&deck_, &toolbar_, &edit_, &markers_, &selection_,
                                                           &inspector_, &status_strip_, &keyboard_})
        addAndMakeVisible(c);
    addChildComponent(new_track_);
    addChildComponent(channel_);
    addAndMakeVisible(tracks_);
    addAndMakeVisible(composer_);
    addChildComponent(sequencer_);
    deck_.on_view = [this](bool producer) { show_workspace(producer ? Workspace::Producer : Workspace::Composer); };
    edit_.on_stage = [this](int stage) { go_to_stage(stage); };
    toolbar_.on_zoom = [this](bool larger) {
        if (workspace_ == Workspace::Composer) composer_.zoom(larger);
        else sequencer_.zoom(larger);
    };
    toolbar_.on_pencil = [this](bool on) { sequencer_.set_pencil_mode(on); };
    // The Composer keys and the inspector run notation commands on the
    // score being written, and hand the keyboard back to it.
    auto run = [this](composer::Command c) {
        composer_.perform(c);
        composer_.grabKeyboardFocus();
    };
    toolbar_.on_command = run;
    inspector_.on_command = run;
    // The keyboard deck: step input into the Composer view, which grows the
    // deck (and shrinks the edit window) as the keys zoom.
    keyboard_.on_command = run;
    keyboard_.cursor = [this] { return composer_.cursor(); };
    keyboard_.on_step = [this](int midi, bool chord) {
        if (workspace_ != Workspace::Composer || !composer_.note_entry()) return false;
        const bool ok = composer_.enter_midi(midi, chord);
        composer_.grabKeyboardFocus();
        return ok;
    };
    keyboard_.on_height_changed = [this] { resized(); };

    layout_.setItemLayout(0, 220, 520, 300);
    layout_.setItemLayout(1, 5, 5, 5);
    layout_.setItemLayout(2, 300, -1.0, -1.0);
    divider_ = std::make_unique<juce::StretchableLayoutResizerBar>(&layout_, 1, true);
    addAndMakeVisible(*divider_);

    context_.status = [this](const juce::String& message) {
        status_strip_.set_message(message);
        status_age_ = 0;
    };
    context_.show_plugin_window = [this](const std::string& id) { show_plugin_window(id); };
    context_.show_insert_window = [this](const std::string& owner, int slot) { show_insert_window(owner, slot); };

    listener_ = context_.session.add_listener([this](session::Change c) {
        // A MIDI keyboard plays whichever track is selected.
        if (c == session::Change::Selection) {
            context_.engine.set_live_track(context_.session.selected_track());
            // The selection's range shows on the ruler (and punch follows it).
            const auto& sel = context_.session.selection();
            const auto r = sequencer_.range();
            if (r.first != sel.from || r.second != sel.to) {
                syncing_range_ = true;
                sequencer_.set_range(sel.from, sel.to);
                syncing_range_ = false;
                if (punch_) set_punch(true);
            }
            // A loop follows the range; clearing the range ends it.
            if (context_.engine.loop() && context_.engine.loop_range() != std::make_pair(sel.from, sel.to)) {
                if (sel.has_range()) context_.engine.set_loop(true, sel.from, sel.to);
                else set_loop(false);
            }
        }
        if (c == session::Change::Document) {
            // Close plug-in windows whose plug-in changed or went away.
            for (auto it = plugin_windows_.begin(); it != plugin_windows_.end();) {
                const auto& w = *it->second;
                auto* now = w.slot() < 0 ? context_.engine.plugin_for(w.owner())
                                         : context_.engine.insert_plugin(w.owner(), w.slot());
                if (now != it->second->instance()) it = plugin_windows_.erase(it);
                else ++it;
            }
            // Built-in processor windows follow the document, and close when
            // their slot holds something else.
            for (auto it = builtin_windows_.begin(); it != builtin_windows_.end();) {
                if (insert_kind(it->second->owner(), it->second->slot()) != it->second->kind()) {
                    it = builtin_windows_.erase(it);
                } else {
                    it->second->refresh();
                    ++it;
                }
            }
        }
    });

    context_.engine.on_transport_changed = [this] { deck_.update_position(); };
    // A recording pass was placed (one undo step), or failed.
    context_.engine.on_recording_finished = [this](const juce::String& message, bool ok) {
        context_.status(message);
        deck_.update_position();
        if (!ok)
            juce::AlertWindow::showAsync(juce::MessageBoxOptions::makeOptionsOk(juce::MessageBoxIconType::WarningIcon,
                                                                                "Recording", message, {}, this),
                                         [](int) {});
    };
    // The ruler's range is the shared selection's range (docs/spec/ui): a
    // drag on the ruler, or notes selected in either view, set it.
    sequencer_.on_range_changed = [this] {
        if (syncing_range_) return;
        const auto r = sequencer_.range();
        context_.session.set_selection_range(r.first, r.second);
        if (punch_) set_punch(true);  // follow the new range, or turn off when it was cleared
    };
    // The keyboard profile chosen last time.
    if (const auto saved = hw::ui_settings().getValue("shortcut_profile"); saved.isNotEmpty())
        use_shortcut_profile(saved.toStdString());
    startTimerHz(30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    close_video_window();
    context_.session.remove_listener(listener_);
    context_.engine.on_transport_changed = nullptr;
    context_.engine.on_recording_finished = nullptr;
    plugin_windows_.clear();
    builtin_windows_.clear();
    context_.show_insert_window = nullptr;
}

void MainComponent::paint(juce::Graphics& g) { g.fillAll(hw::tok::chassis); }

void MainComponent::resized()
{
    constexpr int kPad = 10, kGap = hw::tok::gap_panel, kRight = 268;
    auto r = getLocalBounds();
    status_strip_.setBounds(r.removeFromBottom(18));
    r.reduce(kPad, kPad);
    deck_.setBounds(r.removeFromTop(shell::TopDeck::height_for(getWidth())));
    // The keyboard deck across the bottom, as tall as its key zoom needs.
    keyboard_.setBounds(r.removeFromBottom(keyboard_.preferred_height()));
    r.removeFromBottom(kGap);
    r.removeFromTop(kGap);
    toolbar_.setBounds(r.removeFromTop(60));
    r.removeFromTop(kGap);

    const bool producer = workspace_ == Workspace::Producer;
    // Under ~1280 px the right panel drops below the edit window.
    const bool narrow = getWidth() < 1280;
    juce::Rectangle<int> right_area;
    if (!narrow) {
        right_area = r.removeFromRight(kRight);
        r.removeFromRight(kGap);
    }
    // Left rack | edit window, with the divider between them.
    juce::Component* items[] = {&markers_, divider_.get(), &edit_};
    layout_.layOutComponents(items, 3, r.getX(), r.getY(), r.getWidth(), r.getHeight(), false, true);
    auto rack = markers_.getBounds();
    divider_->setBounds(divider_->getBounds().reduced(1, 0));
    // The rack: Composer = tracks, then markers; Producer = new track, then tracks.
    if (producer) {
        new_track_.setBounds(rack.removeFromTop(200));
        rack.removeFromTop(kGap);
        tracks_.setBounds(rack);
    } else {
        const int markers_h = juce::jlimit(120, 260, rack.getHeight() / 3);
        markers_.setBounds(rack.removeFromBottom(markers_h));
        rack.removeFromBottom(kGap);
        tracks_.setBounds(rack);
    }
    auto edit = edit_.getBounds();
    if (narrow) {
        right_area = edit.removeFromBottom(producer ? 330 : 190);
        edit.removeFromBottom(kGap);
        edit_.setBounds(edit);
    }
    if (producer) {
        channel_.setBounds(narrow ? right_area : right_area.withHeight(std::min(right_area.getHeight(), 420)));
    } else if (narrow) {
        // Selection, then the inspector in columns beside it.
        selection_.setBounds(right_area.removeFromLeft(kRight));
        right_area.removeFromLeft(kGap);
        inspector_.set_horizontal(true);
        inspector_.setBounds(right_area);
    } else {
        // The Composer column: what is selected, then the inspector. A tall
        // keyboard deck makes it short: the inspector goes compact, then the
        // selection readout gives up height (down to 110 px).
        const bool compact = right_area.getHeight() < 190 + kGap + composer_ui::Inspector::preferred_height(false);
        const int insp_h = composer_ui::Inspector::preferred_height(compact);
        const int sel_h = juce::jlimit(110, 190, right_area.getHeight() - kGap - insp_h);
        selection_.setBounds(right_area.removeFromTop(sel_h));
        right_area.removeFromTop(kGap);
        inspector_.set_horizontal(false);
        inspector_.set_compact(compact);
        inspector_.setBounds(right_area.withHeight(std::min(right_area.getHeight(), insp_h)));
    }
    const auto content = edit_.getBounds().getIntersection(edit_.content_area() + edit_.getPosition());
    composer_.setBounds(content);
    sequencer_.setBounds(content);
}

void MainComponent::go_to_stage(int stage)
{
    switch (stage) {
        case 0: show_workspace(Workspace::Composer); break;
        case 1: {
            // Score: parts and the full score.
            show_workspace(Workspace::Composer);
            edit_.set_stage(1);
            juce::PopupMenu m;
            m.addItem("Export parts (PDF)...", [this] { export_parts_pdf_dialog(); });
            m.addItem("Export full score (PDF)...", [this] { export_full_score_dialog(); });
            m.addItem("Export MusicXML...", [this] { export_musicxml(); });
            m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&edit_.stage_key(1)));
            break;
        }
        case 2:
            show_workspace(Workspace::Producer);
            show_mixer();
            break;
        case 3:
            show_workspace(Workspace::Producer);
            edit_.set_stage(3);
            context_.commands.invokeDirectly(cmd::exportStems, true);
            break;
        default: break;
    }
}

void MainComponent::show_workspace(Workspace w)
{
    // One project, two views: the playhead, the selection and each view's
    // own zoom and scroll stay as they are.
    workspace_ = w;
    const bool producer = w == Workspace::Producer;
    composer_.setVisible(!producer);
    sequencer_.setVisible(producer);
    markers_.setVisible(!producer);
    selection_.setVisible(!producer);
    inspector_.setVisible(!producer);
    new_track_.setVisible(producer);
    channel_.setVisible(producer);
    deck_.set_producer(producer);
    toolbar_.set_producer(producer);
    edit_.set_stage(producer ? 2 : 0);
    if (producer) channel_.refresh();
    resized();
    if (w == Workspace::Composer) composer_.grabKeyboardFocus();
    else sequencer_.grabKeyboardFocus();
}

void MainComponent::timerCallback()
{
    const Ticks tick = context_.engine.position();
    const bool playing = context_.engine.playing();
    deck_.update_position();
    const auto& sel = context_.session.selection();
    deck_.set_range(sel.from, sel.to);
    toolbar_.update(punch_, sequencer_.pencil_mode());
    if (composer_.isVisible()) {
        composer_.set_playhead(tick, playing);
        // The Composer keys light from the editor: entry, value, dot, rest, voice.
        if (const auto cur = composer_.cursor()) {
            toolbar_.set_entry_state(composer_.note_entry(), cur->duration, cur->rest_mode);
            inspector_.set_voice(cur->voice);
        } else {
            toolbar_.set_entry_state(composer_.note_entry(), notation::Duration{}, false);
            inspector_.set_voice(-1);
        }
    }
    if (sequencer_.isVisible()) sequencer_.set_playhead(tick, playing);
    // A plug-in that crashed in its own process: the session plays on without it.
    if (const auto crashed = context_.engine.take_newly_crashed_plugins(); !crashed.isEmpty())
        context_.status(crashed.joinIntoString(", ") + (crashed.size() == 1 ? " has" : " have") +
                        " crashed and gone silent; everything else keeps playing. Tools > Reload Crashed Plug-ins "
                        "starts it again from its last state.");
    if (const int late = context_.engine.take_late_plugin_blocks(); late > 0 && playing)
        context_.status("A plug-in could not keep up (" + juce::String(late) + " short dropout" +
                        (late == 1 ? "" : "s") + "); a larger audio buffer gives it more time");
    // Never silent: a disk that cannot keep up with the audio clips is said.
    if (const int dropouts = context_.engine.take_disk_underruns(); dropouts > 0)
        context_.status("The disk could not keep up with the audio clips (" + juce::String(dropouts) +
                        " short dropout" + (dropouts == 1 ? "" : "s") + ")");

    // Crash protection: an autosave every minute while there are unsaved changes.
    if (++ticks_ >= 30 * 60) {
        ticks_ = 0;
        if (context_.session.dirty()) {
            capture_plugin_states();
            context_.session.write_autosave(user_data_directory().getFullPathName().toStdString());
            status_strip_.set_autosaved(juce::Time::getCurrentTime());
        }
    }
    if (++status_age_ == 30 * 8) status_strip_.set_message({});
}

// --- File ------------------------------------------------------------------------------------------

bool MainComponent::confirm_discard()
{
    if (!context_.session.dirty()) return true;
    const int r = juce::AlertWindow::showYesNoCancelBox(
        juce::MessageBoxIconType::QuestionIcon, "Unsaved changes",
        "Save the changes to \"" + juce::String(context_.session.title()).upToLastOccurrenceOf(" *", false, false) + "\"?",
        "Save", "Don't save", "Cancel", this, nullptr);
    if (r == 0) return false;
    if (r == 1) return save(false);
    return true;
}

void MainComponent::open_file(const juce::File& file)
{
    std::string path = file.getFullPathName().toStdString();
    if (session::Session::has_newer_autosave(path)) {
        const bool recover = juce::AlertWindow::showOkCancelBox(
            juce::MessageBoxIconType::QuestionIcon, "Recover unsaved work?",
            "VC Composer 2 found an autosave of this project that is newer than the saved file. Open the autosave?",
            "Open autosave", "Open saved file", this, nullptr);
        if (recover) path += ".autosave";
    }
    std::string error;
    if (!context_.session.open(path, &error)) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Could not open", error);
        return;
    }
    if (juce::String(path).endsWith(".autosave")) {
        // Recovered: keep the original file as the save target.
        context_.session.edit("Recovered from autosave", [](project::ProjectData&) { return true; });
    }
    context_.engine.locate(0);
    context_.status("Opened " + file.getFileName());
}

void MainComponent::capture_plugin_states()
{
    // Instruments and effect inserts alike, on tracks and buses.
    context_.engine.capture_plugin_states();
}

bool MainComponent::save(bool save_as)
{
    capture_plugin_states();
    std::string path = context_.session.path();
    if (save_as || path.empty()) {
        juce::FileChooser chooser("Save project", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                                      .getChildFile(juce::String(context_.session.data().name) + ".vcc2"),
                                  "*.vcc2");
        if (!chooser.browseForFileToSave(true)) return false;
        path = chooser.getResult().withFileExtension("vcc2").getFullPathName().toStdString();
    }
    std::string error;
    if (!context_.session.save_as(path, &error)) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Could not save", error);
        return false;
    }
    context_.status("Saved " + juce::File(path).getFileName());
    return true;
}

void MainComponent::import_musicxml()
{
    chooser_ = std::make_unique<juce::FileChooser>("Import MusicXML", juce::File(), "*.musicxml;*.mxl;*.xml");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc) {
                              const auto file = fc.getResult();
                              if (file == juce::File()) return;
                              auto finish = [this, file](bool replace) {
                                  juce::String message;
                                  const bool ok = import_musicxml_file(context_, file, replace, message);
                                  context_.status(message);
                                  if (!ok)
                                      juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                             "Import MusicXML", message);
                              };
                              if (context_.session.data().tracks.empty()) {
                                  finish(true);
                                  return;
                              }
                              juce::AlertWindow::showAsync(
                                  juce::MessageBoxOptions()
                                      .withIconType(juce::MessageBoxIconType::QuestionIcon)
                                      .withTitle("Import MusicXML")
                                      .withMessage("Add the score's parts to this project as new tracks, or start a "
                                                   "new project from the score (tempo, meter and key from the file)?")
                                      .withButton("Add Tracks")
                                      .withButton("Replace Project")
                                      .withButton("Cancel"),
                                  [finish](int r) {
                                      if (r == 1) finish(false);
                                      else if (r == 2) finish(true);
                                  });
                          });
}

void MainComponent::export_musicxml()
{
    chooser_ = std::make_unique<juce::FileChooser>(
        "Export MusicXML",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile(juce::File::createLegalFileName(context_.session.data().name) + ".musicxml"),
        "*.musicxml;*.mxl");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this](const juce::FileChooser& fc) {
                              auto file = fc.getResult();
                              if (file == juce::File()) return;
                              if (!file.hasFileExtension("mxl") && !file.hasFileExtension("musicxml") &&
                                  !file.hasFileExtension("xml"))
                                  file = file.withFileExtension("musicxml");
                              juce::String message;
                              export_musicxml_file(context_, file, false, message);
                              context_.status(message);
                          });
}

void MainComponent::export_parts_pdf_dialog()
{
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle("Export Parts (PDF)")
            .withMessage("Print each part to its own PDF in a folder, or every part into one PDF with a bookmark per "
                         "part?")
            .withButton("One PDF per Part")
            .withButton("All Parts in One PDF")
            .withButton("Cancel"),
        [this](int r) {
            if (r == 0) return;
            const bool one_file = r == 2;
            const auto docs = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
            chooser_ = one_file ? std::make_unique<juce::FileChooser>(
                                      "Save all parts as",
                                      docs.getChildFile(juce::File::createLegalFileName(context_.session.data().name) +
                                                        " - parts.pdf"),
                                      "*.pdf")
                                : std::make_unique<juce::FileChooser>("Choose a folder for the parts", docs);
            const int mode = one_file ? juce::FileBrowserComponent::saveMode |
                                             juce::FileBrowserComponent::warnAboutOverwriting
                                       : juce::FileBrowserComponent::openMode |
                                             juce::FileBrowserComponent::canSelectDirectories;
            chooser_->launchAsync(mode, [this, one_file](const juce::FileChooser& fc) {
                auto target = fc.getResult();
                if (target == juce::File()) return;
                if (one_file) target = target.withFileExtension("pdf");
                else if (!target.isDirectory()) return;
                juce::String message;
                export_parts_pdf(context_, target, message);
                context_.status(message);
            });
        });
}

void MainComponent::export_full_score_dialog()
{
    auto* w = new juce::AlertWindow("Export Full Score (PDF)", "Every instrument track that holds notation, in track "
                                    "order, bracketed by section.",
                                    juce::MessageBoxIconType::NoIcon);
    w->addComboBox("pitch", {"Transposed score", "Score in C (concert pitch)"}, "Pitch");
    w->addComboBox("paper", {"A3", "Tabloid", "A4", "Letter"}, "Paper");
    w->addComboBox("bars", {"2", "3", "4", "5", "6", "8"}, "Bars per system");
    w->getComboBoxComponent("bars")->setSelectedItemIndex(2);
    w->addComboBox("hide", {"Hide staves where a part rests", "Show every staff on every system"}, "Empty staves");
    w->addComboBox("order", {"Orchestral score order", "Track order"}, "Order");
    w->addButton("Export...", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int r) {
        if (r != 1) return;
        FullScoreSettings settings;
        settings.concert_pitch = w->getComboBoxComponent("pitch")->getSelectedItemIndex() == 1;
        settings.paper = w->getComboBoxComponent("paper")->getText().toStdString();
        settings.bars_per_system = w->getComboBoxComponent("bars")->getText().getIntValue();
        settings.hide_empty_staves = w->getComboBoxComponent("hide")->getSelectedItemIndex() == 0;
        settings.score_order = w->getComboBoxComponent("order")->getSelectedItemIndex() == 0;
        const auto docs = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        chooser_ = std::make_unique<juce::FileChooser>(
            "Save the full score as",
            docs.getChildFile(juce::File::createLegalFileName(context_.session.data().name) +
                              (settings.concert_pitch ? " - score in C.pdf" : " - full score.pdf")),
            "*.pdf");
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this, settings](const juce::FileChooser& fc) {
                                  auto file = fc.getResult();
                                  if (file == juce::File()) return;
                                  juce::String message;
                                  export_full_score_pdf(context_, file.withFileExtension("pdf"), settings, message);
                                  context_.status(message);
                              });
    }), true);
}

void MainComponent::export_midi()
{
    chooser_ = std::make_unique<juce::FileChooser>("Export MIDI", juce::File(), "*.mid");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this](const juce::FileChooser& fc) {
                              const auto file = fc.getResult();
                              if (file == juce::File()) return;
                              std::string error;
                              const bool ok = files::write_file_atomic(
                                  file.withFileExtension("mid").getFullPathName().toStdString(),
                                  project::export_project_midi(context_.session.project()), &error);
                              context_.status(ok ? "Exported MIDI (temp tracks left out)" : juce::String("Export failed: ") + error);
                          });
}

namespace {

void export_parts_svg(AppContext& c, const juce::File& folder)
{
    engraving::LayoutEngine engine;
    if (music_font().loaded) engine.set_glyph_table(music_font().glyphs);
    int pages = 0;
    for (const auto* t : c.session.music_tracks()) {
        const auto* v = t->active_version();
        if (!v || t->kind != project::TrackKind::Instrument) continue;
        engraving::PrintOptions po;
        po.title = c.session.data().name;
        po.subtitle = t->name;
        const auto laid = engraving::lay_out_pages(v->score, engine, po);
        const auto svgs = engraving::render_pages_svg(laid, {}, music_font().loaded ? &music_font().glyphs : nullptr);
        for (std::size_t i = 0; i < svgs.size(); ++i) {
            auto f = folder.getChildFile(juce::File::createLegalFileName(juce::String(t->name)) + " - page " +
                                         juce::String(i + 1) + ".svg");
            files::write_file_atomic(f.getFullPathName().toStdString(), svgs[i]);
            ++pages;
        }
    }
    c.status("Exported " + juce::String(pages) + " part page(s) as SVG to " + folder.getFullPathName());
}

}  // namespace

void MainComponent::save_as_template()
{
    auto* w = new juce::AlertWindow("Save as template", "A name for your custom template", juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("name", juce::String(context_.session.data().name));
    w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int r) {
        if (r != 1) return;
        const juce::String name = w->getTextEditorContents("name").trim();
        if (name.isEmpty()) return;
        juce::String slug = name.toLowerCase().retainCharacters("abcdefghijklmnopqrstuvwxyz0123456789 ").replaceCharacter(' ', '_');
        if (slug.isEmpty()) slug = "custom";
        const std::string id = "user." + slug.toStdString();
        auto tpl = templates::save_user_template(context_.session.project(), id, name.toStdString(), "");
        std::string error;
        if (!context_.catalogs.templates->add_user_template(tpl, &error)) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Could not save template", error);
            return;
        }
        auto folder = user_data_directory().getChildFile("Templates");
        folder.createDirectory();
        const auto pack = context_.catalogs.templates->export_pack({id}, "user");
        files::write_file_atomic(folder.getChildFile(slug + ".json").getFullPathName().toStdString(), json::write(pack));
        context_.status("Saved template \"" + name + "\" - it is in the template browser under Custom");
    }), true);
}

juce::PopupMenu MainComponent::theme_menu()
{
    // The spec's themeable colours (docs/spec/ui/SPEC.md): live, remembered.
    auto& theme = hw::Theme::get();
    juce::PopupMenu m;
    m.addSectionHeader("LED colour");
    for (const auto& c : hw::Theme::led_choices())
        m.addItem(juce::String(c.name) + " LEDs", true, theme.led() == c.colour,
                  [colour = c.colour] { hw::Theme::get().set_led(colour); });
    m.addSectionHeader("LCD colour");
    for (const auto& c : hw::Theme::lcd_choices())
        m.addItem(juce::String(c.name) + " LCDs", true, theme.lcd() == c.colour,
                  [colour = c.colour] { hw::Theme::get().set_lcd(colour); });
    m.addSectionHeader("Staves");
    m.addItem("Dark staves", true, !theme.paper(), [] { hw::Theme::get().set_paper(false); });
    m.addItem("Paper staves", true, theme.paper(), [] { hw::Theme::get().set_paper(true); });
    return m;
}

juce::PopupMenu MainComponent::shortcuts_menu()
{
    juce::PopupMenu m;
    std::vector<composer::ShortcutProfile> profiles{composer::make_composer2_profile()};
    for (auto& p : composer::built_in_profiles()) profiles.push_back(std::move(p));
    for (const auto& p : profiles) {
        const std::string name = p.name();
        juce::String label(name);
        if (name == "Composer 2") label << "  (5 quarter, 4 eighth, 6 half)";
        m.addItem(label, true, context_.shortcuts.name() == name, [this, name] {
            if (use_shortcut_profile(name)) context_.status("Keyboard shortcuts: " + juce::String(name));
        });
    }
    return m;
}

bool MainComponent::use_shortcut_profile(const std::string& name)
{
    std::vector<composer::ShortcutProfile> profiles{composer::make_composer2_profile()};
    for (auto& p : composer::built_in_profiles()) profiles.push_back(std::move(p));
    for (auto& p : profiles)
        if (p.name() == name) {
            context_.shortcuts = std::move(p);
            toolbar_.refresh_legends();
            inspector_.refresh_legends();
            hw::ui_settings().setValue("shortcut_profile", juce::String(name));
            hw::ui_settings().saveIfNeeded();
            return true;
        }
    return false;
}

void MainComponent::show_control_gallery()
{
    if (!gallery_window_) {
        class Window final : public juce::DocumentWindow {
        public:
            Window() : juce::DocumentWindow("Control Gallery", hw::tok::chassis, juce::DocumentWindow::closeButton)
            {
                setUsingNativeTitleBar(true);
                setContentOwned(new hw::Gallery(), true);
                setResizable(false, false);
            }
            void closeButtonPressed() override { setVisible(false); }
        };
        gallery_window_ = std::make_unique<Window>();
        gallery_window_->centreWithSize(gallery_window_->getWidth(), gallery_window_->getHeight());
    }
    gallery_window_->setVisible(true);
    gallery_window_->toFront(true);
}

void MainComponent::show_mixer()
{
    if (!mixer_) {
        mixer_ = std::make_unique<MixerWindow>(context_);
        mixer_->centreWithSize(1000, 460);
    }
    mixer_->setVisible(true);
    mixer_->toFront(true);
}

void MainComponent::show_plugin_window(const std::string& id)
{
    auto* instance = context_.engine.plugin_for(id);
    if (!instance) {
        context_.status("This track is played by the built-in placeholder; load an instrument plug-in first");
        return;
    }
    auto& w = plugin_windows_["instrument:" + id];
    if (!w || w->instance() != instance) w = std::make_unique<PluginWindow>(context_, id, -1, *instance);
    w->setVisible(true);
    w->toFront(true);
}

std::string MainComponent::insert_kind(const std::string& owner, int slot) const
{
    const std::vector<project::InsertSlot>* inserts = nullptr;
    if (const auto* t = context_.session.data().find_track(owner)) inserts = &t->inserts;
    else if (const auto* b = context_.session.data().find_bus(owner)) inserts = &b->inserts;
    if (!inserts || slot < 0 || slot >= static_cast<int>(inserts->size())) return {};
    return (*inserts)[static_cast<size_t>(slot)].kind;
}

void MainComponent::show_insert_window(const std::string& owner, int slot)
{
    const auto kind = insert_kind(owner, slot);
    if (kind.empty()) return;
    const std::string key = owner + "#" + std::to_string(slot);
    juce::String owner_name;
    if (const auto* t = context_.session.data().find_track(owner)) owner_name = t->name;
    else if (const auto* b = context_.session.data().find_bus(owner)) owner_name = b->name;

    if (kind == "plugin") {
        context_.engine.rebuild_now();  // a slot filled a moment ago has its plug-in now
        auto* instance = context_.engine.insert_plugin(owner, slot);
        if (!instance) {
            const auto why = context_.engine.insert_status(owner, slot);
            context_.status(why.isNotEmpty() ? why : juce::String("That plug-in is not loaded"));
            return;
        }
        auto& w = plugin_windows_["insert:" + key];
        if (!w || w->instance() != instance) w = std::make_unique<PluginWindow>(context_, owner, slot, *instance);
        w->setName(instance->getName() + " - " + owner_name);
        w->setVisible(true);
        w->toFront(true);
        return;
    }
    auto& w = builtin_windows_[key];
    if (!w || w->kind() != kind) {
        juce::String title = juce::String(kind).substring(0, 1).toUpperCase() + juce::String(kind).substring(1);
        w = std::make_unique<BuiltinInsertWindow>(context_, owner, slot, title + " - " + owner_name + " (insert " +
                                                                              juce::String(slot + 1) + ")");
    }
    w->setVisible(true);
    w->toFront(true);
}

void MainComponent::show_audio_settings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent(context_.engine.devices(), 0, 2, 0, 2, true, false, true, false);
    selector->setSize(520, 420);
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(selector);
    o.dialogTitle = "Audio and MIDI settings";
    o.dialogBackgroundColour = colours::background;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void MainComponent::show_plugin_scanner()
{
    // JUCE's plug-in list, with what our scanner knows underneath: how it
    // scans, and why each failed plug-in failed.
    class Scanner final : public juce::Component, private juce::ChangeListener, private juce::Timer {
    public:
        explicit Scanner(AppContext& c)
            : context_(c),
              list_(c.plugins.formats(), c.plugins.known(), c.plugins.dead_mans_pedal(), c.plugins.properties(), true)
        {
            // Out of process, plug-ins are scanned four at a time: one that
            // crashes or hangs only ends its own scanner process.
            if (c.plugins.scans_out_of_process()) list_.setNumberOfThreadsForScanning(4);
            addAndMakeVisible(list_);
            mode_.setFont(juce::FontOptions(12.0f));
            addAndMakeVisible(mode_);
            failures_.setMultiLine(true);
            failures_.setReadOnly(true);
            failures_.setColour(juce::TextEditor::backgroundColourId, colours::panel);
            failures_.setColour(juce::TextEditor::textColourId, colours::accent_warm);
            addAndMakeVisible(failures_);
            hosting_label_.setText("Run plug-ins", juce::dontSendNotification);
            hosting_label_.setColour(juce::Label::textColourId, colours::text_dim);
            addAndMakeVisible(hosting_label_);
            hosting_.addItemList(PluginHost::hosting_mode_names(), 1);
            hosting_.setSelectedItemIndex(static_cast<int>(c.plugins.hosting_mode()), juce::dontSendNotification);
            hosting_.setEnabled(c.plugins.host_tool().existsAsFile());
            hosting_.setTooltip("Outside VC Composer, a plug-in that crashes takes down only its own process. "
                                "Sharing a process uses less memory; a crash there silences everything it carries.");
            hosting_.onChange = [this] {
                context_.plugins.set_hosting_mode(
                    static_cast<PluginHost::HostingMode>(hosting_.getSelectedItemIndex()));
                // Move the open project's plug-ins over now, keeping their state.
                const int n = context_.engine.restart_plugins();
                context_.status("Plug-ins now run: " + hosting_.getText().toLowerCase() +
                                (n > 0 ? " (" + juce::String(n) + " restarted with their settings)" : juce::String()));
                refresh();
            };
            addAndMakeVisible(hosting_);
            addAndMakeVisible(processes_);
            retry_.setButtonText("Retry failed plug-ins");
            retry_.setTooltip("Forget the failures, so the next scan tries them again, and let plug-ins that crashed "
                              "share processes again");
            retry_.onClick = [this] {
                for (const auto& [target, reason] : context_.plugins.scan_failures())
                    context_.plugins.clear_scan_failure(target);
                context_.plugins.known().clearBlacklistedFiles();
                context_.plugins.clear_crash_isolation();
                refresh();
            };
            addAndMakeVisible(retry_);
            c.plugins.known().addChangeListener(this);
            refresh();
            startTimer(1000);  // failures arrive from scanning threads
            setSize(720, 560);
        }
        ~Scanner() override
        {
            context_.plugins.known().removeChangeListener(this);
            context_.plugins.save_list();
            context_.engine.rebuild_now();
        }
        void paint(juce::Graphics& g) override { g.fillAll(colours::background); }
        void resized() override
        {
            auto r = getLocalBounds();
            auto foot = r.removeFromBottom(failures_.isVisible() ? 180 : 64).reduced(8, 4);
            auto mode_row = foot.removeFromTop(26);
            hosting_label_.setBounds(mode_row.removeFromLeft(90));
            hosting_.setBounds(mode_row.removeFromLeft(330));
            processes_.setBounds(mode_row.withTrimmedLeft(10));
            auto top = foot.removeFromTop(24);
            retry_.setBounds(top.removeFromRight(170));
            mode_.setBounds(top);
            failures_.setBounds(foot.withTrimmedTop(4));
            list_.setBounds(r);
        }

    private:
        void changeListenerCallback(juce::ChangeBroadcaster*) override { refresh(); }
        void timerCallback() override { refresh(); }
        void refresh()
        {
            const bool out = context_.plugins.scans_out_of_process();
            mode_.setText(out ? "Plug-ins are scanned in a separate process: one that crashes or hangs cannot take VC "
                                "Composer down."
                              : "The plug-in scanner tool is missing, so plug-ins are scanned inside the app (a crasher "
                                "is skipped on the next launch). Reinstall to restore safe scanning.",
                          juce::dontSendNotification);
            mode_.setColour(juce::Label::textColourId, out ? colours::text_dim : colours::missing);
            const auto running = context_.plugins.host_processes();
            int instances = 0;
            for (const auto& [label, n] : running) instances += n;
            processes_.setText(running.empty() ? juce::String()
                                               : juce::String(instances) + " plug-in" + (instances == 1 ? "" : "s") +
                                                     " in " + juce::String(static_cast<int>(running.size())) +
                                                     " process" + (running.size() == 1 ? "" : "es"),
                               juce::dontSendNotification);
            processes_.setColour(juce::Label::textColourId, colours::text_dim);
            juce::String text;
            for (const auto& [target, reason] : context_.plugins.scan_failures())
                text << juce::File::createFileWithoutCheckingPath(target).getFileName() << ": " << reason << "\n";
            const bool show = text.isNotEmpty();
            if (failures_.getText() != text) failures_.setText(text, false);
            retry_.setVisible(show || context_.plugins.known().getBlacklistedFiles().size() > 0);
            if (failures_.isVisible() != show) {
                failures_.setVisible(show);
                resized();
            }
        }

        AppContext& context_;
        juce::PluginListComponent list_;
        juce::Label mode_;
        juce::TextEditor failures_;
        juce::TextButton retry_;
        juce::ComboBox hosting_;
        juce::Label hosting_label_, processes_;
    };
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(new Scanner(context_));
    o.dialogTitle = "Instrument and effect plug-ins";
    o.dialogBackgroundColour = colours::background;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

// --- Recording and audio ------------------------------------------------------------------------

void MainComponent::toggle_record()
{
    auto& engine = context_.engine;
    const bool on = !engine.record_enabled();
    engine.set_record_enabled(on);
    if (on) {
        int armed = 0;
        for (const auto& t : context_.session.data().tracks) armed += t.record_armed ? 1 : 0;
        context_.status(armed == 0 ? juce::String("Record enabled - arm a track (the red button in the track list) to record onto it")
                                   : "Record enabled - Play records onto " + juce::String(armed) +
                                         (armed == 1 ? " armed track" : " armed tracks"));
    } else {
        context_.status("Record off");
    }
    deck_.update_position();
    context_.commands.commandStatusChanged();
}

void MainComponent::set_loop(bool on)
{
    auto& session = context_.session;
    auto sel = session.selection();
    if (on && !sel.has_range()) {
        // No range yet: loop the bar the playhead is in.
        const auto& m = session.data().meter;
        const int bar = m.bar_at(context_.engine.position());
        syncing_range_ = false;
        session.set_selection_range(m.bar_start(bar), m.bar_start(bar + 1));
        sel = session.selection();
    }
    const bool changed = on != context_.engine.loop();
    context_.engine.set_loop(on, sel.from, sel.to);
    deck_.update_position();
    context_.commands.commandStatusChanged();
    if (changed) {
        if (on) {
            const auto& d = session.data();
            context_.status("Loop " + shell::bars_beats_ticks(d.meter, sel.from) + juce::String::fromUTF8(" \xe2\x86\x92 ") +
                            shell::bars_beats_ticks(d.meter, sel.to));
        } else {
            context_.status("Loop off");
        }
    }
}

void MainComponent::set_punch(bool on)
{
    const auto range = sequencer_.range();
    if (on && range.second <= range.first) {
        if (!punch_) context_.status("Drag across the sequencer ruler to choose the punch range first");
        on = false;
    }
    const bool changed = on != punch_;
    punch_ = on;
    context_.engine.set_punch(on, range.first, range.second);
    sequencer_.set_punch_display(on);
    toolbar_.update(on, sequencer_.pencil_mode());
    if (changed) {
        if (on) {
            const auto& m = context_.session.data().meter;
            auto where = [&](Ticks t) {
                const int bar = m.bar_at(t);
                const auto sig = m.at_bar(bar);
                return juce::String(bar) + "." + juce::String((t - m.bar_start(bar)) / sig.beat_ticks() + 1);
            };
            context_.status("Punch in at " + where(range.first) + ", out at " + where(range.second));
        } else if (range.second > range.first) {
            context_.status("Punch off");
        }
    }
    context_.commands.commandStatusChanged();
}

void MainComponent::import_audio()
{
    chooser_ = std::make_unique<juce::FileChooser>("Import audio", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::canSelectMultipleItems,
                          [this](const juce::FileChooser& fc) {
                              const auto files = fc.getResults();
                              if (!files.isEmpty()) import_audio_files(files);
                          });
}

std::vector<std::string> MainComponent::import_audio_files(const juce::Array<juce::File>& files)
{
    auto& s = context_.session;
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    struct Item {
        juce::File file;
        WaveformCache::FileInfo info;
    };
    std::vector<Item> items;
    juce::StringArray unreadable;
    for (const auto& f : files) {
        Item item{f, {}};
        if (WaveformCache::read_info(formats, f, item.info)) items.push_back(item);
        else unreadable.add(f.getFileName());
    }
    if (items.empty()) {
        juce::AlertWindow::showAsync(juce::MessageBoxOptions::makeOptionsOk(
                                         juce::MessageBoxIconType::WarningIcon, "Could not import audio",
                                         "None of these files could be read as audio:\n" + unreadable.joinIntoString("\n"), {}, this),
                                     [](int) {});
        return {};
    }

    // The selected audio (or reference) track, or a new audio track.
    std::string track_id;
    if (const auto* t = s.data().find_track(s.selected_track());
        t && (t->kind == project::TrackKind::Audio || t->kind == project::TrackKind::Reference))
        track_id = t->id;
    if (track_id.empty())
        track_id = s.add_audio_track(items.front().file.getFileNameWithoutExtension().toStdString(),
                                     items.front().info.channels > 1);
    if (track_id.empty()) return {};

    // Inside a saved project the files are copied into "Audio Files" and
    // stored relative to the project; otherwise they are referenced where they are.
    const std::string folder = s.audio_folder();
    const juce::File audio_dir = folder.empty() ? juce::File() : juce::File(juce::String::fromUTF8(folder.c_str()));
    if (audio_dir != juce::File()) audio_dir.createDirectory();
    int copied = 0;
    std::vector<std::pair<std::string, project::AudioClip>> clips;
    double at = context_.engine.position_seconds();
    for (const auto& item : items) {
        std::string stored = item.file.getFullPathName().toStdString();
        if (audio_dir.isDirectory()) {
            juce::File dest = item.file.isAChildOf(audio_dir) ? item.file : audio_dir.getChildFile(item.file.getFileName());
            if (dest != item.file && dest.exists())
                dest = audio_dir.getNonexistentChildFile(item.file.getFileNameWithoutExtension(), item.file.getFileExtension(), false);
            if (dest == item.file || item.file.copyFileTo(dest)) {
                stored = (audio_dir.getFileName() + "/" + dest.getRelativePathFrom(audio_dir)).toStdString();
                copied += dest == item.file ? 0 : 1;
            }
        }
        project::AudioClip c;
        c.name = item.file.getFileNameWithoutExtension().toStdString();
        c.file = stored;
        c.start_seconds = at;
        c.length = item.info.length_seconds;
        c.channels = item.info.channels;
        c.file_sample_rate = item.info.sample_rate;
        clips.emplace_back(track_id, c);
        at += c.length;  // several files play one after another
    }
    const auto ids = s.add_audio_clips(clips, clips.size() == 1 ? "Import " + clips.front().second.name : "Import audio", false);
    s.select_track(track_id);
    juce::String msg = "Imported " + juce::String(static_cast<int>(ids.size())) + (ids.size() == 1 ? " audio file" : " audio files");
    if (copied > 0) msg << " (copied into " << audio_dir.getFileName() << ")";
    else if (folder.empty()) msg << " - referenced in place; save the project to keep copies with it";
    if (!unreadable.isEmpty()) msg << "; could not read " << unreadable.joinIntoString(", ");
    context_.status(msg);
    return ids;
}

void MainComponent::new_audio_track()
{
    auto* w = new juce::AlertWindow("New Audio Track", "A track for recorded or imported audio", juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("name", "Audio");
    w->addComboBox("format", {"Mono", "Stereo"}, "Input");
    w->getComboBoxComponent("format")->setSelectedItemIndex(0, juce::dontSendNotification);
    w->addButton("Add", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int r) {
        if (r != 1) return;
        const bool stereo = w->getComboBoxComponent("format")->getSelectedItemIndex() == 1;
        const juce::String name = w->getTextEditorContents("name").trim();
        context_.session.add_audio_track(name.isEmpty() ? "Audio" : name.toStdString(), stereo);
    }), true);
}

void MainComponent::new_bus(bool fx)
{
    auto* w = new juce::AlertWindow(fx ? "New Aux (FX) Bus" : "New Group Bus",
                                    fx ? "An effects return fed by track sends" : "A subgroup / stem that tracks can be routed to",
                                    juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("name", fx ? "Reverb" : "Group");
    w->addButton("Add", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([this, w, fx](int r) {
        if (r != 1) return;
        const juce::String name = w->getTextEditorContents("name").trim();
        const std::string id = context_.session.add_bus(name.toStdString(), fx ? "fx" : "group");
        if (const auto* b = context_.session.data().find_bus(id))
            context_.status("Added " + juce::String(fx ? "aux bus " : "group bus ") + juce::String(b->name) + " - see the Mixer");
    }), true);
}

void MainComponent::show_about()
{
    juce::AlertWindow::showMessageBoxAsync(
        juce::MessageBoxIconType::InfoIcon, "VC Composer 2",
        juce::String("Version ") + JUCE_APPLICATION_VERSION_STRING +
            "\n\nA notation-first scoring workstation.\n\n"
            "Built with JUCE (commercial JUCE 8 licence).\n"
            "Music font: Bravura, SIL Open Font License 1.1.\n"
            "Sounds marked \"Placeholder\" are the built-in stand-in until a library is assigned.");
}

// --- Commands ----------------------------------------------------------------------------------------

void MainComponent::getAllCommands(juce::Array<juce::CommandID>& c)
{
    c.addArray({cmd::newProject, cmd::newFromTemplate, cmd::open, cmd::save, cmd::saveAs, cmd::exportMidi,
                cmd::exportScoreSvg, cmd::importMusicXml, cmd::exportMusicXml, cmd::exportPartsPdf, cmd::exportFullScorePdf, cmd::exportStems, cmd::saveAsTemplate, cmd::undo, cmd::redo, cmd::play, cmd::rewind,
                cmd::toggleMetronome, cmd::toggleRecord, cmd::togglePunch, cmd::toggleLoop, cmd::addAudioTrack, cmd::importAudio,
                cmd::addAuxBus, cmd::addGroupBus, cmd::showComposer, cmd::showSequencer, cmd::showMixer, cmd::showVideo,
                cmd::showControlGallery, cmd::toggleView, cmd::zoomIn, cmd::zoomOut,
                cmd::showCueManager, cmd::importPicture, cmd::addHit, cmd::chordExplorer,
                cmd::ostinatoCreator, cmd::addTemplate, cmd::addTrack, cmd::duplicateVersion, cmd::removeTrack,
                cmd::audioSettings, cmd::scanPlugins, cmd::loadInstrument, cmd::openInstrumentWindow, cmd::reloadCrashedPlugins,
                cmd::about});
}

void MainComponent::getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    using KP = juce::KeyPress;
    const auto cmdKey = juce::ModifierKeys::commandModifier;
    const auto shift = juce::ModifierKeys::shiftModifier;
    const bool has_track = !context_.session.selected_track().empty();
    switch (id) {
        case cmd::newProject: info.setInfo("New Empty Project", "", "File", 0); info.addDefaultKeypress('n', cmdKey | shift); break;
        case cmd::newFromTemplate: info.setInfo("New from Template...", "", "File", 0); info.addDefaultKeypress('n', cmdKey); break;
        case cmd::open: info.setInfo("Open...", "", "File", 0); info.addDefaultKeypress('o', cmdKey); break;
        case cmd::save: info.setInfo("Save", "", "File", 0); info.addDefaultKeypress('s', cmdKey); break;
        case cmd::saveAs: info.setInfo("Save As...", "", "File", 0); info.addDefaultKeypress('s', cmdKey | shift); break;
        case cmd::exportMidi: info.setInfo("Export MIDI...", "", "File", 0); info.addDefaultKeypress('e', cmdKey); break;
        case cmd::exportScoreSvg: info.setInfo("Export Parts (SVG pages)...", "", "File", 0); break;
        case cmd::importMusicXml:
            info.setInfo("Import MusicXML...", "Adds a MusicXML or .mxl score's parts as tracks", "File", 0);
            break;
        case cmd::exportMusicXml:
            info.setInfo("Export MusicXML...", "For Sibelius, Finale, Dorico, MuseScore and engravers", "File", 0);
            break;
        case cmd::exportFullScorePdf:
            info.setInfo("Export Full Score (PDF)...", "Every part on one system, transposed or in C", "File", 0);
            break;
        case cmd::exportStems:
            info.setInfo("Export Stems...", "The mix and each stem as WAV, for the project or every cue", "File", 0);
            info.addDefaultKeypress('e', cmdKey | juce::ModifierKeys::shiftModifier);
            break;
        case cmd::exportPartsPdf:
            info.setInfo("Export Parts (PDF)...", "Printed parts, one PDF each or all in one", "File", 0);
            info.addDefaultKeypress('p', cmdKey | juce::ModifierKeys::shiftModifier);
            break;
        case cmd::saveAsTemplate: info.setInfo("Save as Template...", "", "File", 0); break;
        case cmd::undo:
            info.setInfo(context_.session.can_undo() ? "Undo " + juce::String(context_.session.undo_label()) : juce::String("Undo"),
                         "", "Edit", context_.session.can_undo() ? 0 : juce::ApplicationCommandInfo::isDisabled);
            info.addDefaultKeypress('z', cmdKey);
            break;
        case cmd::redo:
            info.setInfo("Redo", "", "Edit", context_.session.can_redo() ? 0 : juce::ApplicationCommandInfo::isDisabled);
            info.addDefaultKeypress('z', cmdKey | shift);
            break;
        case cmd::play: info.setInfo(context_.engine.playing() ? "Stop" : "Play", "", "Transport", 0); info.addDefaultKeypress(KP::spaceKey, 0); break;
        case cmd::rewind:
            info.setInfo("Return to Start", "", "Transport", 0);
            info.addDefaultKeypress(KP::homeKey, 0);
            info.addDefaultKeypress(KP::returnKey, 0);  // Enter (docs/spec/ui shortcut table)
            break;
        case cmd::toggleMetronome:
            info.setInfo("Metronome", "", "Transport", context_.engine.metronome() ? juce::ApplicationCommandInfo::isTicked : 0);
            info.addDefaultKeypress('k', cmdKey);
            break;
        case cmd::toggleRecord:
            // R, as in most DAWs; in the Composer's note entry R is the rest key and wins there.
            info.setInfo("Record", "Record enable", "Transport", context_.engine.record_enabled() ? juce::ApplicationCommandInfo::isTicked : 0);
            info.addDefaultKeypress('r', 0);
            break;
        case cmd::toggleLoop:
            info.setInfo("Loop Playback", "Play the selected range over and over", "Transport",
                         context_.engine.loop() ? juce::ApplicationCommandInfo::isTicked : 0);
            info.addDefaultKeypress('l', 0);
            break;
        case cmd::togglePunch:
            info.setInfo("Punch In/Out", "Record only inside the sequencer ruler range", "Transport",
                         punch_ ? juce::ApplicationCommandInfo::isTicked : 0);
            info.addDefaultKeypress('p', cmdKey | shift);
            break;
        case cmd::addAudioTrack: info.setInfo("New Audio Track...", "", "Track", 0); info.addDefaultKeypress('t', cmdKey | shift); break;
        case cmd::importAudio: info.setInfo("Import Audio...", "", "File", 0); info.addDefaultKeypress('i', cmdKey | juce::ModifierKeys::altModifier); break;
        case cmd::addAuxBus: info.setInfo("New Aux (FX) Bus...", "", "Track", 0); break;
        case cmd::addGroupBus: info.setInfo("New Group Bus...", "", "Track", 0); break;
        case cmd::showComposer:
            info.setInfo("Composer View", "", "View", workspace_ == Workspace::Composer ? juce::ApplicationCommandInfo::isTicked : 0);
            break;
        case cmd::showSequencer:
            info.setInfo("Producer View", "", "View", workspace_ == Workspace::Sequencer ? juce::ApplicationCommandInfo::isTicked : 0);
            break;
        case cmd::showMixer: info.setInfo("Mixer", "", "View", 0); info.addDefaultKeypress('m', cmdKey); break;
        case cmd::showControlGallery: info.setInfo("Control Gallery", "", "View", 0); break;
        case cmd::zoomIn: info.setInfo("Zoom In", "", "View", 0); info.addDefaultKeypress('=', cmdKey); break;
        case cmd::zoomOut: info.setInfo("Zoom Out", "", "View", 0); info.addDefaultKeypress('-', cmdKey); break;
        case cmd::toggleView:
            info.setInfo("Switch Composer / Producer", "", "View", 0);
            info.addDefaultKeypress(juce::KeyPress::tabKey, juce::ModifierKeys::ctrlModifier);
            info.addDefaultKeypress('`', juce::ModifierKeys::commandModifier);
            break;
        case cmd::showVideo: info.setInfo("Picture", "", "View", 0); info.addDefaultKeypress('p', cmdKey | juce::ModifierKeys::altModifier); break;
        case cmd::showCueManager: info.setInfo("Cue Manager", "", "View", 0); info.addDefaultKeypress('u', cmdKey | juce::ModifierKeys::altModifier); break;
        case cmd::importPicture: info.setInfo("Import Picture...", "", "File", 0); info.addDefaultKeypress('i', cmdKey | shift); break;
        case cmd::addHit: info.setInfo("Mark Hit at Playhead", "", "Transport", 0); info.addDefaultKeypress('h', cmdKey | shift); break;
        case cmd::chordExplorer: info.setInfo("Chord Explorer...", "", "Tools", 0); info.addDefaultKeypress('h', cmdKey | shift); break;
        case cmd::ostinatoCreator: info.setInfo("Ostinato Creator...", "", "Tools", 0); info.addDefaultKeypress('o', cmdKey | shift); break;
        case cmd::addTemplate: info.setInfo("Add Template to Project...", "", "Tools", 0); break;
        case cmd::addTrack: info.setInfo("Add Track...", "", "Track", 0); info.addDefaultKeypress('t', cmdKey); break;
        case cmd::duplicateVersion: info.setInfo("Duplicate as New Version", "", "Track", has_track ? 0 : juce::ApplicationCommandInfo::isDisabled); info.addDefaultKeypress('d', cmdKey | shift); break;
        case cmd::removeTrack: info.setInfo("Remove Track", "", "Track", has_track ? 0 : juce::ApplicationCommandInfo::isDisabled); break;
        case cmd::loadInstrument: info.setInfo("Instrument...", "", "Track", has_track ? 0 : juce::ApplicationCommandInfo::isDisabled); break;
        case cmd::openInstrumentWindow: info.setInfo("Open Instrument Window", "", "Track", has_track ? 0 : juce::ApplicationCommandInfo::isDisabled); break;
        case cmd::audioSettings: info.setInfo("Audio and MIDI Settings...", "", "Tools", 0); info.addDefaultKeypress(',', cmdKey); break;
        case cmd::scanPlugins: info.setInfo("Plug-ins...", "", "Tools", 0); break;
        case cmd::reloadCrashedPlugins:
            info.setInfo("Reload Crashed Plug-ins", "Starts crashed plug-ins again from their last state", "Tools", 0);
            break;
        case cmd::about: info.setInfo("About VC Composer 2", "", "Help", 0); break;
        default: break;
    }
}

bool MainComponent::perform(const InvocationInfo& info)
{
    auto& s = context_.session;
    switch (info.commandID) {
        case cmd::newProject:
            if (confirm_discard()) s.new_empty();
            break;
        case cmd::newFromTemplate:
            if (confirm_discard()) show_template_browser(context_, true);
            break;
        case cmd::open:
            if (!confirm_discard()) break;
            chooser_ = std::make_unique<juce::FileChooser>("Open project", juce::File(), "*.vcc2");
            chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this](const juce::FileChooser& fc) {
                                      if (fc.getResult() != juce::File()) open_file(fc.getResult());
                                  });
            break;
        case cmd::save: save(false); break;
        case cmd::saveAs: save(true); break;
        case cmd::exportMidi: export_midi(); break;
        case cmd::exportScoreSvg:
            chooser_ = std::make_unique<juce::FileChooser>("Choose a folder for the parts", juce::File());
            chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                  [this](const juce::FileChooser& fc) {
                                      if (fc.getResult().isDirectory()) export_parts_svg(context_, fc.getResult());
                                  });
            break;
        case cmd::importMusicXml: import_musicxml(); break;
        case cmd::exportMusicXml: export_musicxml(); break;
        case cmd::exportPartsPdf: export_parts_pdf_dialog(); break;
        case cmd::exportFullScorePdf: export_full_score_dialog(); break;
        case cmd::exportStems: show_stem_export(context_); break;
        case cmd::saveAsTemplate: save_as_template(); break;
        case cmd::undo: s.undo(); break;
        case cmd::redo: s.redo(); break;
        case cmd::play: context_.engine.toggle_play(); break;
        case cmd::rewind: context_.engine.locate(0); break;
        case cmd::toggleMetronome: context_.engine.set_metronome(!context_.engine.metronome()); break;
        case cmd::toggleRecord: toggle_record(); break;
        case cmd::togglePunch: set_punch(!punch_); break;
        case cmd::toggleLoop: set_loop(!context_.engine.loop()); break;
        case cmd::addAudioTrack: new_audio_track(); break;
        case cmd::importAudio: import_audio(); break;
        case cmd::addAuxBus: new_bus(true); break;
        case cmd::addGroupBus: new_bus(false); break;
        case cmd::showComposer: show_workspace(Workspace::Composer); break;
        case cmd::showSequencer: show_workspace(Workspace::Sequencer); break;
        case cmd::showMixer: show_mixer(); break;
        case cmd::showControlGallery: show_control_gallery(); break;
        case cmd::toggleView:
            show_workspace(workspace_ == Workspace::Composer ? Workspace::Producer : Workspace::Composer);
            break;
        case cmd::zoomIn:
        case cmd::zoomOut:
            if (workspace_ == Workspace::Composer) composer_.zoom(info.commandID == cmd::zoomIn);
            else sequencer_.zoom(info.commandID == cmd::zoomIn);
            break;
        case cmd::showVideo: show_video_window(context_); break;
        case cmd::showCueManager: show_cue_manager(context_); break;
        case cmd::importPicture:
            chooser_ = std::make_unique<juce::FileChooser>("Import picture", juce::File(), "*.mov;*.mp4;*.m4v;*.mxf;*.avi;*.mkv");
            chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this](const juce::FileChooser& fc) {
                                      if (fc.getResult() == juce::File()) return;
                                      juce::String error;
                                      if (import_picture_file(context_, fc.getResult(), error)) show_video_window(context_);
                                      else juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                                  "Could not import picture", error);
                                  });
            break;
        case cmd::addHit: {
            const double now = context_.engine.position_seconds();
            s.add_marker(now, "hit");
            context_.status("Hit at " + juce::String(picture::project_timecode(s.data(), now)));
            break;
        }
        case cmd::chordExplorer: show_chord_explorer(context_); break;
        case cmd::ostinatoCreator: show_ostinato_creator(context_); break;
        case cmd::addTemplate: show_template_browser(context_, false); break;
        case cmd::addTrack: TrackPanel::show_add_track_menu(context_, &tracks_); break;
        case cmd::duplicateVersion:
            if (!s.selected_track().empty()) s.duplicate_version(s.selected_track());
            break;
        case cmd::removeTrack:
            if (!s.selected_track().empty()) s.remove_track(s.selected_track());
            break;
        case cmd::loadInstrument:
            if (!s.selected_track().empty()) TrackPanel::show_track_menu(context_, s.selected_track());
            break;
        case cmd::openInstrumentWindow:
            if (!s.selected_track().empty()) show_plugin_window(s.selected_track());
            break;
        case cmd::audioSettings: show_audio_settings(); break;
        case cmd::scanPlugins: show_plugin_scanner(); break;
        case cmd::reloadCrashedPlugins: {
            const int n = context_.engine.reload_crashed_plugins();
            context_.status(n == 0 ? juce::String("No plug-in has crashed")
                                   : "Reloaded " + juce::String(n) + " crashed plug-in" + (n == 1 ? "" : "s"));
            break;
        }
        case cmd::about: show_about(); break;
        default: return false;
    }
    return true;
}

// --- Menus -------------------------------------------------------------------------------------

juce::StringArray MainComponent::getMenuBarNames() { return {"File", "Edit", "View", "Transport", "Track", "Tools", "Help"}; }

juce::PopupMenu MainComponent::getMenuForIndex(int index, const juce::String&)
{
    juce::PopupMenu m;
    auto* c = &context_.commands;
    switch (index) {
        case 0:
            for (auto id : {cmd::newFromTemplate, cmd::newProject, cmd::open}) m.addCommandItem(c, id);
            m.addSeparator();
            m.addCommandItem(c, cmd::importPicture);
            m.addCommandItem(c, cmd::importAudio);
            m.addCommandItem(c, cmd::importMusicXml);
            m.addSeparator();
            for (auto id : {cmd::save, cmd::saveAs, cmd::saveAsTemplate}) m.addCommandItem(c, id);
            m.addSeparator();
            for (auto id : {cmd::exportStems, cmd::exportMidi, cmd::exportMusicXml, cmd::exportFullScorePdf, cmd::exportPartsPdf,
                            cmd::exportScoreSvg})
                m.addCommandItem(c, id);
            break;
        case 1:
            m.addCommandItem(c, cmd::undo);
            m.addCommandItem(c, cmd::redo);
            m.addSeparator();
            m.addSubMenu("Keyboard Shortcuts", shortcuts_menu());
            break;
        case 2:
            for (auto id : {cmd::showComposer, cmd::showSequencer, cmd::toggleView, cmd::showMixer, cmd::showVideo,
                            cmd::showCueManager})
                m.addCommandItem(c, id);
            m.addSeparator();
            m.addCommandItem(c, cmd::zoomIn);
            m.addCommandItem(c, cmd::zoomOut);
            m.addSeparator();
            m.addSubMenu("Theme", theme_menu());
            m.addCommandItem(c, cmd::showControlGallery);
            break;
        case 3:
            for (auto id : {cmd::play, cmd::rewind}) m.addCommandItem(c, id);
            m.addSeparator();
            for (auto id : {cmd::toggleLoop, cmd::toggleRecord, cmd::togglePunch}) m.addCommandItem(c, id);
            m.addSeparator();
            for (auto id : {cmd::toggleMetronome, cmd::addHit}) m.addCommandItem(c, id);
            break;
        case 4:
            for (auto id : {cmd::addTrack, cmd::addAudioTrack}) m.addCommandItem(c, id);
            for (auto id : {cmd::addAuxBus, cmd::addGroupBus}) m.addCommandItem(c, id);
            m.addSeparator();
            for (auto id : {cmd::duplicateVersion, cmd::loadInstrument, cmd::openInstrumentWindow, cmd::removeTrack})
                m.addCommandItem(c, id);
            m.addSeparator();
            {
                const std::string track = context_.session.selected_track();
                const bool shown = !track.empty() && sequencer_.automation_shown(track);
                m.addItem(shown ? "Hide Automation" : "Show Automation", !track.empty(), false, [this, track, shown] {
                    sequencer_.show_automation(track, !shown);
                    if (!shown) show_workspace(Workspace::Sequencer);
                });
                m.addItem("Show Master Automation", true, sequencer_.automation_shown("bus-master"), [this] {
                    const bool on = !sequencer_.automation_shown("bus-master");
                    sequencer_.show_automation("bus-master", on);
                    if (on) show_workspace(Workspace::Sequencer);
                });
                m.addItem("Hide All Automation", [this] { sequencer_.hide_all_automation(); });
            }
            break;
        case 5:
            for (auto id : {cmd::chordExplorer, cmd::ostinatoCreator, cmd::addTemplate}) m.addCommandItem(c, id);
            m.addSeparator();
            for (auto id : {cmd::audioSettings, cmd::scanPlugins, cmd::reloadCrashedPlugins}) m.addCommandItem(c, id);
            break;
        case 6: m.addCommandItem(c, cmd::about); break;
        default: break;
    }
    return m;
}

}  // namespace vcc::desktop
