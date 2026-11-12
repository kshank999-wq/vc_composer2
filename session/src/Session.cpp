#include "vcc/session/Session.h"

#include <algorithm>
#include <filesystem>

#include "vcc/core/Files.h"
#include "vcc/project/Render.h"

namespace vcc::session {

namespace fs = std::filesystem;

// --- Catalogs -------------------------------------------------------------------

namespace {

bool starts_with(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

void finish_catalogs(Catalogs& c)
{
    std::vector<std::string> progs;
    for (const auto& p : c.progressions.all()) progs.push_back(p.id);
    c.templates->set_known_articulations(c.articulation_ids);
    c.templates->set_known_progressions(progs);
    c.templates->set_known_ostinato_presets(c.ostinato_presets.ids());
}

void read_articulations(Catalogs& c, const json::Value& doc)
{
    for (const auto& a : doc.get("articulations") ? doc.get("articulations")->as_array() : json::Array{})
        c.articulation_ids.push_back(a.get_string("id"));
}

void error(Catalogs& c, const std::string& where, const std::string& message)
{
    c.diagnostics.push_back({templates::Diagnostic::Level::Error, where, message});
}

}  // namespace

int Catalogs::error_count() const noexcept
{
    return static_cast<int>(std::count_if(diagnostics.begin(), diagnostics.end(), [](const templates::Diagnostic& d) {
        return d.level == templates::Diagnostic::Level::Error;
    }));
}

std::unique_ptr<Catalogs> Catalogs::load_from_documents(const std::map<std::string, std::string>& docs)
{
    auto c = std::make_unique<Catalogs>();
    c->templates = std::make_unique<templates::TemplateRegistry>(c->instruments);
    std::map<std::string, json::Value> parsed;
    for (const auto& [name, text] : docs) {
        json::ParseError pe;
        auto v = json::parse(text, &pe);
        if (!v) {
            error(*c, name, pe.to_string());
            continue;
        }
        parsed.emplace(name, std::move(*v));
    }
    // Order matters: instruments and catalogs before the templates that refer to them.
    for (const auto& [name, doc] : parsed)
        if (starts_with(name, "instruments/")) c->instruments.load(doc, name, c->diagnostics);
    for (const auto& [name, doc] : parsed) {
        std::string err;
        if (name == "articulations.json") read_articulations(*c, doc);
        else if (name == "progressions.json" && !c->progressions.load(doc, &err)) error(*c, name, err);
        else if (name == "ostinato_presets.json" && !c->ostinato_presets.load(doc, &err)) error(*c, name, err);
    }
    finish_catalogs(*c);
    for (const auto& [name, doc] : parsed)
        if (starts_with(name, "templates/")) c->templates->load_pack(doc, name, c->diagnostics);
    return c;
}

std::unique_ptr<Catalogs> Catalogs::load_from_directory(const std::string& dir)
{
    std::map<std::string, std::string> docs;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(fs::u8path(dir), ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        std::string rel = fs::relative(entry.path(), fs::u8path(dir)).generic_u8string();
        if (auto text = files::read_file(entry.path().u8string())) docs.emplace(rel, std::move(*text));
    }
    auto c = load_from_documents(docs);
    if (docs.empty()) error(*c, dir, "no content found");
    return c;
}

// --- Session --------------------------------------------------------------------------

Session::Session(const Catalogs& catalogs) : catalogs_(catalogs) {}

int Session::add_listener(Listener listener)
{
    int id = next_listener_++;
    listeners_.emplace(id, std::move(listener));
    return id;
}

void Session::remove_listener(int id) { listeners_.erase(id); }

void Session::notify(Change change)
{
    // Copy first: a listener may add or remove listeners while being told.
    auto copy = listeners_;
    for (auto& [id, l] : copy)
        if (listeners_.count(id)) l(change);
}

void Session::document_changed()
{
    const bool was_dirty = dirty_;
    dirty_ = true;
    if (!selected_track_.empty() && !data().find_track(selected_track_)) {
        selected_track_.clear();
        auto tracks = music_tracks();
        if (!tracks.empty()) selected_track_ = tracks.front()->id;
        notify(Change::Selection);
    }
    notify(Change::Document);
    if (!was_dirty) notify(Change::File);
}

void Session::reset(project::Project p, const std::string& path)
{
    project_ = std::move(p);
    path_ = path;
    dirty_ = false;
    selected_track_.clear();
    auto tracks = music_tracks();
    if (!tracks.empty()) selected_track_ = tracks.front()->id;
    notify(Change::Document);
    notify(Change::Selection);
    notify(Change::File);
}

std::string Session::title() const
{
    std::string name = path_.empty() ? std::string("Untitled") : fs::u8path(path_).stem().u8string();
    if (path_.empty() && data().name != "Untitled") name = data().name;
    return dirty_ ? name + " *" : name;
}

void Session::new_empty() { reset(project::Project(), {}); }

templates::InstantiateResult Session::new_from_template(const std::string& id, const templates::InstantiateOptions& o)
{
    templates::InstantiateResult r;
    const templates::Template* t = catalogs_.templates->get(id);
    if (!t) {
        r.error = "no template '" + id + "'";
        return r;
    }
    project::Project fresh;
    templates::InstantiateOptions opt = o;
    opt.mode = templates::InstantiateMode::Create;
    r = templates::instantiate(fresh, *t, catalogs_.instruments, opt);
    if (!r.ok) return r;
    reset(std::move(fresh), {});
    dirty_ = true;
    notify(Change::File);
    return r;
}

templates::InstantiateResult Session::add_template(const std::string& id, const templates::InstantiateOptions& o)
{
    templates::InstantiateResult r;
    const templates::Template* t = catalogs_.templates->get(id);
    if (!t) {
        r.error = "no template '" + id + "'";
        return r;
    }
    templates::InstantiateOptions opt = o;
    if (opt.mode == templates::InstantiateMode::Create) opt.mode = templates::InstantiateMode::Merge;
    r = templates::instantiate(project_, *t, catalogs_.instruments, opt);
    if (r.ok) document_changed();
    return r;
}

bool Session::open(const std::string& path, std::string* error)
{
    auto loaded = project::Project::load(path, error);
    if (!loaded) return false;
    reset(std::move(*loaded), path);
    return true;
}

bool Session::save(std::string* error)
{
    if (path_.empty()) {
        if (error) *error = "choose where to save first";
        return false;
    }
    return save_as(path_, error);
}

bool Session::save_as(const std::string& path, std::string* error)
{
    if (!project_.save(path, error)) return false;
    path_ = path;
    dirty_ = false;
    // A clean save makes any older autosave obsolete.
    std::error_code ec;
    fs::remove(fs::u8path(path + ".autosave"), ec);
    notify(Change::File);
    return true;
}

std::string Session::autosave_path(const std::string& fallback_dir) const
{
    if (!path_.empty()) return path_ + ".autosave";
    return (fs::u8path(fallback_dir) / "Untitled.vcc2.autosave").u8string();
}

bool Session::write_autosave(const std::string& fallback_dir, std::string* error) const
{
    return files::write_file_atomic(autosave_path(fallback_dir), json::write(project_.to_json()), error);
}

bool Session::has_newer_autosave(const std::string& path)
{
    std::error_code ec;
    auto a = fs::u8path(path + ".autosave");
    if (!fs::exists(a, ec)) return false;
    return fs::last_write_time(a, ec) > fs::last_write_time(fs::u8path(path), ec);
}

bool Session::edit(const std::string& label, const std::function<bool(project::ProjectData&)>& change)
{
    if (!project_.edit(label, change)) return false;
    document_changed();
    return true;
}

bool Session::undo()
{
    if (!project_.undo()) return false;
    document_changed();
    return true;
}

bool Session::redo()
{
    if (!project_.redo()) return false;
    document_changed();
    return true;
}

std::string Session::add_track(const std::string& name, const std::string& instrument)
{
    std::string id = project_.add_track(name, project::TrackKind::Instrument, instrument);
    if (const auto* def = catalogs_.instruments.find(instrument)) {
        project_.edit_coalescing("Add track " + name, [&](project::ProjectData& d) {
            project::Track* t = d.find_track(id);
            t->family = def->family;
            t->color = templates::family_color(def->family);
            t->assignment.status = project::ResolutionStatus::Placeholder;
            t->assignment.placeholder_program = std::max(0, def->gm_program);
            t->assignment.patch_display = "GM placeholder";
            t->assignment.message = "Built-in placeholder sound until a library patch or plug-in is assigned.";
            if (def->unpitched) t->midi_channel = 9;
            return true;
        });
    }
    document_changed();
    select_track(id);
    return id;
}

std::string Session::add_reference_track(const std::string& name)
{
    std::string id = project_.add_track(name, project::TrackKind::Reference);
    document_changed();
    select_track(id);
    return id;
}

bool Session::remove_track(const std::string& id)
{
    const project::Track* t = data().find_track(id);
    if (!t) return false;
    return edit("Remove " + t->name, [&](project::ProjectData& d) {
        d.tracks.erase(std::remove_if(d.tracks.begin(), d.tracks.end(), [&](const project::Track& x) { return x.id == id; }),
                       d.tracks.end());
        for (auto& x : d.tracks)
            if (x.folder_id == id) x.folder_id.clear();
        for (auto& s : d.snapshots) s.track_versions.erase(id);
        return true;
    });
}

namespace {
template <typename F>
std::function<bool(project::ProjectData&)> on_track(const std::string& id, F f)
{
    return [id, f](project::ProjectData& d) {
        project::Track* t = d.find_track(id);
        return t && f(*t);
    };
}
}  // namespace

bool Session::rename_track(const std::string& id, const std::string& name)
{
    if (name.empty()) return false;
    return edit("Rename track", on_track(id, [&](project::Track& t) {
        if (t.name == name) return false;
        t.name = name;
        return true;
    }));
}

bool Session::set_gain(const std::string& id, double gain_db)
{
    gain_db = std::clamp(gain_db, -96.0, 12.0);
    if (!project_.edit_coalescing("Gain " + id, on_track(id, [&](project::Track& t) {
            t.gain_db = gain_db;
            return true;
        })))
        return false;
    document_changed();
    return true;
}

bool Session::set_pan(const std::string& id, double pan)
{
    pan = std::clamp(pan, -1.0, 1.0);
    if (!project_.edit_coalescing("Pan " + id, on_track(id, [&](project::Track& t) {
            t.pan = pan;
            return true;
        })))
        return false;
    document_changed();
    return true;
}

bool Session::set_mute(const std::string& id, bool mute)
{
    return edit(mute ? "Mute" : "Unmute", on_track(id, [&](project::Track& t) {
        if (t.mute == mute) return false;
        t.mute = mute;
        return true;
    }));
}

bool Session::set_solo(const std::string& id, bool solo)
{
    return edit(solo ? "Solo" : "Unsolo", on_track(id, [&](project::Track& t) {
        if (t.solo == solo) return false;
        t.solo = solo;
        return true;
    }));
}

bool Session::set_output_bus(const std::string& id, const std::string& bus_id)
{
    if (!data().find_bus(bus_id)) return false;
    return edit("Route output", on_track(id, [&](project::Track& t) {
        if (t.output_bus_id == bus_id) return false;
        t.output_bus_id = bus_id;
        return true;
    }));
}

bool Session::set_bus_gain(const std::string& bus_id, double gain_db)
{
    gain_db = std::clamp(gain_db, -96.0, 12.0);
    if (!project_.edit_coalescing("Bus gain " + bus_id, [&](project::ProjectData& d) {
            project::Bus* b = d.find_bus(bus_id);
            if (!b) return false;
            b->gain_db = gain_db;
            return true;
        }))
        return false;
    document_changed();
    return true;
}

bool Session::set_include_in_export(const std::string& id, bool include)
{
    return edit(include ? "Include in export" : "Exclude from export", on_track(id, [&](project::Track& t) {
        if (t.include_in_export == include) return false;
        t.include_in_export = include;
        return true;
    }));
}

bool Session::set_active_version(const std::string& id, const std::string& version)
{
    if (!project_.set_active_version(id, version)) return false;
    document_changed();
    return true;
}

std::optional<std::string> Session::duplicate_version(const std::string& id)
{
    auto v = project_.duplicate_version(id);
    if (v) document_changed();
    return v;
}

bool Session::set_instrument_plugin(const std::string& id, const project::PluginRef& plugin)
{
    return edit(plugin.empty() ? "Use placeholder sound" : "Load " + plugin.name, on_track(id, [&](project::Track& t) {
        t.instrument_plugin = plugin;
        if (!plugin.empty()) {
            t.assignment.status = project::ResolutionStatus::Exact;
            t.assignment.patch_display = plugin.name;
            t.assignment.message = "Played by " + plugin.name + (plugin.vendor.empty() ? "" : " (" + plugin.vendor + ")") +
                                   ". Map articulations in the plug-in or with an articulation map.";
        } else {
            t.assignment.status = project::ResolutionStatus::Placeholder;
            t.assignment.patch_display = "GM placeholder";
            t.assignment.message = "Built-in placeholder sound until a library patch or plug-in is assigned.";
        }
        return true;
    }));
}

void Session::store_plugin_state(const std::string& id, const std::string& state)
{
    bool changed = false;
    project_.modify_unrecorded([&](project::ProjectData& d) {
        if (project::Track* t = d.find_track(id); t && t->instrument_plugin.state_base64 != state) {
            t->instrument_plugin.state_base64 = state;
            changed = true;
        }
    });
    if (changed && !dirty_) {
        dirty_ = true;
        notify(Change::File);
    }
}

bool Session::set_tempo(double bpm)
{
    if (bpm < 20 || bpm > 400) return false;
    if (!project_.edit_coalescing("Tempo", [&](project::ProjectData& d) {
            d.tempo.set(0, bpm);
            return true;
        }))
        return false;
    document_changed();
    return true;
}

bool Session::set_meter(const TimeSignature& sig)
{
    if (!sig.is_valid()) return false;
    return edit("Time signature " + sig.to_string(), [&](project::ProjectData& d) {
        if (d.meter.at_bar(1) == sig) return false;
        d.meter.set(1, sig);
        return true;
    });
}

bool Session::set_key(const std::string& key)
{
    if (!theory::Key::parse(key)) return false;
    return edit("Key " + key, [&](project::ProjectData& d) {
        if (d.key == key) return false;
        d.key = key;
        return true;
    });
}

bool Session::commit_score(const std::string& id, const notation::Score& score, const std::string& label)
{
    return edit(label.empty() ? "Edit notation" : label, on_track(id, [&](project::Track& t) {
        project::TrackVersion* v = t.active_version();
        if (!v) return false;
        v->score = score;
        return true;
    }));
}

project::InsertResult Session::insert_notes(const std::string& id, Ticks start, Ticks end, std::vector<NoteEvent> notes,
                                            project::CollisionPolicy policy, const std::string& source)
{
    auto r = project_.insert_notes(id, start, end, std::move(notes), policy, source);
    if (r.ok) document_changed();
    return r;
}

project::InsertResult Session::send_progression(const std::string& id, const theory::ProgressionRender& render,
                                                const std::string& progression_id, project::CollisionPolicy policy)
{
    return insert_notes(id, render.start, render.end, render.notes, policy, "chord_explorer:" + progression_id);
}

project::InsertResult Session::send_ostinato(const std::string& id, const ostinato::Variant& v,
                                             const std::string& preset_id, project::CollisionPolicy policy)
{
    return insert_notes(id, v.start, v.end, v.notes, policy, "ostinato:" + preset_id + "/" + v.id);
}

void Session::select_track(const std::string& id)
{
    if (id == selected_track_ || (!id.empty() && !data().find_track(id))) return;
    selected_track_ = id;
    notify(Change::Selection);
}

std::vector<const project::Track*> Session::music_tracks() const
{
    std::vector<const project::Track*> out;
    for (const auto& t : data().tracks)
        if (t.kind != project::TrackKind::Folder && t.kind != project::TrackKind::Video) out.push_back(&t);
    return out;
}

bool Session::audible(const project::Track& track) const
{
    if (track.mute) return false;
    bool any_solo = std::any_of(data().tracks.begin(), data().tracks.end(), [](const project::Track& t) { return t.solo; });
    return !any_solo || track.solo;
}

Ticks Session::content_end() const
{
    Ticks end = data().meter.bar_start(33);
    for (const auto& t : data().tracks) {
        const project::TrackVersion* v = t.active_version();
        if (!v) continue;
        end = std::max(end, v->score.end_tick());
        for (const auto& r : v->regions) end = std::max(end, r.end);
    }
    // Round up to a bar and leave four empty bars to write into.
    int bar = data().meter.bar_at(end - 1) + 4;
    return data().meter.bar_start(bar + 1);
}

const templates::InstrumentDef* Session::instrument_for(const project::Track& track) const
{
    return catalogs_.instruments.find(track.instrument_standard_id);
}

std::pair<int, int> Session::range_for(const project::Track& track) const
{
    if (const auto* def = instrument_for(track)) return {def->range_low, def->range_high};
    return {21, 108};
}

}  // namespace vcc::session
