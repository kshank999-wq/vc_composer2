#include "ui/library/LibraryBrowser.h"

#include <algorithm>

#include <set>

#include "AppResources.h"
#include "ui/Theme.h"

namespace vcc::desktop {

namespace {

/// A short phrase in the instrument's comfortable middle register (or a
/// groove for a kit), for auditions.
std::vector<NoteEvent> phrase_for(const templates::InstrumentDef* def)
{
    std::vector<NoteEvent> notes;
    if (def && def->unpitched) {
        const int kit[] = {36, 42, 38, 42, 36, 36, 38, 46};
        for (int i = 0; i < 8; ++i) notes.push_back({i * kTicksPerQuarter / 2, kTicksPerQuarter / 2, kit[i], 100, 9, {}});
        return notes;
    }
    const int low = def ? def->comfortable_low : 48, high = def ? def->comfortable_high : 84;
    const int mid = (low + high) / 2;
    const int base = mid - (mid % 12);
    const int steps[] = {0, 4, 7, 12, 7, 4, 0};
    for (int i = 0; i < 7; ++i) notes.push_back({i * kTicksPerQuarter / 2, kTicksPerQuarter / 2, base + steps[i], 90, 0, {}});
    notes.back().duration = kTicksPerQuarter * 2;
    return notes;
}

const char* view_names[] = {"All sounds", "For the selected track", "Instruments (new track)"};

}  // namespace

// --- Service -----------------------------------------------------------------------------------

LibraryService::LibraryService(AppContext& context) : context_(context), lib_(context.catalogs.instruments)
{
    std::string error;
    if (const auto doc = json::parse(embedded_library_catalog())) lib_.load_catalog(*doc, &error);
    file_ = user_data_directory().getChildFile("Library.json");
    if (const auto doc = json::parse(file_.loadFileAsString().toStdString())) lib_.load_user(*doc);
    refresh();
    context_.plugins.known().addChangeListener(this);  // a scan found or lost plug-ins
}

LibraryService::~LibraryService() { context_.plugins.known().removeChangeListener(this); }

juce::File LibraryService::file() const { return file_; }

void LibraryService::set_file(const juce::File& f)
{
    file_ = f;
    if (const auto doc = json::parse(file_.loadFileAsString().toStdString())) lib_.load_user(*doc);
    else lib_.load_user(json::Value::make_object());
    refresh();
}

void LibraryService::refresh()
{
    std::vector<library::InstalledPlugin> installed;
    for (const auto& d : context_.plugins.instruments())
        installed.push_back({d.name.toStdString(), d.manufacturerName.toStdString(), d.pluginFormatName.toStdString(),
                             d.createIdentifierString().toStdString()});
    lib_.set_installed(std::move(installed));
    lib_.detect();
    context_.session.set_installed_patches(lib_.installed_patches());
    sendChangeMessage();
}

void LibraryService::changed()
{
    file_.getParentDirectory().createDirectory();
    file_.replaceWithText(juce::String(lib_.user_json().to_text(2)));
    context_.session.set_installed_patches(lib_.installed_patches());
    sendChangeMessage();
}

project::PluginRef LibraryService::plugin_ref(const std::string& identifier) const
{
    for (const auto& d : context_.plugins.instruments())
        if (d.createIdentifierString().toStdString() == identifier) return context_.plugins.ref_for(d);
    return {};
}

juce::String LibraryService::audition(const library::Entry& e)
{
    const auto* def = context_.catalogs.instruments.find(e.instrument_id);
    const auto notes = phrase_for(def);
    project::PluginRef ref;
    if (e.kind == library::EntryKind::UserPatch) {
        if (const auto* p = lib_.find_patch(e.key.substr(6))) ref = p->plugin;
    } else if (e.kind == library::EntryKind::Plugin) {
        ref = plugin_ref(e.plugin_identifier);
    }
    if (!ref.empty() && e.player_installed) {
        juce::String error;
        if (context_.engine.preview_plugin(ref, notes, 110.0, error))
            return "Auditioning " + juce::String(e.display) + " through " + juce::String(ref.name);
        return "Could not load " + juce::String(ref.name) + ": " + error;
    }
    // A catalog section lives inside its player: it is chosen there, so the
    // audition is the placeholder until it is assigned and saved.
    context_.engine.preview(notes, def ? def->family : std::string(), def && !def->unpitched ? def->gm_program : -1, 110.0,
                            false);
    return e.kind == library::EntryKind::CatalogPatch
               ? "Placeholder audition: assign it to load " + juce::String(e.player) + ", choose " +
                     juce::String(e.display) + " there, then Save sound"
               : "Auditioning " + juce::String(e.display) + " on the built-in placeholder";
}

juce::String LibraryService::assign(const library::Entry& e, const std::string& track_id)
{
    const auto* t = context_.session.data().find_track(track_id);
    if (!t) return "Select a track first";
    project::PluginRef ref;
    std::string map;
    if (e.kind == library::EntryKind::UserPatch) {
        if (const auto* p = lib_.find_patch(e.key.substr(6))) {
            ref = p->plugin;
            map = p->expression_map;
        }
    } else {
        ref = plugin_ref(e.plugin_identifier);
    }
    if (ref.empty() || !e.player_installed)
        return juce::String(e.player) + " is not installed here: scan for plug-ins or choose another sound";

    project::InstrumentAssignment how;
    const bool same = e.instrument_id.empty() || e.instrument_id == t->instrument_standard_id || t->instrument_standard_id.empty();
    const bool fallback = !same && std::find(t->fallback_instruments.begin(), t->fallback_instruments.end(),
                                             e.instrument_id) != t->fallback_instruments.end();
    how.status = same ? project::ResolutionStatus::Exact
                      : fallback ? project::ResolutionStatus::Fallback : project::ResolutionStatus::Exact;
    how.resolved_instrument_id = e.instrument_id.empty() ? t->instrument_standard_id : e.instrument_id;
    how.patch_id = e.kind == library::EntryKind::UserPatch ? e.key.substr(6) : std::string();
    how.patch_display = e.display;
    for (const auto& a : t->articulation_intents)
        if (!e.metadata_known || std::find(e.articulations.begin(), e.articulations.end(), a) == e.articulations.end())
            how.unsupported_articulations.push_back(a);
    switch (e.kind) {
        case library::EntryKind::UserPatch:
            how.message = "Saved patch " + e.display + (e.validation == "verified" ? "" : " (not verified yet)");
            break;
        case library::EntryKind::CatalogPatch:
            how.message = "Load " + e.display + " in " + e.player +
                          " (its window is open), then Save sound to confirm it: inferred from the catalog";
            break;
        case library::EntryKind::Plugin:
            how.message = e.instrument_id.empty() ? "Played by " + e.display + ": confirm it suits the part"
                                                  : "Inferred from the plug-in's name: audition and Save sound to confirm";
            break;
    }
    if (!same && !fallback) how.message += " (it was made for another instrument)";
    if (!context_.session.assign_sound(track_id, ref, how, map)) return "The track is locked (approved)";
    if (e.kind == library::EntryKind::UserPatch)
        if (const auto* p = lib_.find_patch(e.key.substr(6)); p && p->articulation_map)
            context_.session.set_articulation_map(track_id, p->articulation_map);
    if (e.kind != library::EntryKind::UserPatch && context_.show_plugin_window) context_.show_plugin_window(track_id);
    return "Assigned " + juce::String(e.display) + " to " + juce::String(t->name);
}

std::string LibraryService::save_track_sound(const std::string& track_id, const std::string& display, juce::String& message)
{
    if (context_.capture_plugin_states) context_.capture_plugin_states();
    const auto* t = context_.session.data().find_track(track_id);
    if (!t || t->instrument_plugin.empty()) {
        message = "The track plays the placeholder: assign a plug-in first";
        return {};
    }
    library::UserPatch p;
    p.plugin = t->instrument_plugin;
    p.display = display.empty() ? t->name + " - " + t->instrument_plugin.name : display;
    p.provider = t->instrument_plugin.vendor;
    p.player = t->instrument_plugin.name;
    p.instrument_id = t->instrument_standard_id;
    p.articulations = t->articulation_intents;
    p.expression_map = t->expression_map;
    p.articulation_map = t->articulation_map;
    p.validation = "verified";
    const auto id = lib_.add_patch(p);
    changed();
    project::InstrumentAssignment how = t->assignment;
    how.status = project::ResolutionStatus::Exact;
    how.patch_id = id;
    how.patch_display = p.display;
    how.unsupported_articulations.clear();
    how.message = "Saved patch " + p.display + " (verified)";
    context_.session.assign_sound(track_id, t->instrument_plugin, how);
    message = "Saved \"" + juce::String(p.display) + "\": templates use it for " +
              juce::String(context_.catalogs.instruments.find(p.instrument_id)
                               ? context_.catalogs.instruments.find(p.instrument_id)->name
                               : std::string("this instrument"));
    return id;
}

// --- Browser ----------------------------------------------------------------------------------

LibraryBrowser::LibraryBrowser(AppContext& context) : context_(context)
{
    setComponentID("library_browser");
    search_.setTitle("Search the library");
    search_.setTextToShowWhenEmpty("Search sounds, libraries, players, techniques...", colours::text_dim);
    search_.onTextChange = [this] { refilter(); };
    view_box_.setTitle("Library view");
    for (int i = 0; i < 3; ++i) view_box_.addItem(view_names[i], i + 1);
    view_box_.setSelectedId(1, juce::dontSendNotification);
    view_box_.onChange = [this] { set_view(static_cast<View>(view_box_.getSelectedId() - 1)); };
    family_box_.setTitle("Family");
    family_box_.addItem("Any family", 1);
    for (std::size_t i = 0; i < templates::family_order().size(); ++i)
        family_box_.addItem(templates::family_display(templates::family_order()[i]), static_cast<int>(i) + 2);
    family_box_.setSelectedId(1, juce::dontSendNotification);
    family_box_.onChange = [this] { refilter(); };
    technique_box_.setTitle("Technique");
    technique_box_.addItem("Any technique", 1);
    {
        std::set<std::string> techniques;
        if (context_.library)
            for (const auto& lib : context_.library->lib().libraries())
                for (const auto& p : lib.patches) techniques.insert(p.articulations.begin(), p.articulations.end());
        int id = 2;
        for (const auto& t : techniques) technique_box_.addItem(t, id++);
    }
    technique_box_.setSelectedId(1, juce::dontSendNotification);
    technique_box_.onChange = [this] { refilter(); };
    installed_.setToggleState(true, juce::dontSendNotification);
    installed_.setTooltip("Only what a plug-in installed here can load");
    installed_.onClick = [this] { refilter(); };
    favorites_.onClick = [this] { refilter(); };
    list_.setTitle("Library");
    list_.setModel(this);
    list_.setRowHeight(40);
    detail_.setFont(juce::FontOptions(11.0f));
    detail_.setColour(juce::Label::textColourId, colours::text_dim);
    detail_.setJustificationType(juce::Justification::topLeft);
    detail_.setMinimumHorizontalScale(1.0f);
    audition_.onClick = [this] { audition_selected(); };
    assign_.onClick = [this] { assign_selected(); };
    star_.onClick = [this] { toggle_favorite_selected(); };
    save_.onClick = [this] { save_track_sound(); };
    save_.setTooltip("Save the selected track's sound (its plug-in, as set up) as a patch");
    more_.onClick = [this] { show_more_menu(); };
    for (auto* c : std::initializer_list<juce::Component*>{&search_, &view_box_, &family_box_, &technique_box_, &installed_,
                                                          &favorites_, &list_, &detail_, &audition_, &assign_, &star_,
                                                          &save_, &more_})
        addAndMakeVisible(*c);
    if (context_.library) context_.library->addChangeListener(this);
    listener_ = context_.session.add_listener([this](session::Change c) {
        if (c == session::Change::Selection && view_ == View::ForTrack) refilter();
    });
    refilter();
}

LibraryBrowser::~LibraryBrowser()
{
    if (context_.library) context_.library->removeChangeListener(this);
    context_.session.remove_listener(listener_);
}

void LibraryBrowser::changeListenerCallback(juce::ChangeBroadcaster*) { refilter(); }

void LibraryBrowser::set_view(View v)
{
    view_ = v;
    view_box_.setSelectedId(static_cast<int>(v) + 1, juce::dontSendNotification);
    const bool sounds = v != View::Instruments;
    for (auto* c : std::initializer_list<juce::Component*>{&technique_box_, &installed_, &favorites_, &star_, &save_})
        c->setVisible(sounds);
    assign_.setButtonText(sounds ? "Assign" : "Add track");
    refilter();
}

void LibraryBrowser::refilter()
{
    entries_.clear();
    candidates_.clear();
    instruments_.clear();
    const int fam = family_box_.getSelectedId() - 2;
    const std::string family = fam >= 0 ? templates::family_order()[static_cast<std::size_t>(fam)] : std::string();
    if (view_ == View::Instruments) {
        const auto q = search_.getText().toLowerCase();
        for (const auto& i : context_.catalogs.instruments.all()) {
            if (!family.empty() && i.family != family) continue;
            juce::String hay = juce::String(i.name + " " + i.family + " " + i.region + " " + i.id).toLowerCase();
            for (const auto& t : i.tags) hay << " " << juce::String(t).toLowerCase();
            if (q.isEmpty() || hay.contains(q)) instruments_.push_back(&i);
        }
    } else if (context_.library) {
        auto& lib = context_.library->lib();
        if (view_ == View::ForTrack) {
            const auto* t = context_.session.data().find_track(context_.session.selected_track());
            if (t && !t->instrument_standard_id.empty())
                candidates_ = lib.candidates(t->instrument_standard_id, t->articulation_intents, t->fallback_instruments, 50);
            for (const auto& c : candidates_) entries_.push_back(c.entry);
            detail_.setText(!t ? juce::String("Select a track to see candidates for it")
                            : candidates_.empty() ? "Nothing installed here is known to play " +
                                                        juce::String(t->name) + ": try All sounds"
                                                  : juce::String(static_cast<int>(candidates_.size())) +
                                                        " candidate(s) for " + juce::String(t->name),
                            juce::dontSendNotification);
        } else {
            library::Filter f;
            f.query = search_.getText().toStdString();
            f.family = family;
            if (technique_box_.getSelectedId() > 1) f.technique = technique_box_.getText().toStdString();
            f.installed_only = installed_.getToggleState();
            f.favorites_only = favorites_.getToggleState();
            entries_ = lib.search(f);
            detail_.setText(juce::String(static_cast<int>(entries_.size())) + " sound(s)", juce::dontSendNotification);
        }
    }
    list_.updateContent();
    list_.deselectAllRows();
    list_.repaint();
}

std::vector<std::string> LibraryBrowser::listed() const
{
    std::vector<std::string> out;
    if (view_ == View::Instruments)
        for (const auto* d : instruments_) out.push_back(d->id);
    else
        for (const auto& e : entries_) out.push_back(e.key);
    return out;
}

void LibraryBrowser::select(int row) { list_.selectRow(row); }

const library::Entry* LibraryBrowser::entry_at(int row) const
{
    return view_ != View::Instruments && row >= 0 && row < static_cast<int>(entries_.size())
               ? &entries_[static_cast<std::size_t>(row)]
               : nullptr;
}

int LibraryBrowser::getNumRows()
{
    return view_ == View::Instruments ? static_cast<int>(instruments_.size()) : static_cast<int>(entries_.size());
}

void LibraryBrowser::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected)
{
    g.fillAll(selected ? colours::selection : colours::panel);
    if (view_ == View::Instruments) {
        if (row < 0 || row >= static_cast<int>(instruments_.size())) return;
        const auto* def = instruments_[static_cast<std::size_t>(row)];
        g.setColour(colour_from_hex(templates::family_color(def->family)));
        g.fillRect(0, 0, 4, h);
        g.setColour(colours::text);
        g.setFont(juce::FontOptions(13.0f));
        g.drawText(def->name, 10, 2, w - 14, h / 2, juce::Justification::bottomLeft, true);
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(11.0f));
        juce::String sub = templates::family_display(def->family);
        if (!def->region.empty()) sub << "  -  " << def->region;
        g.drawText(sub, 10, h / 2, w - 14, h / 2 - 2, juce::Justification::topLeft, true);
        return;
    }
    const auto* e = entry_at(row);
    if (!e) return;
    g.setColour(colour_from_hex(templates::family_color(e->family)));
    g.fillRect(0, 0, 4, h);
    juce::String badge;
    juce::Colour badge_colour = colours::text_dim;
    if (view_ == View::ForTrack && row < static_cast<int>(candidates_.size())) {
        const auto m = candidates_[static_cast<std::size_t>(row)].match;
        badge = library::to_string(m);
        badge_colour = m == library::Match::Exact ? colours::exact : m == library::Match::Fallback ? colours::fallback
                                                                                                : colours::accent_warm;
    } else {
        badge = e->kind == library::EntryKind::UserPatch ? "saved" : e->kind == library::EntryKind::CatalogPatch ? "catalog"
                                                                                                                : "plug-in";
    }
    g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    const int bw = 58;
    g.setColour(badge_colour);
    g.drawText(badge.toUpperCase(), w - bw - 4, 4, bw, 14, juce::Justification::centredRight);
    g.setColour(e->player_installed ? colours::text : colours::text_dim);
    g.setFont(juce::FontOptions(13.0f));
    g.drawText((e->favorite ? juce::String::fromUTF8("\xe2\x98\x85 ") : juce::String()) + juce::String(e->display), 10, 2,
               w - bw - 18, h / 2, juce::Justification::bottomLeft, true);
    g.setColour(colours::text_dim);
    g.setFont(juce::FontOptions(11.0f));
    juce::String sub = juce::String(e->player);
    if (!e->library.empty() && e->library != e->player) sub << " - " << e->library;
    if (!e->player_installed) sub << " - not installed";
    g.drawText(sub, 10, h / 2, w - 14, h / 2 - 2, juce::Justification::topLeft, true);
}

