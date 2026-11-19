#include "vcc/session/Session.h"
#include "vcc/project/ArticulationMap.h"
#include "vcc/session/Package.h"
#include "vcc/session/Relink.h"
#include "vcc/project/Percussion.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>

#include "vcc/core/Files.h"
#include "vcc/cues/Cues.h"
#include "vcc/interchange/Transposition.h"
#include "vcc/picture/Picture.h"
#include "vcc/project/Automation.h"
#include "vcc/project/Performance.h"
#include "vcc/project/Render.h"
#include "vcc/project/Routing.h"
#include "vcc/project/Stems.h"
#include "vcc/theory/Chord.h"

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
    ++changes_;
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
    if (opt.installed.empty()) opt.installed = installed_patches_;
    r = templates::instantiate(fresh, *t, catalogs_.instruments, opt);
    if (!r.ok) return r;
    reset(std::move(fresh), {});
    apply_assigned_sounds(all_track_ids());  // the composer's chosen sounds, wherever they apply
    dirty_ = true;
    notify(Change::File);
    return r;
}

SetupResult Session::create_project(const ProjectSetup& setup)
{
    SetupResult out;
    if (setup.key && !theory::Key::parse(*setup.key)) {
        out.error = "unknown key '" + *setup.key + "'";
        return out;
    }
    if (setup.meter && !setup.meter->is_valid()) {
        out.error = "invalid time signature";
        return out;
    }
    if (setup.tempo && (*setup.tempo < 20 || *setup.tempo > 400)) {
        out.error = "tempo must be 20-400 BPM";
        return out;
    }
    if (const auto tc = Timecode::parse(setup.start_timecode); !tc || !timecode_to_frames(*tc, setup.frame_rate)) {
        out.error = "start timecode '" + setup.start_timecode + "' is not valid at this frame rate";
        return out;
    }
    std::string file;
    if (!setup.folder.empty()) {
        const auto dir = fs::u8path(setup.folder) / fs::u8path(package::safe_name(setup.name));
        file = (dir / fs::u8path(package::safe_name(setup.name) + ".vcc")).generic_u8string();
        std::error_code ec;
        if (fs::exists(fs::u8path(file), ec)) {
            out.error = file + " already exists: choose another name or folder";
            return out;
        }
        fs::create_directories(dir, ec);
        if (ec) {
            out.error = "could not create " + dir.generic_u8string() + ": " + ec.message();
            return out;
        }
    }

    project::Project fresh;
    if (!setup.template_id.empty()) {
        const templates::Template* t = catalogs_.templates->get(setup.template_id);
        if (!t) {
            out.error = "no template '" + setup.template_id + "'";
            return out;
        }
        templates::InstantiateOptions opt;
        opt.mode = templates::InstantiateMode::Create;
        opt.layout = setup.track_layout;
        opt.installed = installed_patches_;
        const auto r = templates::instantiate(fresh, *t, catalogs_.instruments, opt);
        if (!r.ok) {
            out.error = r.error;
            return out;
        }
        out.warnings = r.warnings;
        fresh = project::Project(fresh.data(), fresh.ids().peek());
    }
    fresh.modify_unrecorded([&](project::ProjectData& d) {
        d.name = setup.name.empty() ? d.name : setup.name;
        if (setup.key) d.key = *setup.key;
        if (setup.meter) d.meter = MeterMap(*setup.meter);
        if (setup.tempo) d.tempo = TempoMap(*setup.tempo);
        d.frame_rate = setup.frame_rate;
        d.start_timecode = setup.start_timecode;
        d.sample_rate = setup.sample_rate;
    });
    reset(std::move(fresh), {});
    apply_assigned_sounds(all_track_ids());
    dirty_ = true;
    if (!file.empty()) {
        std::string error;
        if (!save_as(file, &error)) {
            out.error = "created, but not saved: " + error;
            return out;
        }
        out.project_file = file;
    }
    notify(Change::File);
    out.ok = true;
    return out;
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
    if (opt.installed.empty()) opt.installed = installed_patches_;
    r = templates::instantiate(project_, *t, catalogs_.instruments, opt);
    if (r.ok) {
        document_changed();
        apply_assigned_sounds(all_track_ids());  // only tracks with no plug-in of their own
    }
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
    return (fs::u8path(fallback_dir) / "Untitled.vcc.autosave").u8string();
}

bool Session::write_autosave(const std::string& fallback_dir, std::string* error) const
{
    return write_autosave_job(autosave_job(fallback_dir), error);
}

Session::AutosaveJob Session::autosave_job(const std::string& fallback_dir) const
{
    return {project::Project(project_.data(), project_.ids().peek()), autosave_path(fallback_dir)};
}

bool Session::write_autosave_job(const AutosaveJob& job, std::string* error)
{
    return job.copy.save(job.path, error);
}

bool Session::has_newer_autosave(const std::string& path)
{
    std::error_code ec;
    auto a = fs::u8path(path + ".autosave");
    if (!fs::exists(a, ec)) return false;
    return fs::last_write_time(a, ec) > fs::last_write_time(fs::u8path(path), ec);
}

void Session::set_track_locks(std::map<std::string, std::string> locks)
{
    if (locks == project_.track_locks()) return;
    project_.set_track_locks(std::move(locks));
    notify(Change::Document);
}

bool Session::adopt_document(const json::Value& doc, const std::string& label, std::string* error)
{
    auto loaded = project::Project::from_json(doc, error);
    if (!loaded) return false;
    project_.replace_document(label, loaded->data(), loaded->ids().peek());
    document_changed();
    return true;
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
            t->percussion_map = project::default_percussion_map(instrument);
            // The staves the instrument reads (clef, transposition, grand
            // staff), as a template would give it.
            if (auto* v = t->active_version(); v && v->score.total_note_count() == 0)
                v->score.reset_staves(templates::instrument_score(*def, t->name).staves());
            return true;
        });
    }
    apply_assigned_sounds({id});  // "Violins I" comes with the composer's violin sound
    document_changed();
    select_track(id);
    return id;
}

bool Session::is_sketch(const project::Track& track) const
{
    if (track.role == "sketch") return true;
    const auto* f = track.folder_id.empty() ? nullptr : data().find_track(track.folder_id);
    return f && f->kind == project::TrackKind::Folder && f->family == "sketch";
}

