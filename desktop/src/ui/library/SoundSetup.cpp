#include "ui/library/SoundSetup.h"

#include <algorithm>
#include <map>
#include <set>

#include "ui/Dialogs.h"
#include "ui/Theme.h"
#include "ui/hw/HwTheme.h"
#include "ui/library/LibraryBrowser.h"

namespace vcc::desktop {

namespace {

/// Its own window: resizable, a maximise button, full screen, and where it
/// was last (on any screen). Not kept on top, so a player's window opens in
/// front of it.
class SetupWindow final : public juce::DocumentWindow, private juce::DeletedAtShutdown {
public:
    explicit SetupWindow(SoundSetupPanel* panel)
        : juce::DocumentWindow("Sound Setup", colours::background, juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        setContentOwned(panel, false);
        setResizable(true, false);
        setResizeLimits(960, 620, 8000, 6000);
        const auto saved = juce::Rectangle<int>::fromString(hw::ui_settings().getValue("sound_setup_bounds"));
        bool on_screen = false;
        for (const auto& d : juce::Desktop::getInstance().getDisplays().displays)
            on_screen = on_screen || d.userArea.intersects(saved.reduced(40));
        if (on_screen && saved.getWidth() >= 960) setBounds(saved);
        else dialogs::fit_to_screen(*this, 1440, 900);
        setVisible(true);
    }
    void closeButtonPressed() override
    {
        if (!isFullScreen()) {
            hw::ui_settings().setValue("sound_setup_bounds", getBounds().toString());
            hw::ui_settings().saveIfNeeded();
        }
        setVisible(false);
        juce::Component::SafePointer<SetupWindow> self(this);
        juce::MessageManager::callAsync([self] {
            if (self != nullptr) delete self.getComponent();
        });
    }
};

juce::Component::SafePointer<SetupWindow>& window_slot()
{
    static juce::Component::SafePointer<SetupWindow> w;
    return w;
}
SoundSetupPanel*& panel_slot()
{
    static SoundSetupPanel* p = nullptr;
    return p;
}

juce::String label_of(const std::string& articulation)
{
    if (articulation.empty()) return "Whole instrument";
    juce::String s(articulation);
    s = s.replace("_", " ");
    return s.substring(0, 1).toUpperCase() + s.substring(1);
}

void style_heading(juce::Label& l, float size)
{
    l.setFont(juce::FontOptions(size, juce::Font::bold));
    l.setColour(juce::Label::textColourId, colours::text);
}

void style_text(juce::Label& l)
{
    l.setFont(juce::FontOptions(14.5f));
    l.setColour(juce::Label::textColourId, colours::text_dim);
    l.setJustificationType(juce::Justification::topLeft);
    l.setMinimumHorizontalScale(1.0f);
}

/// A simple list of rows drawn by a function.
class Rows : public juce::ListBox, private juce::ListBoxModel {
public:
    std::function<int()> count;
    std::function<void(int row, juce::Graphics&, int w, int h, bool selected)> draw;
    std::function<void(int row)> chosen, double_clicked;
    Rows()
    {
        setModel(this);
        setColour(juce::ListBox::backgroundColourId, colours::panel);
        setColour(juce::ListBox::outlineColourId, colours::outline);
        setOutlineThickness(1);
    }
    ~Rows() override { setModel(nullptr); }

private:
    int getNumRows() override { return count ? count() : 0; }
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (selected) g.fillAll(colours::selection);
        if (draw) draw(row, g, w, h, selected);
        g.setColour(colours::outline.withAlpha(0.5f));
        g.drawHorizontalLine(h - 1, 0.0f, static_cast<float>(w));
    }
    void selectedRowsChanged(int row) override
    {
        if (row >= 0 && chosen) chosen(row);
    }
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
    {
        if (double_clicked) double_clicked(row);
    }
};

}  // namespace

class SoundSetupPanel::TrackList final : public Rows {};
class SoundSetupPanel::PlayerList final : public Rows {};
class SoundSetupPanel::SavedList final : public Rows {};
class SoundSetupPanel::LibraryList final : public Rows {};

