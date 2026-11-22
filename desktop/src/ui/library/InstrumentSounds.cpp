#include "ui/library/InstrumentSounds.h"

#include <algorithm>
#include <map>
#include <set>

#include "audio/PluginHost.h"
#include "ui/Theme.h"
#include "ui/library/LibraryBrowser.h"
#include "vcc/templates/Templates.h"

namespace vcc::desktop {

namespace {

juce::String label_of(const std::string& articulation)
{
    if (articulation.empty()) return "Whole instrument";
    juce::String s(articulation);
    s = s.replace("_", " ");
    return s.substring(0, 1).toUpperCase() + s.substring(1);
}

void heading(juce::Label& l, float size, juce::Colour c = colours::text)
{
    l.setFont(juce::FontOptions(size, juce::Font::bold));
    l.setColour(juce::Label::textColourId, c);
}

void body(juce::Label& l)
{
    l.setFont(juce::FontOptions(14.5f));
    l.setColour(juce::Label::textColourId, colours::text_dim);
    l.setJustificationType(juce::Justification::topLeft);
    l.setMinimumHorizontalScale(1.0f);
}

}  // namespace

/// A list drawn by functions.
class InstrumentSoundsPage::List final : public juce::ListBox, private juce::ListBoxModel {
public:
    std::function<int()> count;
    std::function<void(int, juce::Graphics&, int, int, bool)> draw;
    std::function<void(int)> chosen;
    List()
    {
        setModel(this);
        setColour(juce::ListBox::backgroundColourId, colours::panel);
        setColour(juce::ListBox::outlineColourId, colours::outline);
        setOutlineThickness(1);
    }
    ~List() override { setModel(nullptr); }

private:
    int getNumRows() override { return count ? count() : 0; }
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (selected) g.fillAll(colours::selection);
        if (draw) draw(row, g, w, h, selected);
    }
    void selectedRowsChanged(int row) override
    {
        if (row >= 0 && chosen) chosen(row);
    }
};

