#include "vcc/session/Session.h"

#include <algorithm>
#include <filesystem>
#include <set>

#include "vcc/core/Files.h"
#include "vcc/cues/Cues.h"
#include "vcc/picture/Picture.h"
#include "vcc/project/Automation.h"
#include "vcc/project/Performance.h"
#include "vcc/project/Render.h"
#include "vcc/project/Stems.h"

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
    if (!selection_.track.empty() && !data().find_track(selection_.track)) {
        selection_.track.clear();
        selection_.notes.clear();
        auto tracks = music_tracks();
        if (!tracks.empty()) selection_.track = tracks.front()->id;
        notify(Change::Selection);
    } else if (!selection_.notes.empty()) {
        // Notes that an edit removed leave the selection.
        Selection kept = selection_;
        const auto* t = data().find_track(kept.track);
        const auto* v = t ? t->active_version() : nullptr;
        kept.notes.erase(std::remove_if(kept.notes.begin(), kept.notes.end(),
                                        [&](notation::EventId id) { return !v || !v->score.find_note(id); }),
                         kept.notes.end());
        if (kept.notes != selection_.notes) {
            selection_ = kept;
            notify(Change::Selection);
        }
    }
    notify(Change::Document);
    if (!was_dirty) notify(Change::File);
}

void Session::reset(project::Project p, const std::string& path)
{
    project_ = std::move(p);
    path_ = path;
    dirty_ = false;
    selection_ = {};
    ++generation_;
    auto tracks = music_tracks();
    if (!tracks.empty()) selection_.track = tracks.front()->id;
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
            // The staves the instrument reads (clef, transposition, grand
            // staff), as a template would give it.
            if (auto* v = t->active_version(); v && v->score.total_note_count() == 0)
                v->score.reset_staves(templates::instrument_score(*def, t->name).staves());
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

void Session::store_insert_plugin_state(const std::string& owner_id, int slot, const std::string& state)
{
    bool changed = false;
    project_.modify_unrecorded([&](project::ProjectData& d) {
        std::vector<project::InsertSlot>* inserts = nullptr;
        if (project::Track* t = d.find_track(owner_id)) inserts = &t->inserts;
        else if (project::Bus* b = d.find_bus(owner_id)) inserts = &b->inserts;
        if (!inserts || slot < 0 || slot >= static_cast<int>(inserts->size())) return;
        auto& ins = (*inserts)[static_cast<std::size_t>(slot)];
        if (ins.kind == "plugin" && ins.plugin.state_base64 != state) {
            ins.plugin.state_base64 = state;
            changed = true;
        }
    });
    if (changed && !dirty_) {
        dirty_ = true;
        notify(Change::File);
    }
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

bool Session::set_delay_compensation(bool on)
{
    return edit(on ? "Delay compensation on" : "Delay compensation off", [&](project::ProjectData& d) {
        if (d.delay_compensation == on) return false;
        d.delay_compensation = on;
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

// --- Picture, markers and cues ---------------------------------------------------------

bool Session::set_frame_rate(FrameRate rate)
{
    return edit(std::string("Frame rate ") + to_string(rate), [&](project::ProjectData& d) {
        if (d.frame_rate == rate) return false;
        d.frame_rate = rate;
        return true;
    });
}

bool Session::set_start_timecode(const std::string& tc)
{
    auto parsed = Timecode::parse(tc);
    if (!parsed || !timecode_to_frames(*parsed, data().frame_rate)) return false;
    return edit("Start timecode " + tc, [&](project::ProjectData& d) {
        if (d.start_timecode == tc) return false;
        d.start_timecode = tc;
        return true;
    });
}

std::string Session::import_picture(project::PictureVersion picture)
{
    picture.id = project_.ids().next("pic");
    if (picture.name.empty()) picture.name = "Cut " + std::to_string(data().pictures.size() + 1);
    const std::string id = picture.id;
    edit("Import picture " + picture.name, [&](project::ProjectData& d) {
        d.pictures.push_back(picture);
        d.active_picture_id = id;
        return true;
    });
    return id;
}

bool Session::set_active_picture(const std::string& id)
{
    return edit("Switch picture", [&](project::ProjectData& d) {
        if (d.active_picture_id == id) return false;
        if (!id.empty() && std::none_of(d.pictures.begin(), d.pictures.end(),
                                        [&](const project::PictureVersion& p) { return p.id == id; }))
            return false;
        d.active_picture_id = id;
        return true;
    });
}

namespace {
template <typename F>
std::function<bool(project::ProjectData&)> on_picture(const std::string& id, F f)
{
    return [id, f](project::ProjectData& d) {
        for (auto& p : d.pictures)
            if (p.id == id) return f(p);
        return false;
    };
}
}  // namespace

bool Session::set_picture_offset(const std::string& id, double offset)
{
    if (!project_.edit_coalescing("Picture offset " + id, on_picture(id, [&](project::PictureVersion& p) {
            p.offset_seconds = offset;
            return true;
        })))
        return false;
    document_changed();
    return true;
}

bool Session::set_picture_start_timecode(const std::string& id, const std::string& tc)
{
    if (!Timecode::parse(tc)) return false;
    return edit("Picture start timecode", on_picture(id, [&](project::PictureVersion& p) {
        if (!timecode_to_frames(*Timecode::parse(tc), p.rate)) return false;
        p.start_timecode = tc;
        return true;
    }));
}

bool Session::set_changed_regions(const std::string& id, std::vector<project::FlaggedRegion> regions)
{
    return edit("Flag changed regions", on_picture(id, [&](project::PictureVersion& p) {
        p.changed_regions = regions;
        return true;
    }));
}

bool Session::remove_picture(const std::string& id)
{
    return edit("Remove picture", [&](project::ProjectData& d) {
        auto it = std::find_if(d.pictures.begin(), d.pictures.end(), [&](const project::PictureVersion& p) { return p.id == id; });
        if (it == d.pictures.end()) return false;
        d.pictures.erase(it);
        if (d.active_picture_id == id) d.active_picture_id = d.pictures.empty() ? std::string{} : d.pictures.back().id;
        return true;
    });
}

std::string Session::add_marker(double seconds, const std::string& kind, const std::string& label)
{
    const std::string id = project_.ids().next("mk");
    edit("Add " + kind, [&](project::ProjectData& d) {
        project::Marker m;
        m.id = id;
        m.kind = kind;
        m.seconds = std::max(0.0, seconds);
        m.label = label;
        // A marker inside a cue belongs to it.
        for (const auto& c : d.cues)
            if (m.seconds >= d.tempo.seconds_at(c.start) && m.seconds <= d.tempo.seconds_at(c.end)) m.cue_id = c.id;
        d.markers.push_back(m);
        std::stable_sort(d.markers.begin(), d.markers.end(),
                         [](const project::Marker& a, const project::Marker& b) { return a.seconds < b.seconds; });
        return true;
    });
    return id;
}

bool Session::update_marker(const std::string& id, const std::function<void(project::Marker&)>& change)
{
    return edit("Edit marker", [&](project::ProjectData& d) {
        for (auto& m : d.markers)
            if (m.id == id) {
                change(m);
                std::stable_sort(d.markers.begin(), d.markers.end(),
                                 [](const project::Marker& a, const project::Marker& b) { return a.seconds < b.seconds; });
                return true;
            }
        return false;
    });
}

bool Session::remove_marker(const std::string& id)
{
    return edit("Remove marker", [&](project::ProjectData& d) {
        auto it = std::find_if(d.markers.begin(), d.markers.end(), [&](const project::Marker& m) { return m.id == id; });
        if (it == d.markers.end()) return false;
        d.markers.erase(it);
        return true;
    });
}

bool Session::fit_tempo_to_marker(const std::string& marker_id, Ticks target_tick, Ticks from_tick, std::string* error)
{
    const project::Marker* marker = nullptr;
    for (const auto& m : data().markers)
        if (m.id == marker_id) marker = &m;
    if (!marker) {
        if (error) *error = "no such marker";
        return false;
    }
    auto fitted = picture::fit_tempo_to_hit(data().tempo, from_tick, target_tick, marker->seconds, error);
    if (!fitted) return false;
    return edit("Fit tempo to " + (marker->label.empty() ? marker->kind : marker->label), [&](project::ProjectData& d) {
        d.tempo = *fitted;
        return true;
    });
}

bool Session::set_production(const project::Production& p)
{
    return edit("Production details", [&](project::ProjectData& d) {
        d.production = p;
        return true;
    });
}

std::string Session::add_reel(const std::string& name)
{
    const std::string id = project_.ids().next("reel");
    edit("Add " + name, [&](project::ProjectData& d) {
        d.reels.push_back({id, name});
        return true;
    });
    return id;
}

std::string Session::add_scene(const std::string& reel_id, const std::string& name)
{
    const std::string id = project_.ids().next("scene");
    edit("Add scene " + name, [&](project::ProjectData& d) {
        d.scenes.push_back({id, reel_id, name});
        return true;
    });
    return id;
}

std::string Session::add_cue(Ticks start, Ticks end, const std::string& reel_id, const std::string& title)
{
    const std::string id = project_.ids().next("cue");
    std::string code;
    edit("Add cue", [&](project::ProjectData& d) {
        project::Cue c;
        c.id = id;
        c.code = code = cues::next_code(d, reel_id);
        c.title = title.empty() ? "Untitled cue" : title;
        c.start = std::max<Ticks>(0, std::min(start, end));
        c.end = std::max(start, end);
        if (c.end == c.start) c.end = c.start + d.meter.at_tick(c.start).ticks_per_bar() * 8;
        c.reel_id = reel_id;
        c.composer = d.production.composer;
        c.status = "spotted";
        c.deliverables = {{"Stereo mix", false}, {"Stems", false}, {"Score and parts", false}};
        d.cues.push_back(c);
        return true;
    });
    return id;
}

bool Session::update_cue(const std::string& id, const std::function<void(project::Cue&)>& change,
                         const std::string& label, bool allow_locked)
{
    return edit(label, [&](project::ProjectData& d) {
        project::Cue* c = d.find_cue(id);
        if (!c || (c->locked() && !allow_locked)) return false;
        change(*c);
        if (c->end < c->start) std::swap(c->start, c->end);
        return true;
    });
}

bool Session::remove_cue(const std::string& id, bool allow_locked)
{
    return edit("Remove cue", [&](project::ProjectData& d) {
        auto it = std::find_if(d.cues.begin(), d.cues.end(), [&](const project::Cue& c) { return c.id == id; });
        if (it == d.cues.end() || (it->locked() && !allow_locked)) return false;
        d.cues.erase(it);
        for (auto& m : d.markers)
            if (m.cue_id == id) m.cue_id.clear();
        return true;
    });
}

bool Session::spot_cue(const std::string& id, const std::string& in_tc, const std::string& out_tc, bool move_music)
{
    auto in_s = picture::project_seconds_for_timecode(data(), in_tc);
    auto out_s = picture::project_seconds_for_timecode(data(), out_tc);
    if (!in_s || !out_s || *out_s <= *in_s) return false;
    return update_cue(id, [&](project::Cue& c) {
        c.spot_in = in_tc;
        c.spot_out = out_tc;
        if (move_music) {
            c.start = data().tempo.tick_at_seconds(std::max(0.0, *in_s));
            c.end = data().tempo.tick_at_seconds(std::max(0.0, *out_s));
        }
    }, "Spot cue " + id);
}

void Session::select_track(const std::string& id)
{
    if (id == selection_.track || (!id.empty() && !data().find_track(id))) return;
    selection_.track = id;
    selection_.notes.clear();
    notify(Change::Selection);
}

void Session::set_selection(Selection s)
{
    if (!s.track.empty() && !data().find_track(s.track)) return;
    const auto* t = data().find_track(s.track);
    const auto* v = t ? t->active_version() : nullptr;
    s.notes.erase(std::remove_if(s.notes.begin(), s.notes.end(),
                                 [&](notation::EventId id) { return !v || !v->score.find_note(id); }),
                  s.notes.end());
    std::sort(s.notes.begin(), s.notes.end());
    s.notes.erase(std::unique(s.notes.begin(), s.notes.end()), s.notes.end());
    if (s.to <= s.from) s.from = s.to = 0;
    if (s == selection_) return;
    selection_ = std::move(s);
    notify(Change::Selection);
}

void Session::set_selection_range(Ticks from, Ticks to)
{
    Selection s = selection_;
    s.from = from;
    s.to = to;
    set_selection(std::move(s));
}

void Session::select_notes(const std::string& track_id, std::vector<notation::EventId> notes)
{
    Selection s = selection_;
    s.track = track_id;
    s.notes = std::move(notes);
    if (const auto* t = data().find_track(track_id); t && t->active_version() && !s.notes.empty()) {
        Ticks lo = 0, hi = 0;
        bool any = false;
        for (auto id : s.notes)
            if (const auto* n = t->active_version()->score.find_note(id)) {
                lo = any ? std::min(lo, n->start_tick) : n->start_tick;
                hi = any ? std::max(hi, n->end_tick()) : n->end_tick();
                any = true;
            }
        if (any) {
            s.from = lo;
            s.to = hi;
        }
    }
    set_selection(std::move(s));
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

// --- Console and recording ---------------------------------------------------------------

bool Session::update_track_mix(const std::string& id, const std::string& label,
                               const std::function<void(project::Track&)>& change)
{
    if (!project_.edit_coalescing(label, on_track(id, [&](project::Track& t) {
            change(t);
            t.gain_db = std::clamp(t.gain_db, -96.0, 12.0);
            t.pan = std::clamp(t.pan, -1.0, 1.0);
            t.strip.trim_db = std::clamp(t.strip.trim_db, -20.0, 20.0);
            return true;
        })))
        return false;
    document_changed();
    return true;
}

bool Session::update_bus(const std::string& bus_id, const std::string& label,
                         const std::function<void(project::Bus&)>& change)
{
    if (!project_.edit_coalescing(label, [&](project::ProjectData& d) {
            project::Bus* b = d.find_bus(bus_id);
            if (!b) return false;
            const std::string kind = b->kind, id = b->id;
            change(*b);
            b->id = id;  // identity and kind are not editable here
            b->kind = kind;
            b->gain_db = std::clamp(b->gain_db, -96.0, 12.0);
            b->pan = std::clamp(b->pan, -1.0, 1.0);
            if (b->output == b->id) b->output = "bus-master";
            return true;
        }))
        return false;
    document_changed();
    return true;
}

std::string Session::add_bus(const std::string& name, const std::string& kind)
{
    if (kind != "group" && kind != "fx") return {};
    const std::string id = project_.ids().next("bus");
    const bool ok = edit("Add bus " + name, [&](project::ProjectData& d) {
        project::Bus b;
        b.id = id;
        b.name = d.unique_bus_name(name.empty() ? (kind == "fx" ? "FX" : "Group") : name);
        b.kind = kind;
        b.output = d.find_bus("bus-master") ? "bus-master" : "";
        if (kind == "fx") {
            project::InsertSlot reverb;
            reverb.kind = "reverb";
            reverb.suggestion = false;
            b.inserts.push_back(reverb);
        }
        d.buses.push_back(b);
        return true;
    });
    return ok ? id : std::string{};
}

bool Session::remove_bus(const std::string& bus_id)
{
    const project::Bus* b = data().find_bus(bus_id);
    if (!b || b->kind == "master") return false;
    return edit("Remove bus " + b->name, [&](project::ProjectData& d) {
        auto drop = [&](std::vector<project::Send>& sends) {
            sends.erase(std::remove_if(sends.begin(), sends.end(),
                                       [&](const project::Send& s) { return s.bus_id == bus_id; }),
                        sends.end());
        };
        for (auto& t : d.tracks) {
            if (t.output_bus_id == bus_id) t.output_bus_id = "bus-master";
            drop(t.sends);
        }
        for (auto& x : d.buses) {
            if (x.output == bus_id) x.output = "bus-master";
            drop(x.sends);
        }
        d.buses.erase(std::remove_if(d.buses.begin(), d.buses.end(),
                                     [&](const project::Bus& x) { return x.id == bus_id; }),
                      d.buses.end());
        return true;
    });
}

std::string Session::add_audio_track(const std::string& name, bool stereo)
{
    std::string id = project_.add_track(name.empty() ? "Audio" : name, project::TrackKind::Audio);
    project_.modify_unrecorded([&](project::ProjectData& d) {
        if (project::Track* t = d.find_track(id)) {
            t->input_stereo = stereo;
            t->family = "audio";
        }
    });
    document_changed();
    select_track(id);
    return id;
}

void Session::set_record_armed(const std::string& id, bool armed)
{
    const project::Track* t = data().find_track(id);
    if (!t || t->record_armed == armed) return;
    project_.modify_unrecorded([&](project::ProjectData& d) {
        if (project::Track* x = d.find_track(id)) x->record_armed = armed;
    });
    notify(Change::Document);
}

std::vector<std::string> Session::add_audio_clips(
    const std::vector<std::pair<std::string, project::AudioClip>>& clips, const std::string& label, bool as_takes)
{
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < clips.size(); ++i) ids.push_back(project_.ids().next("clip"));
    const bool ok = edit(label, [&](project::ProjectData& d) {
        for (std::size_t i = 0; i < clips.size(); ++i) {
            project::Track* t = d.find_track(clips[i].first);
            if (!t) return false;
            project::TrackVersion* v = t->active_version();
            if (!v) return false;
            project::AudioClip c = clips[i].second;
            if (c.length <= 0.0 || c.file.empty()) return false;
            c.id = ids[i];
            c.lane = 0;
            if (as_takes) {
                int free_lane = 1;
                for (const auto& x : v->audio_clips) free_lane = std::max(free_lane, x.lane + 1);
                for (auto& x : v->audio_clips)
                    if (x.lane == 0 && x.start_seconds < c.end_seconds() && c.start_seconds < x.end_seconds())
                        x.lane = free_lane;
            }
            v->audio_clips.push_back(std::move(c));
            std::stable_sort(v->audio_clips.begin(), v->audio_clips.end(),
                             [](const project::AudioClip& a, const project::AudioClip& b) {
                                 return a.lane != b.lane ? a.lane < b.lane : a.start_seconds < b.start_seconds;
                             });
        }
        return true;
    });
    if (!ok) ids.clear();
    return ids;
}

bool Session::update_audio_clip(const std::string& track_id, const std::string& clip_id, const std::string& label,
                                const std::function<void(project::AudioClip&)>& change)
{
    if (!project_.edit_coalescing(label, on_track(track_id, [&](project::Track& t) {
            project::TrackVersion* v = t.active_version();
            if (!v) return false;
            for (auto& c : v->audio_clips)
                if (c.id == clip_id) {
                    change(c);
                    c.id = clip_id;
                    c.source_offset = std::max(0.0, c.source_offset);
                    c.fade_in = std::clamp(c.fade_in, 0.0, c.length);
                    c.fade_out = std::clamp(c.fade_out, 0.0, c.length - c.fade_in);
                    return c.length > 0.0 && !c.file.empty();
                }
            return false;
        })))
        return false;
    document_changed();
    return true;
}

bool Session::remove_audio_clip(const std::string& track_id, const std::string& clip_id)
{
    return edit("Delete clip", on_track(track_id, [&](project::Track& t) {
        project::TrackVersion* v = t.active_version();
        if (!v) return false;
        const auto before = v->audio_clips.size();
        v->audio_clips.erase(std::remove_if(v->audio_clips.begin(), v->audio_clips.end(),
                                            [&](const project::AudioClip& c) { return c.id == clip_id; }),
                             v->audio_clips.end());
        return v->audio_clips.size() != before;
    }));
}

std::string Session::audio_folder() const
{
    if (path_.empty()) return {};
    const auto slash = path_.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? std::string(".") : path_.substr(0, slash);
    return dir + "/Audio Files";
}

std::string Session::resolve_audio_path(const std::string& file) const
{
    if (file.empty()) return {};
    const bool absolute = file[0] == '/' || file[0] == '\\' || (file.size() > 2 && file[1] == ':');
    if (absolute || path_.empty()) return file;
    const auto slash = path_.find_last_of("/\\");
    return (slash == std::string::npos ? std::string(".") : path_.substr(0, slash)) + "/" + file;
}

// --- Automation --------------------------------------------------------------------------

namespace {

/// Runs `f` on the owner's lanes (and mode), a track or a bus. Returns false
/// when the owner does not exist or `f` declines.
template <typename F>
std::function<bool(project::ProjectData&)> on_owner(const std::string& id, F f)
{
    return [id, f](project::ProjectData& d) {
        if (project::Track* t = d.find_track(id)) return f(t->automation, t->automation_mode, t, static_cast<project::Bus*>(nullptr));
        if (project::Bus* b = d.find_bus(id)) return f(b->automation, b->automation_mode, static_cast<project::Track*>(nullptr), b);
        return false;
    };
}

std::optional<double> static_of(const project::Track* t, const project::Bus* b, const std::string& param)
{
    if (t) return project::static_value(*t, param);
    if (b) return project::static_value(*b, param);
    return std::nullopt;
}

}  // namespace

const std::vector<project::AutomationLane>* Session::lanes_of(const std::string& owner_id) const
{
    if (const auto* t = data().find_track(owner_id)) return &t->automation;
    if (const auto* b = data().find_bus(owner_id)) return &b->automation;
    return nullptr;
}

bool Session::set_automation_mode(const std::string& owner_id, const std::string& mode)
{
    const auto& modes = project::automation_modes();
    if (std::find(modes.begin(), modes.end(), mode) == modes.end()) return false;
    return edit("Automation " + mode, on_owner(owner_id, [&](auto&, std::string& m, auto*, auto*) {
        if (m == mode) return false;
        m = mode;
        return true;
    }));
}

std::string Session::ensure_lane(const std::string& owner_id, const std::string& param, const std::string& name)
{
    const auto info = project::param_info(param);
    if (!info) return {};
    if (const auto* lanes = lanes_of(owner_id)) {
        if (const auto* l = project::find_lane(*lanes, param)) {
            if (!l->shown) {
                const std::string id = l->id;
                edit_lane(owner_id, id, "Show lane", [](project::AutomationLane& x) { x.shown = true; });
            }
            return l->id;
        }
    } else {
        return {};
    }
    const std::string id = project_.ids().next("lane");
    const bool ok = edit("Add automation " + (name.empty() ? info->name : name),
                         on_owner(owner_id, [&](std::vector<project::AutomationLane>& lanes, std::string&,
                                                project::Track* t, project::Bus* b) {
                             project::AutomationLane lane;
                             lane.id = id;
                             lane.param = param;
                             lane.name = name.empty() ? info->name : name;
                             const auto v = static_of(t, b, param);
                             lane.points.push_back({0, v ? *v : info->default_value, "linear"});
                             project::normalise(lane);
                             lanes.push_back(std::move(lane));
                             return true;
                         }));
    return ok ? id : std::string{};
}

bool Session::edit_lane(const std::string& owner_id, const std::string& lane_id, const std::string& label,
                        const std::function<void(project::AutomationLane&)>& change)
{
    if (!project_.edit_coalescing(label, on_owner(owner_id, [&](std::vector<project::AutomationLane>& lanes,
                                                                std::string&, auto*, auto*) {
            for (auto& l : lanes)
                if (l.id == lane_id) {
                    const std::string param = l.param, id = l.id;
                    change(l);
                    l.param = param;
                    l.id = id;
                    project::normalise(l);
                    return true;
                }
            return false;
        })))
        return false;
    document_changed();
    return true;
}

bool Session::remove_lane(const std::string& owner_id, const std::string& lane_id)
{
    return edit("Remove automation", on_owner(owner_id, [&](std::vector<project::AutomationLane>& lanes,
                                                            std::string&, auto*, auto*) {
        const auto before = lanes.size();
        lanes.erase(std::remove_if(lanes.begin(), lanes.end(),
                                   [&](const project::AutomationLane& l) { return l.id == lane_id; }),
                    lanes.end());
        return lanes.size() != before;
    }));
}

bool Session::commit_automation_pass(const std::vector<AutomationWrite>& writes)
{
    if (writes.empty()) return false;
    std::vector<std::string> new_ids;
    for (std::size_t i = 0; i < writes.size(); ++i) new_ids.push_back(project_.ids().next("lane"));
    return edit("Write automation", [&](project::ProjectData& d) {
        for (std::size_t i = 0; i < writes.size(); ++i) {
            const auto& w = writes[i];
            const auto info = project::param_info(w.param);
            if (!info) return false;
            const bool ok = on_owner(w.owner_id, [&](std::vector<project::AutomationLane>& lanes, std::string&,
                                                     project::Track* t, project::Bus* b) {
                project::AutomationLane* lane = project::find_lane(lanes, w.param);
                if (!lane) {
                    project::AutomationLane l;
                    l.id = new_ids[i];
                    l.param = w.param;
                    l.name = w.name.empty() ? info->name : w.name;
                    // Outside the pass the lane holds what the control held.
                    const auto v = static_of(t, b, w.param);
                    l.points.push_back({0, v ? *v : info->default_value, "linear"});
                    lanes.push_back(std::move(l));
                    lane = &lanes.back();
                }
                project::write_pass(*lane, w.from, w.to, w.points);
                lane->shown = true;
                return true;
            })(d);
            if (!ok) return false;
        }
        return true;
    });
}

// --- Stems -------------------------------------------------------------------------------

std::vector<project::StemDef> Session::stems() const
{
    if (!data().stems.empty()) return data().stems;
    return project::default_stems(data(), [this](const project::Track& t) {
        const auto* def = instrument_for(t);
        return def ? def->family : std::string();
    });
}

bool Session::set_stems(std::vector<project::StemDef> stems)
{
    std::set<std::string> used;
    for (const auto& st : stems) used.insert(st.id);
    for (auto& st : stems)
        if (st.id.empty()) {
            do st.id = project_.ids().next("stem");
            while (used.count(st.id));
            used.insert(st.id);
        }
    return edit("Edit stems", [&](project::ProjectData& d) {
        d.stems = stems;
        return true;
    });
}

// --- Performance overrides --------------------------------------------------------------

const std::map<notation::EventId, project::NoteOverride>* Session::performance_of(const std::string& track_id) const
{
    const project::Track* t = project_.data().find_track(track_id);
    const project::TrackVersion* v = t ? t->active_version() : nullptr;
    return v ? &v->performance : nullptr;
}

bool Session::edit_performance(const std::string& track_id, const std::vector<notation::EventId>& notes,
                               const std::string& label,
                               const std::function<void(notation::EventId, project::NoteOverride&)>& change)
{
    const project::Track* t = project_.data().find_track(track_id);
    if (!t || !t->active_version() || notes.empty()) return false;
    const auto performable = project::performable_notes(*t);
    std::vector<notation::EventId> targets;
    for (auto id : notes)
        if (std::binary_search(performable.begin(), performable.end(), id) &&
            std::find(targets.begin(), targets.end(), id) == targets.end())
            targets.push_back(id);
    if (targets.empty()) return false;
    if (!project_.edit_coalescing(label, on_track(track_id, [&](project::Track& tr) {
            auto& perf = tr.active_version()->performance;
            for (auto id : targets) {
                project::NoteOverride o;
                if (auto it = perf.find(id); it != perf.end()) o = it->second;
                change(id, o);
                o = project::clamp_override(o);
                if (o.empty())
                    perf.erase(id);
                else
                    perf[id] = o;
            }
            return true;
        })))
        return false;
    document_changed();
    return true;
}

bool Session::clear_performance(const std::string& track_id, const std::vector<notation::EventId>& notes)
{
    const auto* perf = performance_of(track_id);
    if (!perf || perf->empty()) return false;
    bool any = notes.empty();
    for (auto id : notes) any = any || perf->count(id) > 0;
    if (!any) return false;
    if (!project_.edit(notes.empty() ? "Clear performance" : "Reset performance", on_track(track_id, [&](project::Track& t) {
            auto& p = t.active_version()->performance;
            if (notes.empty())
                p.clear();
            else
                for (auto id : notes) p.erase(id);
            return true;
        })))
        return false;
    document_changed();
    return true;
}

bool Session::humanize(const std::string& track_id, const std::vector<notation::EventId>& notes,
                       const project::HumanizeSettings& settings)
{
    const project::Track* t = project_.data().find_track(track_id);
    if (!t || !t->active_version()) return false;
    const auto performable = project::performable_notes(*t);
    std::vector<notation::EventId> targets;
    for (auto id : notes.empty() ? performable : notes)
        if (std::binary_search(performable.begin(), performable.end(), id)) targets.push_back(id);
    if (targets.empty()) return false;
    if (!project_.edit("Humanize", on_track(track_id, [&](project::Track& tr) {
            auto& perf = tr.active_version()->performance;
            for (auto id : targets) {
                project::NoteOverride o;
                if (auto it = perf.find(id); it != perf.end()) o = it->second;
                o = project::humanized(o, id, settings);
                if (o.empty())
                    perf.erase(id);
                else
                    perf[id] = o;
            }
            return true;
        })))
        return false;
    document_changed();
    return true;
}

// --- MIDI -> notation ----------------------------------------------------------------------

NotatePlan Session::plan_note_edit(const std::string& track_id, notation::EventId note, const NoteTarget& target) const
{
    const project::Track* t = project_.data().find_track(track_id);
    if (!t) {
        NotatePlan p;
        p.error = "No such track";
        return p;
    }
    return session::plan_note_edit(*t, note, target);
}

NotatePlan Session::plan_notate_region(const std::string& track_id, const std::string& region_id,
                                       const RegionNotateSettings& settings) const
{
    const project::Track* t = project_.data().find_track(track_id);
    if (!t) {
        NotatePlan p;
        p.error = "No such track";
        return p;
    }
    return session::plan_notate_region(*t, region_id, settings);
}

bool Session::apply_notate(const std::string& track_id, const NotatePlan& plan, std::size_t option,
                           const std::string& label, bool coalesce)
{
    if (!plan.ok() || option >= plan.options.size()) return false;
    const auto& o = plan.options[option];
    auto change = on_track(track_id, [&](project::Track& t) {
            auto* v = t.active_version();
            if (!v || v->score.revision() != plan.base_revision) return false;
            v->score = o.score;
            v->performance = o.performance;
            for (const auto& id : o.removed_regions)
                v->regions.erase(std::remove_if(v->regions.begin(), v->regions.end(),
                                                [&](const project::MidiRegion& r) { return r.id == id; }),
                                 v->regions.end());
            return true;
        });
    if (!(coalesce ? project_.edit_coalescing(label, change) : project_.edit(label, change))) return false;
    document_changed();
    return true;
}

}  // namespace vcc::session