SoundSetupPanel::SoundSetupPanel(AppContext& context)
    : context_(context), track_list_(std::make_unique<TrackList>()), player_list_(std::make_unique<PlayerList>()),
      saved_list_(std::make_unique<SavedList>()), library_list_(std::make_unique<LibraryList>())
{
    panel_slot() = this;
    setComponentID("sound_setup");
    tracks_title_.setText("TRACKS", juce::dontSendNotification);
    style_heading(tracks_title_, 12.0f);
    style_heading(track_title_, 22.0f);
    style_text(track_info_);
    step1_.setText("1.  CHOOSE THE PLAYER (EASTWEST OPUS, PLAY, KONTAKT...)", juce::dontSendNotification);
    step2_.setText("2.  LOAD THE SOUND IN THE PLAYER'S WINDOW", juce::dontSendNotification);
    step3_.setText("3.  NAME IT AND SAVE IT", juce::dontSendNotification);
    saved_title_.setText("YOUR SAVED SOUNDS FOR THIS INSTRUMENT (DOUBLE-CLICK TO USE)", juce::dontSendNotification);
    for (auto* l : {&step1_, &step2_, &step3_, &saved_title_}) {
        style_heading(*l, 12.0f);
        l->setColour(juce::Label::textColourId, colours::accent_warm);
    }
    style_text(step2_text_);
    step2_text_.setText("The player's own window opens over this one (move it to another screen if you like). Browse your "
                        "library there and load the patch you want - for example in Opus: Hollywood Strings > 1st Violins > "
                        "Pizzicato. Play a note on your keyboard to hear it.",
                        juce::dontSendNotification);
    style_text(status_);
    status_.setColour(juce::Label::textColourId, colours::accent);
    name_label_.setText("Name", juce::dontSendNotification);
    artic_label_.setText("Plays", juce::dontSendNotification);
    for (auto* l : {&name_label_, &artic_label_}) style_text(*l);
    name_.setTextToShowWhenEmpty("Violins I - Pizzicato", colours::text_dim);
    name_.setFont(juce::FontOptions(15.0f));
    everywhere_.setToggleState(true, juce::dontSendNotification);
    everywhere_.setTooltip("Sound Assignments: new tracks and templates with this instrument and articulation get this sound by themselves");

    // Tracks.
    track_list_->setRowHeight(46);
    track_list_->count = [this] { return static_cast<int>(tracks_.size()); };
    track_list_->draw = [this](int row, juce::Graphics& g, int w, int h, bool) {
        const auto* t = context_.session.data().find_track(tracks_[static_cast<std::size_t>(row)]);
        if (!t) return;
        const bool plugin = !t->instrument_plugin.empty();
        const bool saved = plugin && !t->assignment.patch_id.empty();
        g.setColour(saved ? colours::exact : plugin ? colours::fallback : colours::placeholder);
        g.fillEllipse(10.0f, h / 2.0f - 5.0f, 10.0f, 10.0f);
        g.setColour(colours::text);
        g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
        juce::String name(t->name);
        if (!t->articulation_group.empty()) name = "   " + label_of(t->articulation_group);
        g.drawText(name, 28, 4, w - 34, h / 2 - 2, juce::Justification::bottomLeft, true);
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(12.0f));
        const juce::String sound = plugin ? juce::String(t->assignment.patch_display.empty() ? t->instrument_plugin.name
                                                                                            : t->assignment.patch_display)
                                          : juce::String("Built-in sound");
        g.drawText(sound, 28, h / 2 + 1, w - 34, h / 2 - 4, juce::Justification::topLeft, true);
    };
    track_list_->chosen = [this](int row) { show_track(tracks_[static_cast<std::size_t>(row)]); };

    // Players.
    player_list_->setRowHeight(34);
    player_list_->count = [this] { return static_cast<int>(players_.size()); };
    player_list_->draw = [this](int row, juce::Graphics& g, int w, int h, bool) {
        const auto* t = context_.session.data().find_track(track_);
        const auto& p = players_[static_cast<std::size_t>(row)];
        const bool current = t && t->instrument_plugin.identifier == p.second;
        g.setColour(current ? colours::exact : colours::text);
        g.setFont(juce::FontOptions(14.0f));
        g.drawText((current ? juce::String::fromUTF8("\xe2\x97\x8f ") : juce::String("   ")) + p.first, 8, 0, w - 16, h,
                   juce::Justification::centredLeft, true);
    };
    player_list_->double_clicked = [this](int row) {
        status_.setText(load_player(players_[static_cast<std::size_t>(row)].second), juce::dontSendNotification);
    };

    // Saved sounds.
    saved_list_->setRowHeight(30);
    saved_list_->count = [this] { return static_cast<int>(saved_for_track().size()); };
    saved_list_->draw = [this](int row, juce::Graphics& g, int w, int h, bool) {
        const auto saved = saved_for_track();
        if (row >= static_cast<int>(saved.size())) return;
        g.setColour(colours::text);
        g.setFont(juce::FontOptions(13.5f));
        g.drawText(saved[static_cast<std::size_t>(row)].first, 8, 0, w - 16, h, juce::Justification::centredLeft, true);
    };
    saved_list_->double_clicked = [this](int row) {
        const auto saved = saved_for_track();
        if (row < static_cast<int>(saved.size()))
            status_.setText(use_saved(saved[static_cast<std::size_t>(row)].second), juce::dontSendNotification);
    };

    load_.onClick = [this] {
        const int row = player_list_->getSelectedRow();
        if (row < 0 || row >= static_cast<int>(players_.size())) {
            status_.setText("Choose a player in the list first (EastWest Opus, PLAY, Kontakt...)", juce::dontSendNotification);
            return;
        }
        status_.setText(load_player(players_[static_cast<std::size_t>(row)].second), juce::dontSendNotification);
    };
    open_window_.onClick = [this] { status_.setText(open_player_window(), juce::dontSendNotification); };
    save_.onClick = [this] {
        const int a = articulation_.getSelectedId() - 2;
        const std::string artic = a >= 0 && a < static_cast<int>(articulations_.size()) ? articulations_[static_cast<std::size_t>(a)]
                                                                                       : std::string();
        status_.setText(save_sound(name_.getText(), artic, everywhere_.getToggleState()), juce::dontSendNotification);
    };
    use_saved_.onClick = [this] {
        const auto saved = saved_for_track();
        const int row = saved_list_->getSelectedRow();
        if (row < 0 || row >= static_cast<int>(saved.size())) {
            status_.setText("Choose one of your saved sounds first", juce::dontSendNotification);
            return;
        }
        status_.setText(use_saved(saved[static_cast<std::size_t>(row)].second), juce::dontSendNotification);
    };
    next_.onClick = [this] { next_track(1); };
    scan_.onClick = [this] { context_.commands.invokeDirectly(cmd::scanPlugins, true); };
    articulation_.onChange = [this] {
        if (name_.getText().isEmpty() || name_.getProperties()["auto"]) {
            name_.setText(suggested_name(), false);
            name_.getProperties().set("auto", true);
        }
    };
    name_.onTextChange = [this] { name_.getProperties().set("auto", false); };

    for (auto* k : {&load_, &open_window_, &save_, &use_saved_, &next_, &prev_, &scan_, &full_}) {
        k->set_legend_size(13.0f);
        k->setWantsKeyboardFocus(false);
    }
    load_.set_sublegend("on this track + open its window");
    open_window_.set_sublegend("load or change the patch");
    save_.set_sublegend("keep this sound, named");
    use_saved_.set_sublegend("the chosen saved sound");
    load_.setComponentID("setup_load");
    save_.setComponentID("setup_save");
    full_.setComponentID("setup_full");
    full_.onClick = [this] {
        if (auto* w = findParentComponentOfClass<juce::ResizableWindow>()) w->setFullScreen(!w->isFullScreen());
    };
    prev_.onClick = [this] { next_track(-1); };
    load_.setTooltip("Puts the chosen player on this track and opens its window, to load a patch in it");
    save_.setTooltip("Keeps the player with the patch you loaded (its settings) as a sound of your own");
    next_.setTooltip("The next track in the list");
    for (auto* c : std::initializer_list<juce::Component*>{
             &tracks_title_, track_list_.get(), &track_title_, &track_info_, &step1_, player_list_.get(), &load_, &scan_, &step2_,
             &step2_text_, &open_window_, &step3_, &name_label_, &name_, &artic_label_, &articulation_, &everywhere_, &save_,
             &saved_title_, saved_list_.get(), &use_saved_, &status_, &next_, &prev_, &full_})
        addAndMakeVisible(c);

    // The Libraries page.
    instruments_page_ = std::make_unique<InstrumentSoundsPage>(context_);
    addChildComponent(*instruments_page_);
    instruments_tab_.onClick = [this] { show_page(0); };
    sounds_tab_.onClick = [this] { show_page(1); };
    libraries_tab_.onClick = [this] { show_page(2); };
    instruments_tab_.setComponentID("setup_instruments_tab");
    instruments_tab_.set_legend_size(14.0f);
    instruments_tab_.setWantsKeyboardFocus(false);
    addAndMakeVisible(instruments_tab_);
    sounds_tab_.setComponentID("setup_sounds_tab");
    libraries_tab_.setComponentID("setup_libraries_tab");
    for (auto* k : {&sounds_tab_, &libraries_tab_, &scan_drives_, &add_folder_, &locate_, &remove_, &recheck_}) {
        k->set_legend_size(k == &sounds_tab_ || k == &libraries_tab_ ? 14.0f : 13.0f);
        k->setWantsKeyboardFocus(false);
    }
    scan_drives_.set_sublegend("every connected drive");
    add_folder_.set_sublegend("a folder to scan too");
    locate_.set_sublegend("show where it is now");
    recheck_.set_sublegend("is each one still there?");
    libraries_title_.setText("YOUR SAMPLE LIBRARIES", juce::dontSendNotification);
    style_heading(libraries_title_, 12.0f);
    libraries_title_.setColour(juce::Label::textColourId, colours::accent_warm);
    style_text(libraries_hint_);
    libraries_hint_.setText("VC Composer keeps where each library is (the drive's name and the folder). If a drive is "
                            "disconnected, renamed or gets a new letter, it looks for the library again and asks you "
                            "whether the place it found is right.",
                            juce::dontSendNotification);
    library_list_->setRowHeight(40);
    library_list_->count = [this] { return context_.library ? static_cast<int>(context_.library->lib().locations().size()) : 0; };
    library_list_->draw = [this](int row, juce::Graphics& g, int w, int h, bool) {
        const auto& locs = context_.library->lib().locations();
        if (row >= static_cast<int>(locs.size())) return;
        const auto& l = locs[static_cast<std::size_t>(row)];
        g.setColour(l.present ? colours::exact : colours::missing);
        g.fillEllipse(10.0f, h / 2.0f - 5.0f, 10.0f, 10.0f);
        const int name_w = juce::jmax(220, w * 36 / 100), vendor_w = 150, drive_w = 150;
        g.setColour(colours::text);
        g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
        g.drawText(juce::String(l.name), 28, 0, name_w - 32, h, juce::Justification::centredLeft, true);
        g.setFont(juce::FontOptions(13.0f));
        g.setColour(colours::text_dim);
        g.drawText(juce::String(l.vendor), name_w, 0, vendor_w - 8, h, juce::Justification::centredLeft, true);
        g.setColour(l.present ? colours::text : colours::missing);
        g.drawText(l.present ? juce::String(l.volume) : "NOT FOUND (" + juce::String(l.volume) + ")", name_w + vendor_w, 0, drive_w - 8, h,
                   juce::Justification::centredLeft, true);
        g.setColour(colours::text_dim);
        g.drawText(juce::String(l.relative.empty() ? l.path : l.relative), name_w + vendor_w + drive_w, 0, w - (name_w + vendor_w + drive_w) - 8, h,
                   juce::Justification::centredLeft, true);
    };
    scan_drives_.onClick = [this] {
        if (!context_.library || context_.library->scanning()) return;
        status_.setText("Scanning every drive for sample libraries...", juce::dontSendNotification);
        juce::Component::SafePointer<SoundSetupPanel> self(this);
        context_.library->scan_drives([self](int found, int added) {
            if (self == nullptr) return;
            self->status_.setText("Found " + juce::String(found) + " libraries (" + juce::String(added) + " new); each one's place is kept",
                                  juce::dontSendNotification);
            self->refresh_libraries();
        });
    };
    add_folder_.onClick = [this] {
        auto chooser = std::make_shared<juce::FileChooser>("A folder with sample libraries");
        juce::Component::SafePointer<SoundSetupPanel> self(this);
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                             [self, chooser](const juce::FileChooser& fc) {
                                 if (self == nullptr || fc.getResult() == juce::File() || !self->context_.library) return;
                                 self->context_.library->lib().library_folders().push_back(fc.getResult().getFullPathName().toStdString());
                                 self->context_.library->changed();
                                 self->scan_drives_.triggerClick();
                             });
    };
    locate_.onClick = [this] {
        const int row = library_list_->getSelectedRow();
        if (!context_.library || row < 0) {
            status_.setText("Choose a library in the list first", juce::dontSendNotification);
            return;
        }
        auto chooser = std::make_shared<juce::FileChooser>(
            "Where is " + juce::String(context_.library->lib().locations()[static_cast<std::size_t>(row)].name) + " now?");
        juce::Component::SafePointer<SoundSetupPanel> self(this);
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                             [self, chooser, row](const juce::FileChooser& fc) {
                                 if (self == nullptr || fc.getResult() == juce::File()) return;
                                 self->status_.setText(self->locate(static_cast<std::size_t>(row), fc.getResult()),
                                                       juce::dontSendNotification);
                             });
    };
    remove_.onClick = [this] {
        const int row = library_list_->getSelectedRow();
        if (!context_.library || row < 0) return;
        context_.library->lib().remove_location(context_.library->lib().locations()[static_cast<std::size_t>(row)].path);
        context_.library->changed();
        refresh_libraries();
    };
    recheck_.onClick = [this] {
        if (context_.library) context_.library->recheck_locations(false);
        refresh_libraries();
    };
    for (auto* c : std::initializer_list<juce::Component*>{&sounds_tab_, &libraries_tab_, library_list_.get(), &libraries_title_,
                                                          &libraries_hint_, &scan_drives_, &add_folder_, &locate_, &remove_, &recheck_})
        addChildComponent(c);
    sounds_tab_.setVisible(true);
    libraries_tab_.setVisible(true);
    if (context_.library) context_.library->addChangeListener(this);

    listener_ = context_.session.add_listener([this](session::Change) {
        juce::Component::SafePointer<SoundSetupPanel> self(this);
        juce::MessageManager::callAsync([self] {
            if (self == nullptr) return;
            self->refresh_tracks();
            self->track_list_->repaint();
            self->player_list_->repaint();
        });
    });
    refresh_tracks();
    refresh_players();
    const auto selected = context_.session.selected_track();
    show_track(std::find(tracks_.begin(), tracks_.end(), selected) != tracks_.end() ? selected
               : tracks_.empty()                                                    ? std::string()
                                                                                    : tracks_.front());
    show_page(0);
}