InstrumentSoundsPage::InstrumentSoundsPage(AppContext& context)
    : context_(context), instruments_(std::make_unique<List>()), artics_(std::make_unique<List>())
{
    setComponentID("instrument_sounds");
    instruments_title_.setText("INSTRUMENTS", juce::dontSendNotification);
    artics_title_.setText("ARTICULATIONS", juce::dontSendNotification);
    for (auto* l : {&instruments_title_, &artics_title_}) heading(*l, 12.0f, colours::accent_warm);
    heading(title_, 26.0f);
    body(subtitle_);
    heading(current_, 16.0f);
    body(hint_);
    body(status_);
    status_.setColour(juce::Label::textColourId, colours::accent);
    step1_.setText("1.  CHOOSE THE PLAYER", juce::dontSendNotification);
    step2_.setText("2.  LOAD THE SOUND IN ITS WINDOW", juce::dontSendNotification);
    step3_.setText("3.  NAME IT AND SAVE IT", juce::dontSendNotification);
    for (auto* l : {&step1_, &step2_, &step3_}) heading(*l, 13.0f, colours::accent_warm);
    hint_.setText("The player's window opens floating, in front (move it to another screen if you like). Load the patch "
                  "there - in Opus: Browse > EW Hollywood Strings > 1st Violins > Legato. Play your keyboard to hear it.",
                  juce::dontSendNotification);
    name_label_.setText("Name", juce::dontSendNotification);
    body(name_label_);
    name_.setFont(juce::FontOptions(16.0f));
    name_.onTextChange = [this] { name_.getProperties().set("auto", false); };
    search_.setTextToShowWhenEmpty("Search instruments...", colours::text_dim);
    search_.setFont(juce::FontOptions(15.0f));
    search_.onTextChange = [this] { set_search(search_.getText()); };

    for (auto* k : {&choose_, &open_, &save_, &edit_, &clear_, &add_instrument_, &add_articulation_}) {
        k->set_legend_size(14.0f);
        k->setWantsKeyboardFocus(false);
    }
    choose_.set_sublegend("Opus, PLAY, Kontakt...  (opens its window)");
    open_.set_sublegend("bring it to the front");
    save_.set_sublegend("the default for this articulation");
    edit_.set_sublegend("load it to change it");
    choose_.setComponentID("sounds_choose_player");
    save_.setComponentID("sounds_save");
    choose_.onClick = [this] { show_player_menu(); };
    open_.onClick = [this] { status_.setText(open_player_window(), juce::dontSendNotification); };
    save_.onClick = [this] { status_.setText(save_sound(name_.getText(), fill_empty_.getToggleState()), juce::dontSendNotification); };
    edit_.onClick = [this] { status_.setText(edit_assigned(), juce::dontSendNotification); };
    clear_.onClick = [this] { clear_assigned(); };
    add_instrument_.onClick = [this] { show_add_instrument(); };
    add_articulation_.onClick = [this] { show_add_articulation(); };

    // Instruments, by family.
    instruments_->setRowHeight(34);
    instruments_->count = [this] { return static_cast<int>(rows_.size()); };
    instruments_->draw = [this](int row, juce::Graphics& g, int w, int h, bool) {
        if (row >= static_cast<int>(rows_.size())) return;
        const auto& r = rows_[static_cast<std::size_t>(row)];
        if (r.header) {
            g.fillAll(colours::background);
            g.setColour(colours::accent_warm);
            g.setFont(juce::FontOptions(12.5f, juce::Font::bold));
            g.drawText(r.text.toUpperCase(), 10, 0, w - 16, h, juce::Justification::bottomLeft, true);
            return;
        }
        int set = 0;
        if (context_.library)
            for (const auto& a : context_.library->lib().assignments())
                if (a.instrument_id == r.id) ++set;
        g.setColour(set > 0 ? colours::exact : colours::outline);
        g.fillEllipse(14.0f, h / 2.0f - 4.0f, 8.0f, 8.0f);
        g.setColour(colours::text);
        g.setFont(juce::FontOptions(15.0f));
        g.drawText(r.text, 30, 0, w - 90, h, juce::Justification::centredLeft, true);
        if (set > 0) {
            g.setColour(colours::text_dim);
            g.setFont(juce::FontOptions(12.0f));
            g.drawText(juce::String(set) + (set == 1 ? " sound" : " sounds"), w - 80, 0, 72, h, juce::Justification::centredRight);
        }
    };
    instruments_->chosen = [this](int row) {
        if (row >= static_cast<int>(rows_.size())) return;
        if (rows_[static_cast<std::size_t>(row)].header) return;
        show_instrument(rows_[static_cast<std::size_t>(row)].id);
    };

    // Articulations, as cards.
    artics_->setRowHeight(58);
    artics_->count = [this] { return static_cast<int>(articulations_.size()); };
    artics_->draw = [this](int row, juce::Graphics& g, int w, int h, bool selected) {
        if (row >= static_cast<int>(articulations_.size())) return;
        const auto& a = articulations_[static_cast<std::size_t>(row)];
        const auto sound = assigned_name(a);
        auto card = juce::Rectangle<float>(6.0f, 4.0f, w - 12.0f, h - 8.0f);
        g.setColour(selected ? colours::panel_raised.brighter(0.08f) : colours::panel_raised);
        g.fillRoundedRectangle(card, 6.0f);
        g.setColour(selected ? colours::accent : colours::outline);
        g.drawRoundedRectangle(card.reduced(0.5f), 6.0f, selected ? 2.0f : 1.0f);
        g.setColour(sound.isNotEmpty() ? colours::exact : colours::placeholder);
        g.fillEllipse(20.0f, h / 2.0f - 6.0f, 12.0f, 12.0f);
        g.setColour(colours::text);
        g.setFont(juce::FontOptions(17.0f, juce::Font::bold));
        g.drawText(label_of(a), 44, 6, w / 2 - 50, h - 12, juce::Justification::centredLeft, true);
        g.setFont(juce::FontOptions(14.0f));
        g.setColour(sound.isNotEmpty() ? colours::text : colours::text_dim);
        g.drawText(sound.isNotEmpty() ? sound : juce::String("built-in sound"), w / 2, 6, w / 2 - 20,
                   h - 12, juce::Justification::centredRight, true);
    };
    artics_->chosen = [this](int row) {
        if (row < static_cast<int>(articulations_.size())) select_articulation(articulations_[static_cast<std::size_t>(row)]);
    };

    saved_label_.setText("OR USE A SOUND YOU SAVED  (no need to open the player again)", juce::dontSendNotification);
    heading(saved_label_, 13.0f, colours::accent_warm);
    saved_.setTextWhenNothingSelected("Your saved sounds...");
    saved_.setComponentID("sounds_saved");
    saved_.setTitle("Your saved sounds");
    name_.setTitle("Sound name");
    search_.setTitle("Search instruments");
    saved_.onChange = [this] {
        const int i = saved_.getSelectedId() - 1;
        if (i >= 0 && i < static_cast<int>(saved_ids_.size())) {
            status_.setText(use_saved(saved_ids_[static_cast<std::size_t>(i)]), juce::dontSendNotification);
            saved_.setSelectedId(0, juce::dontSendNotification);
        }
    };
    addAndMakeVisible(saved_label_);
    addAndMakeVisible(saved_);
    for (auto* c : std::initializer_list<juce::Component*>{
             &instruments_title_, &search_, instruments_.get(), &add_instrument_, &artics_title_, artics_.get(), &add_articulation_,
             &title_, &subtitle_, &current_, &step1_, &choose_, &step2_, &hint_, &open_, &step3_, &name_label_, &name_, &fill_empty_,
             &save_, &edit_, &clear_, &status_})
        addAndMakeVisible(c);
    if (context_.library) context_.library->addChangeListener(this);
    rebuild_instruments();
    // Start where the composer is: the selected track's instrument, else the first.
    std::string start;
    if (const auto* t = context_.session.data().find_track(context_.session.selected_track())) start = t->instrument_standard_id;
    const auto listed = listed_instruments();
    if (start.empty() || std::find(listed.begin(), listed.end(), start) == listed.end()) start = listed.empty() ? "" : listed.front();
    if (!start.empty()) show_instrument(start);
}

