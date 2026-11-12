#include "vcc/project/Project.h"

#include <algorithm>
#include <set>

#include "vcc/core/Files.h"
#include "vcc/notation/ScoreFile.h"
#include "vcc/theory/Pitch.h"

namespace vcc::project {

// --- Enums --------------------------------------------------------------------

namespace {
const std::pair<TrackKind, const char*> kKinds[] = {
    {TrackKind::Instrument, "instrument"}, {TrackKind::MidiOnly, "midi"}, {TrackKind::Audio, "audio"},
    {TrackKind::Aux, "aux"}, {TrackKind::Reference, "reference"}, {TrackKind::Video, "video"},
    {TrackKind::Folder, "folder"}};
const std::pair<ResolutionStatus, const char*> kStatuses[] = {
    {ResolutionStatus::Exact, "exact"}, {ResolutionStatus::Fallback, "fallback"},
    {ResolutionStatus::Placeholder, "placeholder"}, {ResolutionStatus::Unresolved, "unresolved"}};
}  // namespace

const char* to_string(TrackKind k) noexcept
{
    for (const auto& p : kKinds)
        if (p.first == k) return p.second;
    return "instrument";
}
std::optional<TrackKind> track_kind_from_string(const std::string& t) noexcept
{
    for (const auto& p : kKinds)
        if (t == p.second) return p.first;
    return std::nullopt;
}
const char* to_string(ResolutionStatus s) noexcept
{
    for (const auto& p : kStatuses)
        if (p.first == s) return p.second;
    return "unresolved";
}
std::optional<ResolutionStatus> resolution_status_from_string(const std::string& t) noexcept
{
    for (const auto& p : kStatuses)
        if (t == p.second) return p.first;
    return std::nullopt;
}

// --- Lookups --------------------------------------------------------------------

const TrackVersion* Track::active_version() const noexcept
{
    for (const auto& v : versions)
        if (v.id == active_version_id) return &v;
    return versions.empty() ? nullptr : &versions.front();
}
TrackVersion* Track::active_version() noexcept
{
    for (auto& v : versions)
        if (v.id == active_version_id) return &v;
    return versions.empty() ? nullptr : &versions.front();
}

const Track* ProjectData::find_track(const std::string& id) const noexcept
{
    for (const auto& t : tracks)
        if (t.id == id) return &t;
    return nullptr;
}
Track* ProjectData::find_track(const std::string& id) noexcept
{
    for (auto& t : tracks)
        if (t.id == id) return &t;
    return nullptr;
}
const Bus* ProjectData::find_bus(const std::string& id) const noexcept
{
    for (const auto& b : buses)
        if (b.id == id) return &b;
    return nullptr;
}
Bus* ProjectData::find_bus(const std::string& id) noexcept
{
    for (auto& b : buses)
        if (b.id == id) return &b;
    return nullptr;
}
const Track* ProjectData::find_track_by_name(const std::string& wanted) const noexcept
{
    for (const auto& t : tracks)
        if (t.name == wanted) return &t;
    return nullptr;
}
const Bus* ProjectData::find_bus_by_name(const std::string& wanted) const noexcept
{
    for (const auto& b : buses)
        if (b.name == wanted) return &b;
    return nullptr;
}

namespace {
template <typename Exists>
std::string unique_name(const std::string& wanted, Exists exists)
{
    if (!exists(wanted)) return wanted;
    for (int n = 2;; ++n) {
        std::string candidate = wanted + " (" + std::to_string(n) + ")";
        if (!exists(candidate)) return candidate;
    }
}
}  // namespace

std::string ProjectData::unique_track_name(const std::string& wanted) const
{
    return unique_name(wanted, [&](const std::string& n) { return find_track_by_name(n) != nullptr; });
}
std::string ProjectData::unique_bus_name(const std::string& wanted) const
{
    return unique_name(wanted, [&](const std::string& n) { return find_bus_by_name(n) != nullptr; });
}

// --- Score synchronisation ----------------------------------------------------------

namespace {

void sync_score(notation::Score& score, const MeterMap& meter, const std::optional<notation::KeySignature>& key)
{
    std::vector<std::pair<Ticks, music::TimeSignature>> wanted;
    for (const auto& c : meter.changes())
        wanted.emplace_back(meter.bar_start(c.bar),
                            music::TimeSignature{static_cast<std::uint16_t>(c.signature.numerator),
                                                 static_cast<std::uint16_t>(c.signature.denominator)});
    const auto& have = score.time_signature_changes();
    bool same = have.size() == wanted.size();
    for (std::size_t i = 0; same && i < have.size(); ++i)
        same = have[i].tick == wanted[i].first &&
               have[i].time_signature.numerator == wanted[i].second.numerator &&
               have[i].time_signature.denominator == wanted[i].second.denominator;
    if (!same) {
        // Clear and rebuild in order, so each change lands on a barline of
        // the meter before it.
        std::vector<Ticks> old;
        for (const auto& c : have)
            if (c.tick > 0) old.push_back(c.tick);
        for (Ticks t : old) score.remove_time_signature(t);
        for (const auto& w : wanted) score.set_time_signature(w.first, w.second);
    }
    if (key) {
        notation::KeySignature current = score.key_signature_at(0);
        if (current.fifths != key->fifths || current.is_minor != key->is_minor) score.set_key_signature(0, *key);
    }
}

std::optional<notation::KeySignature> key_signature_for(const std::string& key_text)
{
    auto key = theory::Key::parse(key_text);
    if (!key) return std::nullopt;
    notation::KeySignature ks;
    ks.fifths = static_cast<std::int8_t>(key->fifths());
    ks.is_minor = theory::is_minor_family(key->mode);
    return ks;
}

notation::Score score_for_track(const std::string& name)
{
    notation::Score score;
    notation::Staff staff = score.staff(0);
    staff.name = name;
    score.set_staff(0, staff);
    return score;
}

}  // namespace

void ProjectData::sync_scores()
{
    auto ks = key_signature_for(key);
    for (auto& t : tracks)
        for (auto& v : t.versions) sync_score(v.score, meter, ks);
}

// --- History --------------------------------------------------------------------

Project::Project()
{
    Bus master;
    master.id = "bus-master";
    master.name = "Master";
    master.kind = "master";
    data_.buses.push_back(master);
}

Project::Project(ProjectData data, std::uint64_t next_id) : data_(std::move(data)), ids_(next_id) {}

bool Project::edit(const std::string& label, const std::function<bool(ProjectData&)>& change)
{
    ProjectData working = data_;
    bool ok = false;
    try {
        ok = change(working);
    } catch (...) {
        return false;
    }
    if (!ok) return false;
    working.sync_scores();
    undo_.push_back({label, std::move(data_), {}});
    data_ = std::move(working);
    redo_.clear();
    return true;
}

std::string Project::undo_label() const { return undo_.empty() ? std::string{} : undo_.back().label; }

bool Project::undo()
{
    if (undo_.empty()) return false;
    Step s = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back({s.label, {}, std::move(data_)});
    data_ = std::move(s.before);
    return true;
}

bool Project::redo()
{
    if (redo_.empty()) return false;
    Step s = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back({s.label, std::move(data_), {}});
    data_ = std::move(s.after);
    return true;
}

// --- Edits ------------------------------------------------------------------------

std::string Project::add_track(const std::string& name, TrackKind kind, const std::string& instrument)
{
    std::string id = ids_.next("trk");
    std::string version = ids_.next("ver");
    edit("Add track " + name, [&](ProjectData& d) {
        Track t;
        t.id = id;
        t.name = d.unique_track_name(name);
        t.kind = kind;
        t.instrument_standard_id = instrument;
        t.include_in_export = kind != TrackKind::Reference;
        t.output_bus_id = "bus-master";
        if (kind != TrackKind::Folder) {
            t.versions.push_back({version, "Version 1", {}, score_for_track(t.name), {}, {}, std::nullopt});
            t.active_version_id = version;
        }
        d.tracks.push_back(t);
        return true;
    });
    return id;
}

InsertResult Project::insert_notes(const std::string& track_id, Ticks start, Ticks end, std::vector<NoteEvent> notes,
                                   CollisionPolicy policy, const std::string& source)
{
    InsertResult r;
    const Track* t = data_.find_track(track_id);
    if (!t) { r.error = "no such track"; return r; }
    if (t->kind == TrackKind::Reference || t->kind == TrackKind::Folder || t->kind == TrackKind::Video) {
        r.error = "'" + t->name + "' cannot hold MIDI";
        return r;
    }
    if (end <= start) { r.error = "empty range"; return r; }
    std::string fault = validate_notes(notes, start, end);
    if (!fault.empty()) { r.error = fault; return r; }

    const TrackVersion* v = t->active_version();
    if (!v) { r.error = "track has no version"; return r; }
    bool occupied = false;
    int max_lane = 0;
    for (const auto& reg : v->regions) {
        if (reg.start < end && reg.end > start) {
            occupied = true;
            max_lane = std::max(max_lane, reg.lane);
        }
    }
    if (occupied && policy == CollisionPolicy::Ask) {
        r.needs_decision = true;
        r.error = "the range already holds material on '" + t->name + "': choose replace, merge or new lane";
        return r;
    }

    std::string region_id = ids_.next("rgn");
    std::sort(notes.begin(), notes.end());
    bool ok = edit("Insert " + (source.empty() ? std::string("notes") : source) + " on " + t->name, [&](ProjectData& d) {
        TrackVersion* ver = d.find_track(track_id)->active_version();
        MidiRegion region{region_id, start, end, 0, notes, source};
        if (occupied && policy == CollisionPolicy::Replace) {
            for (auto& reg : ver->regions) {
                if (reg.lane != 0 || !(reg.start < end && reg.end > start)) continue;
                std::vector<NoteEvent> kept;
                for (auto n : reg.notes) {
                    if (n.start >= start && n.start < end) continue;      // replaced
                    if (n.start < start && n.end() > start) n.duration = start - n.start;  // trimmed
                    kept.push_back(n);
                }
                reg.notes = std::move(kept);
            }
            // Regions now wholly inside the replaced range and empty go away.
            ver->regions.erase(std::remove_if(ver->regions.begin(), ver->regions.end(),
                                              [&](const MidiRegion& reg) {
                                                  return reg.lane == 0 && reg.start >= start && reg.end <= end &&
                                                         reg.notes.empty();
                                              }),
                               ver->regions.end());
        } else if (occupied && policy == CollisionPolicy::NewLane) {
            region.lane = max_lane + 1;
        }
        ver->regions.push_back(std::move(region));
        return true;
    });
    r.ok = ok;
    r.region_id = ok ? region_id : std::string{};
    return r;
}

std::optional<std::string> Project::duplicate_version(const std::string& track_id, const std::string& name)
{
    const Track* t = data_.find_track(track_id);
    if (!t || !t->active_version()) return std::nullopt;
    std::string id = ids_.next("ver");
    std::vector<std::string> region_ids;
    for (std::size_t i = 0; i < t->active_version()->regions.size(); ++i) region_ids.push_back(ids_.next("rgn"));
    std::string label = name.empty() ? "Version " + std::to_string(t->versions.size() + 1) : name;
    bool ok = edit("Duplicate version of " + t->name, [&](ProjectData& d) {
        Track* tr = d.find_track(track_id);
        TrackVersion copy = *tr->active_version();
        copy.id = id;
        copy.name = label;
        for (std::size_t i = 0; i < copy.regions.size(); ++i) copy.regions[i].id = region_ids[i];
        tr->versions.push_back(copy);
        tr->active_version_id = id;
        return true;
    });
    if (!ok) return std::nullopt;
    return id;
}

bool Project::set_active_version(const std::string& track_id, const std::string& version_id)
{
    return edit("Switch version", [&](ProjectData& d) {
        Track* t = d.find_track(track_id);
        if (!t) return false;
        bool exists = std::any_of(t->versions.begin(), t->versions.end(),
                                  [&](const TrackVersion& v) { return v.id == version_id; });
        if (!exists || t->active_version_id == version_id) return false;
        t->active_version_id = version_id;
        return true;
    });
}

bool Project::rename_version(const std::string& track_id, const std::string& version_id, const std::string& name)
{
    return edit("Rename version", [&](ProjectData& d) {
        Track* t = d.find_track(track_id);
        if (!t || name.empty()) return false;
        for (auto& v : t->versions)
            if (v.id == version_id) { v.name = name; return true; }
        return false;
    });
}

std::string Project::create_snapshot(const std::string& name)
{
    std::string id = ids_.next("snap");
    edit("Create snapshot " + name, [&](ProjectData& d) {
        Snapshot s{id, name, {}};
        for (const auto& t : d.tracks)
            if (!t.active_version_id.empty()) s.track_versions[t.id] = t.active_version_id;
        d.snapshots.push_back(s);
        return true;
    });
    return id;
}

bool Project::recall_snapshot(const std::string& snapshot_id)
{
    return edit("Recall snapshot", [&](ProjectData& d) {
        auto it = std::find_if(d.snapshots.begin(), d.snapshots.end(),
                               [&](const Snapshot& s) { return s.id == snapshot_id; });
        if (it == d.snapshots.end()) return false;
        for (const auto& entry : it->track_versions) {
            const std::string& version_id = entry.second;
            Track* t = d.find_track(entry.first);
            if (!t) continue;  // deleted since: nothing to recall
            bool exists = std::any_of(t->versions.begin(), t->versions.end(),
                                      [&](const TrackVersion& v) { return v.id == version_id; });
            if (exists) t->active_version_id = version_id;
        }
        return true;
    });
}

std::vector<const Track*> Project::export_tracks() const
{
    std::vector<const Track*> out;
    for (const auto& t : data_.tracks)
        if (t.include_in_export && t.kind != TrackKind::Folder && t.kind != TrackKind::Video) out.push_back(&t);
    return out;
}

// --- Serialization ------------------------------------------------------------------

namespace {

json::Value notes_to_json(const std::vector<NoteEvent>& notes)
{
    json::Array a;
    for (const auto& n : notes) {
        json::Array e{n.start, n.duration, n.pitch, n.velocity, n.channel};
        if (!n.articulation.empty()) e.emplace_back(n.articulation);
        a.emplace_back(e);
    }
    return a;
}

bool notes_from_json(const json::Value& v, std::vector<NoteEvent>& out, std::string& err)
{
    for (const auto& e : v.as_array()) {
        const auto& a = e.as_array();
        if (a.size() < 5 || !a[0].is_int() || !a[1].is_int()) {
            err = "malformed note";
            return false;
        }
        NoteEvent n{a[0].as_int(), a[1].as_int(), static_cast<int>(a[2].as_int()), static_cast<int>(a[3].as_int()),
                    static_cast<int>(a[4].as_int()), a.size() > 5 ? a[5].as_string() : std::string{}};
        if (n.duration <= 0 || n.pitch < 0 || n.pitch > 127 || n.velocity < 1 || n.velocity > 127 ||
            n.channel < 0 || n.channel > 15) {
            err = "note out of range at tick " + std::to_string(n.start);
            return false;
        }
        out.push_back(n);
    }
    return true;
}

json::Value string_array(const std::vector<std::string>& v)
{
    json::Array a;
    for (const auto& s : v) a.emplace_back(s);
    return a;
}

json::Value sends_to_json(const std::vector<std::pair<std::string, double>>& sends)
{
    json::Array a;
    for (const auto& s : sends) a.emplace_back(json::Object{{"to", s.first}, {"level_db", s.second}});
    return a;
}

std::vector<std::pair<std::string, double>> sends_from_json(const json::Value* v)
{
    std::vector<std::pair<std::string, double>> out;
    if (v)
        for (const auto& s : v->as_array()) out.emplace_back(s.get_string("to"), s.get_double("level_db", -12.0));
    return out;
}

using Migration = bool (*)(json::Value& doc, std::string& error);

/// One entry per older schema version, upgrading it by exactly one version.
/// Empty while schema 1 is the only format that has shipped; when schema 2
/// arrives, {1, migrate_1_to_2} goes here and the test in test_project.cpp
/// holds the table to covering every version below the current one.
const std::map<int, Migration>& migrations()
{
    static const std::map<int, Migration> table = {};
    return table;
}

}  // namespace

json::Value Project::to_json() const
{
    const ProjectData& d = data_;
    json::Object root;
    root.set("format", "vc-composer-2-project");
    root.set("schema_version", kProjectSchemaVersion);
    root.set("next_id", static_cast<long long>(ids_.peek()));
    root.set("name", d.name);
    root.set("key", d.key);
    json::Array meter;
    for (const auto& c : d.meter.changes())
        meter.emplace_back(json::Object{{"bar", c.bar}, {"signature", c.signature.to_string()}});
    root.set("meter", meter);
    json::Array tempo;
    for (const auto& c : d.tempo.changes()) tempo.emplace_back(json::Object{{"tick", c.tick}, {"bpm", c.bpm}});
    root.set("tempo", tempo);
    root.set("frame_rate", to_string(d.frame_rate));
    root.set("start_timecode", d.start_timecode);
    root.set("sample_rate", d.sample_rate);

    json::Array tracks;
    for (const auto& t : d.tracks) {
        json::Object o{{"id", t.id}, {"name", t.name}, {"kind", to_string(t.kind)}, {"folder_id", t.folder_id},
                       {"family", t.family}, {"role", t.role}, {"color", t.color},
                       {"instrument_standard_id", t.instrument_standard_id},
                       {"articulation_intents", string_array(t.articulation_intents)},
                       {"fallback_instruments", string_array(t.fallback_instruments)},
                       {"plugin_requirements", string_array(t.plugin_requirements)}};
        const auto& a = t.assignment;
        o.set("assignment", json::Object{{"status", to_string(a.status)},
                                         {"resolved_instrument_id", a.resolved_instrument_id},
                                         {"patch_id", a.patch_id},
                                         {"patch_display", a.patch_display},
                                         {"placeholder_program", a.placeholder_program},
                                         {"unsupported_articulations", string_array(a.unsupported_articulations)},
                                         {"message", a.message}});
        o.set("expression_map", t.expression_map);
        o.set("output_bus_id", t.output_bus_id);
        o.set("sends", sends_to_json(t.sends));
        o.set("gain_db", t.gain_db);
        o.set("pan", t.pan);
        o.set("mute", t.mute);
        o.set("solo", t.solo);
        o.set("include_in_export", t.include_in_export);
        o.set("midi_channel", t.midi_channel);
        o.set("template_origin", t.template_origin);
        json::Array versions;
        for (const auto& v : t.versions) {
            json::Array regions;
            for (const auto& r : v.regions)
                regions.emplace_back(json::Object{{"id", r.id}, {"start", r.start}, {"end", r.end},
                                                  {"lane", r.lane}, {"source", r.source},
                                                  {"notes", notes_to_json(r.notes)}});
            json::Object vo{{"id", v.id}, {"name", v.name}, {"notes", v.notes},
                            {"score", notation::score_to_json(v.score)}, {"regions", regions},
                            {"articulation_overrides", string_array(v.articulation_overrides)}};
            if (v.gain_db) vo.set("gain_db", *v.gain_db);
            versions.emplace_back(vo);
        }
        o.set("versions", versions);
        o.set("active_version_id", t.active_version_id);
        tracks.emplace_back(o);
    }
    root.set("tracks", tracks);

    json::Array buses;
    for (const auto& b : d.buses) {
        json::Array inserts;
        for (const auto& i : b.inserts)
            inserts.emplace_back(json::Object{{"kind", i.kind}, {"plugin_ref", i.plugin_ref},
                                              {"suggestion", i.suggestion}, {"bypass", i.bypass}});
        buses.emplace_back(json::Object{{"id", b.id}, {"name", b.name}, {"kind", b.kind}, {"output", b.output},
                                        {"sends", sends_to_json(b.sends)}, {"inserts", inserts},
                                        {"gain_db", b.gain_db}});
    }
    root.set("buses", buses);

    json::Array cues;
    for (const auto& c : d.cues)
        cues.emplace_back(json::Object{{"id", c.id}, {"code", c.code}, {"title", c.title}, {"start", c.start},
                                       {"end", c.end}, {"status", c.status}, {"notes", c.notes}});
    root.set("cues", cues);

    json::Array snaps;
    for (const auto& s : d.snapshots) {
        json::Object tv;
        for (const auto& [k, v] : s.track_versions) tv.set(k, v);
        snaps.emplace_back(json::Object{{"id", s.id}, {"name", s.name}, {"track_versions", tv}});
    }
    root.set("snapshots", snaps);
    root.set("applied_templates", string_array(d.applied_templates));
    return root;
}

std::optional<Project> Project::from_json(const json::Value& input, std::string* error)
{
    auto fail = [&](const std::string& why) -> std::optional<Project> {
        if (error) *error = why;
        return std::nullopt;
    };
    if (input.get_string("format") != "vc-composer-2-project") return fail("not a VC Composer 2 project file");
    json::Value doc = input;
    int version = static_cast<int>(doc.get_int("schema_version", 0));
    if (version > kProjectSchemaVersion)
        return fail("project was saved by a newer VC Composer 2 (schema " + std::to_string(version) +
                    "); this build reads up to schema " + std::to_string(kProjectSchemaVersion));
    if (version < 1) return fail("unknown project schema version " + std::to_string(version));
    while (version < kProjectSchemaVersion) {
        auto it = migrations().find(version);
        if (it == migrations().end()) return fail("no migration from schema " + std::to_string(version));
        std::string err;
        if (!it->second(doc, err)) return fail("migration from schema " + std::to_string(version) + " failed: " + err);
        doc.object().set("schema_version", ++version);
    }

    ProjectData d;
    d.name = doc.get_string("name", "Untitled");
    d.key = doc.get_string("key", "C major");
    bool first = true;
    for (const auto& m : doc.get("meter") ? doc.get("meter")->as_array() : json::Array{}) {
        auto sig = TimeSignature::parse(m.get_string("signature"));
        int bar = static_cast<int>(m.get_int("bar", 1));
        if (!sig || bar < 1) return fail("invalid meter change");
        if (first) { d.meter = MeterMap(*sig); first = false; if (bar != 1) return fail("meter must start at bar 1"); }
        else d.meter.set(bar, *sig);
    }
    first = true;
    for (const auto& t : doc.get("tempo") ? doc.get("tempo")->as_array() : json::Array{}) {
        double bpm = t.get_double("bpm", 0);
        Ticks tick = t.get_int("tick", -1);
        if (bpm <= 0 || bpm > 1000 || tick < 0) return fail("invalid tempo change");
        if (first) { d.tempo = TempoMap(bpm); first = false; if (tick != 0) d.tempo.set(tick, bpm); }
        else d.tempo.set(tick, bpm);
    }
    auto fr = frame_rate_from_string(doc.get_string("frame_rate", "24"));
    if (!fr) return fail("invalid frame rate");
    d.frame_rate = *fr;
    d.start_timecode = doc.get_string("start_timecode", "01:00:00:00");
    d.sample_rate = static_cast<int>(doc.get_int("sample_rate", 48000));

    std::set<std::string> seen_ids;
    auto unique = [&](const std::string& id) { return !id.empty() && seen_ids.insert(id).second; };

    for (const auto& j : doc.get("tracks") ? doc.get("tracks")->as_array() : json::Array{}) {
        Track t;
        t.id = j.get_string("id");
        if (!unique(t.id)) return fail("missing or duplicate track id '" + t.id + "'");
        t.name = j.get_string("name");
        auto kind = track_kind_from_string(j.get_string("kind", "instrument"));
        if (!kind) return fail("track " + t.id + ": unknown kind");
        t.kind = *kind;
        t.folder_id = j.get_string("folder_id");
        t.family = j.get_string("family");
        t.role = j.get_string("role");
        t.color = j.get_string("color", "#808080");
        t.instrument_standard_id = j.get_string("instrument_standard_id");
        t.articulation_intents = j.get_string_array("articulation_intents");
        t.fallback_instruments = j.get_string_array("fallback_instruments");
        t.plugin_requirements = j.get_string_array("plugin_requirements");
        if (const auto* a = j.get("assignment")) {
            auto st = resolution_status_from_string(a->get_string("status", "unresolved"));
            if (!st) return fail("track " + t.id + ": unknown resolution status");
            t.assignment.status = *st;
            t.assignment.resolved_instrument_id = a->get_string("resolved_instrument_id");
            t.assignment.patch_id = a->get_string("patch_id");
            t.assignment.patch_display = a->get_string("patch_display");
            t.assignment.placeholder_program = static_cast<int>(a->get_int("placeholder_program"));
            t.assignment.unsupported_articulations = a->get_string_array("unsupported_articulations");
            t.assignment.message = a->get_string("message");
        }
        t.expression_map = j.get_string("expression_map", "default");
        t.output_bus_id = j.get_string("output_bus_id");
        t.sends = sends_from_json(j.get("sends"));
        t.gain_db = j.get_double("gain_db");
        t.pan = j.get_double("pan");
        t.mute = j.get_bool("mute");
        t.solo = j.get_bool("solo");
        t.include_in_export = j.get_bool("include_in_export", t.kind != TrackKind::Reference);
        t.midi_channel = static_cast<int>(j.get_int("midi_channel"));
        t.template_origin = j.get_string("template_origin");
        for (const auto& vj : j.get("versions") ? j.get("versions")->as_array() : json::Array{}) {
            TrackVersion v;
            v.id = vj.get_string("id");
            if (!unique(v.id)) return fail("missing or duplicate version id '" + v.id + "'");
            v.name = vj.get_string("name");
            v.notes = vj.get_string("notes");
            v.articulation_overrides = vj.get_string_array("articulation_overrides");
            if (const auto* g = vj.get("gain_db")) v.gain_db = g->as_double();
            if (const auto* sj = vj.get("score")) {
                notation::ScoreLoadResult loaded = notation::score_from_json(*sj, v.score);
                if (!loaded.ok) return fail("version " + v.id + " score: " + loaded.error);
            }
            for (const auto& rj : vj.get("regions") ? vj.get("regions")->as_array() : json::Array{}) {
                MidiRegion r;
                r.id = rj.get_string("id");
                if (!unique(r.id)) return fail("missing or duplicate region id '" + r.id + "'");
                r.start = rj.get_int("start");
                r.end = rj.get_int("end");
                r.lane = static_cast<int>(rj.get_int("lane"));
                r.source = rj.get_string("source");
                std::string err;
                if (const auto* nj = rj.get("notes"); nj && !notes_from_json(*nj, r.notes, err))
                    return fail("region " + r.id + ": " + err);
                if (r.end <= r.start) return fail("region " + r.id + ": empty range");
                v.regions.push_back(std::move(r));
            }
            t.versions.push_back(std::move(v));
        }
        t.active_version_id = j.get_string("active_version_id");
        if (!t.versions.empty() && !t.active_version())
            return fail("track " + t.id + ": active version not found");
        if (!t.versions.empty() &&
            std::none_of(t.versions.begin(), t.versions.end(),
                         [&](const TrackVersion& v) { return v.id == t.active_version_id; }))
            return fail("track " + t.id + ": active version '" + t.active_version_id + "' not found");
        d.tracks.push_back(std::move(t));
    }

    for (const auto& j : doc.get("buses") ? doc.get("buses")->as_array() : json::Array{}) {
        Bus b;
        b.id = j.get_string("id");
        if (!unique(b.id)) return fail("missing or duplicate bus id '" + b.id + "'");
        b.name = j.get_string("name");
        b.kind = j.get_string("kind", "group");
        b.output = j.get_string("output");
        b.sends = sends_from_json(j.get("sends"));
        b.gain_db = j.get_double("gain_db");
        for (const auto& i : j.get("inserts") ? j.get("inserts")->as_array() : json::Array{})
            b.inserts.push_back({i.get_string("kind"), i.get_string("plugin_ref"), i.get_bool("suggestion", true),
                                 i.get_bool("bypass")});
        d.buses.push_back(std::move(b));
    }
    for (const auto& j : doc.get("cues") ? doc.get("cues")->as_array() : json::Array{}) {
        Cue c{j.get_string("id"), j.get_string("code"), j.get_string("title"), j.get_int("start"),
              j.get_int("end"), j.get_string("status", "writing"), j.get_string("notes")};
        if (!unique(c.id)) return fail("missing or duplicate cue id '" + c.id + "'");
        d.cues.push_back(c);
    }
    for (const auto& j : doc.get("snapshots") ? doc.get("snapshots")->as_array() : json::Array{}) {
        Snapshot s{j.get_string("id"), j.get_string("name"), {}};
        if (!unique(s.id)) return fail("missing or duplicate snapshot id '" + s.id + "'");
        if (const auto* tv = j.get("track_versions"))
            for (const auto& m : tv->as_object()) s.track_versions[m.first] = m.second.as_string();
        d.snapshots.push_back(s);
    }
    d.applied_templates = doc.get_string_array("applied_templates");

    // References must resolve: a dangling bus or folder is a corrupt file.
    for (const auto& t : d.tracks) {
        if (!t.output_bus_id.empty() && !d.find_bus(t.output_bus_id))
            return fail("track " + t.id + " routes to missing bus " + t.output_bus_id);
        if (!t.folder_id.empty() && !d.find_track(t.folder_id))
            return fail("track " + t.id + " is in missing folder " + t.folder_id);
    }

    d.sync_scores();
    auto next = static_cast<std::uint64_t>(std::max<std::int64_t>(1, doc.get_int("next_id", 1)));
    return Project(std::move(d), next);
}

bool Project::save(const std::string& path, std::string* error) const
{
    return files::write_file_atomic(path, json::write(to_json()), error);
}

std::optional<Project> Project::load(const std::string& path, std::string* error)
{
    auto text = files::read_file(path, error);
    if (!text) return std::nullopt;
    json::ParseError pe;
    auto doc = json::parse(*text, &pe);
    if (!doc) {
        if (error) *error = path + ": " + pe.to_string();
        return std::nullopt;
    }
    return from_json(*doc, error);
}

}  // namespace vcc::project