SoundSetupPanel::~SoundSetupPanel()
{
    if (panel_slot() == this) panel_slot() = nullptr;
    if (context_.library) context_.library->removeChangeListener(this);
    context_.session.remove_listener(listener_);
}

void SoundSetupPanel::changeListenerCallback(juce::ChangeBroadcaster*) { refresh_libraries(); }

void SoundSetupPanel::refresh_libraries()
{
    library_list_->updateContent();
    library_list_->repaint();
    if (!context_.library) return;
    const auto& locs = context_.library->lib().locations();
    int missing = 0;
    for (const auto& l : locs) missing += l.present ? 0 : 1;
    libraries_title_.setText("YOUR SAMPLE LIBRARIES  (" + juce::String(static_cast<int>(locs.size())) + " found" +
                                 (missing > 0 ? ", " + juce::String(missing) + " NOT FOUND" : juce::String()) + ")" +
                                 (context_.library->scanning() ? "  -  SCANNING..." : ""),
                             juce::dontSendNotification);
}

void SoundSetupPanel::show_page(int page)
{
    page_ = juce::jlimit(0, 2, page);
    instruments_tab_.setToggleState(page_ == 0, juce::dontSendNotification);
    sounds_tab_.setToggleState(page_ == 1, juce::dontSendNotification);
    libraries_tab_.setToggleState(page_ == 2, juce::dontSendNotification);
    instruments_page_->setVisible(page_ == 0);
    for (auto* c : std::initializer_list<juce::Component*>{track_list_.get(), &tracks_title_, &track_title_, &track_info_, &step1_,
                                                          player_list_.get(), &load_, &scan_, &step2_, &step2_text_, &open_window_,
                                                          &step3_, &name_label_, &name_, &artic_label_, &articulation_, &everywhere_,
                                                          &save_, &saved_title_, saved_list_.get(), &use_saved_, &next_, &prev_})
        c->setVisible(page_ == 1);
    for (auto* c : std::initializer_list<juce::Component*>{library_list_.get(), &libraries_title_, &libraries_hint_, &scan_drives_,
                                                          &add_folder_, &locate_, &remove_, &recheck_})
        c->setVisible(page_ == 2);
    status_.setVisible(page_ != 0);
    refresh_libraries();
    resized();
    repaint();
}