InstrumentSoundsPage::~InstrumentSoundsPage()
{
    if (context_.library) context_.library->removeChangeListener(this);
    release_slot();
}

void InstrumentSoundsPage::release_slot()
{
    if (!context_.engine.has_setup_instrument()) return;
    context_.engine.set_setup_instrument({});
    context_.engine.set_live_track(context_.session.selected_track());
    loaded_player_.clear();
}

void InstrumentSoundsPage::changeListenerCallback(juce::ChangeBroadcaster*)
{
    instruments_->repaint();
    artics_->repaint();
    refresh_detail();
}

juce::String InstrumentSoundsPage::instrument_name(const std::string& id) const
{
    if (const auto* d = context_.catalogs.instruments.find(id)) return d->name;
    if (context_.library)
        if (const auto* c = context_.library->lib().find_custom_instrument(id)) return c->name;
    return id;
}

std::string InstrumentSoundsPage::family_of(const std::string& id) const
{
    if (const auto* d = context_.catalogs.instruments.find(id)) return d->family;
    if (context_.library)
        if (const auto* c = context_.library->lib().find_custom_instrument(id)) return c->family;
    return "other";
}

std::vector<std::string> InstrumentSoundsPage::listed_instruments() const
{
    std::vector<std::string> out;
    for (const auto& r : rows_)
        if (!r.header) out.push_back(r.id);
    return out;
}

void InstrumentSoundsPage::set_search(const juce::String& text)
{
    search_text_ = text.trim();
    rebuild_instruments();
}

void InstrumentSoundsPage::rebuild_instruments()
{
    // Every instrument by family (in the registry's order), yours under their family.
    std::map<std::string, std::vector<std::pair<juce::String, std::string>>> by_family;
    auto wanted = [&](const juce::String& name) { return search_text_.isEmpty() || name.containsIgnoreCase(search_text_); };
    for (const auto& d : context_.catalogs.instruments.all())
        if (wanted(d.name)) by_family[d.family].emplace_back(juce::String(d.name), d.id);
    if (context_.library)
        for (const auto& c : context_.library->lib().custom_instruments())
            if (wanted(c.name)) by_family[c.family.empty() ? "other" : c.family].emplace_back(juce::String(c.name) + "  (yours)", c.id);
    rows_.clear();
    std::vector<std::string> order = templates::family_order();
    for (const auto& [f, _] : by_family)
        if (std::find(order.begin(), order.end(), f) == order.end()) order.push_back(f);
    for (const auto& f : order) {
        auto it = by_family.find(f);
        if (it == by_family.end() || it->second.empty()) continue;
        rows_.push_back({true, {}, juce::String(templates::family_display(f))});
        for (const auto& [name, id] : it->second) rows_.push_back({false, id, name});
    }
    instruments_->updateContent();
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (!rows_[i].header && rows_[i].id == instrument_) instruments_->selectRow(static_cast<int>(i), true, true);
    instruments_->repaint();
}