std::string Session::add_sketch_track(const std::string& name, const std::string& instrument)
{
    const auto before = project_.history_size();
    const std::string folder_id = project_.ids().next("fld");
    const std::string id = add_track(name, instrument);
    project_.edit("Add sketch track " + name, [&](project::ProjectData& d) {
        // The Sketch area's folder, made at the top if there is none.
        std::string folder;
        for (const auto& t : d.tracks)
            if (t.kind == project::TrackKind::Folder && t.family == "sketch") folder = t.id;
        if (folder.empty()) {
            project::Track f;
            f.id = folder_id;
            f.kind = project::TrackKind::Folder;
            f.name = d.unique_track_name("Sketch");
            f.family = "sketch";
            f.color = templates::family_color("sketch");
            f.include_in_export = false;
            d.tracks.insert(d.tracks.begin(), f);
            folder = folder_id;
        }
        auto it = std::find_if(d.tracks.begin(), d.tracks.end(), [&](const project::Track& t) { return t.id == id; });
        if (it == d.tracks.end()) return false;
        project::Track moved = *it;
        d.tracks.erase(it);
        moved.folder_id = folder;
        moved.role = "sketch";
        moved.output_bus_id = "bus-master";
        // After the area's last track.
        std::size_t at = 0;
        for (std::size_t i = 0; i < d.tracks.size(); ++i)
            if (d.tracks[i].id == folder || d.tracks[i].folder_id == folder) at = i + 1;
        d.tracks.insert(d.tracks.begin() + static_cast<std::ptrdiff_t>(at), moved);
        return true;
    });
    project_.squash_undo(project_.history_size() - before, "Add sketch track " + name);
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
        // A part takes its articulation tracks with it.
        d.tracks.erase(std::remove_if(d.tracks.begin(), d.tracks.end(),
                                      [&](const project::Track& x) { return x.id == id || x.articulation_parent == id; }),
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
    const project::Track* self = data().find_track(id);
    if (!self) return false;
    // A mix group moves every member's fader by the same amount.
    const double delta = gain_db - self->gain_db;
    const auto linked = data().linked_tracks(id, "gain");
    if (!project_.edit_coalescing("Gain " + id, [&](project::ProjectData& d) {
            for (const auto& m : linked)
                if (project::Track* t = d.find_track(m))
                    t->gain_db = m == id ? gain_db : std::clamp(t->gain_db + delta, -96.0, 12.0);
            return true;
        }))
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
    const auto linked = data().linked_tracks(id, "mute");
    return edit(mute ? "Mute" : "Unmute", [&](project::ProjectData& d) {
        bool any = false;
        for (const auto& m : linked)
            if (project::Track* t = d.find_track(m); t && t->mute != mute) {
                t->mute = mute;
                any = true;
            }
        return any;
    });
}

bool Session::set_solo(const std::string& id, bool solo)
{
    const auto linked = data().linked_tracks(id, "solo");
    return edit(solo ? "Solo" : "Unsolo", [&](project::ProjectData& d) {
        bool any = false;
        for (const auto& m : linked)
            if (project::Track* t = d.find_track(m); t && t->solo != solo) {
                t->solo = solo;
                any = true;
            }
        return any;
    });
}

// --- VCAs and mix groups (Main Spec §12) ---------------------------------------------------

std::string Session::add_vca(const std::string& name, const std::vector<std::string>& members)
{
    std::string id;
    edit("Add VCA " + name, [&](project::ProjectData& d) {
        project::Vca v;
        v.id = project_.ids().next("vca");
        v.name = name.empty() ? "VCA " + std::to_string(d.vcas.size() + 1) : name;
        for (const auto& m : members)
            if (d.find_track(m)) v.members.push_back(m);
        id = v.id;
        d.vcas.push_back(std::move(v));
        return true;
    });
    return id;
}

bool Session::update_vca(const std::string& vca_id, const std::string& label, const std::function<void(project::Vca&)>& change)
{
    return edit(label, [&](project::ProjectData& d) {
        for (auto& v : d.vcas)
            if (v.id == vca_id) {
                change(v);
                v.gain_db = std::clamp(v.gain_db, -96.0, 12.0);
                return true;
            }
        return false;
    });
}

bool Session::set_vca_gain(const std::string& vca_id, double gain_db)
{
    gain_db = std::clamp(gain_db, -96.0, 12.0);
    if (!project_.edit_coalescing("VCA " + vca_id, [&](project::ProjectData& d) {
            for (auto& v : d.vcas)
                if (v.id == vca_id) {
                    v.gain_db = gain_db;
                    return true;
                }
            return false;
        }))
        return false;
    document_changed();
    return true;
}

bool Session::remove_vca(const std::string& vca_id)
{
    return edit("Remove VCA", [&](project::ProjectData& d) {
        const auto before = d.vcas.size();
        d.vcas.erase(std::remove_if(d.vcas.begin(), d.vcas.end(), [&](const project::Vca& v) { return v.id == vca_id; }),
                     d.vcas.end());
        return d.vcas.size() != before;
    });
}

std::string Session::add_mix_group(const std::string& name, const std::vector<std::string>& members)
{
    std::string id;
    edit("Add mix group " + name, [&](project::ProjectData& d) {
        project::MixGroup g;
        g.id = project_.ids().next("grp");
        g.name = name.empty() ? "Group " + std::to_string(d.mix_groups.size() + 1) : name;
        for (const auto& m : members)
            if (d.find_track(m)) g.members.push_back(m);
        id = g.id;
        d.mix_groups.push_back(std::move(g));
        return true;
    });
    return id;
}

bool Session::update_mix_group(const std::string& group_id, const std::string& label,
                               const std::function<void(project::MixGroup&)>& change)
{
    return edit(label, [&](project::ProjectData& d) {
        for (auto& g : d.mix_groups)
            if (g.id == group_id) {
                change(g);
                return true;
            }
        return false;
    });
}

bool Session::remove_mix_group(const std::string& group_id)
{
    return edit("Remove mix group", [&](project::ProjectData& d) {
        const auto before = d.mix_groups.size();
        d.mix_groups.erase(std::remove_if(d.mix_groups.begin(), d.mix_groups.end(),
                                          [&](const project::MixGroup& g) { return g.id == group_id; }),
                           d.mix_groups.end());
        return d.mix_groups.size() != before;
    });
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

bool Session::set_playback(const std::string& id, const std::string& playback)
{
    if (playback != "midi" && playback != "audio" && playback != "both") return false;
    const std::string label = playback == "midi" ? "Play MIDI" : playback == "audio" ? "Play audio lane" : "Play MIDI and audio";
    return edit(label, on_track(id, [&](project::Track& t) {
        if (t.kind == project::TrackKind::Audio || t.kind == project::TrackKind::Aux || t.playback == playback) return false;
        t.playback = playback;
        return true;
    }));
}

bool Session::set_active_version(const std::string& id, const std::string& version)
{
    if (!project_.set_active_version(id, version)) return false;
    document_changed();
    return true;
}

std::optional<std::string> Session::new_version(const std::string& id, bool copy_current)
{
    const auto* t = data().find_track(id);
    if (!t || static_cast<int>(t->versions.size()) >= kMaxVersions) return std::nullopt;
    const auto before = project_.history_size();
    auto v = project_.duplicate_version(id);
    if (!v) return std::nullopt;
    if (!copy_current) {
        project_.modify_unrecorded([&](project::ProjectData& d) {
            auto* tr = d.find_track(id);
            auto* ver = tr ? tr->active_version() : nullptr;
            if (!ver) return;
            // The same staves, clefs, key and meter; no music.
            auto staves = ver->score.staves();
            notation::Score fresh = ver->score;
            fresh.clear();
            fresh.reset_staves(staves);
            for (const auto& c : ver->score.time_signature_changes()) fresh.set_time_signature(c.tick, c.time_signature);
            for (const auto& c : ver->score.key_signature_changes()) fresh.set_key_signature(c.tick, c.key);
            ver->score = std::move(fresh);
            ver->regions.clear();
            ver->performance.clear();
            ver->phrases.clear();
            ver->audio_clips.clear();
        });
    }
    project_.squash_undo(project_.history_size() - before, copy_current ? "New version (copy)" : "New empty version");
    document_changed();
    return v;
}

bool Session::set_score_info(const project::ScoreInfo& info)
{
    return edit("Score info", [&](project::ProjectData& d) {
        d.score_info = info;
        d.score_info.title_scale = std::clamp(info.title_scale, 0.5, 3.0);
        return true;
    });
}

project::ScoreInfo Session::score_info() const
{
    auto si = data().score_info;
    if (si.title.empty()) si.title = data().production.title.empty() ? data().name : data().production.title;
    if (si.composer.empty()) si.composer = data().production.composer;
    return si;
}

bool Session::clear_track(const std::string& id)
{
    const auto* t = data().find_track(id);
    if (!t || !t->active_version()) return false;
    return edit("Clear " + t->name, on_track(id, [&](project::Track& tr) {
        auto* ver = tr.active_version();
        if (!ver) return false;
        const bool had = ver->score.total_note_count() > 0 || !ver->regions.empty() || !ver->performance.empty() ||
                         !ver->score.dynamics().empty() || !ver->score.hairpins().empty() || !ver->score.texts().empty();
        if (!had) return false;
        auto staves = ver->score.staves();
        notation::Score fresh = ver->score;
        fresh.clear();
        fresh.reset_staves(staves);
        for (const auto& c : ver->score.time_signature_changes()) fresh.set_time_signature(c.tick, c.time_signature);
        for (const auto& c : ver->score.key_signature_changes()) fresh.set_key_signature(c.tick, c.key);
        ver->score = std::move(fresh);
        ver->regions.clear();
        ver->performance.clear();
        return true;
    }));
}

bool Session::delete_version(const std::string& id, const std::string& version_id)
{
    const auto* t = data().find_track(id);
    if (!t || t->versions.size() < 2) return false;
    return edit("Delete version", on_track(id, [&](project::Track& tr) {
        auto it = std::find_if(tr.versions.begin(), tr.versions.end(), [&](const project::TrackVersion& v) { return v.id == version_id; });
        if (it == tr.versions.end() || it->approved) return false;
        const auto index = static_cast<std::size_t>(it - tr.versions.begin());
        const bool was_current = tr.active_version_id == version_id;
        tr.versions.erase(it);
        if (was_current) tr.active_version_id = tr.versions[index == 0 ? 0 : index - 1].id;
        return true;
    }));
}

std::optional<std::string> Session::duplicate_version(const std::string& id)
{
    if (const auto* t = data().find_track(id); t && static_cast<int>(t->versions.size()) >= kMaxVersions) return std::nullopt;
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

bool Session::set_percussion_map(const std::string& id, const std::string& map)
{
    if (!map.empty() && !project::percussion_map_named(map)) return false;
    return edit(map.empty() ? "No percussion map" : "Percussion map", on_track(id, [&](project::Track& t) {
        if (t.percussion_map == map) return false;
        t.percussion_map = map;
        return true;
    }));
}

bool Session::set_hairpin_expression(const std::string& id, std::size_t staff, Ticks start_tick, int start_value,
                                     int end_value)
{
    start_value = std::clamp(start_value, 0, 127);
    end_value = std::clamp(end_value, 0, 127);
    if (!project_.edit_coalescing("Swell " + std::to_string(staff) + ":" + std::to_string(start_tick),
                                  [&](project::ProjectData& d) {
                                      for (auto& t : d.tracks) {
                                          if (t.id != id) continue;
                                          auto* v = t.active_version();
                                          if (v == nullptr) return false;
                                          for (const auto& h : v->score.hairpins()) {
                                              if (h.staff != staff || h.start_tick != start_tick) continue;
                                              auto changed = h;
                                              changed.expression_start = static_cast<std::int16_t>(start_value);
                                              changed.expression_end = static_cast<std::int16_t>(end_value);
                                              if (notation::hairpin_expression_start(h) == start_value &&
                                                  notation::hairpin_expression_end(h) == end_value)
                                                  return false;
                                              v->score.remove_hairpin(h.start_tick, h.voice, h.staff);
                                              v->score.add_hairpin(changed);
                                              return true;
                                          }
                                          return false;
                                      }
                                      return false;
                                  }))
        return false;
    document_changed();
    return true;
}

bool Session::set_accent_strength(const std::string& id, int strength)
{
    strength = std::clamp(strength, 1, 100);
    if (!project_.edit_coalescing("Accent strength", [&](project::ProjectData& d) {
            for (auto& t : d.tracks)
                if (t.id == id) {
                    if (t.accent_strength == strength) return false;
                    t.accent_strength = strength;
                    return true;
                }
            return false;
        }))
        return false;
    document_changed();
    return true;
}

bool Session::set_articulation_map(const std::string& id, const std::optional<notation::ExpressionMap>& map,
                                   const std::string& builtin)
{
    if (map && !map->is_valid()) return false;
    return edit("Articulation map", on_track(id, [&](project::Track& t) {
        t.articulation_map = map;
        t.expression_map = map ? map->name : builtin;
        return true;
    }));
}

std::vector<std::string> Session::all_track_ids() const
{
    std::vector<std::string> out;
    for (const auto& t : data().tracks) out.push_back(t.id);
    return out;
}

const templates::InstalledPatch* Session::assigned_sound(const project::Track& track) const
{
    auto find = [&](const std::string& art) -> const templates::InstalledPatch* {
        for (const auto& a : sound_assignments_)
            if (a.instrument_id == track.instrument_standard_id && a.articulation == art && !a.patch.plugin.empty()) return &a.patch;
        return nullptr;
    };
    if (!track.articulation_group.empty() && !track.articulation_parent.empty()) {
        if (const auto* p = find(track.articulation_group)) return p;
        // The natural track: the instrument's everyday sound.
        if (track.articulation_group != "natural") return nullptr;
    }
    for (const char* art : {"", "natural", "legato", "sustain"})
        if (const auto* p = find(art)) return p;
    return nullptr;
}

int Session::apply_assigned_sounds(const std::vector<std::string>& track_ids)
{
    if (sound_assignments_.empty()) return 0;
    int n = 0;
    project_.modify_unrecorded([&](project::ProjectData& d) {
        for (const auto& id : track_ids) {
            project::Track* t = d.find_track(id);
            if (!t || t->kind != project::TrackKind::Instrument || !t->instrument_plugin.empty()) continue;
            const auto* p = assigned_sound(*t);
            if (!p) continue;
            t->instrument_plugin = p->plugin;
            t->assignment.status = project::ResolutionStatus::Exact;
            t->assignment.patch_display = p->display;
            t->assignment.message = "Your assigned sound for this instrument and articulation: " + p->display;
            if (!p->expression_map.empty() && p->expression_map != "default") t->expression_map = p->expression_map;
            if (p->articulation_map) t->articulation_map = *p->articulation_map;
            ++n;
        }
    });
    if (n > 0) document_changed();
    return n;
}

bool Session::assign_sound(const std::string& id, const project::PluginRef& plugin, const project::InstrumentAssignment& how,
                           const std::string& expression_map)
{
    return edit("Assign " + (how.patch_display.empty() ? plugin.name : how.patch_display), on_track(id, [&](project::Track& t) {
        t.instrument_plugin = plugin;
        t.assignment = how;
        if (!expression_map.empty()) t.expression_map = expression_map;
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

bool Session::set_loudness_target(double lufs)
{
    lufs = std::clamp(lufs, -36.0, -6.0);
    char label[48];
    std::snprintf(label, sizeof label, "Loudness target %.0f LUFS", lufs);
    return edit(label, [&](project::ProjectData& d) {
        if (std::abs(d.loudness_target - lufs) < 1e-9) return false;
        d.loudness_target = lufs;
        return true;
    });
}

void Session::set_key_zoom(double zoom)
{
    zoom = std::clamp(zoom, 1.0, 3.0);
    bool changed = false;
    project_.modify_unrecorded([&](project::ProjectData& d) {
        if (std::abs(d.key_zoom - zoom) > 1e-9) {
            d.key_zoom = zoom;
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

int Session::apply_overrides_to_score(const std::string& track_id, const std::vector<notation::EventId>& notes, int* left)
{
    int written = 0, failed = 0;
    for (const auto id : notes) {
        const project::Track* t = project_.data().find_track(track_id);
        if (!t) break;
        const auto plan = session::plan_apply_override(*t, id);
        if (plan.ok() && apply_notate(track_id, plan, 0, "Apply to Score", true))
            ++written;
        else if (const auto* v = t->active_version(); v && v->performance.count(id))
            ++failed;
    }
    if (left) *left = failed;
    return written;
}

bool Session::set_producer_edits(const std::string& policy)
{
    if (policy != "ask" && policy != "apply" && policy != "keep") return false;
    return edit(policy == "ask" ? "Ask before Producer edits change the score"
                : policy == "apply" ? "Producer edits always apply to the score"
                                    : "Producer edits always stay overrides",
                [&](project::ProjectData& d) {
                    if (d.producer_edits == policy) return false;
                    d.producer_edits = policy;
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

bool Session::commit_score_with_chord(const std::string& id, const notation::Score& score, const std::string& label,
                                      Ticks tick, const std::string& symbol)
{
    const bool chord = !symbol.empty() && tick >= 0 && theory::parse_chord_symbol(symbol);
    return edit(label.empty() ? "Edit notation" : label, [&](project::ProjectData& d) {
        project::Track* t = d.find_track(id);
        project::TrackVersion* v = t ? t->active_version() : nullptr;
        if (!v) return false;
        v->score = score;
        if (chord) {
            auto& marks = d.chords;
            auto it = std::lower_bound(marks.begin(), marks.end(), tick,
                                       [](const project::ChordMark& m, Ticks at) { return m.tick < at; });
            if (it != marks.end() && it->tick == tick) it->text = symbol;
            else marks.insert(it, project::ChordMark{tick, symbol});
        }
        return true;
    });
}

std::map<std::string, std::set<std::string>> Session::region_ids() const
{
    std::map<std::string, std::set<std::string>> out;
    for (const auto& t : data().tracks)
        if (const auto* v = t.active_version())
            for (const auto& r : v->regions) out[t.id].insert(r.id);
    return out;
}

int Session::lock_new_regions_to_notation(const std::map<std::string, std::set<std::string>>& before, const std::string& label)
{
    std::vector<std::pair<std::string, std::string>> fresh;  // track, region
    for (const auto& t : data().tracks) {
        const auto* v = t.active_version();
        if (!v || t.kind != project::TrackKind::Instrument || v->score.staff_count() == 0) continue;
        const auto it = before.find(t.id);
        for (const auto& r : v->regions)
            if (r.lane == 0 && !r.notes.empty() && (it == before.end() || !it->second.count(r.id))) fresh.emplace_back(t.id, r.id);
    }
    if (fresh.empty()) return 0;
    const auto steps_before = project_.history_size();
    int n = 0;
    for (const auto& [track, region] : fresh) {
        // Its controllers (sustain pedal, modulation, pitch bend) stay as a
        // region of their own: the notes go to the staff, the gestures keep playing.
        project::MidiRegion gestures;
        if (const auto* t = data().find_track(track))
            if (const auto* v = t->active_version())
                for (const auto& r : v->regions)
                    if (r.id == region) {
                        gestures = r;
                        gestures.notes.clear();
                    }
        const auto plan = plan_notate_region(track, region);
        if (!plan.error.empty() || plan.options.empty()) continue;
        if (!apply_notate(track, plan, 0, label)) continue;
        ++n;
        if (!gestures.controllers.empty())
            edit(label, on_track(track, [&](project::Track& t) {
                if (auto* v = t.active_version()) v->regions.push_back(gestures);
                return true;
            }));
    }
    // One undo step with what brought the MIDI in.
    if (project_.history_size() > steps_before) project_.squash_undo(project_.history_size() - steps_before + 1, label);
    return n;
}

project::InsertResult Session::insert_notes(const std::string& id, Ticks start, Ticks end, std::vector<NoteEvent> notes,
                                            project::CollisionPolicy policy, const std::string& source)
{
    // On a track with a staff the notes are written there, as every send is:
    // the MIDI always matches the notation. MIDI-only tracks get a region.
    if (const project::Track* t = data().find_track(id);
        t && t->kind == project::TrackKind::Instrument && t->active_version() && t->active_version()->score.staff_count() > 0) {
        const auto events = written_events(*t, notes, end);
        if (!events.empty()) return write_events(id, events, {}, policy, "Insert " + source + " on " + t->name);
    }
    auto r = project_.insert_notes(id, start, end, std::move(notes), policy, source);
    if (r.ok) document_changed();
    return r;
}

project::InsertResult Session::send_progression(const std::string& id, const theory::ProgressionRender& render,
                                                const std::string& progression_id, project::CollisionPolicy policy)
{
    const std::string source = "chord_explorer:" + progression_id;
    // The progression's symbols over those bars.
    std::vector<project::ChordMark> marks;
    for (const auto& rc : render.chords)
        if (marks.empty() || marks.back().tick < rc.start) marks.push_back({rc.start, rc.chord.name()});
    // Notation first, as the ostinatos are: the chords written on the track's
    // staff (a piano's left hand on the bass staff), the symbols over them, one
    // undo step. Only a track with no staff - MIDI-only - gets a MIDI region.
    if (const project::Track* track = data().find_track(id);
        track && track->kind != project::TrackKind::MidiOnly && track->active_version() &&
        track->active_version()->score.staff_count() > 0) {
        const auto events = written_events(*track, render.notes, render.end);
        if (!events.empty()) return write_events(id, events, marks, policy, "Insert " + source + " on " + track->name);
    }
    auto r = insert_notes(id, render.start, render.end, render.notes, policy, source);
    if (!r.ok) return r;
    const auto* t = data().find_track(id);
    const std::string label = "Insert " + source + " on " + (t ? t->name : std::string());
    if (project_.edit_coalescing(label, [&](project::ProjectData& d) {
            auto& v = d.chords;
            v.erase(std::remove_if(v.begin(), v.end(),
                                   [&](const project::ChordMark& m) { return m.tick >= render.start && m.tick < render.end; }),
                    v.end());
            v.insert(v.end(), marks.begin(), marks.end());
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
            return true;
        }))
        document_changed();
    return r;
}

// --- Chord symbols ---------------------------------------------------------------------

bool Session::set_chord_symbol(Ticks tick, const std::string& text)
{
    if (tick < 0) return false;
    std::string clean = text;
    while (!clean.empty() && std::isspace(static_cast<unsigned char>(clean.back()))) clean.pop_back();
    while (!clean.empty() && std::isspace(static_cast<unsigned char>(clean.front()))) clean.erase(clean.begin());
    if (clean.empty()) return remove_chord_symbol(tick);
    if (!theory::parse_chord_symbol(clean)) return false;
    return edit("Chord symbol " + clean, [&](project::ProjectData& d) {
        auto& v = d.chords;
        auto it = std::lower_bound(v.begin(), v.end(), tick, [](const project::ChordMark& m, Ticks t) { return m.tick < t; });
        if (it != v.end() && it->tick == tick) {
            if (it->text == clean) return false;
            it->text = clean;
        } else {
            v.insert(it, project::ChordMark{tick, clean});
        }
        return true;
    });
}

bool Session::remove_chord_symbol(Ticks tick)
{
    return edit("Remove chord symbol", [&](project::ProjectData& d) {
        auto& v = d.chords;
        const auto n = v.size();
        v.erase(std::remove_if(v.begin(), v.end(), [&](const project::ChordMark& m) { return m.tick == tick; }), v.end());
        return v.size() != n;
    });
}

bool Session::replace_chord_symbols(Ticks from, Ticks to, const std::vector<project::ChordMark>& chords,
                                    const std::string& label)
{
    if (to <= from) return false;
    std::vector<project::ChordMark> in;
    for (const auto& m : chords)
        if (m.tick >= from && m.tick < to && theory::parse_chord_symbol(m.text)) in.push_back(m);
    std::sort(in.begin(), in.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
    in.erase(std::unique(in.begin(), in.end(), [](const auto& a, const auto& b) { return a.tick == b.tick; }), in.end());
    return edit(label, [&](project::ProjectData& d) {
        auto& v = d.chords;
        std::vector<project::ChordMark> out;
        for (const auto& m : v)
            if (m.tick < from || m.tick >= to) out.push_back(m);
        out.insert(out.end(), in.begin(), in.end());
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
        const bool same = out.size() == v.size() &&
                          std::equal(out.begin(), out.end(), v.begin(), [](const auto& a, const auto& b) {
                              return a.tick == b.tick && a.text == b.text;
                          });
        if (same) return false;
        v = std::move(out);
        return true;
    });
}

const project::ChordMark* Session::chord_symbol_at(Ticks tick) const
{
    const project::ChordMark* best = nullptr;
    for (const auto& m : data().chords) {
        if (m.tick > tick) break;
        best = &m;
    }
    return best;
}

std::vector<Session::WrittenEvent> Session::written_events(const project::Track& track, const std::vector<NoteEvent>& notes,
                                                         Ticks end) const
{
    std::vector<WrittenEvent> events;
    const project::TrackVersion* ver = track.active_version();
    if (!ver || ver->score.staff_count() == 0) return events;
    const auto& v_notes = notes;
    const auto& score = ver->score;
    const auto& staff = score.staff(0);
    const project::PercussionMap* drums = track.percussion_map.empty() ? nullptr : project::percussion_map_named(track.percussion_map);
    const bool percussion = drums != nullptr || staff.clef == notation::Clef::Percussion;
    // Written as the pattern reads, not as briefly as it sounds: each note
    // takes its grid step (the smallest distance between onsets), or up to
    // the next note when that comes sooner - sixteenths, not 32nds and rests.
    std::vector<Ticks> onsets;
    for (const auto& n : v_notes) onsets.push_back(n.start);
    std::sort(onsets.begin(), onsets.end());
    onsets.erase(std::unique(onsets.begin(), onsets.end()), onsets.end());
    Ticks step = 0;
    for (std::size_t i = 1; i < onsets.size(); ++i) {
        const Ticks gap = onsets[i] - onsets[i - 1];
        if (gap > 0 && (step == 0 || gap < step)) step = gap;
    }
    auto written_length = [&](const NoteEvent& n) {
        const auto next = std::upper_bound(onsets.begin(), onsets.end(), n.start);
        Ticks len = std::max(n.duration, step);
        if (next != onsets.end()) len = std::min(len, *next - n.start);
        len = std::min(len, end - n.start);
        return std::max<Ticks>(len, n.duration > 0 ? std::min(n.duration, end - n.start) : 0);
    };
    // Notes sounding together for the same time are one chord on the page.
    std::map<std::pair<Ticks, Ticks>, std::vector<int>> groups;
    for (const auto& n : v_notes) {
        int written = n.pitch;
        if (percussion) {
            // On a drum staff each sound has its line (and head) in the kit's map.
            if (drums)
                if (const auto* sound = drums->by_midi(n.pitch)) written = sound->written;
        } else {
            written = n.pitch - staff.total_transposition();
        }
        if (written < 0 || written > 127 || n.duration <= 0) continue;
        groups[{n.start, written_length(n)}].push_back(written);
    }
    for (auto& [span, pitches] : groups) {
        std::sort(pitches.begin(), pitches.end());
        pitches.erase(std::unique(pitches.begin(), pitches.end()), pitches.end());
        WrittenEvent e;
        e.start = span.first;
        e.duration = span.second;
        const auto key = percussion ? notation::KeySignature{}
                                    : interchange::written_key(score.key_signature_at(span.first), staff.transposition_semitones);
        for (const int midi : pitches) e.pitches.push_back(notation::spell_midi_note(midi, key));
        events.push_back(std::move(e));
    }
    return events;
}

project::InsertResult Session::send_ostinato(const std::string& id, const ostinato::Variant& v,
                                             const std::string& preset_id, project::CollisionPolicy policy)
{
    // Notation first: the figure is written on the track's staff (it plays
    // from there, as everything written does). Only a track with no staff -
    // MIDI-only - gets it as a MIDI region.
    const project::Track* t = data().find_track(id);
    const project::TrackVersion* ver = t ? t->active_version() : nullptr;
    if (!t || !ver || ver->score.staff_count() == 0 || t->kind == project::TrackKind::MidiOnly)
        return insert_notes(id, v.start, v.end, v.notes, policy, "ostinato:" + preset_id + "/" + v.id);

    const auto events = written_events(*t, v.notes, v.end);
    if (events.empty()) return insert_notes(id, v.start, v.end, v.notes, policy, "ostinato:" + preset_id + "/" + v.id);
    return write_events(id, events, {}, policy, "Ostinato " + v.id + " on " + t->name);
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

bool Session::set_steady_tempo(Ticks from, Ticks until, double bpm)
{
    if (!(bpm >= 20.0 && bpm <= 400.0) || from < 0) return false;
    const TempoMap& old = data().tempo;
    const bool bounded = until > from;
    const double after = old.bpm_at(until);
    TempoMap next(old.bpm_at(0));
    for (const auto& c : old.changes()) {
        if (c.tick >= from && (!bounded || c.tick <= until)) continue;
        next.set(c.tick, c.bpm);
    }
    next.set(from, bpm);
    if (bounded && std::abs(after - bpm) > 1e-9) next.set(until, after);
    std::ostringstream label;
    label.precision(2);
    label << std::fixed << "Tempo " << bpm;
    return edit(label.str(), [&](project::ProjectData& d) {
        d.tempo = next;
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

std::vector<const project::Track*> Session::composer_tracks() const
{
    std::vector<const project::Track*> out;
    for (const auto* t : music_tracks())
        if (!t->articulations_expanded) out.push_back(t);
    return out;
}

std::vector<const project::Track*> Session::staff_tracks() const
{
    std::vector<const project::Track*> out;
    for (const auto* t : music_tracks())
        if (t->articulation_parent.empty()) out.push_back(t);
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

// --- Routing -------------------------------------------------------------------------------

namespace {
/// The sends and inserts of a track or a bus, for edits that treat both alike.
template <typename F>
bool on_source(project::ProjectData& d, const std::string& id, F&& f)
{
    if (auto* t = d.find_track(id)) return f(t->sends, t->inserts);
    if (auto* b = d.find_bus(id)) return f(b->sends, b->inserts);
    return false;
}
}  // namespace

bool Session::route_output(const std::string& source_id, const std::string& bus_id, std::string* why)
{
    const auto& d = data();
    auto no = [&](const std::string& reason) {
        if (why) *why = reason;
        return false;
    };
    const auto* to = d.find_bus(bus_id);
    if (!to) return no("there is no such bus");
    if (source_id == "bus-master") return no("the master goes to the hardware output");
    std::string loop;
    if (project::would_loop(d, source_id, bus_id, &loop)) return no(loop);
    if (d.find_track(source_id)) return set_output_bus(source_id, bus_id) || no("it already goes there");
    if (!d.find_bus(source_id)) return no("there is no such track or bus");
    return edit("Route output", [&](project::ProjectData& pd) {
        auto* b = pd.find_bus(source_id);
        if (!b || b->output == bus_id) return false;
        b->output = bus_id;
        return true;
    }) || no("it already goes there");
}

bool Session::add_send(const std::string& source_id, const std::string& bus_id, bool pre_fader, double level_db, std::string* why)
{
    const auto& d = data();
    auto no = [&](const std::string& reason) {
        if (why) *why = reason;
        return false;
    };
    if (!d.find_bus(bus_id) || bus_id == "bus-master") return no("sends go to an FX or group bus");
    if (source_id == "bus-master") return no("the master has no sends");
    std::string loop;
    if (project::would_loop(d, source_id, bus_id, &loop)) return no(loop);
    bool exists = false;
    if (const auto* t = d.find_track(source_id))
        exists = std::any_of(t->sends.begin(), t->sends.end(), [&](const project::Send& s) { return s.bus_id == bus_id; });
    else if (const auto* b = d.find_bus(source_id))
        exists = std::any_of(b->sends.begin(), b->sends.end(), [&](const project::Send& s) { return s.bus_id == bus_id; });
    else
        return no("there is no such track or bus");
    if (exists) return no("it already sends there");
    return edit("Add send", [&](project::ProjectData& pd) {
        return on_source(pd, source_id, [&](std::vector<project::Send>& sends, auto&) {
            project::Send s(bus_id, std::clamp(level_db, -96.0, 12.0));
            s.pre_fader = pre_fader;
            sends.push_back(s);
            return true;
        });
    });
}

bool Session::remove_send(const std::string& source_id, int index)
{
    return edit("Remove send", [&](project::ProjectData& pd) {
        return on_source(pd, source_id, [&](std::vector<project::Send>& sends, auto&) {
            if (index < 0 || index >= static_cast<int>(sends.size())) return false;
            sends.erase(sends.begin() + index);
            return true;
        });
    });
}

bool Session::update_send(const std::string& source_id, int index, const std::string& label,
                          const std::function<void(project::Send&)>& change)
{
    if (!project_.edit_coalescing(label, [&](project::ProjectData& pd) {
            return on_source(pd, source_id, [&](std::vector<project::Send>& sends, auto&) {
                if (index < 0 || index >= static_cast<int>(sends.size())) return false;
                const std::string bus = sends[static_cast<std::size_t>(index)].bus_id;
                change(sends[static_cast<std::size_t>(index)]);
                auto& s = sends[static_cast<std::size_t>(index)];
                s.bus_id = bus;  // where it goes is route_output's / add_send's to change
                s.level_db = std::clamp(s.level_db, -96.0, 12.0);
                s.pan = std::clamp(s.pan, -1.0, 1.0);
                return true;
            });
        }))
        return false;
    document_changed();
    return true;
}

bool Session::move_insert(const std::string& owner_id, int from, int to)
{
    return edit("Move insert", [&](project::ProjectData& pd) {
        return on_source(pd, owner_id, [&](auto&, std::vector<project::InsertSlot>& inserts) {
            const int n = static_cast<int>(inserts.size());
            if (from < 0 || from >= n || to < 0 || to >= n || from == to) return false;
            auto slot = inserts[static_cast<std::size_t>(from)];
            inserts.erase(inserts.begin() + from);
            inserts.insert(inserts.begin() + to, slot);
            return true;
        });
    });
}

// --- Track Trans ---------------------------------------------------------------------------

bool Session::can_transpose(const project::Track& t, std::string* why) const
{
    auto no = [&](const char* reason) {
        if (why) *why = reason;
        return false;
    };
    if (t.kind != project::TrackKind::Instrument && t.kind != project::TrackKind::MidiOnly)
        return no("Only instrument and MIDI tracks transpose; audio is never repitched");
    if (t.midi_channel == 9) return no("The drum channel plays drum sounds, not pitches");
    if (const auto* def = instrument_for(t); def && def->unpitched) return no("Unpitched percussion has no pitch to transpose");
    return true;
}

bool Session::set_track_transpose(const std::string& track_id, int semitones)
{
    const auto* t = data().find_track(track_id);
    if (!t || !can_transpose(*t)) return false;
    semitones = std::clamp(semitones, -48, 48);
    if (!project_.edit_coalescing("Transpose " + t->name, on_track(track_id, [&](project::Track& tr) {
            if (tr.transpose == semitones) return false;
            tr.transpose = semitones;
            return true;
        })))
        return false;
    document_changed();
    return true;
}

bool Session::set_tracks_transpose(const std::vector<std::string>& track_ids, int semitones)
{
    semitones = std::clamp(semitones, -48, 48);
    return edit("Transpose tracks", [&](project::ProjectData& d) {
        bool any = false;
        for (const auto& id : track_ids) {
            auto* t = d.find_track(id);
            if (!t || !can_transpose(*t) || t->transpose == semitones) continue;
            t->transpose = semitones;
            any = true;
        }
        return any;
    });
}

bool Session::set_transpose_mode(const std::string& track_id, const std::string& mode)
{
    if (mode != "notation" && mode != "playback") return false;
    return edit("Transpose mode", on_track(track_id, [&](project::Track& t) {
        if (t.transpose_mode == mode) return false;
        t.transpose_mode = mode;
        return true;
    }));
}

bool Session::set_transpose_bypass(const std::string& track_id, bool bypass)
{
    return edit(bypass ? "Bypass transpose" : "Transpose on", on_track(track_id, [&](project::Track& t) {
        if (t.transpose_bypass == bypass) return false;
        t.transpose_bypass = bypass;
        return true;
    }));
}

bool Session::commit_track_transpose(const std::string& track_id)
{
    const auto* t0 = data().find_track(track_id);
    if (!t0 || t0->transpose == 0) return false;
    return edit("Apply transpose to notes", on_track(track_id, [&](project::Track& t) {
        const int by = t.transpose;
        for (auto& v : t.versions) {
            v.score = transposed_score(v.score, by);
            for (auto& r : v.regions)
                for (auto& n : r.notes) n.pitch = std::clamp(n.pitch + by, 0, 127);
        }
        t.transpose = 0;
        return true;
    }));
}

notation::Score Session::transposed_score(const notation::Score& score, int semitones)
{
    notation::Score out = score;
    if (semitones == 0) return out;
    for (std::size_t s = 0; s < score.staff_count(); ++s) {
        const auto& staff = score.staff(s);
        if (staff.clef == notation::Clef::Percussion) continue;
        for (const auto& n : score.notes(s)) {
            if (n.is_rest) continue;
            const int midi = notation::midi_note_number(n.pitch) + semitones;
            if (midi < 0 || midi > 127) continue;
            // Respelled as the written key at that point spells it.
            const auto key = interchange::written_key(score.key_signature_at(n.start_tick), staff.transposition_semitones);
            notation::ScoreNote moved = n;
            moved.pitch = notation::spell_midi_note(midi, key);
            out.modify_note(n.id, moved);
        }
    }
    return out;
}

std::string Session::interval_name(int semitones)
{
    if (semitones == 0) return "no transposition";
    static const char* names[] = {"unison", "minor 2nd", "major 2nd", "minor 3rd", "major 3rd", "perfect 4th", "tritone",
                                  "perfect 5th", "minor 6th", "major 6th", "minor 7th", "major 7th"};
    const int a = std::abs(semitones);
    std::string name;
    if (a % 12 == 0) name = a == 12 ? "octave" : std::to_string(a / 12) + " octaves";
    else {
        name = names[a % 12];
        if (a > 12) name += " + " + std::string(a / 12 == 1 ? "an octave" : std::to_string(a / 12) + " octaves");
    }
    return name + (semitones > 0 ? " up" : " down");
}

// --- Instrument range guardrails -------------------------------------------------------

const char* Session::range_policy_name(RangePolicy policy) noexcept
{
    switch (policy) {
        case RangePolicy::Strict: return "strict";
        case RangePolicy::Warn: return "warn";
        case RangePolicy::Extended: return "extended";
        case RangePolicy::Off: return "off";
    }
    return "off";
}

Session::RangeLimits Session::range_limits(const project::Track& track) const
{
    RangeLimits r;
    const auto* def = instrument_for(track);
    const bool pitched = track.kind == project::TrackKind::Instrument || track.kind == project::TrackKind::MidiOnly;
    if (def) {
        r.playable_low = def->range_low;
        r.playable_high = def->range_high;
        r.comfortable_low = def->comfortable_low;
        r.comfortable_high = def->comfortable_high;
        r.has_extended = def->has_extended;
        r.instrument = def->name;
    }
    RangePolicy fallback = RangePolicy::Off;
    if (def && pitched && !def->unpitched && def->family != "synth" &&
        track.midi_channel != 9)
        fallback = RangePolicy::Strict;
    r.policy = fallback;
    if (track.range_policy == "strict") r.policy = RangePolicy::Strict;
    else if (track.range_policy == "warn") r.policy = RangePolicy::Warn;
    else if (track.range_policy == "extended") r.policy = RangePolicy::Extended;
    else if (track.range_policy == "off") r.policy = RangePolicy::Off;
    r.policy_is_default = track.range_policy.empty();
    if (!def && r.policy != RangePolicy::Off) r.policy = RangePolicy::Off;  // nothing to check against
    r.low = r.playable_low;
    r.high = r.playable_high;
    if (r.policy == RangePolicy::Extended && def) {
        r.low = def->extended_low;
        r.high = def->extended_high;
    }
    return r;
}

bool Session::set_range_policy(const std::string& track_id, const std::string& policy)
{
    if (!policy.empty() && policy != "strict" && policy != "warn" && policy != "extended" && policy != "off") return false;
    return edit("Range policy", on_track(track_id, [&](project::Track& t) {
        if (t.range_policy == policy) return false;
        t.range_policy = policy;
        return true;
    }));
}

std::vector<Session::RangeIssue> Session::range_issues(const project::Track& track, const notation::Score& score) const
{
    std::vector<RangeIssue> out;
    const auto limits = range_limits(track);
    if (limits.policy == RangePolicy::Off) return out;
    for (std::size_t s = 0; s < score.staff_count(); ++s) {
        const auto& staff = score.staff(s);
        if (staff.clef == notation::Clef::Percussion) continue;
        for (const auto& n : score.notes(s)) {
            if (n.is_rest) continue;
            // Heard pitch: written + the instrument's transposition + the track's Trans.
            const int sounding = notation::midi_note_number(n.pitch) + staff.total_transposition() + track.effective_transpose();
            if (sounding >= limits.low && sounding <= limits.high) continue;
            out.push_back({n.id, s, n.start_tick, sounding, sounding > limits.high});
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const RangeIssue& a, const RangeIssue& b) { return a.tick < b.tick; });
    return out;
}

std::vector<Session::RangeIssue> Session::range_issues(const project::Track& track) const
{
    const auto* v = track.active_version();
    return v ? range_issues(track, v->score) : std::vector<RangeIssue>{};
}

int Session::fit_into_range(notation::Score& score, const std::vector<RangeIssue>& issues, const RangeLimits& limits)
{
    int moved = 0;
    for (const auto& issue : issues) {
        const auto* n = score.find_note(issue.id);
        if (!n) continue;
        int shift = 0;
        int s = issue.sounding;
        while (s > limits.high && s - 12 >= limits.low) { s -= 12; shift -= 1; }
        while (s < limits.low && s + 12 <= limits.high) { s += 12; shift += 1; }
        if (shift == 0 || s < limits.low || s > limits.high) continue;
        notation::ScoreNote copy = *n;
        copy.pitch.octave = static_cast<std::int8_t>(copy.pitch.octave + shift);
        const std::size_t staff = score.staff_index_of(issue.id);
        score.remove_note(issue.id);
        if (score.insert_note(staff, copy) != notation::kInvalidEventId) ++moved;
    }
    return moved;
}

int Session::move_into_range(const std::string& track_id)
{
    const auto* t = data().find_track(track_id);
    if (!t || !t->active_version()) return 0;
    const auto issues = range_issues(*t);
    if (issues.empty()) return 0;
    notation::Score score = t->active_version()->score;
    const int moved = fit_into_range(score, issues, range_limits(*t));
    if (moved == 0) return 0;
    return commit_score(track_id, score, "Move into range") ? moved : 0;
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
    if (kind != "group" && kind != "fx" && kind != "cue") return {};
    const std::string id = project_.ids().next("bus");
    const bool ok = edit("Add bus " + name, [&](project::ProjectData& d) {
        project::Bus b;
        b.id = id;
        b.name = d.unique_bus_name(name.empty() ? (kind == "fx" ? "FX" : kind == "cue" ? "Cue" : "Group") : name);
        b.kind = kind;
        // A cue mix (headphones) goes to its own device outputs, never the master.
        b.output = kind == "cue" ? "hw:1" : d.find_bus("bus-master") ? "bus-master" : "";
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

std::string Session::add_midi_track(const std::string& name)
{
    std::string id = project_.add_track(name.empty() ? "MIDI" : name, project::TrackKind::MidiOnly);
    project_.modify_unrecorded([&](project::ProjectData& d) {
        if (project::Track* t = d.find_track(id)) {
            t->input_stereo = true;  // a synth's stereo return
            t->family = "midi";
        }
    });
    document_changed();
    select_track(id);
    return id;
}

bool Session::set_midi_input(const std::string& id, const std::string& device)
{
    return edit(device.empty() ? "MIDI in: all inputs" : "MIDI in: " + device, on_track(id, [&](project::Track& t) {
        if ((t.kind != project::TrackKind::MidiOnly && t.kind != project::TrackKind::Instrument) || t.midi_input == device) return false;
        t.midi_input = device;
        return true;
    }));
}

bool Session::set_midi_output(const std::string& id, const std::string& device)
{
    return edit(device.empty() ? "MIDI out: none" : "MIDI out: " + device, on_track(id, [&](project::Track& t) {
        if ((t.kind != project::TrackKind::MidiOnly && t.kind != project::TrackKind::Instrument) || t.midi_output == device) return false;
        t.midi_output = device;
        return true;
    }));
}

void Session::set_record_armed(const std::string& id, bool armed)
{
    const project::Track* t = data().find_track(id);
    if (!t || t->record_armed == armed) return;
    const auto linked = data().linked_tracks(id, "arm");
    project_.modify_unrecorded([&](project::ProjectData& d) {
        for (const auto& m : linked)
            if (project::Track* x = d.find_track(m)) x->record_armed = armed;
    });
    notify(Change::Document);
}

namespace {
/// Puts a clip on lane 0; with `as_takes`, the lane-0 clips it overlaps move
/// to the next free take lane.
void place_clip(project::TrackVersion& v, project::AudioClip c, bool as_takes)
{
    c.lane = 0;
    if (as_takes) {
        int free_lane = 1;
        for (const auto& x : v.audio_clips) free_lane = std::max(free_lane, x.lane + 1);
        for (auto& x : v.audio_clips)
            if (x.lane == 0 && x.start_seconds < c.end_seconds() && c.start_seconds < x.end_seconds()) x.lane = free_lane;
    }
    v.audio_clips.push_back(std::move(c));
    std::stable_sort(v.audio_clips.begin(), v.audio_clips.end(), [](const project::AudioClip& a, const project::AudioClip& b) {
        return a.lane != b.lane ? a.lane < b.lane : a.start_seconds < b.start_seconds;
    });
}
}  // namespace

std::vector<std::string> Session::add_recorded_regions(const std::vector<std::pair<std::string, project::MidiRegion>>& regions,
                                                      const std::string& label, bool as_takes)
{
    const auto before = region_ids();
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < regions.size(); ++i) ids.push_back(project_.ids().next("rgn"));
    const bool ok = !regions.empty() && edit(label, [&](project::ProjectData& d) {
        for (std::size_t i = 0; i < regions.size(); ++i) {
            project::Track* t = d.find_track(regions[i].first);
            project::TrackVersion* v = t ? t->active_version() : nullptr;
            if (!v || t->kind == project::TrackKind::Audio || t->kind == project::TrackKind::Aux) return false;
            project::MidiRegion r = regions[i].second;
            if (r.end <= r.start || (r.notes.empty() && r.controllers.empty())) return false;
            r.id = ids[i];
            r.lane = 0;
            if (as_takes) {
                int free_lane = 1;
                for (const auto& x : v->regions) free_lane = std::max(free_lane, x.lane + 1);
                for (auto& x : v->regions)
                    if (x.lane == 0 && x.start < r.end && r.start < x.end) x.lane = free_lane;
            }
            v->regions.push_back(std::move(r));
        }
        return true;
    });
    if (!ok) ids.clear();
    // Played on a track with a staff: written there (the played feel kept) - the
    // MIDI always matches the notation. MIDI-only tracks keep the region (takes, comping).
    else lock_new_regions_to_notation(before, label);
    return ids;
}

bool Session::comp_take(const std::string& track_id, const std::string& take_clip_id, double from, double to)
{
    if (to <= from) return false;
    std::vector<std::string> new_ids{project_.ids().next("clip"), project_.ids().next("clip")};
    return edit("Comp take", on_track(track_id, [&](project::Track& t) {
        project::TrackVersion* v = t.active_version();
        if (!v) return false;
        const project::AudioClip* take = nullptr;
        for (const auto& c : v->audio_clips)
            if (c.id == take_clip_id && c.lane > 0) take = &c;
        if (!take) return false;
        // The take's part, kept inside the take.
        from = std::max(from, take->start_seconds);
        to = std::min(to, take->end_seconds());
        if (to - from < 0.005) return false;
        project::AudioClip part = *take;
        part.id = new_ids[0];
        part.lane = 0;
        part.source_offset = take->source_offset + (from - take->start_seconds);
        part.start_seconds = from;
        part.length = to - from;
        part.fade_in = std::min(part.fade_in, 0.005);
        part.fade_out = std::min(part.fade_out, 0.005);
        // The main lane makes room: clips the part covers are trimmed, split
        // when it falls inside one, or removed when it covers them.
        std::vector<project::AudioClip> kept;
        for (const auto& c : v->audio_clips) {
            if (c.lane != 0 || c.end_seconds() <= from || c.start_seconds >= to) {
                kept.push_back(c);
                continue;
            }
            if (c.start_seconds < from) {
                project::AudioClip left = c;
                left.length = from - c.start_seconds;
                left.fade_out = std::min(left.fade_out, left.length);
                kept.push_back(left);
            }
            if (c.end_seconds() > to) {
                project::AudioClip right = c;
                right.id = c.start_seconds < from ? new_ids[1] : c.id;
                right.source_offset = c.source_offset + (to - c.start_seconds);
                right.start_seconds = to;
                right.length = c.end_seconds() - to;
                right.fade_in = std::min(right.fade_in, right.length);
                kept.push_back(right);
            }
        }
        kept.push_back(part);
        std::stable_sort(kept.begin(), kept.end(), [](const project::AudioClip& a, const project::AudioClip& b) {
            return a.lane != b.lane ? a.lane < b.lane : a.start_seconds < b.start_seconds;
        });
        v->audio_clips = std::move(kept);
        return true;
    }));
}

bool Session::set_record_source(const std::string& id, const std::string& source)
{
    if (source != "midi" && source != "audio") return false;
    return edit(source == "midi" ? "Record MIDI" : "Record audio", on_track(id, [&](project::Track& t) {
        if (t.kind == project::TrackKind::Audio || t.kind == project::TrackKind::Aux || t.record_source == source) return false;
        t.record_source = source;
        return true;
    }));
}

std::vector<std::string> Session::place_prints(const std::vector<std::pair<std::string, project::AudioClip>>& prints,
                                               bool printed_fx, bool play_audio)
{
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < prints.size(); ++i) ids.push_back(project_.ids().next("clip"));
    bool all_audio = true;
    for (const auto& p : prints)
        if (const auto* t = data().find_track(p.first); !t || t->kind != project::TrackKind::Audio) all_audio = false;
    const std::string label = all_audio ? std::string("Bounce in place")
                              : prints.size() == 1 ? "Print to audio" : "Print " + std::to_string(prints.size()) + " tracks to audio";
    const bool ok = !prints.empty() && edit(label, [&](project::ProjectData& d) {
        for (std::size_t i = 0; i < prints.size(); ++i) {
            project::Track* t = d.find_track(prints[i].first);
            project::TrackVersion* v = t ? t->active_version() : nullptr;
            if (!v || t->kind == project::TrackKind::Aux) return false;
            project::AudioClip c = prints[i].second;
            if (c.length <= 0.0 || c.file.empty()) return false;
            c.id = ids[i];
            // An audio track's bounce in place: the clips it replaces move to
            // a take lane (nothing is lost; undo puts them back).
            place_clip(*v, std::move(c), true);
            t->printed_fx = printed_fx;
            if (play_audio && t->kind != project::TrackKind::Audio) t->playback = "audio";
            if (t->kind != project::TrackKind::Audio) t->print_signature = project::render_signature(d, *t);
        }
        return true;
    });
    if (!ok) ids.clear();
    return ids;
}

std::vector<Session::ArticulationSplit> Session::preview_articulation_tracks(const std::string& track_id) const
{
    std::vector<ArticulationSplit> out;
    const auto* t = data().find_track(track_id);
    const auto* v = t ? t->active_version() : nullptr;
    if (!v || !t->articulation_parent.empty()) return out;
    const auto map = project::expression_map_for(*t);
    auto bump = [&](const std::string& g, int n) {
        for (auto& o : out)
            if (o.group == g) {
                o.notes += n;
                return;
            }
        out.push_back({g, project::articulation_group_name(g), n});
    };
    for (std::size_t s = 0; s < v->score.staff_count(); ++s)
        for (const auto& n : v->score.notes(s))
            if (!n.is_rest) bump(project::articulation_group(map, n.articulations), 1);
    if (out.empty()) {
        // No notes yet (a template): every sound the map selects.
        bump("natural", 0);
        for (const auto& rule : map.articulations)
            if (notation::selects_sound(rule.action)) bump(notation::to_string(rule.articulation), 0);
    }
    // Natural first, then as the map lists them.
    std::stable_sort(out.begin(), out.end(), [](const ArticulationSplit& a, const ArticulationSplit& b) {
        return (a.group == "natural") > (b.group == "natural");
    });
    return out;
}

std::vector<const project::Track*> Session::articulation_tracks(const std::string& track_id) const
{
    std::vector<const project::Track*> out;
    for (const auto& t : data().tracks)
        if (t.articulation_parent == track_id) out.push_back(&t);
    return out;
}

std::vector<std::string> Session::expand_articulations(const std::string& track_id, const std::vector<std::string>& groups)
{
    const auto* t = data().find_track(track_id);
    if (!t || t->kind != project::TrackKind::Instrument || !t->articulation_parent.empty() || t->articulations_expanded) return {};
    std::vector<std::string> wanted = groups;
    if (wanted.empty())
        for (const auto& p : preview_articulation_tracks(track_id)) wanted.push_back(p.group);
    if (wanted.empty()) return {};
    std::vector<std::string> ids;
    const bool ok = edit("Expand " + t->name + " to articulation tracks", [&](project::ProjectData& d) {
        auto it = std::find_if(d.tracks.begin(), d.tracks.end(), [&](const project::Track& x) { return x.id == track_id; });
        if (it == d.tracks.end()) return false;
        it->articulations_expanded = true;
        const project::Track parent = *it;
        auto children = project::make_articulation_tracks(parent, wanted, project_.ids());
        for (const auto& c : children) ids.push_back(c.id);
        d.tracks.insert(std::next(it), children.begin(), children.end());
        return true;
    });
    if (!ok) ids.clear();
    // Each articulation track with the composer's sound for it (legato, spiccato...).
    if (ok) {
        project_.modify_unrecorded([&](project::ProjectData& d) {
            for (const auto& cid : ids)
                if (auto* c = d.find_track(cid)) c->instrument_plugin = {};  // copied from the part: the assigned one goes in
        });
        apply_assigned_sounds(ids);
        // Those with none assigned keep the part's own sound, as before.
        const auto* part = data().find_track(track_id);
        const auto plugin = part ? part->instrument_plugin : project::PluginRef{};
        project_.modify_unrecorded([&](project::ProjectData& d) {
            for (const auto& cid : ids)
                if (auto* c = d.find_track(cid); c && c->instrument_plugin.empty()) c->instrument_plugin = plugin;
        });
    }
    return ids;
}

bool Session::consolidate_articulations(const std::string& track_id)
{
    const auto* t = data().find_track(track_id);
    if (!t || !t->articulations_expanded) return false;
    return edit("Consolidate " + t->name + " to one track", [&](project::ProjectData& d) {
        d.tracks.erase(std::remove_if(d.tracks.begin(), d.tracks.end(),
                                      [&](const project::Track& x) { return x.articulation_parent == track_id; }),
                       d.tracks.end());
        if (auto* p = d.find_track(track_id)) p->articulations_expanded = false;
        return true;
    });
}

project::FreezeState Session::freeze_state(const std::string& track_id) const
{
    const auto* t = data().find_track(track_id);
    return t ? project::freeze_state(data(), *t) : project::FreezeState::Live;
}

bool Session::set_instrument_unloaded(const std::string& track_id, bool unloaded)
{
    const auto* t = data().find_track(track_id);
    if (!t || t->instrument_unloaded == unloaded) return false;
    if (unloaded && (t->playback != "audio" || t->print_signature.empty())) return false;
    return edit(unloaded ? "Unload " + t->name : "Reload " + t->name, [&](project::ProjectData& d) {
        d.find_track(track_id)->instrument_unloaded = unloaded;
        return true;
    });
}

bool Session::set_instrument_suspended(const std::string& track_id, bool suspended)
{
    const auto* t = data().find_track(track_id);
    if (!t || t->instrument_suspended == suspended) return false;
    if (t->kind != project::TrackKind::Instrument && t->kind != project::TrackKind::MidiOnly) return false;
    return edit((suspended ? "Suspend " : "Resume ") + t->name, [&](project::ProjectData& d) {
        d.find_track(track_id)->instrument_suspended = suspended;
        return true;
    });
}

bool Session::unfreeze(const std::string& track_id)
{
    const auto* t = data().find_track(track_id);
    if (!t || (t->playback == "midi" && !t->instrument_unloaded)) return false;
    return edit("Unfreeze " + t->name, [&](project::ProjectData& d) {
        auto* tr = d.find_track(track_id);
        tr->playback = "midi";
        tr->instrument_unloaded = false;
        return true;
    });
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
            place_clip(*v, std::move(c), as_takes);
        }
        return true;
    });
    if (!ok) ids.clear();
    return ids;
}

namespace {
/// Moves the rest of a clip group (Main Spec §9 "region groups") by
/// `seconds`: clips on the active versions of every track, and MIDI regions
/// by the ticks that time spans where they start. `moved` were moved already.
void move_group(project::ProjectData& d, const std::string& group, double seconds, const std::set<std::string>& moved)
{
    if (group.empty() || std::abs(seconds) < 1e-12) return;
    for (auto& t : d.tracks) {
        project::TrackVersion* v = t.active_version();
        if (!v) continue;
        for (auto& c : v->audio_clips)
            if (c.group_id == group && !moved.count(c.id)) c.start_seconds = std::max(0.0, c.start_seconds + seconds);
        for (auto& r : v->regions) {
            if (r.group_id != group || moved.count(r.id)) continue;
            const Ticks to = std::max<Ticks>(0, d.tempo.tick_at_seconds(d.tempo.seconds_at(r.start) + seconds));
            const Ticks delta = to - r.start;
            r.start += delta;
            r.end += delta;
            for (auto& n : r.notes) n.start += delta;
            for (auto& cc : r.controllers) cc.tick += delta;
        }
    }
}
}  // namespace

bool Session::update_audio_clip(const std::string& track_id, const std::string& clip_id, const std::string& label,
                                const std::function<void(project::AudioClip&)>& change)
{
    if (!project_.edit_coalescing(label, [&](project::ProjectData& d) {
            project::Track* t = d.find_track(track_id);
            project::TrackVersion* v = t ? t->active_version() : nullptr;
            if (!v) return false;
            for (auto& c : v->audio_clips)
                if (c.id == clip_id) {
                    const double before = c.start_seconds;
                    change(c);
                    c.id = clip_id;
                    c.source_offset = std::max(0.0, c.source_offset);
                    c.fade_in = std::clamp(c.fade_in, 0.0, c.length);
                    c.fade_out = std::clamp(c.fade_out, 0.0, c.length - c.fade_in);
                    if (c.length <= 0.0 || c.file.empty()) return false;
                    const auto group = c.group_id;
                    const double by = c.start_seconds - before;
                    move_group(d, group, by, {clip_id});
                    return true;
                }
            return false;
        }))
        return false;
    document_changed();
    return true;
}

std::string Session::group_items(const std::vector<ClipRef>& items)
{
    if (items.size() < 2) return {};
    const std::string id = project_.ids().next("cgrp");
    const bool ok = edit("Group", [&](project::ProjectData& d) {
        int n = 0;
        for (const auto& [track, item] : items) {
            project::Track* t = d.find_track(track);
            project::TrackVersion* v = t ? t->active_version() : nullptr;
            if (!v) continue;
            for (auto& c : v->audio_clips)
                if (c.id == item) c.group_id = id, ++n;
            for (auto& r : v->regions)
                if (r.id == item) r.group_id = id, ++n;
        }
        return n >= 2;
    });
    return ok ? id : std::string();
}

bool Session::ungroup_items(const std::vector<ClipRef>& items)
{
    std::set<std::string> groups;
    for (const auto& t : data().tracks)
        if (const auto* v = t.active_version())
            for (const auto& [track, item] : items) {
                if (track != t.id) continue;
                for (const auto& c : v->audio_clips)
                    if (c.id == item && !c.group_id.empty()) groups.insert(c.group_id);
                for (const auto& r : v->regions)
                    if (r.id == item && !r.group_id.empty()) groups.insert(r.group_id);
            }
    if (groups.empty()) return false;
    return edit("Ungroup", [&](project::ProjectData& d) {
        for (auto& t : d.tracks)
            for (auto& v : t.versions) {
                for (auto& c : v.audio_clips)
                    if (groups.count(c.group_id)) c.group_id.clear();
                for (auto& r : v.regions)
                    if (groups.count(r.group_id)) r.group_id.clear();
            }
        return true;
    });
}

bool Session::remove_audio_clips(const std::vector<ClipRef>& clips)
{
    if (clips.empty()) return false;
    return edit(clips.size() == 1 ? "Delete clip" : "Delete clips", [&](project::ProjectData& d) {
        bool any = false;
        for (const auto& clip : clips) {
            const auto& track = clip.first;
            const auto& id = clip.second;
            project::Track* t = d.find_track(track);
            project::TrackVersion* v = t ? t->active_version() : nullptr;
            if (!v) continue;
            const auto before = v->audio_clips.size();
            v->audio_clips.erase(std::remove_if(v->audio_clips.begin(), v->audio_clips.end(),
                                                [&](const project::AudioClip& c) { return c.id == id; }),
                                 v->audio_clips.end());
            any = any || v->audio_clips.size() != before;
        }
        return any;
    });
}

bool Session::update_audio_clips(const std::vector<ClipRef>& clips, const std::string& label,
                                 const std::function<void(project::AudioClip&)>& change)
{
    if (clips.empty()) return false;
    return edit(label, [&](project::ProjectData& d) {
        bool any = false;
        std::set<std::string> moved;
        std::vector<std::pair<std::string, double>> group_moves;
        for (const auto& clip : clips) moved.insert(clip.second);
        for (const auto& clip : clips) {
            const auto& track = clip.first;
            const auto& id = clip.second;
            project::Track* t = d.find_track(track);
            project::TrackVersion* v = t ? t->active_version() : nullptr;
            if (!v) continue;
            for (auto& c : v->audio_clips)
                if (c.id == id) {
                    const double before = c.start_seconds;
                    change(c);
                    c.id = id;
                    any = true;
                    if (!c.group_id.empty() && std::abs(c.start_seconds - before) > 1e-12 &&
                        std::none_of(group_moves.begin(), group_moves.end(), [&](const auto& g) { return g.first == c.group_id; }))
                        group_moves.emplace_back(c.group_id, c.start_seconds - before);
                }
        }
        for (const auto& [group, by] : group_moves) move_group(d, group, by, moved);
        return any;
    });
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

namespace {
/// Grows a region to whole bars around what it holds.
void fit_region(project::MidiRegion& r, const MeterMap& meter)
{
    Ticks lo = r.start, hi = r.end;
    for (const auto& n : r.notes) {
        lo = std::min(lo, n.start);
        hi = std::max(hi, n.end());
    }
    for (const auto& c : r.controllers) {
        lo = std::min(lo, c.tick);
        hi = std::max(hi, c.tick + 1);
    }
    if (lo < r.start) r.start = meter.bar_start(meter.bar_at(std::max<Ticks>(0, lo)));
    if (hi > r.end) r.end = meter.bar_start(meter.bar_at(hi - 1) + 1);
    std::stable_sort(r.controllers.begin(), r.controllers.end(),
                     [](const ControllerEvent& a, const ControllerEvent& b) { return a.tick < b.tick; });
}

/// The notes a MIDI tool works on: the chosen ones, or every note of the
/// main-lane regions.
template <class F>
bool for_region_notes(project::TrackVersion& v, const std::vector<Session::RegionNote>& chosen, F&& f)
{
    bool any = false;
    for (auto& r : v.regions) {
        std::vector<std::size_t> idx;
        for (std::size_t i = 0; i < r.notes.size(); ++i) {
            if (chosen.empty()) {
                if (r.lane == 0) idx.push_back(i);
                continue;
            }
            for (const auto& c : chosen)
                if (c.region == r.id && c.start == r.notes[i].start && c.pitch == r.notes[i].pitch) {
                    idx.push_back(i);
                    break;
                }
        }
        if (!idx.empty()) any = f(r, idx) || any;
    }
    return any;
}
}  // namespace

int Session::quantize_regions(const std::string& track_id, const std::vector<RegionNote>& notes,
                              const project::QuantizeSettings& q)
{
    if (q.grid <= 0) return 0;
    int moved = 0;
    const bool ok = edit("Quantize", [&](project::ProjectData& d) {
        project::Track* t = d.find_track(track_id);
        project::TrackVersion* v = t ? t->active_version() : nullptr;
        if (!v) return false;
        return for_region_notes(*v, notes, [&](project::MidiRegion& r, const std::vector<std::size_t>& idx) {
            std::vector<NoteEvent> picked, rest;
            for (std::size_t i = 0; i < r.notes.size(); ++i)
                (std::find(idx.begin(), idx.end(), i) != idx.end() ? picked : rest).push_back(r.notes[i]);
            const int n = project::quantize_notes(picked, q);
            if (n == 0) return false;
            moved += n;
            rest.insert(rest.end(), picked.begin(), picked.end());
            project::remove_overlaps(rest);
            r.notes = std::move(rest);
            fit_region(r, d.meter);
            return true;
        });
    });
    return ok ? moved : 0;
}

int Session::change_region_velocities(const std::string& track_id, const std::vector<RegionNote>& notes,
                                      const project::VelocityChange& change)
{
    int changed = 0;
    const bool ok = edit("Velocity", [&](project::ProjectData& d) {
        project::Track* t = d.find_track(track_id);
        project::TrackVersion* v = t ? t->active_version() : nullptr;
        if (!v) return false;
        // One pass over every chosen note, so "compress" sees them all together.
        std::vector<NoteEvent*> all;
        for_region_notes(*v, notes, [&](project::MidiRegion& r, const std::vector<std::size_t>& idx) {
            for (auto i : idx) all.push_back(&r.notes[i]);
            return true;
        });
        changed = project::change_velocities(all, change);
        return changed > 0;
    });
    return ok ? changed : 0;
}

bool Session::draw_region_controller(const std::string& track_id, const std::string& region_id, int cc, Ticks from,
                                     Ticks to, std::vector<ControllerEvent> points)
{
    if (to <= from || cc < 0 || cc > kChannelPressureController) return false;
    return edit("Draw CC", [&](project::ProjectData& d) {
        project::Track* t = d.find_track(track_id);
        project::TrackVersion* v = t ? t->active_version() : nullptr;
        if (!v) return false;
        for (auto& r : v->regions) {
            if (r.id != region_id) continue;
            r.controllers.erase(std::remove_if(r.controllers.begin(), r.controllers.end(),
                                               [&](const ControllerEvent& e) {
                                                   return e.controller == cc && e.tick >= from && e.tick < to;
                                               }),
                                r.controllers.end());
            for (auto p : points) {
                if (p.tick < from || p.tick >= to) continue;
                p.controller = cc;
                p.channel = t->midi_channel;
                p.value = std::clamp(p.value, 0, controller_max(cc));
                r.controllers.push_back(p);
            }
            fit_region(r, d.meter);
            return true;
        }
        return false;
    });
}

bool Session::comp_region_take(const std::string& track_id, const std::string& take_region_id, Ticks from, Ticks to)
{
    if (to <= from) return false;
    const std::string new_id = project_.ids().next("rgn");
    return edit("Comp take", [&](project::ProjectData& d) {
        project::Track* t = d.find_track(track_id);
        project::TrackVersion* v = t ? t->active_version() : nullptr;
        if (!v) return false;
        const project::MidiRegion* take = nullptr;
        for (const auto& r : v->regions)
            if (r.id == take_region_id && r.lane > 0) take = &r;
        if (!take) return false;
        from = std::max(from, take->start);
        to = std::min(to, take->end);
        if (to <= from) return false;
        auto inside = [&](Ticks tick) { return tick >= from && tick < to; };
        std::vector<NoteEvent> notes;
        std::vector<ControllerEvent> controllers;
        for (const auto& n : take->notes)
            if (inside(n.start)) notes.push_back(n);
        for (const auto& e : take->controllers)
            if (inside(e.tick)) controllers.push_back(e);
        // The main lane makes room: what starts in the range goes.
        bool changed = false;
        for (auto& r : v->regions) {
            if (r.lane != 0) continue;
            const auto n0 = r.notes.size(), c0 = r.controllers.size();
            r.notes.erase(std::remove_if(r.notes.begin(), r.notes.end(), [&](const NoteEvent& n) { return inside(n.start); }),
                          r.notes.end());
            r.controllers.erase(std::remove_if(r.controllers.begin(), r.controllers.end(),
                                               [&](const ControllerEvent& e) { return inside(e.tick); }),
                                r.controllers.end());
            changed = changed || r.notes.size() != n0 || r.controllers.size() != c0;
        }
        if (notes.empty() && controllers.empty()) return changed;
        // Into the main-lane region the range starts in (or overlaps), else a new one.
        project::MidiRegion* target = nullptr;
        for (auto& r : v->regions)
            if (r.lane == 0 && r.start <= from && from < r.end) target = &r;
        if (!target)
            for (auto& r : v->regions)
                if (r.lane == 0 && r.start < to && from < r.end) target = &r;
        if (!target) {
            project::MidiRegion r;
            r.id = new_id;
            r.start = d.meter.bar_start(d.meter.bar_at(from));
            r.end = d.meter.bar_start(d.meter.bar_at(to - 1) + 1);
            r.source = take->source;
            v->regions.push_back(std::move(r));
            target = &v->regions.back();
        }
        target->notes.insert(target->notes.end(), notes.begin(), notes.end());
        target->controllers.insert(target->controllers.end(), controllers.begin(), controllers.end());
        project::remove_overlaps(target->notes);
        fit_region(*target, d.meter);
        return true;
    });
}

bool Session::promote_region_take(const std::string& track_id, const std::string& take_region_id)
{
    return edit("Promote take", [&](project::ProjectData& d) {
        project::Track* t = d.find_track(track_id);
        project::TrackVersion* v = t ? t->active_version() : nullptr;
        if (!v) return false;
        project::MidiRegion* take = nullptr;
        for (auto& r : v->regions)
            if (r.id == take_region_id) take = &r;
        if (!take || take->lane == 0) return false;
        const int lane = take->lane;
        for (auto& r : v->regions)
            if (r.lane == 0 && r.start < take->end && take->start < r.end) r.lane = lane;
        take->lane = 0;
        return true;
    });
}

bool Session::set_sidechain(const std::string& owner_id, int slot, const std::string& source_id)
{
    if (source_id == owner_id && !source_id.empty()) return false;
    return edit("Sidechain", [&](project::ProjectData& d) {
        if (!source_id.empty() && !d.find_track(source_id) && !d.find_bus(source_id)) return false;
        std::string* strip_key = nullptr;
        std::vector<project::InsertSlot>* inserts = nullptr;
        if (project::Track* t = d.find_track(owner_id)) {
            strip_key = &t->sidechain;
            inserts = &t->inserts;
        } else if (project::Bus* b = d.find_bus(owner_id)) {
            strip_key = &b->sidechain;
            inserts = &b->inserts;
        } else {
            return false;
        }
        std::string* target = strip_key;
        if (slot >= 0) {
            if (slot >= static_cast<int>(inserts->size())) return false;
            auto& ins = (*inserts)[static_cast<std::size_t>(slot)];
            // Built-in compressors and gates, and effect plug-ins (those with a
            // sidechain input take the key; others ignore it).
            if (ins.kind != "compressor" && ins.kind != "gate" && ins.kind != "plugin") return false;
            target = &ins.sidechain;
        }
        if (*target == source_id) return false;
        *target = source_id;
        return true;
    });
}

bool Session::set_input_quantize(const std::string& track_id, Ticks grid)
{
    return edit("Input quantize", on_track(track_id, [&](project::Track& t) {
        if (t.input_quantize == std::max<Ticks>(0, grid)) return false;
        t.input_quantize = std::max<Ticks>(0, grid);
        return true;
    }));
}

std::vector<std::string> Session::split_audio_clips(const std::vector<std::string>& track_ids, double at)
{
    std::vector<std::string> ids;
    const bool ok = edit("Split clip", [&](project::ProjectData& d) {
        bool any = false;
        for (const auto& id : track_ids) {
            project::Track* t = d.find_track(id);
            project::TrackVersion* v = t ? t->active_version() : nullptr;
            if (!v) continue;
            std::vector<project::AudioClip> rights;
            for (auto& c : v->audio_clips) {
                // Never a sliver: at least 5 ms each side.
                if (at <= c.start_seconds + 0.005 || at >= c.end_seconds() - 0.005) continue;
                project::AudioClip right = c;
                right.id = project_.ids().next("clip");
                right.start_seconds = at;
                right.source_offset = c.source_offset + (at - c.start_seconds);
                right.length = c.end_seconds() - at;
                right.fade_in = 0.0;
                right.fade_out = std::min(c.fade_out, right.length);
                c.length = at - c.start_seconds;
                c.fade_out = 0.0;
                c.fade_in = std::min(c.fade_in, c.length);
                ids.push_back(right.id);
                rights.push_back(std::move(right));
            }
            for (auto& r : rights) v->audio_clips.push_back(std::move(r));
            std::stable_sort(v->audio_clips.begin(), v->audio_clips.end(),
                             [](const project::AudioClip& a, const project::AudioClip& b) {
                                 return a.lane != b.lane ? a.lane < b.lane : a.start_seconds < b.start_seconds;
                             });
            any = any || !rights.empty();
        }
        return any;
    });
    if (!ok) ids.clear();
    return ids;
}

std::vector<std::string> Session::split_regions(const std::vector<std::string>& track_ids, Ticks at)
{
    std::vector<std::string> ids;
    const bool ok = edit("Split region", [&](project::ProjectData& d) {
        bool any = false;
        for (const auto& id : track_ids) {
            project::Track* t = d.find_track(id);
            project::TrackVersion* v = t ? t->active_version() : nullptr;
            if (!v) continue;
            std::vector<project::MidiRegion> rights;
            for (auto& r : v->regions) {
                if (at <= r.start || at >= r.end) continue;
                project::MidiRegion right;
                right.id = project_.ids().next("rgn");
                right.start = at;
                right.end = r.end;
                right.lane = r.lane;
                right.source = r.source;
                std::vector<NoteEvent> left_notes;
                for (const auto& n : r.notes) (n.start < at ? left_notes : right.notes).push_back(n);
                std::vector<ControllerEvent> left_cc;
                for (const auto& e : r.controllers) (e.tick < at ? left_cc : right.controllers).push_back(e);
                r.notes = std::move(left_notes);
                r.controllers = std::move(left_cc);
                r.end = at;
                ids.push_back(right.id);
                rights.push_back(std::move(right));
            }
            for (auto& r : rights) v->regions.push_back(std::move(r));
            any = any || !rights.empty();
        }
        return any;
    });
    if (!ok) ids.clear();
    return ids;
}

bool Session::crossfade_audio_clips(const std::string& track_id, const std::string& first_id,
                                    const std::string& second_id, double seconds)
{
    if (seconds <= 0.0) return false;
    return edit("Crossfade", on_track(track_id, [&](project::Track& t) {
        project::TrackVersion* v = t.active_version();
        if (!v) return false;
        project::AudioClip *a = nullptr, *b = nullptr;
        for (auto& c : v->audio_clips) {
            if (c.id == first_id) a = &c;
            if (c.id == second_id) b = &c;
        }
        if (!a || !b || a == b || a->lane != b->lane) return false;
        if (b->start_seconds < a->start_seconds) std::swap(a, b);
        const double overlap = a->end_seconds() - b->start_seconds;
        if (overlap < -1e-6 || b->end_seconds() <= a->end_seconds()) return false;  // apart, or b inside a
        double fade = overlap;
        if (overlap < seconds) {
            // Only meeting (or barely overlapping): the second starts earlier,
            // into the audio its file has before the clip.
            const double extra = std::min(seconds - overlap, b->source_offset);
            b->start_seconds -= extra;
            b->source_offset -= extra;
            b->length += extra;
            fade = overlap + extra;
        }
        fade = std::min({fade, a->length, b->length});
        if (fade < 0.001) return false;
        a->fade_out = fade;
        b->fade_in = fade;
        a->fade_in = std::min(a->fade_in, a->length - fade);
        b->fade_out = std::min(b->fade_out, b->length - fade);
        return true;
    }));
}

namespace {
/// Moves the clips on `lane` that start at or after `at` by `by` seconds.
void shift_lane_after(project::TrackVersion& v, int lane, double at, double by, const std::string& skip = {})
{
    if (std::abs(by) < 1e-12) return;
    for (auto& c : v.audio_clips)
        if (c.lane == lane && c.id != skip && c.start_seconds >= at - 1e-6) c.start_seconds = std::max(0.0, c.start_seconds + by);
}
void sort_clips(project::TrackVersion& v)
{
    std::stable_sort(v.audio_clips.begin(), v.audio_clips.end(), [](const project::AudioClip& a, const project::AudioClip& b) {
        return a.lane != b.lane ? a.lane < b.lane : a.start_seconds < b.start_seconds;
    });
}
}  // namespace

bool Session::remove_audio_clips_shuffle(const std::vector<ClipRef>& clips)
{
    if (clips.empty()) return false;
    return edit(clips.size() == 1 ? "Delete clip" : "Delete clips", [&](project::ProjectData& d) {
        bool any = false;
        for (const auto& clip : clips) {
            project::Track* t = d.find_track(clip.first);
            project::TrackVersion* v = t ? t->active_version() : nullptr;
            if (!v) continue;
            auto it = std::find_if(v->audio_clips.begin(), v->audio_clips.end(),
                                   [&clip](const project::AudioClip& c) { return c.id == clip.second; });
            if (it == v->audio_clips.end()) continue;
            const project::AudioClip gone = *it;
            v->audio_clips.erase(it);
            shift_lane_after(*v, gone.lane, gone.end_seconds(), -gone.length);
            any = true;
        }
        return any;
    });
}

bool Session::update_audio_clip_shuffle(const std::string& track_id, const std::string& clip_id, const std::string& label,
                                        const std::function<void(project::AudioClip&)>& change)
{
    if (!project_.edit_coalescing(label, on_track(track_id, [&](project::Track& t) {
            project::TrackVersion* v = t.active_version();
            if (!v) return false;
            for (auto& c : v->audio_clips)
                if (c.id == clip_id) {
                    const double old_end = c.end_seconds();
                    change(c);
                    c.id = clip_id;
                    c.source_offset = std::max(0.0, c.source_offset);
                    c.fade_in = std::clamp(c.fade_in, 0.0, c.length);
                    c.fade_out = std::clamp(c.fade_out, 0.0, c.length - c.fade_in);
                    if (c.length <= 0.0 || c.file.empty()) return false;
                    const int lane = c.lane;
                    const double moved = c.end_seconds() - old_end;
                    shift_lane_after(*v, lane, old_end, moved, clip_id);
                    return true;
                }
            return false;
        })))
        return false;
    document_changed();
    return true;
}

bool Session::shuffle_move_audio_clip(const std::string& track_id, const std::string& clip_id, double target_seconds)
{
    return edit("Move clip", on_track(track_id, [&](project::Track& t) {
        project::TrackVersion* v = t.active_version();
        if (!v) return false;
        auto it = std::find_if(v->audio_clips.begin(), v->audio_clips.end(),
                               [&clip_id](const project::AudioClip& c) { return c.id == clip_id; });
        if (it == v->audio_clips.end()) return false;
        project::AudioClip moving = *it;
        const double was = moving.start_seconds;
        v->audio_clips.erase(it);
        shift_lane_after(*v, moving.lane, moving.end_seconds(), -moving.length);
        // The boundary nearest the target: 0, or a start or an end on the lane.
        double best = 0.0;
        for (const auto& c : v->audio_clips) {
            if (c.lane != moving.lane) continue;
            for (double b : {c.start_seconds, c.end_seconds()})
                if (std::abs(b - target_seconds) < std::abs(best - target_seconds)) best = b;
        }
        shift_lane_after(*v, moving.lane, best, moving.length);
        moving.start_seconds = best;
        v->audio_clips.push_back(moving);
        sort_clips(*v);
        return std::abs(best - was) > 1e-9;  // nothing moved: no undo step
    })) && (document_changed(), true);
}

bool Session::delete_audio_range(const std::vector<std::string>& track_ids, double from, double to, bool shuffle)
{
    from = std::max(0.0, from);
    if (to - from < 1e-6) return false;
    return edit("Delete range", [&](project::ProjectData& d) {
        bool any = false;
        for (const auto& id : track_ids) {
            project::Track* t = d.find_track(id);
            project::TrackVersion* v = t ? t->active_version() : nullptr;
            if (!v) continue;
            std::vector<project::AudioClip> kept;
            bool changed = false;
            for (const auto& c : v->audio_clips) {
                if (c.lane != 0 || c.end_seconds() <= from + 1e-9 || c.start_seconds >= to - 1e-9) {
                    kept.push_back(c);
                    continue;
                }
                changed = true;
                if (c.start_seconds < from) {  // the part before the range stays
                    project::AudioClip left = c;
                    left.length = from - c.start_seconds;
                    left.fade_out = 0.0;
                    left.fade_in = std::min(left.fade_in, left.length);
                    kept.push_back(left);
                }
                if (c.end_seconds() > to) {  // and the part after it
                    project::AudioClip right = c;
                    if (c.start_seconds < from) right.id = project_.ids().next("clip");
                    right.start_seconds = to;
                    right.source_offset = c.source_offset + (to - c.start_seconds);
                    right.length = c.end_seconds() - to;
                    right.fade_in = 0.0;
                    right.fade_out = std::min(right.fade_out, right.length);
                    kept.push_back(right);
                }
            }
            if (!changed) continue;
            v->audio_clips = std::move(kept);
            if (shuffle) shift_lane_after(*v, 0, to, -(to - from));
            sort_clips(*v);
            any = true;
        }
        return any;
    });
}

bool Session::nudge_audio_clips(const std::string& track_id, const std::vector<std::string>& clip_ids, double seconds)
{
    if (clip_ids.empty() || seconds == 0.0) return false;
    return project_.edit_coalescing("Nudge clip", [&](project::ProjectData& d) {
        project::Track* tp = d.find_track(track_id);
        project::TrackVersion* v = tp ? tp->active_version() : nullptr;
        if (!v) return false;
        // The group moves together; it stops at zero.
        double earliest = 1e300;
        for (const auto& c : v->audio_clips)
            if (std::find(clip_ids.begin(), clip_ids.end(), c.id) != clip_ids.end()) earliest = std::min(earliest, c.start_seconds);
        if (earliest == 1e300) return false;
        const double by = std::max(seconds, -earliest);
        if (std::abs(by) < 1e-9) return false;
        std::set<std::string> groups;
        for (auto& c : v->audio_clips)
            if (std::find(clip_ids.begin(), clip_ids.end(), c.id) != clip_ids.end()) {
                c.start_seconds += by;
                if (!c.group_id.empty()) groups.insert(c.group_id);
            }
        const std::set<std::string> moved(clip_ids.begin(), clip_ids.end());
        for (const auto& g : groups) move_group(d, g, by, moved);
        return true;
    }) && (document_changed(), true);
}

std::string Session::replace_audio_clips(const std::string& track_id, const std::vector<std::string>& clip_ids,
                                         project::AudioClip replacement, const std::string& label)
{
    if (clip_ids.empty() || replacement.file.empty() || replacement.length <= 0.0) return {};
    const std::string id = project_.ids().next("clip");
    const bool ok = edit(label, on_track(track_id, [&](project::Track& t) {
        project::TrackVersion* v = t.active_version();
        if (!v) return false;
        const auto before = v->audio_clips.size();
        v->audio_clips.erase(std::remove_if(v->audio_clips.begin(), v->audio_clips.end(),
                                            [&](const project::AudioClip& c) {
                                                return std::find(clip_ids.begin(), clip_ids.end(), c.id) != clip_ids.end();
                                            }),
                             v->audio_clips.end());
        if (v->audio_clips.size() == before) return false;
        replacement.id = id;
        v->audio_clips.push_back(replacement);
        std::stable_sort(v->audio_clips.begin(), v->audio_clips.end(),
                         [](const project::AudioClip& a, const project::AudioClip& b) {
                             return a.lane != b.lane ? a.lane < b.lane : a.start_seconds < b.start_seconds;
                         });
        return true;
    }));
    return ok ? id : std::string();
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

// --- Review ----------------------------------------------------------------------------------

namespace vcc::session {

std::string Session::create_snapshot(const std::string& name)
{
    const auto id = project_.create_snapshot(name.empty() ? "Snapshot " + std::to_string(data().snapshots.size() + 1) : name);
    document_changed();
    return id;
}

bool Session::rename_version(const std::string& track_id, const std::string& version_id, const std::string& name)
{
    if (!project_.rename_version(track_id, version_id, name)) return false;
    document_changed();
    return true;
}

bool Session::rename_project(const std::string& name)
{
    if (name.empty()) return false;
    return edit("Rename project", [&](project::ProjectData& d) {
        if (d.name == name) return false;
        d.name = name;
        return true;
    });
}

int Session::relink_media(const std::map<std::string, std::string>& relinks)
{
    int changed = 0;
    const bool ok = edit("Relink media", [&](project::ProjectData& d) {
        changed = relink::apply(d, relinks, path_);
        return changed > 0;
    });
    return ok ? changed : 0;
}

bool Session::recall_snapshot(const std::string& snapshot_id)
{
    if (!project_.recall_snapshot(snapshot_id)) return false;
    document_changed();
    return true;
}

bool Session::rename_snapshot(const std::string& snapshot_id, const std::string& name)
{
    if (name.empty()) return false;
    return edit("Rename snapshot", [&](project::ProjectData& d) {
        for (auto& s : d.snapshots)
            if (s.id == snapshot_id && s.name != name) {
                s.name = name;
                return true;
            }
        return false;
    });
}

bool Session::remove_snapshot(const std::string& snapshot_id)
{
    return edit("Delete snapshot", [&](project::ProjectData& d) {
        const auto before = d.snapshots.size();
        d.snapshots.erase(std::remove_if(d.snapshots.begin(), d.snapshots.end(),
                                         [&](const project::Snapshot& s) { return s.id == snapshot_id; }),
                          d.snapshots.end());
        return d.snapshots.size() != before;
    });
}

std::map<std::string, std::string> Session::active_versions() const
{
    std::map<std::string, std::string> out;
    for (const auto& t : data().tracks)
        if (!t.active_version_id.empty()) out[t.id] = t.active_version_id;
    return out;
}

namespace {
/// Applies `versions` to `d`; true when anything changed.
bool apply_versions(project::ProjectData& d, const std::map<std::string, std::string>& versions)
{
    bool changed = false;
    for (const auto& entry : versions) {
        const std::string& version_id = entry.second;
        project::Track* t = d.find_track(entry.first);
        if (!t || t->active_version_id == version_id) continue;
        const bool exists = std::any_of(t->versions.begin(), t->versions.end(),
                                        [&](const project::TrackVersion& v) { return v.id == version_id; });
        if (!exists) continue;
        t->active_version_id = version_id;
        changed = true;
    }
    return changed;
}
}  // namespace

bool Session::audition_versions(const std::map<std::string, std::string>& versions)
{
    bool changed = false;
    project_.modify_unrecorded([&](project::ProjectData& d) { changed = apply_versions(d, versions); });
    if (changed) document_changed();
    return changed;
}

bool Session::choose_versions(const std::string& label, const std::map<std::string, std::string>& from,
                              const std::map<std::string, std::string>& chosen)
{
    // Back to where the audition began (unrecorded), then the choice as one step.
    project_.modify_unrecorded([&](project::ProjectData& d) { apply_versions(d, from); });
    const bool changed = project_.edit(label, [&](project::ProjectData& d) { return apply_versions(d, chosen); });
    document_changed();
    return changed;
}

bool Session::set_version_approved(const std::string& track_id, const std::string& version_id, bool approved)
{
    return edit(approved ? "Approve version" : "Release approval", on_track(track_id, [&](project::Track& t) {
        for (auto& v : t.versions)
            if (v.id == version_id && v.approved != approved) {
                v.approved = approved;
                return true;
            }
        return false;
    }));
}

bool Session::set_snapshot_approved(const std::string& snapshot_id, bool approved)
{
    return edit(approved ? "Approve snapshot" : "Release snapshot approval", [&](project::ProjectData& d) {
        for (auto& s : d.snapshots)
            if (s.id == snapshot_id && s.approved != approved) {
                s.approved = approved;
                return true;
            }
        return false;
    });
}

std::string Session::VersionDiff::describe() const
{
    if (bars.empty()) return "Same music";
    std::string text = bars.size() == 1 && bars[0].first == bars[0].second ? "bar " : "bars ";
    for (std::size_t i = 0; i < bars.size(); ++i) {
        if (i) text += ", ";
        text += std::to_string(bars[i].first);
        if (bars[i].second != bars[i].first) text += "-" + std::to_string(bars[i].second);
    }
    return text;
}

Session::VersionDiff Session::compare_versions(const std::string& track_id, const std::string& version_a,
                                               const std::string& version_b) const
{
    VersionDiff diff;
    const project::Track* track = data().find_track(track_id);
    if (!track) return diff;
    const auto& d = data();
    // Each bar's material, as text, from one version.
    auto by_bar = [&](const std::string& version_id, std::size_t& count) {
        std::map<int, std::multiset<std::string>> bars;
        project::Track t = *track;
        if (std::none_of(t.versions.begin(), t.versions.end(),
                         [&](const project::TrackVersion& v) { return v.id == version_id; }))
            return bars;
        t.active_version_id = version_id;
        const auto r = project::render_track(t);
        count = r.notes.size();
        auto key = [](const char* kind, std::initializer_list<long long> values) {
            std::string k = kind;
            for (auto v : values) k += ":" + std::to_string(v);
            return k;
        };
        for (const auto& n : r.notes)
            bars[d.meter.bar_at(n.start)].insert(key("n", {n.start, n.duration, n.pitch, n.velocity, n.channel}) + n.articulation);
        for (const auto& n : r.keyswitches)
            bars[d.meter.bar_at(n.start)].insert(key("k", {n.start, n.duration, n.pitch, n.velocity}));
        for (const auto& c : r.controllers)
            bars[d.meter.bar_at(c.tick)].insert(key("c", {c.tick, c.channel, c.controller, c.value}));
        for (const auto& v : t.versions) {
            if (v.id != version_id) continue;
            for (const auto& c : v.audio_clips) {
                char buf[256];
                std::snprintf(buf, sizeof buf, "a:%s:%.6f:%.6f:%.6f:%.3f:%.4f:%.4f:%d:%d", c.file.c_str(), c.start_seconds,
                              c.source_offset, c.length, c.gain_db, c.fade_in, c.fade_out, c.lane, c.muted ? 1 : 0);
                const int first = d.meter.bar_at(d.tempo.tick_at_seconds(c.start_seconds));
                const int last = d.meter.bar_at(std::max<Ticks>(0, d.tempo.tick_at_seconds(c.end_seconds()) - 1));
                for (int b = first; b <= last; ++b) bars[b].insert(buf);
            }
        }
        return bars;
    };
    const auto a = by_bar(version_a, diff.notes_a);
    const auto b = by_bar(version_b, diff.notes_b);
    std::set<int> differ;
    for (const auto& [bar, items] : a) {
        const auto it = b.find(bar);
        if (it == b.end() || it->second != items) differ.insert(bar);
    }
    for (const auto& [bar, items] : b)
        if (!a.count(bar)) differ.insert(bar);
    for (int bar : differ) {
        if (!diff.bars.empty() && diff.bars.back().second == bar - 1) diff.bars.back().second = bar;
        else diff.bars.emplace_back(bar, bar);
    }
    return diff;
}

}  // namespace vcc::session

// --- Review comments --------------------------------------------------------------------------

namespace vcc::session {

std::string Session::now_utc()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

namespace {
void sort_comments(project::ProjectData& d)
{
    std::stable_sort(d.comments.begin(), d.comments.end(),
                     [](const project::Comment& a, const project::Comment& b) { return a.seconds < b.seconds; });
}
}  // namespace

std::string Session::add_comment(double seconds, const std::string& text, const std::string& author,
                                 const std::string& track_id, const std::string& created)
{
    if (text.empty()) return {};
    const std::string id = project_.ids().next("cmt");
    const bool ok = edit("Add comment", [&](project::ProjectData& d) {
        project::Comment c;
        c.id = id;
        c.seconds = std::max(0.0, seconds);
        c.track_id = d.find_track(track_id) ? track_id : std::string();
        c.author = author.empty() ? std::string("Reviewer") : author;
        c.text = text;
        c.created = created.empty() ? now_utc() : created;
        d.comments.push_back(std::move(c));
        sort_comments(d);
        return true;
    });
    return ok ? id : std::string();
}

namespace {
template <typename F>
std::function<bool(project::ProjectData&)> on_comment(const std::string& id, F f)
{
    return [id, f](project::ProjectData& d) {
        for (auto& c : d.comments)
            if (c.id == id) {
                const bool changed = f(c);
                sort_comments(d);
                return changed;
            }
        return false;
    };
}
}  // namespace

bool Session::edit_comment(const std::string& comment_id, const std::string& text)
{
    if (text.empty()) return false;
    return edit("Edit comment", on_comment(comment_id, [text](project::Comment& c) {
        if (c.text == text) return false;
        c.text = text;
        return true;
    }));
}

bool Session::move_comment(const std::string& comment_id, double seconds)
{
    return edit("Move comment", on_comment(comment_id, [seconds](project::Comment& c) {
        const double to = std::max(0.0, seconds);
        if (std::abs(c.seconds - to) < 1e-9) return false;
        c.seconds = to;
        return true;
    }));
}

bool Session::set_comment_resolved(const std::string& comment_id, bool resolved)
{
    return edit(resolved ? "Resolve comment" : "Reopen comment", on_comment(comment_id, [resolved](project::Comment& c) {
        if (c.resolved == resolved) return false;
        c.resolved = resolved;
        return true;
    }));
}

bool Session::reply_to_comment(const std::string& comment_id, const std::string& author, const std::string& text,
                               const std::string& created)
{
    if (text.empty()) return false;
    const std::string when = created.empty() ? now_utc() : created;
    return edit("Reply to comment", on_comment(comment_id, [author, text, when](project::Comment& c) {
        c.replies.push_back({author.empty() ? std::string("Reviewer") : author, text, when});
        return true;
    }));
}

bool Session::remove_comment(const std::string& comment_id)
{
    return edit("Delete comment", [&](project::ProjectData& d) {
        const auto before = d.comments.size();
        d.comments.erase(std::remove_if(d.comments.begin(), d.comments.end(),
                                        [&](const project::Comment& c) { return c.id == comment_id; }),
                         d.comments.end());
        return d.comments.size() != before;
    });
}

const project::Comment* Session::find_comment(const std::string& comment_id) const
{
    for (const auto& c : data().comments)
        if (c.id == comment_id) return &c;
    return nullptr;
}

}  // namespace vcc::session

// --- Writing chords into a score ---------------------------------------------------------------

namespace vcc::session {

namespace {
/// A span within one bar as note values, longest first.
std::vector<notation::Duration> note_values_for(Ticks length)
{
    using NV = notation::NoteValue;
    struct V {
        NV value;
        std::uint8_t dots;
        Ticks ticks;
    };
    static const V kValues[] = {{NV::Whole, 0, 4 * kTicksPerQuarter},     {NV::Half, 1, 3 * kTicksPerQuarter},
                                {NV::Half, 0, 2 * kTicksPerQuarter},      {NV::Quarter, 1, 3 * kTicksPerQuarter / 2},
                                {NV::Quarter, 0, kTicksPerQuarter},       {NV::Eighth, 1, 3 * kTicksPerQuarter / 4},
                                {NV::Eighth, 0, kTicksPerQuarter / 2},    {NV::Sixteenth, 1, 3 * kTicksPerQuarter / 8},
                                {NV::Sixteenth, 0, kTicksPerQuarter / 4}, {NV::ThirtySecond, 0, kTicksPerQuarter / 8}};
    std::vector<notation::Duration> out;
    for (const auto& v : kValues)
        while (length >= v.ticks) {
            notation::Duration d;
            d.value = v.value;
            d.dots = v.dots;
            out.push_back(d);
            length -= v.ticks;
        }
    return out;
}
}  // namespace

project::InsertResult Session::write_events(const std::string& track_id, const std::vector<WrittenEvent>& events,
                                            const std::vector<project::ChordMark>& symbols, project::CollisionPolicy policy,
                                            const std::string& label)
{
    project::InsertResult r;
    const project::Track* t = data().find_track(track_id);
    const project::TrackVersion* v = t ? t->active_version() : nullptr;
    if (!v || v->score.staff_count() == 0) {
        r.error = "the track has no score to write on";
        return r;
    }
    if (events.empty()) {
        r.error = "nothing to write";
        return r;
    }
    Ticks from = events.front().start, to = events.front().start;
    for (const auto& e : events) {
        from = std::min(from, e.start);
        to = std::max(to, e.start + e.duration);
    }
    const std::uint8_t voice = policy == project::CollisionPolicy::Merge || policy == project::CollisionPolicy::NewLane ? 1 : 0;
    // A grand staff (treble over bass): what is below middle C goes on the bass staff.
    const bool grand = v->score.staff_count() >= 2 && v->score.staff(0).clef == notation::Clef::Treble &&
                       v->score.staff(1).clef == notation::Clef::Bass;
    const std::size_t staves = grand ? 2 : 1;
    bool occupied = false;
    for (std::size_t st = 0; st < staves; ++st)
        for (const auto& n : v->score.notes(st))
            if (n.voice == voice && !n.is_rest && n.start_tick < to && n.end_tick() > from) occupied = true;
    if (occupied && policy == project::CollisionPolicy::Ask) {
        r.needs_decision = true;
        return r;
    }
    const bool ok = edit(label, [&](project::ProjectData& d) {
        project::Track* tr = d.find_track(track_id);
        project::TrackVersion* ver = tr ? tr->active_version() : nullptr;
        if (!ver) return false;
        auto& score = ver->score;
        // What was in this voice over the span goes (rests too: the new
        // notes fill it).
        std::vector<notation::EventId> gone;
        for (std::size_t st = 0; st < staves; ++st)
            for (const auto& n : score.notes(st))
                if (n.voice == voice && n.start_tick < to && n.end_tick() > from) gone.push_back(n.id);
        for (auto id : gone) score.remove_note(id);
        for (const auto& whole : events) {
          for (std::size_t st = 0; st < staves; ++st) {
            WrittenEvent e = whole;
            if (grand) {
                e.pitches.clear();
                for (const auto& p : whole.pitches)
                    if ((notation::midi_note_number(p) < 60) == (st == 1)) e.pitches.push_back(p);
            }
            if (e.pitches.empty() || e.duration <= 0) continue;
            // Split at the barlines, then into note values; every piece but
            // the last ties on.
            std::vector<std::pair<Ticks, notation::Duration>> pieces;
            const Ticks end = e.start + e.duration;
            for (Ticks at = e.start; at < end;) {
                const Ticks bar_end = d.meter.bar_start(d.meter.bar_at(at) + 1);
                const Ticks seg_end = std::min(end, bar_end);
                for (const auto& dur : note_values_for(seg_end - at)) {
                    pieces.emplace_back(at, dur);
                    at += dur.ticks();
                }
                at = seg_end;
            }
            for (std::size_t i = 0; i < pieces.size(); ++i) {
                const auto ids = score.insert_chord(st, pieces[i].first, pieces[i].second, e.pitches, voice);
                if (i + 1 < pieces.size())
                    for (auto id : ids)
                        if (const auto* n = score.find_note(id)) {
                            auto tied = *n;
                            tied.tied_to_next = true;
                            score.modify_note(id, tied);
                        }
            }
          }
        }
        // The chord symbols over the span, as Send to Track does.
        auto& marks = d.chords;
        marks.erase(std::remove_if(marks.begin(), marks.end(), [&](const project::ChordMark& m) { return m.tick >= from && m.tick < to; }),
                    marks.end());
        for (const auto& m : symbols)
            if (theory::parse_chord_symbol(m.text)) marks.push_back(m);
        std::sort(marks.begin(), marks.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
        return true;
    });
    r.ok = ok;
    if (!ok) r.error = last_refusal().empty() ? std::string("nothing was written") : last_refusal();
    return r;
}

}  // namespace vcc::session