juce::String SoundSetupPanel::locate(std::size_t index, const juce::File& folder)
{
    if (!context_.library) return {};
    auto& lib = context_.library->lib();
    if (index >= lib.locations().size()) return {};
    const auto l = lib.locations()[index];
    if (!folder.isDirectory()) return "That folder is not there";
    library::Relocation r;
    r.index = index;
    r.name = l.name;
    r.from = l.path;
    r.to = folder.getFullPathName().toStdString();
    r.volume = l.volume;
#if JUCE_MAC
    if (folder.getFullPathName().startsWith("/Volumes/"))
        r.volume = folder.getFullPathName().fromFirstOccurrenceOf("/Volumes/", false, false).upToFirstOccurrenceOf("/", false, false).toStdString();
#elif JUCE_WINDOWS
    r.volume = folder.getFullPathName().upToFirstOccurrenceOf(":", true, false).toStdString();
#endif
    lib.apply_relocation(r);
    context_.library->changed();
    refresh_libraries();
    return juce::String(l.name) + " is now at " + folder.getFullPathName();
}

void SoundSetupPanel::refresh_tracks()
{
    tracks_.clear();
    for (const auto& t : context_.session.data().tracks)
        if (t.kind == project::TrackKind::Instrument && t.midi_output.empty()) tracks_.push_back(t.id);
    track_list_->updateContent();
}