void InstrumentSoundsPage::show_instrument(const std::string& instrument_id)
{
    instrument_ = instrument_id;
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (!rows_[i].header && rows_[i].id == instrument_ && instruments_->getSelectedRow() != static_cast<int>(i))
            instruments_->selectRow(static_cast<int>(i), true, true);
    rebuild_articulations();
    select_articulation(articulations_.size() > 1 ? articulations_[1] : std::string());
}

void InstrumentSoundsPage::rebuild_articulations()
{
    // The articulations the templates use for it, those its library patches
    // cover, those already given a sound, and those you added.
    std::set<std::string> found;
    const std::string base = [&] {
        if (context_.library)
            if (const auto* c = context_.library->lib().find_custom_instrument(instrument_)) return c->based_on;
        return instrument_;
    }();
    if (context_.catalogs.templates)
        for (const auto* t : context_.catalogs.templates->list())
            for (const auto& tt : t->tracks)
                if (tt.instrument_standard_id == base) found.insert(tt.articulation_map.begin(), tt.articulation_map.end());
    if (context_.library) {
        for (const auto& lib : context_.library->lib().libraries())
            for (const auto& p : lib.patches)
                if (p.instrument_id == base) found.insert(p.articulations.begin(), p.articulations.end());
        for (const auto& a : context_.library->lib().assignments())
            if (a.instrument_id == instrument_ && !a.articulation.empty()) found.insert(a.articulation);
        for (const auto& a : context_.library->lib().added_articulations(instrument_)) found.insert(a);
    }
    if (found.empty()) found = {"sustain", "legato", "staccato"};
    articulations_ = {""};
    for (const auto& a : context_.catalogs.articulation_ids)
        if (found.erase(a)) articulations_.push_back(a);
    articulations_.insert(articulations_.end(), found.begin(), found.end());
    artics_->updateContent();
    artics_->repaint();
}

void InstrumentSoundsPage::select_articulation(const std::string& articulation)
{
    articulation_ = articulation;
    const auto it = std::find(articulations_.begin(), articulations_.end(), articulation_);
    if (it != articulations_.end() && artics_->getSelectedRow() != static_cast<int>(it - articulations_.begin()))
        artics_->selectRow(static_cast<int>(it - articulations_.begin()), true, true);
    name_.setText(suggested_name(), false);
    name_.getProperties().set("auto", true);
    refresh_detail();
}

juce::String InstrumentSoundsPage::suggested_name() const
{
    return instrument_name(instrument_) + (articulation_.empty() ? juce::String() : " - " + label_of(articulation_));
}

juce::String InstrumentSoundsPage::assigned_name(const std::string& articulation) const
{
    if (!context_.library) return {};
    if (const auto* p = context_.library->lib().assigned(instrument_, articulation)) return juce::String(p->display);
    return {};
}

void InstrumentSoundsPage::refresh_detail()
{
    title_.setText(suggested_name(), juce::dontSendNotification);
    const auto fam = family_of(instrument_);
    subtitle_.setText(juce::String(templates::family_display(fam)) +
                          (articulation_.empty() ? "  |  the sound for this instrument when no articulation has its own"
                                                 : "  |  the sound for " + label_of(articulation_).toLowerCase() + " notes and tracks"),
                      juce::dontSendNotification);
    const auto now = assigned_name(articulation_);
    current_.setText(now.isNotEmpty() ? "Now: " + now : juce::String("Now: the built-in sound"), juce::dontSendNotification);
    current_.setColour(juce::Label::textColourId, now.isNotEmpty() ? colours::exact : colours::text_dim);
    refresh_saved();
    edit_.setEnabled(now.isNotEmpty());
    clear_.setEnabled(now.isNotEmpty());
    {
        juce::String loaded;
        for (const auto& d : context_.plugins.instruments())
            if (d.createIdentifierString().toStdString() == loaded_player_) loaded = d.name;
        choose_.set_sublegend(context_.engine.has_setup_instrument() && loaded.isNotEmpty()
                                  ? "loaded: " + loaded + "  -  click to choose another"
                                  : juce::String("Opus, PLAY, Kontakt...  (opens its window)"));
    }
    save_.setEnabled(context_.engine.has_setup_instrument());
    open_.setEnabled(context_.engine.has_setup_instrument());
}