void LibraryBrowser::selectedRowsChanged(int row)
{
    if (view_ == View::Instruments) return;
    const auto* e = entry_at(row);
    if (!e) return;
    juce::String text = library::status_text(*e);
    if (view_ == View::ForTrack && row < static_cast<int>(candidates_.size())) {
        const auto& c = candidates_[static_cast<std::size_t>(row)];
        text << "\n" << c.reason;
        if (!c.missing_articulations.empty()) {
            juce::StringArray m;
            for (const auto& a : c.missing_articulations) m.add(a);
            text << "\nNot covered: " << m.joinIntoString(", ");
        }
    } else if (e->validation == "inferred" || e->validation == "unknown") {
        text << "\n" << (e->kind == library::EntryKind::CatalogPatch ? "From the catalog: verify in the player"
                                                                     : "Inferred from the name: verify by ear");
    }
    detail_.setText(text, juce::dontSendNotification);
    star_.setButtonText(e->favorite ? "Unfavorite" : "Favorite");
}

void LibraryBrowser::listBoxItemDoubleClicked(int, const juce::MouseEvent&) { assign_selected(); }

void LibraryBrowser::audition_selected()
{
    const int row = list_.getSelectedRow();
    if (view_ == View::Instruments) {
        if (row < 0 || row >= static_cast<int>(instruments_.size())) return;
        const auto* def = instruments_[static_cast<std::size_t>(row)];
        context_.engine.preview(phrase_for(def), def->family, def->unpitched ? -1 : def->gm_program, 110.0, false);
        context_.status("Auditioning " + juce::String(def->name) + " on the built-in placeholder sound");
        return;
    }
    if (const auto* e = entry_at(row); e && context_.library) context_.status(context_.library->audition(*e));
}