void SoundSetupPanel::refresh_players()
{
    // EastWest first (Opus, PLAY), then by maker.
    std::vector<std::tuple<int, juce::String, juce::String, std::string>> list;
    for (const auto& d : context_.plugins.instruments()) {
        const auto maker = d.manufacturerName.isEmpty() ? juce::String("Other") : d.manufacturerName;
        const int rank = maker.containsIgnoreCase("east") && maker.containsIgnoreCase("west") ? 0 : 1;
        list.emplace_back(rank, maker, d.name + (d.pluginFormatName.isEmpty() ? juce::String() : "  (" + d.pluginFormatName + ")"),
                          d.createIdentifierString().toStdString());
    }
    std::sort(list.begin(), list.end());
    players_.clear();
    for (const auto& [rank, maker, name, id] : list) players_.emplace_back(maker + "  -  " + name, id);
    player_list_->updateContent();
}

void SoundSetupPanel::show_track(const std::string& track_id)
{
    track_ = track_id;
    if (!track_.empty()) context_.session.select_track(track_);
    const auto it = std::find(tracks_.begin(), tracks_.end(), track_);
    if (it != tracks_.end()) track_list_->selectRow(static_cast<int>(it - tracks_.begin()), true, true);
    refresh_track();
}

void SoundSetupPanel::refresh_track()
{
    const auto* t = context_.session.data().find_track(track_);
    if (!t) {
        track_title_.setText("No instrument tracks", juce::dontSendNotification);
        track_info_.setText("Add a track or a template first.", juce::dontSendNotification);
        return;
    }
    track_title_.setText(juce::String(t->name) + (t->articulation_group.empty() ? juce::String() : " - " + label_of(t->articulation_group)),
                         juce::dontSendNotification);
    const auto* def = context_.catalogs.instruments.find(t->instrument_standard_id);
    juce::String info = def ? juce::String(def->name) : juce::String("Instrument");
    info << "  |  Now: "
         << (t->instrument_plugin.empty() ? juce::String("the built-in sound")
                                          : juce::String(t->assignment.patch_display.empty() ? t->instrument_plugin.name
                                                                                             : t->assignment.patch_display));
    track_info_.setText(info, juce::dontSendNotification);
    // Articulations: the track's own first, then the vocabulary.
    articulations_.clear();
    std::set<std::string> seen;
    if (!t->articulation_group.empty() && seen.insert(t->articulation_group).second) articulations_.push_back(t->articulation_group);
    for (const auto& a : t->articulation_intents)
        if (seen.insert(a).second) articulations_.push_back(a);
    for (const auto& a : context_.catalogs.articulation_ids)
        if (seen.insert(a).second) articulations_.push_back(a);
    articulation_.clear(juce::dontSendNotification);
    articulation_.addItem(label_of({}), 1);
    for (std::size_t i = 0; i < articulations_.size(); ++i) articulation_.addItem(label_of(articulations_[i]), static_cast<int>(i) + 2);
    articulation_.setSelectedId(t->articulation_group.empty() ? 1 : 2, juce::dontSendNotification);
    name_.setText(suggested_name(), false);
    name_.getProperties().set("auto", true);
    saved_list_->updateContent();
    saved_list_->repaint();
    player_list_->repaint();
    track_list_->repaint();
}