void InstrumentSoundsPage::show_player_menu()
{
    // As a DAW's instrument insert: the players by maker, EastWest first.
    std::map<juce::String, juce::PopupMenu> by_maker;
    for (const auto& d : context_.plugins.instruments()) {
        const auto maker = d.manufacturerName.isEmpty() ? juce::String("Other") : d.manufacturerName;
        const auto id = d.createIdentifierString().toStdString();
        by_maker[maker].addItem(d.name + (d.pluginFormatName.isEmpty() ? juce::String() : "  (" + d.pluginFormatName + ")"), true,
                                id == loaded_player_,
                                [this, id] { status_.setText(load_player(id), juce::dontSendNotification); });
    }
    juce::PopupMenu m;
    m.addSectionHeader("Instrument plug-ins");
    for (auto& [maker, menu] : by_maker)
        if (maker.containsIgnoreCase("east") && maker.containsIgnoreCase("west")) m.addSubMenu(maker, menu);
    for (auto& [maker, menu] : by_maker)
        if (!(maker.containsIgnoreCase("east") && maker.containsIgnoreCase("west"))) m.addSubMenu(maker, menu);
    if (by_maker.empty()) m.addItem("No instrument plug-ins found yet", false, false, [] {});
    m.addSeparator();
    m.addItem("Scan for Plug-ins...", [this] { context_.commands.invokeDirectly(cmd::scanPlugins, true); });
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&choose_).withMinimumWidth(choose_.getWidth()));
}

juce::String InstrumentSoundsPage::load_player(const std::string& identifier)
{
    for (const auto& d : context_.plugins.instruments())
        if (d.createIdentifierString().toStdString() == identifier) {
            context_.engine.set_setup_instrument(context_.plugins.ref_for(d));
            context_.engine.set_live_track(PlaybackEngine::kSetupTrackId);
            loaded_player_ = identifier;
            if (context_.show_plugin_window) context_.show_plugin_window(PlaybackEngine::kSetupTrackId);
            refresh_detail();
            return d.name + " is loading: its window opens in a moment. Load the patch for " + suggested_name() +
                   " there, play your keyboard, then Save Sound";
        }
    return "That player is not installed here: scan for plug-ins";
}

juce::String InstrumentSoundsPage::open_player_window()
{
    if (!context_.engine.has_setup_instrument()) return "Choose a player first (step 1)";
    if (context_.show_plugin_window) context_.show_plugin_window(PlaybackEngine::kSetupTrackId);
    return "The player's window is in front";
}

juce::String InstrumentSoundsPage::save_sound(const juce::String& name, bool fill_empty)
{
    if (!context_.library) return {};
    auto* instance = context_.engine.plugin_for(PlaybackEngine::kSetupTrackId);
    if (!instance) return "Choose a player and load your sound in it first (steps 1 and 2)";
    project::PluginRef ref;
    for (const auto& d : context_.plugins.instruments())
        if (d.createIdentifierString().toStdString() == loaded_player_) ref = context_.plugins.ref_for(d);
    if (ref.empty()) return "The player is not installed here";
    ref.state_base64 = PluginHost::state_of(*instance);
    auto& lib = context_.library->lib();
    library::UserPatch p;
    p.plugin = ref;
    p.display = (name.trim().isEmpty() ? suggested_name() : name.trim()).toStdString();
    p.provider = ref.vendor;
    p.player = ref.name;
    p.instrument_id = instrument_;
    if (!articulation_.empty()) p.articulations = {articulation_};
    p.validation = "verified";
    const auto id = lib.add_patch(p);
    lib.set_assignment(instrument_, articulation_, id);
    int filled = 0;
    if (fill_empty)
        for (const auto& a : articulations_)
            if (a != articulation_ && !lib.assigned(instrument_, a)) {
                lib.set_assignment(instrument_, a, id);
                ++filled;
            }
    context_.library->changed();
    // This project's matching tracks still on the built-in sound get it now.
    std::vector<std::string> ids;
    for (const auto& t : context_.session.data().tracks) ids.push_back(t.id);
    const int applied = context_.session.apply_assigned_sounds(ids);
    artics_->repaint();
    instruments_->repaint();
    refresh_detail();
    return "Saved \"" + juce::String(p.display) + "\" - the default for " + suggested_name() +
           (filled > 0 ? " and " + juce::String(filled) + " more articulations" : juce::String()) +
           (applied > 0 ? "; " + juce::String(applied) + " tracks in this project use it now" : juce::String());
}

