#include "vcc/project/Project.h"

#include "vcc/project/Automation.h"

#include <algorithm>
#include <set>

#include "vcc/core/Files.h"
#include "vcc/notation/ScoreFile.h"
#include "vcc/theory/Pitch.h"

namespace vcc::project {

bool operator==(const NoteOverride& a, const NoteOverride& b) noexcept
{
    return a.onset == b.onset && a.length == b.length && a.velocity == b.velocity && a.cc == b.cc;
}
bool operator!=(const NoteOverride& a, const NoteOverride& b) noexcept { return !(a == b); }

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

const std::vector<std::string>& cue_statuses()
{
    static const std::vector<std::string> statuses = {"spotted", "writing", "sent", "awaiting_feedback",
                                                      "revisions", "approved", "locked"};
    return statuses;
}

const Cue* ProjectData::find_cue(const std::string& id) const noexcept
{
    for (const auto& c : cues)
        if (c.id == id) return &c;
    return nullptr;
}
Cue* ProjectData::find_cue(const std::string& id) noexcept
{
    for (auto& c : cues)
        if (c.id == id) return &c;
    return nullptr;
}
const PictureVersion* ProjectData::active_picture() const noexcept
{
    for (const auto& p : pictures)
        if (p.id == active_picture_id) return &p;
    return nullptr;
}
PictureVersion* ProjectData::active_picture() noexcept
{
    for (auto& p : pictures)
        if (p.id == active_picture_id) return &p;
    return nullptr;
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

bool Project::edit_coalescing(const std::string& label, const std::function<bool(ProjectData&)>& change)
{
    if (undo_.empty() || undo_.back().label != label || !redo_.empty()) return edit(label, change);
    ProjectData working = data_;
    bool ok = false;
    try {
        ok = change(working);
    } catch (...) {
        return false;
    }
    if (!ok) return false;
    working.sync_scores();
    data_ = std::move(working);  // the existing step's "before" stays as it was
    return true;
}

void Project::modify_unrecorded(const std::function<void(ProjectData&)>& change)
{
    change(data_);
    data_.sync_scores();
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
            t.versions.push_back({version, "Version 1", {}, score_for_track(t.name), {}, {}, {}, std::nullopt, {}, {}});
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
    std::vector<std::string> phrase_ids;
    for (std::size_t i = 0; i < t->active_version()->phrases.size(); ++i) phrase_ids.push_back(ids_.next("phr"));
    std::string label = name.empty() ? "Version " + std::to_string(t->versions.size() + 1) : name;
    bool ok = edit("Duplicate version of " + t->name, [&](ProjectData& d) {
        Track* tr = d.find_track(track_id);
        TrackVersion copy = *tr->active_version();
        copy.id = id;
        copy.name = label;
        for (std::size_t i = 0; i < copy.regions.size(); ++i) copy.regions[i].id = region_ids[i];
        for (std::size_t i = 0; i < copy.phrases.size(); ++i) copy.phrases[i].id = phrase_ids[i];
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

json::Value sends_to_json(const std::vector<Send>& sends)
{
    json::Array a;
    for (const auto& s : sends)
        a.emplace_back(json::Object{{"to", s.bus_id}, {"level_db", s.level_db}, {"pre_fader", s.pre_fader},
                                    {"mute", s.mute}, {"pan", s.pan}, {"pan_follow", s.pan_follow}});
    return a;
}

std::vector<Send> sends_from_json(const json::Value* v)
{
    std::vector<Send> out;
    if (v)
        for (const auto& j : v->as_array()) {
            Send s(j.get_string("to"), j.get_double("level_db", -12.0));
            s.pre_fader = j.get_bool("pre_fader");
            s.mute = j.get_bool("mute");
            s.pan = j.get_double("pan");
            s.pan_follow = j.get_bool("pan_follow", true);
            out.push_back(std::move(s));
        }
    return out;
}

json::Value band_to_json(const EqBand& b)
{
    return json::Object{{"on", b.on}, {"freq", b.freq}, {"gain_db", b.gain_db}, {"q", b.q}, {"bell", b.bell}};
}

EqBand band_from_json(const json::Value* v, const EqBand& fallback)
{
    if (!v) return fallback;
    EqBand b;
    b.on = v->get_bool("on", fallback.on);
    b.freq = v->get_double("freq", fallback.freq);
    b.gain_db = v->get_double("gain_db", fallback.gain_db);
    b.q = v->get_double("q", fallback.q);
    b.bell = v->get_bool("bell", fallback.bell);
    return b;
}

json::Value strip_to_json(const ChannelStrip& s)
{
    return json::Object{{"trim_db", s.trim_db},
                        {"phase_invert", s.phase_invert},
                        {"hpf_on", s.hpf_on},
                        {"hpf_hz", s.hpf_hz},
                        {"lpf_on", s.lpf_on},
                        {"lpf_hz", s.lpf_hz},
                        {"eq_on", s.eq_on},
                        {"lf", band_to_json(s.lf)},
                        {"lmf", band_to_json(s.lmf)},
                        {"hmf", band_to_json(s.hmf)},
                        {"hf", band_to_json(s.hf)},
                        {"comp_on", s.comp_on},
                        {"comp_threshold_db", s.comp_threshold_db},
                        {"comp_ratio", s.comp_ratio},
                        {"comp_attack_ms", s.comp_attack_ms},
                        {"comp_release_ms", s.comp_release_ms},
                        {"comp_makeup_db", s.comp_makeup_db},
                        {"gate_on", s.gate_on},
                        {"gate_threshold_db", s.gate_threshold_db},
                        {"gate_range_db", s.gate_range_db},
                        {"gate_release_ms", s.gate_release_ms},
                        {"dynamics_pre_eq", s.dynamics_pre_eq}};
}

ChannelStrip strip_from_json(const json::Value* v)
{
    ChannelStrip s;
    if (!v) return s;
    const ChannelStrip d;
    s.trim_db = v->get_double("trim_db", d.trim_db);
    s.phase_invert = v->get_bool("phase_invert", d.phase_invert);
    s.hpf_on = v->get_bool("hpf_on", d.hpf_on);
    s.hpf_hz = v->get_double("hpf_hz", d.hpf_hz);
    s.lpf_on = v->get_bool("lpf_on", d.lpf_on);
    s.lpf_hz = v->get_double("lpf_hz", d.lpf_hz);
    s.eq_on = v->get_bool("eq_on", d.eq_on);
    s.lf = band_from_json(v->get("lf"), d.lf);
    s.lmf = band_from_json(v->get("lmf"), d.lmf);
    s.hmf = band_from_json(v->get("hmf"), d.hmf);
    s.hf = band_from_json(v->get("hf"), d.hf);
    s.comp_on = v->get_bool("comp_on", d.comp_on);
    s.comp_threshold_db = v->get_double("comp_threshold_db", d.comp_threshold_db);
    s.comp_ratio = v->get_double("comp_ratio", d.comp_ratio);
    s.comp_attack_ms = v->get_double("comp_attack_ms", d.comp_attack_ms);
    s.comp_release_ms = v->get_double("comp_release_ms", d.comp_release_ms);
    s.comp_makeup_db = v->get_double("comp_makeup_db", d.comp_makeup_db);
    s.gate_on = v->get_bool("gate_on", d.gate_on);
    s.gate_threshold_db = v->get_double("gate_threshold_db", d.gate_threshold_db);
    s.gate_range_db = v->get_double("gate_range_db", d.gate_range_db);
    s.gate_release_ms = v->get_double("gate_release_ms", d.gate_release_ms);
    s.dynamics_pre_eq = v->get_bool("dynamics_pre_eq", d.dynamics_pre_eq);
    return s;
}

json::Value plugin_to_json(const PluginRef& p)
{
    return json::Object{{"format", p.format}, {"identifier", p.identifier}, {"name", p.name},
                        {"vendor", p.vendor}, {"state", p.state_base64}};
}

PluginRef plugin_from_json(const json::Value& pj)
{
    PluginRef p;
    p.format = pj.get_string("format");
    p.identifier = pj.get_string("identifier");
    p.name = pj.get_string("name");
    p.vendor = pj.get_string("vendor");
    p.state_base64 = pj.get_string("state");
    return p;
}

json::Value inserts_to_json(const std::vector<InsertSlot>& inserts)
{
    json::Array a;
    for (const auto& i : inserts) {
        json::Object o{{"kind", i.kind}, {"plugin_ref", i.plugin_ref}, {"suggestion", i.suggestion},
                       {"bypass", i.bypass}};
        if (!i.plugin.empty()) o.set("plugin", plugin_to_json(i.plugin));
        if (!i.params.empty()) {
            json::Object params;
            for (const auto& [k, val] : i.params) params.set(k, val);
            o.set("params", params);
        }
        a.emplace_back(o);
    }
    return a;
}

std::vector<InsertSlot> inserts_from_json(const json::Value* v)
{
    std::vector<InsertSlot> out;
    if (!v) return out;
    for (const auto& j : v->as_array()) {
        InsertSlot i;
        i.kind = j.get_string("kind");
        i.plugin_ref = j.get_string("plugin_ref");
        i.suggestion = j.get_bool("suggestion", true);
        i.bypass = j.get_bool("bypass");
        if (const auto* pj = j.get("plugin")) i.plugin = plugin_from_json(*pj);
        if (const auto* pj = j.get("params"))
            for (const auto& [k, val] : pj->as_object()) i.params[k] = val.as_double();
        out.push_back(std::move(i));
    }
    return out;
}

json::Value automation_to_json(const std::vector<AutomationLane>& lanes)
{
    json::Array a;
    for (const auto& l : lanes) {
        json::Array pts;
        for (const auto& p : l.points) {
            json::Object o{{"tick", p.tick}, {"value", p.value}};
            if (p.shape != "linear") o.set("shape", p.shape);
            pts.emplace_back(o);
        }
        a.emplace_back(json::Object{{"id", l.id}, {"param", l.param}, {"name", l.name}, {"enabled", l.enabled},
                                    {"shown", l.shown}, {"points", pts}});
    }
    return a;
}

/// Reads lanes; returns an error message, or empty.
std::string automation_from_json(const json::Value* v, std::vector<AutomationLane>& out,
                                 const std::function<bool(const std::string&)>& unique)
{
    if (!v) return {};
    for (const auto& j : v->as_array()) {
        AutomationLane l;
        l.id = j.get_string("id");
        if (!unique(l.id)) return "missing or duplicate automation lane id '" + l.id + "'";
        l.param = j.get_string("param");
        if (!param_info(l.param)) return "automation lane " + l.id + ": unknown parameter '" + l.param + "'";
        if (find_lane(out, l.param)) return "two automation lanes for '" + l.param + "'";
        l.name = j.get_string("name");
        l.enabled = j.get_bool("enabled", true);
        l.shown = j.get_bool("shown", true);
        for (const auto& pj : j.get("points") ? j.get("points")->as_array() : json::Array{})
            l.points.push_back({pj.get_int("tick"), pj.get_double("value"), pj.get_string("shape", "linear")});
        normalise(l);
        out.push_back(std::move(l));
    }
    return {};
}

bool valid_automation_mode(const std::string& m)
{
    const auto& modes = automation_modes();
    return std::find(modes.begin(), modes.end(), m) != modes.end();
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
    root.set("delay_compensation", d.delay_compensation);
    root.set("key_zoom", d.key_zoom);
    root.set("loudness_target", d.loudness_target);

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
        if (!t.instrument_plugin.empty())
            o.set("instrument_plugin", json::Object{{"format", t.instrument_plugin.format},
                                                    {"identifier", t.instrument_plugin.identifier},
                                                    {"name", t.instrument_plugin.name},
                                                    {"vendor", t.instrument_plugin.vendor},
                                                    {"state", t.instrument_plugin.state_base64}});
        o.set("output_bus_id", t.output_bus_id);
        o.set("sends", sends_to_json(t.sends));
        o.set("strip", strip_to_json(t.strip));
        o.set("inserts", inserts_to_json(t.inserts));
        o.set("gain_db", t.gain_db);
        o.set("pan", t.pan);
        o.set("mute", t.mute);
        o.set("solo", t.solo);
        o.set("input", t.input);
        o.set("input_stereo", t.input_stereo);
        o.set("monitor", t.monitor);
        o.set("playback", t.playback);
        if (t.printed_fx) o.set("printed_fx", true);
        if (!t.midi_output.empty()) o.set("midi_output", t.midi_output);
        if (!t.automation.empty()) o.set("automation", automation_to_json(t.automation));
        o.set("automation_mode", t.automation_mode);
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
            {
                json::Array perf;
                for (const auto& [id, ov] : v.performance) {
                    if (ov.empty() || !v.score.find_note(id)) continue;
                    json::Object po{{"note", static_cast<std::int64_t>(id)}};
                    if (ov.onset != 0) po.set("onset", ov.onset);
                    if (ov.length != 0) po.set("length", ov.length);
                    if (ov.velocity != 0) po.set("velocity", ov.velocity);
                    if (ov.cc != 0) po.set("cc", ov.cc);
                    perf.emplace_back(std::move(po));
                }
                if (!perf.empty()) vo.set("performance", perf);
            }
            if (!v.audio_clips.empty()) {
                json::Array clips;
                for (const auto& c : v.audio_clips)
                    clips.emplace_back(json::Object{{"id", c.id},
                                                    {"name", c.name},
                                                    {"file", c.file},
                                                    {"start_seconds", c.start_seconds},
                                                    {"source_offset", c.source_offset},
                                                    {"length", c.length},
                                                    {"gain_db", c.gain_db},
                                                    {"fade_in", c.fade_in},
                                                    {"fade_out", c.fade_out},
                                                    {"lane", c.lane},
                                                    {"muted", c.muted},
                                                    {"channels", c.channels},
                                                    {"file_sample_rate", c.file_sample_rate}});
                vo.set("audio_clips", clips);
            }
            if (!v.phrases.empty()) {
                json::Array phrases;
                for (const auto& ph : v.phrases)
                    phrases.emplace_back(
                        json::Object{{"id", ph.id}, {"name", ph.name}, {"start", ph.start}, {"end", ph.end}});
                vo.set("phrases", phrases);
            }
            versions.emplace_back(vo);
        }
        o.set("versions", versions);
        o.set("active_version_id", t.active_version_id);
        tracks.emplace_back(o);
    }
    root.set("tracks", tracks);

    json::Array buses;
    for (const auto& b : d.buses) {
        buses.emplace_back(json::Object{{"id", b.id},
                                        {"name", b.name},
                                        {"kind", b.kind},
                                        {"output", b.output},
                                        {"sends", sends_to_json(b.sends)},
                                        {"inserts", inserts_to_json(b.inserts)},
                                        {"gain_db", b.gain_db},
                                        {"pan", b.pan},
                                        {"mute", b.mute},
                                        {"solo", b.solo},
                                        {"strip", strip_to_json(b.strip)},
                                        {"color", b.color},
                                        {"automation", automation_to_json(b.automation)},
                                        {"automation_mode", b.automation_mode}});
    }
    root.set("buses", buses);

    root.set("production", json::Object{{"title", d.production.title}, {"client", d.production.client},
                                        {"director", d.production.director}, {"composer", d.production.composer}});
    json::Array reels, scenes;
    for (const auto& r : d.reels) reels.emplace_back(json::Object{{"id", r.id}, {"name", r.name}});
    for (const auto& sc : d.scenes)
        scenes.emplace_back(json::Object{{"id", sc.id}, {"reel_id", sc.reel_id}, {"name", sc.name}});
    root.set("reels", reels);
    root.set("scenes", scenes);

    json::Array cues;
    for (const auto& c : d.cues) {
        json::Array deliverables;
        for (const auto& dv : c.deliverables) deliverables.emplace_back(json::Object{{"name", dv.name}, {"done", dv.done}});
        cues.emplace_back(json::Object{{"id", c.id}, {"code", c.code}, {"title", c.title}, {"start", c.start},
                                       {"end", c.end}, {"status", c.status}, {"notes", c.notes},
                                       {"description", c.description}, {"reel_id", c.reel_id},
                                       {"scene_id", c.scene_id}, {"spot_in", c.spot_in}, {"spot_out", c.spot_out},
                                       {"revision", c.revision}, {"deadline", c.deadline}, {"composer", c.composer},
                                       {"template_id", c.template_id}, {"usage", c.usage},
                                       {"deliverables", deliverables}, {"transition_in", c.transition_in},
                                       {"transition_out", c.transition_out}});
    }
    root.set("cues", cues);

    json::Array pictures;
    for (const auto& p : d.pictures) {
        json::Array regions;
        for (const auto& r : p.changed_regions)
            regions.emplace_back(json::Object{{"start", r.start_seconds}, {"end", r.end_seconds}, {"note", r.note}});
        pictures.emplace_back(json::Object{{"id", p.id}, {"name", p.name}, {"path", p.path},
                                           {"rate", to_string(p.rate)}, {"start_timecode", p.start_timecode},
                                           {"offset_seconds", p.offset_seconds}, {"duration_frames", p.duration_frames},
                                           {"width", p.width}, {"height", p.height}, {"changed_regions", regions},
                                           {"notes", p.notes}});
    }
    root.set("pictures", pictures);
    root.set("active_picture_id", d.active_picture_id);
    json::Array markers;
    for (const auto& m : d.markers)
        markers.emplace_back(json::Object{{"id", m.id}, {"kind", m.kind}, {"seconds", m.seconds}, {"label", m.label},
                                          {"cue_id", m.cue_id}, {"streamer_seconds", m.streamer_seconds}});
    {
        json::Array stems;
        for (const auto& st : d.stems)
            stems.emplace_back(json::Object{{"id", st.id},
                                            {"name", st.name},
                                            {"sources", string_array(st.sources)},
                                            {"include_effects", st.include_effects}});
        root.set("stems", stems);
    }
    root.set("markers", markers);

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
    d.delay_compensation = doc.get_bool("delay_compensation", true);
    d.key_zoom = std::clamp(doc.get_double("key_zoom", 1.0), 1.0, 3.0);
    d.loudness_target = std::clamp(doc.get_double("loudness_target", -14.0), -36.0, -6.0);
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
        if (const auto* pj = j.get("instrument_plugin")) t.instrument_plugin = plugin_from_json(*pj);
        t.output_bus_id = j.get_string("output_bus_id");
        t.sends = sends_from_json(j.get("sends"));
        t.strip = strip_from_json(j.get("strip"));
        t.inserts = inserts_from_json(j.get("inserts"));
        t.gain_db = j.get_double("gain_db");
        t.pan = j.get_double("pan");
        t.mute = j.get_bool("mute");
        t.solo = j.get_bool("solo");
        t.input = static_cast<int>(j.get_int("input", 0));
        t.input_stereo = j.get_bool("input_stereo");
        t.monitor = j.get_string("monitor", "auto");
        t.playback = j.get_string("playback", "both");
        t.printed_fx = j.get_bool("printed_fx", false);
        t.midi_output = j.get_string("midi_output", "");
        if (t.playback != "midi" && t.playback != "audio") t.playback = "both";
        if (t.monitor != "auto" && t.monitor != "input" && t.monitor != "off")
            return fail("track " + t.id + ": unknown monitor mode '" + t.monitor + "'");
        if (auto e = automation_from_json(j.get("automation"), t.automation, unique); !e.empty())
            return fail("track " + t.id + ": " + e);
        t.automation_mode = j.get_string("automation_mode", "read");
        if (!valid_automation_mode(t.automation_mode))
            return fail("track " + t.id + ": unknown automation mode '" + t.automation_mode + "'");
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
            for (const auto& pj : vj.get("performance") ? vj.get("performance")->as_array() : json::Array{}) {
                const auto id = static_cast<notation::EventId>(pj.get_int("note"));
                if (id == notation::kInvalidEventId) return fail("version " + v.id + ": performance entry without a note");
                NoteOverride o;
                o.onset = pj.get_int("onset");
                o.length = pj.get_int("length");
                o.velocity = static_cast<int>(std::clamp<std::int64_t>(pj.get_int("velocity"), -127, 127));
                o.cc = static_cast<int>(std::clamp<std::int64_t>(pj.get_int("cc"), -127, 127));
                if (!o.empty() && v.score.find_note(id)) v.performance[id] = o;
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
            for (const auto& pj : vj.get("phrases") ? vj.get("phrases")->as_array() : json::Array{}) {
                Phrase ph;
                ph.id = pj.get_string("id");
                if (!unique(ph.id)) return fail("missing or duplicate phrase id '" + ph.id + "'");
                ph.name = pj.get_string("name");
                ph.start = pj.get_int("start");
                ph.end = pj.get_int("end");
                if (ph.end <= ph.start) return fail("phrase " + ph.id + ": empty range");
                v.phrases.push_back(ph);
            }
            for (const auto& cj : vj.get("audio_clips") ? vj.get("audio_clips")->as_array() : json::Array{}) {
                AudioClip c;
                c.id = cj.get_string("id");
                if (!unique(c.id)) return fail("missing or duplicate audio clip id '" + c.id + "'");
                c.name = cj.get_string("name");
                c.file = cj.get_string("file");
                c.start_seconds = cj.get_double("start_seconds");
                c.source_offset = cj.get_double("source_offset");
                c.length = cj.get_double("length");
                c.gain_db = cj.get_double("gain_db");
                c.fade_in = cj.get_double("fade_in");
                c.fade_out = cj.get_double("fade_out");
                c.lane = static_cast<int>(cj.get_int("lane"));
                c.muted = cj.get_bool("muted");
                c.channels = static_cast<int>(cj.get_int("channels"));
                c.file_sample_rate = cj.get_double("file_sample_rate");
                if (c.file.empty()) return fail("audio clip " + c.id + ": no file");
                if (c.length <= 0.0 || c.source_offset < 0.0) return fail("audio clip " + c.id + ": empty range");
                v.audio_clips.push_back(std::move(c));
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
        b.pan = j.get_double("pan");
        b.mute = j.get_bool("mute");
        b.solo = j.get_bool("solo");
        b.strip = strip_from_json(j.get("strip"));
        b.color = j.get_string("color");
        b.inserts = inserts_from_json(j.get("inserts"));
        if (auto e = automation_from_json(j.get("automation"), b.automation, unique); !e.empty())
            return fail("bus " + b.id + ": " + e);
        b.automation_mode = j.get_string("automation_mode", "read");
        if (!valid_automation_mode(b.automation_mode))
            return fail("bus " + b.id + ": unknown automation mode '" + b.automation_mode + "'");
        d.buses.push_back(std::move(b));
    }
    if (const auto* pj = doc.get("production")) {
        d.production.title = pj->get_string("title");
        d.production.client = pj->get_string("client");
        d.production.director = pj->get_string("director");
        d.production.composer = pj->get_string("composer");
    }
    for (const auto& j : doc.get("reels") ? doc.get("reels")->as_array() : json::Array{}) {
        Reel r{j.get_string("id"), j.get_string("name")};
        if (!unique(r.id)) return fail("missing or duplicate reel id '" + r.id + "'");
        d.reels.push_back(r);
    }
    for (const auto& j : doc.get("scenes") ? doc.get("scenes")->as_array() : json::Array{}) {
        Scene sc{j.get_string("id"), j.get_string("reel_id"), j.get_string("name")};
        if (!unique(sc.id)) return fail("missing or duplicate scene id '" + sc.id + "'");
        d.scenes.push_back(sc);
    }
    for (const auto& j : doc.get("cues") ? doc.get("cues")->as_array() : json::Array{}) {
        Cue c;
        c.id = j.get_string("id");
        c.code = j.get_string("code");
        c.title = j.get_string("title");
        c.start = j.get_int("start");
        c.end = j.get_int("end");
        c.status = j.get_string("status", "writing");
        c.notes = j.get_string("notes");
        c.description = j.get_string("description");
        c.reel_id = j.get_string("reel_id");
        c.scene_id = j.get_string("scene_id");
        c.spot_in = j.get_string("spot_in");
        c.spot_out = j.get_string("spot_out");
        c.revision = static_cast<int>(j.get_int("revision", 1));
        c.deadline = j.get_string("deadline");
        c.composer = j.get_string("composer");
        c.template_id = j.get_string("template_id");
        c.usage = j.get_string("usage", "BI");
        c.transition_in = j.get_string("transition_in");
        c.transition_out = j.get_string("transition_out");
        for (const auto& dv : j.get("deliverables") ? j.get("deliverables")->as_array() : json::Array{})
            c.deliverables.push_back({dv.get_string("name"), dv.get_bool("done")});
        if (!unique(c.id)) return fail("missing or duplicate cue id '" + c.id + "'");
        if (std::find(cue_statuses().begin(), cue_statuses().end(), c.status) == cue_statuses().end())
            return fail("cue " + c.id + ": unknown status '" + c.status + "'");
        if (c.end < c.start) return fail("cue " + c.id + ": ends before it starts");
        if (!c.reel_id.empty() && std::none_of(d.reels.begin(), d.reels.end(), [&](const Reel& r) { return r.id == c.reel_id; }))
            return fail("cue " + c.id + " is in missing reel " + c.reel_id);
        d.cues.push_back(c);
    }
    for (const auto& j : doc.get("pictures") ? doc.get("pictures")->as_array() : json::Array{}) {
        PictureVersion p;
        p.id = j.get_string("id");
        if (!unique(p.id)) return fail("missing or duplicate picture id '" + p.id + "'");
        p.name = j.get_string("name");
        p.path = j.get_string("path");
        auto rate = frame_rate_from_string(j.get_string("rate", "24"));
        if (!rate) return fail("picture " + p.id + ": invalid frame rate");
        p.rate = *rate;
        p.start_timecode = j.get_string("start_timecode", "01:00:00:00");
        p.offset_seconds = j.get_double("offset_seconds");
        p.duration_frames = j.get_int("duration_frames");
        p.width = static_cast<int>(j.get_int("width"));
        p.height = static_cast<int>(j.get_int("height"));
        p.notes = j.get_string("notes");
        for (const auto& r : j.get("changed_regions") ? j.get("changed_regions")->as_array() : json::Array{})
            p.changed_regions.push_back({r.get_double("start"), r.get_double("end"), r.get_string("note")});
        d.pictures.push_back(p);
    }
    d.active_picture_id = doc.get_string("active_picture_id");
    if (!d.active_picture_id.empty() && !d.active_picture()) return fail("active picture not found");
    for (const auto& j : doc.get("stems") ? doc.get("stems")->as_array() : json::Array{}) {
        StemDef st;
        st.id = j.get_string("id");
        if (!unique(st.id)) return fail("missing or duplicate stem id '" + st.id + "'");
        st.name = j.get_string("name");
        st.sources = j.get_string_array("sources");
        st.include_effects = j.get_bool("include_effects", true);
        d.stems.push_back(std::move(st));
    }
    for (const auto& j : doc.get("markers") ? doc.get("markers")->as_array() : json::Array{}) {
        Marker m;
        m.id = j.get_string("id");
        if (!unique(m.id)) return fail("missing or duplicate marker id '" + m.id + "'");
        m.kind = j.get_string("kind", "hit");
        m.seconds = j.get_double("seconds");
        m.label = j.get_string("label");
        m.cue_id = j.get_string("cue_id");
        m.streamer_seconds = j.get_double("streamer_seconds", 2.0);
        d.markers.push_back(m);
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