juce::String SoundSetupPanel::suggested_name() const
{
    const auto* t = context_.session.data().find_track(track_);
    if (!t) return {};
    juce::String base(t->articulation_parent.empty() ? t->name : std::string());
    if (base.isEmpty())
        if (const auto* parent = context_.session.data().find_track(t->articulation_parent)) base = parent->name;
    if (base.isEmpty()) base = t->name;
    const int a = articulation_.getSelectedId() - 2;
    if (a >= 0 && a < static_cast<int>(articulations_.size())) base << " - " << label_of(articulations_[static_cast<std::size_t>(a)]);
    return base;
}

juce::String SoundSetupPanel::load_player(const std::string& identifier)
{
    const auto* t = context_.session.data().find_track(track_);
    if (!t) return "Choose a track first";
    for (const auto& d : context_.plugins.instruments())
        if (d.createIdentifierString().toStdString() == identifier) {
            if (t->instrument_plugin.identifier != identifier && !context_.session.set_instrument_plugin(track_, context_.plugins.ref_for(d)))
                return "The track is locked (approved)";
            if (context_.show_plugin_window) context_.show_plugin_window(track_);
            refresh_track();
            return d.name + " is on " + juce::String(t->name) + ": load your sound in its window, then name it and Save Sound";
        }
    return "That player is not installed here: scan for plug-ins";
}