juce::String InstrumentSoundsPage::edit_assigned()
{
    if (!context_.library) return {};
    const auto* p = context_.library->lib().assigned(instrument_, articulation_);
    if (!p) return "Nothing saved for this articulation yet";
    context_.engine.set_setup_instrument(p->plugin);  // with its settings
    context_.engine.set_live_track(PlaybackEngine::kSetupTrackId);
    loaded_player_ = p->plugin.identifier;
    if (context_.show_plugin_window) context_.show_plugin_window(PlaybackEngine::kSetupTrackId);
    refresh_detail();
    return "Loaded \"" + juce::String(p->display) + "\" with its settings: change it in the player, then Save Sound";
}

void InstrumentSoundsPage::refresh_saved()
{
    saved_.clear(juce::dontSendNotification);
    saved_ids_.clear();
    if (!context_.library) return;
    const auto& patches = context_.library->lib().patches();
    int id = 1;
    bool mine = false;
    for (const auto& p : patches)
        if (p.instrument_id == instrument_) {
            if (!mine) saved_.addSectionHeading("For " + instrument_name(instrument_));
            mine = true;
            saved_.addItem(juce::String(p.display), id++);
            saved_ids_.push_back(p.id);
        }
    bool others = false;
    for (const auto& p : patches)
        if (p.instrument_id != instrument_) {
            if (!others) saved_.addSectionHeading("Other instruments");
            others = true;
            saved_.addItem(juce::String(p.display) + "   (" + instrument_name(p.instrument_id) + ")", id++);
            saved_ids_.push_back(p.id);
        }
    saved_.setEnabled(!saved_ids_.empty());
    saved_.setTextWhenNothingSelected(saved_ids_.empty() ? "None saved yet" : "Your saved sounds...");
}

juce::String InstrumentSoundsPage::use_saved(const std::string& patch_id)
{
    if (!context_.library) return {};
    auto& lib = context_.library->lib();
    const auto* p = lib.find_patch(patch_id);
    if (!p) return "That sound is gone";
    lib.set_assignment(instrument_, articulation_, patch_id);
    context_.library->changed();
    std::vector<std::string> ids;
    for (const auto& t : context_.session.data().tracks) ids.push_back(t.id);
    const int applied = context_.session.apply_assigned_sounds(ids);
    artics_->repaint();
    instruments_->repaint();
    refresh_detail();
    return "\"" + juce::String(p->display) + "\" is now the default for " + suggested_name() +
           (applied > 0 ? "; " + juce::String(applied) + " tracks in this project use it now" : juce::String());
}

void InstrumentSoundsPage::clear_assigned()
{
    if (!context_.library) return;
    context_.library->lib().set_assignment(instrument_, articulation_, {});
    context_.library->changed();
    status_.setText(label_of(articulation_) + " plays the built-in sound again", juce::dontSendNotification);
    artics_->repaint();
    refresh_detail();
}

std::string InstrumentSoundsPage::add_instrument(const juce::String& name, const std::string& based_on)
{
    if (!context_.library || name.trim().isEmpty()) return {};
    const auto* def = context_.catalogs.instruments.find(based_on);
    const auto id = context_.library->lib().add_custom_instrument(name.trim().toStdString(), def ? def->family : std::string("other"),
                                                                  based_on);
    context_.library->changed();
    search_.setText({}, false);
    search_text_.clear();
    rebuild_instruments();
    show_instrument(id);
    return id;
}

void InstrumentSoundsPage::add_articulation(const std::string& articulation)
{
    if (!context_.library || articulation.empty()) return;
    context_.library->lib().add_articulation(instrument_, articulation);
    context_.library->changed();
    rebuild_articulations();
    select_articulation(articulation);
}

