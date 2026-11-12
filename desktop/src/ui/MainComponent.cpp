#include "ui/MainComponent.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include "ui/CueManager.h"
#include "ui/Dialogs.h"
#include "ui/VideoWindow.h"
#include "ui/WaveformCache.h"
#include "vcc/picture/Picture.h"
#include "ui/Theme.h"
#include "vcc/core/Files.h"
#include "vcc/engraving/PrintLayout.h"
#include "vcc/engraving/SvgRenderer.h"
#include "vcc/project/Render.h"

namespace vcc::desktop {

// --- Plug-in windows --------------------------------------------------------------------------

class MainComponent::PluginWindow final : public juce::DocumentWindow {
public:
    PluginWindow(AppContext& c, std::string track_id, juce::AudioPluginInstance& instance)
        : juce::DocumentWindow(instance.getName(), colours::background, juce::DocumentWindow::closeButton),
          context_(c), track_id_(std::move(track_id)), instance_(instance)
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

    void store() { context_.session.store_plugin_state(track_id_, PluginHost::state_of(instance_)); }

    void closeButtonPressed() override
    {
        store();
        setVisible(false);
    }

    juce::AudioPluginInstance* instance() noexcept { return &instance_; }

private:
    AppContext& context_;
    std::string track_id_;
    juce::AudioPluginInstance& instance_;
};

// --- Main component -----------------------------------------------------------------------------

MainComponent::MainComponent(AppContext& context)
    : context_(context), transport_(context), tracks_(context), composer_(context), sequencer_(context)
{
    addAndMakeVisible(transport_);
    addAndMakeVisible(tracks_);
    addAndMakeVisible(composer_);
    addChildComponent(sequencer_);
    addAndMakeVisible(status_);
    status_.setFont(juce::FontOptions(12.0f));
    status_.setColour(juce::Label::textColourId, colours::text_dim);
    status_.setColour(juce::Label::backgroundColourId, colours::panel);

    layout_.setItemLayout(0, 220, 520, 300);
    layout_.setItemLayout(1, 5, 5, 5);
    layout_.setItemLayout(2, 300, -1.0, -1.0);
    divider_ = std::make_unique<juce::StretchableLayoutResizerBar>(&layout_, 1, true);
    addAndMakeVisible(*divider_);

    context_.status = [this](const juce::String& message) {
        status_.setText(message, juce::dontSendNotification);
        status_age_ = 0;
    };
    context_.show_plugin_window = [this](const std::string& id) { show_plugin_window(id); };

    listener_ = context_.session.add_listener([this](session::Change c) {
        // A MIDI keyboard plays whichever track is selected.
        if (c == session::Change::Selection) context_.engine.set_live_track(context_.session.selected_track());
        if (c == session::Change::Document) {
            // Close plug-in windows whose instrument changed or went away.
            for (auto it = plugin_windows_.begin(); it != plugin_windows_.end();) {
                if (context_.engine.plugin_for(it->first) != it->second->instance()) it = plugin_windows_.erase(it);
                else ++it;
            }
        }
    });

    context_.engine.on_transport_changed = [this] { transport_.update_position(); };
    // A recording pass was placed (one undo step), or failed.
    context_.engine.on_recording_finished = [this](const juce::String& message, bool ok) {
        context_.status(message);
        transport_.update_position();
        if (!ok)
            juce::AlertWindow::showAsync(juce::MessageBoxOptions::makeOptionsOk(juce::MessageBoxIconType::WarningIcon,
                                                                                "Recording", message, {}, this),
                                         [](int) {});
    };
    sequencer_.on_range_changed = [this] {
        if (punch_) set_punch(true);  // follow the new range, or turn off when it was cleared
    };
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
}

void MainComponent::paint(juce::Graphics& g) { g.fillAll(colours::background); }

void MainComponent::resized()
{
    auto r = getLocalBounds();
    transport_.setBounds(r.removeFromTop(52));
    status_.setBounds(r.removeFromBottom(22));
    juce::Component* items[] = {&tracks_, divider_.get(), workspace_ == Workspace::Composer
                                                              ? static_cast<juce::Component*>(&composer_)
                                                              : static_cast<juce::Component*>(&sequencer_)};
    layout_.layOutComponents(items, 3, r.getX(), r.getY(), r.getWidth(), r.getHeight(), false, true);
    (workspace_ == Workspace::Composer ? static_cast<juce::Component&>(sequencer_) : composer_)
        .setBounds(items[2]->getBounds());
}

void MainComponent::show_workspace(Workspace w)
{
    workspace_ = w;
    composer_.setVisible(w == Workspace::Composer);
    sequencer_.setVisible(w == Workspace::Sequencer);
    resized();
    if (w == Workspace::Composer) composer_.grabKeyboardFocus();
    else sequencer_.grabKeyboardFocus();
}

void MainComponent::timerCallback()
{
    const Ticks tick = context_.engine.position();
    const bool playing = context_.engine.playing();
    transport_.update_position();
    if (composer_.isVisible()) composer_.set_playhead(tick, playing);
    if (sequencer_.isVisible()) sequencer_.set_playhead(tick, playing);

    // Crash protection: an autosave every minute while there are unsaved changes.
    if (++ticks_ >= 30 * 60) {
        ticks_ = 0;
        if (context_.session.dirty()) {
            capture_plugin_states();
            context_.session.write_autosave(user_data_directory().getFullPathName().toStdString());
        }
    }
    if (++status_age_ == 30 * 8) status_.setText({}, juce::dontSendNotification);
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
    for (const auto* t : context_.session.music_tracks())
        if (auto* p = context_.engine.plugin_for(t->id))
            context_.session.store_plugin_state(t->id, PluginHost::state_of(*p));
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
    auto& w = plugin_windows_[id];
    if (!w || w->instance() != instance) w = std::make_unique<PluginWindow>(context_, id, *instance);
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
    class Scanner final : public juce::PluginListComponent {
    public:
        Scanner(AppContext& c)
            : juce::PluginListComponent(c.plugins.formats(), c.plugins.known(), c.plugins.dead_mans_pedal(),
                                        c.plugins.properties(), true),
              context_(c)
        {
            setSize(640, 480);
        }
        ~Scanner() override
        {
            context_.plugins.save_list();
            context_.engine.rebuild_now();
        }

    private:
        AppContext& context_;
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
    transport_.update_position();
    context_.commands.commandStatusChanged();
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
    transport_.set_punch_state(on);
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
                cmd::exportScoreSvg, cmd::saveAsTemplate, cmd::undo, cmd::redo, cmd::play, cmd::rewind,
                cmd::toggleMetronome, cmd::toggleRecord, cmd::togglePunch, cmd::addAudioTrack, cmd::importAudio,
                cmd::addAuxBus, cmd::addGroupBus, cmd::showComposer, cmd::showSequencer, cmd::showMixer, cmd::showVideo,
                cmd::showCueManager, cmd::importPicture, cmd::addHit, cmd::chordExplorer,
                cmd::ostinatoCreator, cmd::addTemplate, cmd::addTrack, cmd::duplicateVersion, cmd::removeTrack,
                cmd::audioSettings, cmd::scanPlugins, cmd::loadInstrument, cmd::openInstrumentWindow, cmd::about});
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
        case cmd::rewind: info.setInfo("Return to Start", "", "Transport", 0); info.addDefaultKeypress(KP::homeKey, 0); break;
        case cmd::toggleMetronome:
            info.setInfo("Metronome", "", "Transport", context_.engine.metronome() ? juce::ApplicationCommandInfo::isTicked : 0);
            info.addDefaultKeypress('k', cmdKey);
            break;
        case cmd::toggleRecord:
            // R, as in most DAWs; in the Composer's note entry R is the rest key and wins there.
            info.setInfo("Record", "Record enable", "Transport", context_.engine.record_enabled() ? juce::ApplicationCommandInfo::isTicked : 0);
            info.addDefaultKeypress('r', 0);
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
            info.setInfo("Composer (score)", "", "View", workspace_ == Workspace::Composer ? juce::ApplicationCommandInfo::isTicked : 0);
            info.addDefaultKeypress('1', cmdKey);
            break;
        case cmd::showSequencer:
            info.setInfo("Sequencer", "", "View", workspace_ == Workspace::Sequencer ? juce::ApplicationCommandInfo::isTicked : 0);
            info.addDefaultKeypress('2', cmdKey);
            break;
        case cmd::showMixer: info.setInfo("Mixer", "", "View", 0); info.addDefaultKeypress('m', cmdKey); break;
        case cmd::showVideo: info.setInfo("Picture", "", "View", 0); info.addDefaultKeypress('3', cmdKey); break;
        case cmd::showCueManager: info.setInfo("Cue Manager", "", "View", 0); info.addDefaultKeypress('4', cmdKey); break;
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
        case cmd::saveAsTemplate: save_as_template(); break;
        case cmd::undo: s.undo(); break;
        case cmd::redo: s.redo(); break;
        case cmd::play: context_.engine.toggle_play(); break;
        case cmd::rewind: context_.engine.locate(0); break;
        case cmd::toggleMetronome: context_.engine.set_metronome(!context_.engine.metronome()); break;
        case cmd::toggleRecord: toggle_record(); break;
        case cmd::togglePunch: set_punch(!punch_); break;
        case cmd::addAudioTrack: new_audio_track(); break;
        case cmd::importAudio: import_audio(); break;
        case cmd::addAuxBus: new_bus(true); break;
        case cmd::addGroupBus: new_bus(false); break;
        case cmd::showComposer: show_workspace(Workspace::Composer); break;
        case cmd::showSequencer: show_workspace(Workspace::Sequencer); break;
        case cmd::showMixer: show_mixer(); break;
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
            m.addSeparator();
            for (auto id : {cmd::save, cmd::saveAs, cmd::saveAsTemplate}) m.addCommandItem(c, id);
            m.addSeparator();
            for (auto id : {cmd::exportMidi, cmd::exportScoreSvg}) m.addCommandItem(c, id);
            break;
        case 1:
            m.addCommandItem(c, cmd::undo);
            m.addCommandItem(c, cmd::redo);
            break;
        case 2:
            for (auto id : {cmd::showComposer, cmd::showSequencer, cmd::showMixer, cmd::showVideo, cmd::showCueManager})
                m.addCommandItem(c, id);
            break;
        case 3:
            for (auto id : {cmd::play, cmd::rewind}) m.addCommandItem(c, id);
            m.addSeparator();
            for (auto id : {cmd::toggleRecord, cmd::togglePunch}) m.addCommandItem(c, id);
            m.addSeparator();
            for (auto id : {cmd::toggleMetronome, cmd::addHit}) m.addCommandItem(c, id);
            break;
        case 4:
            for (auto id : {cmd::addTrack, cmd::addAudioTrack}) m.addCommandItem(c, id);
            for (auto id : {cmd::addAuxBus, cmd::addGroupBus}) m.addCommandItem(c, id);
            m.addSeparator();
            for (auto id : {cmd::duplicateVersion, cmd::loadInstrument, cmd::openInstrumentWindow, cmd::removeTrack})
                m.addCommandItem(c, id);
            break;
        case 5:
            for (auto id : {cmd::chordExplorer, cmd::ostinatoCreator, cmd::addTemplate}) m.addCommandItem(c, id);
            m.addSeparator();
            for (auto id : {cmd::audioSettings, cmd::scanPlugins}) m.addCommandItem(c, id);
            break;
        case 6: m.addCommandItem(c, cmd::about); break;
        default: break;
    }
    return m;
}

}  // namespace vcc::desktop