juce::String SoundSetupPanel::open_player_window()
{
    const auto* t = context_.session.data().find_track(track_);
    if (!t || t->instrument_plugin.empty()) return "Load a player on this track first (step 1)";
    if (context_.show_plugin_window) context_.show_plugin_window(track_);
    return "The " + juce::String(t->instrument_plugin.name) + " window is open";
}

juce::String SoundSetupPanel::save_sound(const juce::String& name, const std::string& articulation, bool use_everywhere)
{
    const auto* t = context_.session.data().find_track(track_);
    if (!t || !context_.library) return "Choose a track first";
    if (t->instrument_plugin.empty()) return "Load a player and a sound in it first (steps 1 and 2)";
    const auto display = name.trim().isEmpty() ? suggested_name() : name.trim();
    juce::String message;
    const auto id = context_.library->save_track_sound(track_, display.toStdString(), message);
    if (id.empty()) return message;
    auto& lib = context_.library->lib();
    if (const auto* p = lib.find_patch(id)) {
        auto patch = *p;
        patch.articulations = articulation.empty() ? std::vector<std::string>{} : std::vector<std::string>{articulation};
        lib.update_patch(patch);
    }
    if (use_everywhere) lib.set_assignment(t->instrument_standard_id, articulation, id);
    context_.library->changed();
    refresh_track();
    return "Saved \"" + display + "\"" +
           (use_everywhere ? ": every " + juce::String(t->name) + (articulation.empty() ? "" : " " + label_of(articulation).toLowerCase()) +
                                 " track gets it from now on"
                           : "");
}

std::vector<std::pair<juce::String, std::string>> SoundSetupPanel::saved_for_track() const
{
    std::vector<std::pair<juce::String, std::string>> out;
    const auto* t = context_.session.data().find_track(track_);
    if (!t || !context_.library) return out;
    for (const auto& p : context_.library->lib().patches())
        if (p.instrument_id == t->instrument_standard_id) out.emplace_back(juce::String(p.display), p.id);
    return out;
}

juce::String SoundSetupPanel::use_saved(const std::string& patch_id)
{
    if (!context_.library) return {};
    const auto e = context_.library->lib().find("patch:" + patch_id);
    if (!e) return "That sound is gone";
    const auto r = context_.library->assign(*e, track_, true);
    refresh_track();
    return r;
}

void SoundSetupPanel::next_track(int step)
{
    const auto it = std::find(tracks_.begin(), tracks_.end(), track_);
    const long at = (it == tracks_.end() ? -1 : static_cast<long>(it - tracks_.begin())) + step;
    if (at < 0 || at >= static_cast<long>(tracks_.size())) {
        status_.setText(step > 0 ? "That was the last track" : "That is the first track", juce::dontSendNotification);
        return;
    }
    show_track(tracks_[static_cast<std::size_t>(at)]);
    status_.setText({}, juce::dontSendNotification);
}

void SoundSetupPanel::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
    g.setColour(colours::panel);
    g.fillRect(getLocalBounds().removeFromTop(kTopBar));
    g.setColour(colours::outline);
    g.drawHorizontalLine(kTopBar, 0.0f, static_cast<float>(getWidth()));
    if (page_ != 1) return;
    g.setColour(colours::panel);
    g.fillRect(getLocalBounds().withTrimmedTop(kTopBar + 1).removeFromLeft(left_w_));
    g.setColour(colours::outline);
    g.drawVerticalLine(left_w_, static_cast<float>(kTopBar), static_cast<float>(getHeight()));
}