void LibraryBrowser::assign_selected()
{
    const int row = list_.getSelectedRow();
    if (view_ == View::Instruments) {
        if (row >= 0 && row < static_cast<int>(instruments_.size())) {
            const auto* def = instruments_[static_cast<std::size_t>(row)];
            context_.session.add_track(def->name, def->id);
        }
        return;
    }
    const auto* e = entry_at(row);
    if (!e || !context_.library) return;
    std::string track = context_.session.selected_track();
    if (track.empty() && !e->instrument_id.empty()) {
        // Nothing selected: a new track for the sound's instrument.
        const auto* def = context_.catalogs.instruments.find(e->instrument_id);
        track = context_.session.add_track(def ? def->name : e->display, e->instrument_id);
    }
    const auto entry = *e;  // the list may rebuild under us
    context_.status(context_.library->assign(entry, track));
}

void LibraryBrowser::toggle_favorite_selected()
{
    const auto* e = entry_at(list_.getSelectedRow());
    if (!e || !context_.library) return;
    context_.library->lib().set_favorite(e->key, !e->favorite);
    const auto key = e->key;
    context_.library->changed();
    const auto keys = listed();
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == key) list_.selectRow(static_cast<int>(i));
}

void LibraryBrowser::save_track_sound()
{
    if (!context_.library) return;
    const auto track = context_.session.selected_track();
    const auto* t = context_.session.data().find_track(track);
    if (!t) {
        context_.status("Select the track whose sound to save");
        return;
    }
    juce::String message;
    context_.library->save_track_sound(track, {}, message);
    context_.status(message);
}