void InstrumentSoundsPage::show_add_instrument()
{
    auto* w = new juce::AlertWindow("Add an Instrument", "An instrument of your own: it gets sounds like any other, and is written "
                                                         "like the instrument it is based on (range, clef).",
                                    juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("name", "", "Name (for example: Fab Four Guitar)");
    juce::StringArray names;
    std::vector<std::string> ids;
    for (const auto& d : context_.catalogs.instruments.all()) {
        names.add(d.name);
        ids.push_back(d.id);
    }
    w->addComboBox("based", names, "Written like");
    if (auto* box = w->getComboBoxComponent("based")) {
        const auto it = std::find(ids.begin(), ids.end(), instrument_);
        box->setSelectedItemIndex(it != ids.end() ? static_cast<int>(it - ids.begin()) : 0);
    }
    w->addButton("Add", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<InstrumentSoundsPage> self(this);
    w->enterModalState(true, juce::ModalCallbackFunction::create([self, w, ids](int r) {
                           if (self == nullptr || r != 1) return;
                           const auto name = w->getTextEditorContents("name");
                           const int i = w->getComboBoxComponent("based") ? w->getComboBoxComponent("based")->getSelectedItemIndex() : 0;
                           if (i >= 0 && i < static_cast<int>(ids.size())) self->add_instrument(name, ids[static_cast<std::size_t>(i)]);
                       }),
                       true);
}

void InstrumentSoundsPage::show_add_articulation()
{
    juce::PopupMenu m;
    for (const auto& a : context_.catalogs.articulation_ids)
        if (std::find(articulations_.begin(), articulations_.end(), a) == articulations_.end())
            m.addItem(label_of(a), [this, a] { add_articulation(a); });
    m.addSeparator();
    m.addItem("Your own name...", [this] {
        auto* w = new juce::AlertWindow("Add an Articulation", "A name of your own (for example: flautando, sul tasto, swells).",
                                        juce::MessageBoxIconType::NoIcon);
        w->addTextEditor("name", "");
        w->addButton("Add", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<InstrumentSoundsPage> self(this);
        w->enterModalState(true, juce::ModalCallbackFunction::create([self, w](int r) {
                               if (self == nullptr || r != 1) return;
                               auto s = w->getTextEditorContents("name").trim().toLowerCase().replace(" ", "_");
                               if (s.isNotEmpty()) self->add_articulation(s.toStdString());
                           }),
                           true);
    });
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&add_articulation_));
}

void InstrumentSoundsPage::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
}

void InstrumentSoundsPage::resized()
{
    auto r = getLocalBounds();
    // Three columns that grow with the window: instruments, articulations, the sound.
    const int left_w = juce::jlimit(260, 420, getWidth() * 22 / 100);
    auto left = r.removeFromLeft(left_w).reduced(12, 8);
    instruments_title_.setBounds(left.removeFromTop(24));
    search_.setBounds(left.removeFromTop(34));
    left.removeFromTop(8);
    add_instrument_.setBounds(left.removeFromBottom(48).reduced(0, 2));
    left.removeFromBottom(8);
    instruments_->setBounds(left);

    const int mid_w = juce::jlimit(320, 620, getWidth() * 34 / 100);
    auto mid = r.removeFromLeft(mid_w).reduced(12, 8);
    artics_title_.setBounds(mid.removeFromTop(24));
    add_articulation_.setBounds(mid.removeFromBottom(48).reduced(0, 2));
    mid.removeFromBottom(8);
    artics_->setBounds(mid);

    auto right = r.reduced(20, 8);
    title_.setBounds(right.removeFromTop(38));
    subtitle_.setBounds(right.removeFromTop(24));
    current_.setBounds(right.removeFromTop(28));
    status_.setBounds(right.removeFromBottom(48));
    right.removeFromTop(16);
    step1_.setBounds(right.removeFromTop(24));
    choose_.setBounds(right.removeFromTop(62).reduced(0, 2));
    right.removeFromTop(16);
    step2_.setBounds(right.removeFromTop(24));
    hint_.setBounds(right.removeFromTop(64));
    open_.setBounds(right.removeFromTop(54).reduced(0, 2).removeFromLeft(juce::jmin(320, right.getWidth())));
    right.removeFromTop(16);
    step3_.setBounds(right.removeFromTop(24));
    name_label_.setBounds(right.removeFromTop(22));
    name_.setBounds(right.removeFromTop(38));
    right.removeFromTop(6);
    fill_empty_.setBounds(right.removeFromTop(30));
    right.removeFromTop(6);
    auto keys = right.removeFromTop(62);
    save_.setBounds(keys.removeFromLeft(keys.getWidth() * 45 / 100).reduced(0, 2));
    keys.removeFromLeft(10);
    edit_.setBounds(keys.removeFromLeft(keys.getWidth() * 60 / 100).reduced(0, 2));
    keys.removeFromLeft(10);
    clear_.setBounds(keys.reduced(0, 2));
    right.removeFromTop(18);
    saved_label_.setBounds(right.removeFromTop(24));
    saved_.setBounds(right.removeFromTop(36));
}

}  // namespace vcc::desktop