void SoundSetupPanel::resized()
{
    // Proportions that grow with the window (full screen, another display).
    auto r = getLocalBounds();
    // The tabs across the top, Full Screen at the right.
    {
        auto top = r.removeFromTop(kTopBar).reduced(14, 8);
        for (auto* k : {&instruments_tab_, &sounds_tab_, &libraries_tab_}) {
            k->setBounds(top.removeFromLeft(170).reduced(0, 0));
            top.removeFromLeft(10);
        }
        full_.setBounds(top.removeFromRight(140));
        r.removeFromTop(1);
    }
    if (page_ == 0) {
        instruments_page_->setBounds(r);
        return;
    }
    if (page_ == 2) {
        layout_libraries(r);
        return;
    }
    const int left_w = juce::jlimit(280, 460, getWidth() / 4);
    left_w_ = left_w;
    auto left = r.removeFromLeft(left_w).reduced(14);
    tracks_title_.setBounds(left.removeFromTop(26));
    auto nav = left.removeFromBottom(48);
    prev_.setBounds(nav.removeFromLeft(nav.getWidth() / 2).reduced(0, 2).withTrimmedRight(4));
    next_.setBounds(nav.reduced(0, 2).withTrimmedLeft(4));
    left.removeFromBottom(8);
    track_list_->setBounds(left);

    r.reduce(24, 16);
    auto head = r.removeFromTop(40);
    track_title_.setBounds(head);
    track_info_.setBounds(r.removeFromTop(24));
    status_.setBounds(r.removeFromBottom(30));
    r.removeFromBottom(8);
    r.removeFromTop(12);

    // Three columns, left to right as the work goes: player, its window and name, saved sounds.
    const int gap = 24;
    const int cw = (r.getWidth() - 2 * gap) / 3;
    auto col1 = r.removeFromLeft(cw);
    r.removeFromLeft(gap);
    auto col2 = r.removeFromLeft(cw);
    r.removeFromLeft(gap);
    auto col3 = r;

    step1_.setBounds(col1.removeFromTop(26));
    auto keys = col1.removeFromBottom(54);
    scan_.setBounds(keys.removeFromRight(juce::jmin(140, keys.getWidth() / 3)).reduced(0, 2));
    keys.removeFromRight(8);
    load_.setBounds(keys.reduced(0, 2));
    col1.removeFromBottom(10);
    player_list_->setBounds(col1);

    step2_.setBounds(col2.removeFromTop(26));
    step2_text_.setBounds(col2.removeFromTop(110));
    open_window_.setBounds(col2.removeFromTop(54).reduced(0, 2));
    col2.removeFromTop(24);
    step3_.setBounds(col2.removeFromTop(26));
    name_label_.setBounds(col2.removeFromTop(22));
    name_.setBounds(col2.removeFromTop(36));
    col2.removeFromTop(8);
    artic_label_.setBounds(col2.removeFromTop(22));
    articulation_.setBounds(col2.removeFromTop(32));
    col2.removeFromTop(8);
    everywhere_.setBounds(col2.removeFromTop(44));
    col2.removeFromTop(8);
    save_.setBounds(col2.removeFromTop(54).reduced(0, 2));

    saved_title_.setBounds(col3.removeFromTop(26));
    use_saved_.setBounds(col3.removeFromBottom(54).reduced(0, 2));
    col3.removeFromBottom(10);
    saved_list_->setBounds(col3);
}

void SoundSetupPanel::layout_libraries(juce::Rectangle<int> r)
{
    r.reduce(20, 14);
    libraries_title_.setBounds(r.removeFromTop(26));
    libraries_hint_.setBounds(r.removeFromTop(44));
    status_.setBounds(r.removeFromBottom(30));
    r.removeFromBottom(8);
    auto keys = r.removeFromBottom(54);
    const int kw = juce::jmin(200, (keys.getWidth() - 40) / 5);
    for (auto* k : {&scan_drives_, &add_folder_, &locate_, &recheck_, &remove_}) {
        k->setBounds(keys.removeFromLeft(kw).reduced(0, 2));
        keys.removeFromLeft(10);
    }
    r.removeFromBottom(10);
    library_list_->setBounds(r);
}

void show_sound_setup(AppContext& context)
{
    if (context.library) context.library->refresh();
    auto& w = window_slot();
    if (w != nullptr && w->isVisible()) {
        w->setMinimised(false);
        w->toFront(true);
        return;
    }
    w = new SetupWindow(new SoundSetupPanel(context));
    w->toFront(true);
}

SoundSetupPanel* sound_setup_panel() { return panel_slot(); }

}  // namespace vcc::desktop