void LibraryBrowser::show_more_menu()
{
    juce::PopupMenu m;
    m.addItem("Library folders...", [this] {
        const juce::Component::SafePointer<LibraryBrowser> safe(this);
        chooser_ = std::make_unique<juce::FileChooser>("Add a folder where sample libraries are installed");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe](const juce::FileChooser& fc) {
                                  if (!safe || !fc.getResult().isDirectory() || !safe->context_.library) return;
                                  auto& lib = safe->context_.library->lib();
                                  lib.library_folders().push_back(fc.getResult().getFullPathName().toStdString());
                                  const auto found = lib.detect();
                                  safe->context_.library->changed();
                                  safe->context_.status("Library folder added: " + juce::String(static_cast<int>(found.size())) +
                                                        " library(ies) detected");
                              });
    });
    m.addItem("Rescan plug-ins...", [this] { context_.commands.invokeDirectly(cmd::scanPlugins, true); });
    m.addSeparator();
    m.addItem("Export mapping pack...", context_.library && !context_.library->lib().patches().empty(), false, [this] {
        const juce::Component::SafePointer<LibraryBrowser> safe(this);
        chooser_ = std::make_unique<juce::FileChooser>("Export mapping pack",
                                                       juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                                           .getChildFile("My Sounds.vccmap"),
                                                       "*.vccmap");
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safe](const juce::FileChooser& fc) {
                                  if (!safe || fc.getResult() == juce::File() || !safe->context_.library) return;
                                  const auto f = fc.getResult().withFileExtension("vccmap");
                                  const auto pack = safe->context_.library->lib().export_pack(f.getFileNameWithoutExtension().toStdString());
                                  f.replaceWithText(juce::String(pack.to_text(2)));
                                  safe->context_.status("Exported " + juce::String(static_cast<int>(pack["patches"].size())) +
                                                        " patch(es) to " + f.getFileName());
                              });
    });
    m.addItem("Import mapping pack...", [this] {
        const juce::Component::SafePointer<LibraryBrowser> safe(this);
        chooser_ = std::make_unique<juce::FileChooser>("Import mapping pack", juce::File(), "*.vccmap;*.json");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe](const juce::FileChooser& fc) {
                                  if (!safe || !fc.getResult().existsAsFile() || !safe->context_.library) return;
                                  std::vector<std::string> warnings;
                                  const auto doc = json::parse(fc.getResult().loadFileAsString().toStdString());
                                  const int n = doc ? safe->context_.library->lib().import_pack(*doc, &warnings) : 0;
                                  safe->context_.library->changed();
                                  safe->context_.status("Imported " + juce::String(n) + " patch(es)" +
                                                        (warnings.empty() ? juce::String()
                                                                          : " - " + juce::String(warnings.front())));
                              });
    });
    if (const auto* e = entry_at(list_.getSelectedRow()); e && e->kind == library::EntryKind::UserPatch) {
        const auto id = e->key.substr(6);
        m.addSeparator();
        m.addItem("Delete saved patch \"" + juce::String(e->display) + "\"", [this, id] {
            if (!context_.library) return;
            context_.library->lib().remove_patch(id);
            context_.library->changed();
        });
    }
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&more_));
}

void LibraryBrowser::paint(juce::Graphics& g) { g.fillAll(colours::panel); }

void LibraryBrowser::resized()
{
    auto r = getLocalBounds().reduced(6);
    search_.setBounds(r.removeFromTop(26));
    r.removeFromTop(4);
    view_box_.setBounds(r.removeFromTop(24));
    r.removeFromTop(4);
    auto row = r.removeFromTop(24);
    if (view_ == View::Instruments) {
        family_box_.setBounds(row);
    } else {
        family_box_.setBounds(row.removeFromLeft(row.getWidth() / 2 - 2));
        row.removeFromLeft(4);
        technique_box_.setBounds(row);
        r.removeFromTop(2);
        auto toggles = r.removeFromTop(22);
        installed_.setBounds(toggles.removeFromLeft(toggles.getWidth() / 2));
        favorites_.setBounds(toggles);
    }
    r.removeFromTop(4);
    auto buttons2 = r.removeFromBottom(26);
    r.removeFromBottom(4);
    auto buttons1 = r.removeFromBottom(26);
    r.removeFromBottom(4);
    detail_.setBounds(r.removeFromBottom(view_ == View::Instruments ? 0 : 58));
    r.removeFromBottom(4);
    list_.setBounds(r);
    const int w1 = (buttons1.getWidth() - 8) / 3;
    audition_.setBounds(buttons1.removeFromLeft(w1));
    buttons1.removeFromLeft(4);
    assign_.setBounds(buttons1.removeFromLeft(w1));
    buttons1.removeFromLeft(4);
    star_.setBounds(buttons1);
    if (view_ == View::Instruments) {
        more_.setBounds(buttons2);
    } else {
        save_.setBounds(buttons2.removeFromLeft((buttons2.getWidth() - 4) / 2));
        buttons2.removeFromLeft(4);
        more_.setBounds(buttons2);
    }
}

}  // namespace vcc::desktop
