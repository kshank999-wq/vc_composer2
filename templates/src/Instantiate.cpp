#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

#include "vcc/templates/Templates.h"

namespace vcc::templates {

using project::ResolutionStatus;

// --- Resolver -------------------------------------------------------------------

const InstalledPatch* InstrumentResolver::best_patch(const std::string& instrument_id,
                                                     const std::vector<std::string>& articulations) const
{
    const InstalledPatch* best = nullptr;
    int best_score = -1;
    for (const auto& p : installed_) {
        if (p.instrument_standard_id != instrument_id) continue;
        int coverage = 0;
        for (const auto& a : articulations)
            if (std::find(p.articulations.begin(), p.articulations.end(), a) != p.articulations.end()) ++coverage;
        int score = (p.favorite ? 1000 : 0) + (p.validation_status == "verified" ? 100 : 0) + coverage;
        if (score > best_score) {  // first wins ties: catalog order is stable
            best = &p;
            best_score = score;
        }
    }
    return best;
}

Resolution InstrumentResolver::resolve(const std::string& instrument_id, const std::vector<std::string>& artics,
                                       const std::vector<std::string>& fallbacks) const
{
    Resolution r;
    const InstrumentDef* def = instruments_.find(instrument_id);
    const std::string name = def ? def->name : instrument_id;
    auto unsupported = [&](const InstalledPatch& p) {
        std::vector<std::string> out;
        for (const auto& a : artics)
            if (std::find(p.articulations.begin(), p.articulations.end(), a) == p.articulations.end()) out.push_back(a);
        return out;
    };

    if (const InstalledPatch* p = best_patch(instrument_id, artics)) {
        r.status = ResolutionStatus::Exact;
        r.resolved_instrument_id = instrument_id;
        r.patch = p;
        r.unsupported_articulations = unsupported(*p);
        r.message = "Mapped to " + p->display + (p->validation_status == "verified" ? "" : " (" + p->validation_status + ", please verify)");
        return r;
    }
    for (const auto& fb : fallbacks) {
        if (const InstalledPatch* p = best_patch(fb, artics)) {
            const InstrumentDef* fdef = instruments_.find(fb);
            r.status = ResolutionStatus::Fallback;
            r.resolved_instrument_id = fb;
            r.patch = p;
            r.unsupported_articulations = unsupported(*p);
            r.message = "No " + name + " patch is mapped; using approved fallback " +
                        (fdef ? fdef->name : fb) + " (" + p->display + ")";
            return r;
        }
    }
    r.status = ResolutionStatus::Placeholder;
    r.resolved_instrument_id = instrument_id;
    int program = def ? def->gm_program : 0;
    if (program < 0) program = 0;  // drum kits use channel 10, program 0
    r.placeholder_program = program;
    r.unsupported_articulations = artics;
    r.message = "No library mapped for " + name +
                ": the built-in General MIDI placeholder plays this part. Articulations are kept as intents "
                "and will map when a patch is assigned.";
    return r;
}

// --- Instantiate --------------------------------------------------------------------

namespace {

std::string snake(const std::string& s)
{
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c)) out += static_cast<char>(std::tolower(c));
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.empty() || !std::isalpha(static_cast<unsigned char>(out[0]))) out = "t_" + out;
    return out;
}

notation::Clef clef_for(const std::string& c)
{
    if (c == "bass") return notation::Clef::Bass;
    if (c == "alto") return notation::Clef::Alto;
    if (c == "tenor") return notation::Clef::Tenor;
    if (c == "treble_8vb") return notation::Clef::TrebleOctaveDown;
    if (c == "treble_8va") return notation::Clef::TrebleOctaveUp;
    if (c == "bass_8vb") return notation::Clef::BassOctaveDown;
    if (c == "percussion") return notation::Clef::Percussion;
    return notation::Clef::Treble;
}

/// Octave clefs carry their octave as a sounding shift, the way the
/// original notation engine models them: a guitar reads treble 8vb and
/// sounds an octave below what is written.
std::int8_t octave_shift_for(const std::string& c)
{
    if (c == "treble_8vb" || c == "bass_8vb") return -1;
    if (c == "treble_8va") return 1;
    return 0;
}

/// The staves an instrument reads, ready before a note is entered
/// (clef, transposition, grand staff).
notation::Score score_for_instrument(const InstrumentDef& def, const std::string& track_name)
{
    notation::Score score;
    if (def.grand_staff) score.make_grand_staff();
    notation::Staff upper = score.staff(0);
    upper.clef = clef_for(def.clef);
    upper.octave_shift = octave_shift_for(def.clef);
    upper.transposition_semitones = static_cast<std::int8_t>(std::clamp(def.transposition, -48, 48));
    upper.name = def.grand_staff ? track_name + " (upper)" : track_name;
    score.set_staff(0, upper);
    if (def.grand_staff && score.staff_count() > 1) {
        notation::Staff lower = score.staff(1);
        lower.clef = clef_for(def.lower_clef);
        lower.octave_shift = octave_shift_for(def.lower_clef);
        lower.transposition_semitones = upper.transposition_semitones;
        lower.name = track_name + " (lower)";
        score.set_staff(1, lower);
    }
    return score;
}

int family_rank(const std::string& f)
{
    const auto& order = family_order();
    auto it = std::find(order.begin(), order.end(), f);
    return it == order.end() ? static_cast<int>(order.size()) : static_cast<int>(it - order.begin());
}

}  // namespace

notation::Score instrument_score(const InstrumentDef& def, const std::string& track_name)
{
    return score_for_instrument(def, track_name);
}

InstantiateResult instantiate(project::Project& proj, const Template& tpl, const InstrumentRegistry& instruments,
                              const InstantiateOptions& opt)
{
    InstantiateResult res;
    if (!tpl.instantiable()) {
        res.error = tpl.status == "curation_required"
                        ? "'" + tpl.display_name + "' is reserved for community-approved content and cannot be instantiated yet"
                        : "'" + tpl.display_name + "' has no tracks";
        return res;
    }
    const auto& data = proj.data();
    const bool has_music_tracks = std::any_of(data.tracks.begin(), data.tracks.end(), [](const project::Track& t) {
        return t.kind != project::TrackKind::Reference && t.kind != project::TrackKind::Video;
    });
    if (opt.mode == InstantiateMode::Create && has_music_tracks) {
        res.error = "the project already has tracks: choose Add to Existing (merge) or confirm Replace";
        return res;
    }
    if (opt.mode == InstantiateMode::Replace && !opt.replace_confirmed) {
        res.error = "replacing the existing orchestration needs explicit confirmation";
        return res;
    }

    // Which tracks will be created, grouped into family folders in the order
    // Supplement §8 gives, keeping template order within a family.
    struct Planned {
        const TemplateTrack* spec;
        const InstrumentDef* def;
        std::string folder_key;
    };
    std::vector<Planned> planned;
    for (const auto& tt : tpl.tracks) {
        if (tt.optional && !opt.include_optional) continue;
        const InstrumentDef* def = instruments.find(tt.instrument_standard_id);
        if (!def) {
            res.error = "unknown instrument " + tt.instrument_standard_id;  // registry validation prevents this
            return res;
        }
        planned.push_back({&tt, def, tt.group_id.empty() ? def->family : tt.group_id});
    }
    std::stable_sort(planned.begin(), planned.end(), [](const Planned& a, const Planned& b) {
        return family_rank(a.folder_key) < family_rank(b.folder_key);
    });
    std::vector<std::string> folder_keys;
    for (const auto& p : planned)
        if (std::find(folder_keys.begin(), folder_keys.end(), p.folder_key) == folder_keys.end())
            folder_keys.push_back(p.folder_key);

    // Buses: the template's own, or one per family plus a reverb return.
    std::vector<TemplateBus> buses = tpl.buses;
    const bool default_buses = buses.empty();
    if (default_buses) {
        buses.push_back({"fx_reverb", "Reverb", "fx", "master", {}, {"reverb"}});
        for (const auto& k : folder_keys)
            buses.push_back({"bus_" + k, family_display(k), "group", "master", {{"fx_reverb", -12.0}}, {"eq", "compressor"}});
    }

    // IDs are allocated before the edit so they are never reused after undo.
    auto& ids = proj.ids();
    std::map<std::string, std::string> bus_map, folder_map;
    for (const auto& b : buses) bus_map[b.id] = ids.next("bus");
    for (const auto& k : folder_keys) folder_map[k] = ids.next("fld");
    std::vector<std::pair<std::string, std::string>> track_ids;  // track id, version id
    for (std::size_t i = 0; i < planned.size(); ++i) track_ids.emplace_back(ids.next("trk"), ids.next("ver"));

    InstrumentResolver resolver(instruments, opt.installed);
    std::vector<std::string> warnings;

    bool ok = proj.edit("Insert template " + tpl.display_name, [&](project::ProjectData& d) {
        if (opt.mode == InstantiateMode::Replace) {
            // Explicitly confirmed: clear the orchestration, keep reference/temp
            // audio, audio recordings, video, cues and the master bus.
            auto removed_kind = [](const project::Track& t) {
                return t.kind == project::TrackKind::Instrument || t.kind == project::TrackKind::MidiOnly ||
                       t.kind == project::TrackKind::Folder;
            };
            // Buses go only if they were fed exclusively by the removed tracks
            // (directly, or through buses that go too). A bus the composer made
            // for something else - dialogue reference, a stem - stays.
            std::set<std::string> fed_by_removed, fed_by_kept;
            for (const auto& t : d.tracks) {
                auto& set = removed_kind(t) ? fed_by_removed : fed_by_kept;
                set.insert(t.output_bus_id);
                for (const auto& s : t.sends) set.insert(s.bus_id);
            }
            std::set<std::string> doomed;
            for (bool grew = true; grew;) {
                grew = false;
                for (const auto& b : d.buses) {
                    if (b.kind == "master" || doomed.count(b.id) || fed_by_kept.count(b.id)) continue;
                    if (!fed_by_removed.count(b.id)) continue;
                    doomed.insert(b.id);
                    fed_by_removed.insert(b.output);
                    for (const auto& s : b.sends) fed_by_removed.insert(s.bus_id);
                    grew = true;
                }
            }
            d.tracks.erase(std::remove_if(d.tracks.begin(), d.tracks.end(), removed_kind), d.tracks.end());
            d.buses.erase(std::remove_if(d.buses.begin(), d.buses.end(),
                                         [&](const project::Bus& b) { return doomed.count(b.id) > 0; }),
                          d.buses.end());
            for (auto& t : d.tracks)
                if (t.folder_id.size() && !d.find_track(t.folder_id)) t.folder_id.clear();
        }

        std::string master = "bus-master";
        if (!d.find_bus(master)) {
            project::Bus m;
            m.id = master;
            m.name = "Master";
            m.kind = "master";
            d.buses.push_back(m);
        }
        auto map_bus = [&](const std::string& tpl_bus) -> std::string {
            if (tpl_bus.empty() || tpl_bus == "master") return master;
            auto it = bus_map.find(tpl_bus);
            return it == bus_map.end() ? master : it->second;
        };
        for (const auto& b : buses) {
            project::Bus pb;
            pb.id = bus_map[b.id];
            pb.name = d.unique_bus_name(b.name);
            pb.kind = b.kind;
            pb.output = map_bus(b.output);
            for (const auto& s : b.sends) pb.sends.emplace_back(map_bus(s.first), s.second);
            for (const auto& i : b.insert_suggestions) {
                project::InsertSlot slot;
                slot.kind = i;
                pb.inserts.push_back(slot);
            }
            d.buses.push_back(pb);
        }

        int next_channel = 0;
        std::string open_folder;
        for (std::size_t i = 0; i < planned.size(); ++i) {
            // Each family folder sits directly above its tracks.
            if (planned[i].folder_key != open_folder) {
                open_folder = planned[i].folder_key;
                project::Track f;
                f.id = folder_map[open_folder];
                f.kind = project::TrackKind::Folder;
                f.name = d.unique_track_name(family_display(open_folder));
                f.family = open_folder;
                f.color = family_color(open_folder);
                f.include_in_export = false;
                d.tracks.push_back(f);
            }
            const TemplateTrack& tt = *planned[i].spec;
            const InstrumentDef& def = *planned[i].def;
            project::Track t;
            t.id = track_ids[i].first;
            t.name = d.unique_track_name(tt.name);
            t.kind = project::TrackKind::Instrument;
            t.folder_id = folder_map[planned[i].folder_key];
            t.family = def.family;
            t.role = tt.role;
            t.color = tt.color.empty() ? family_color(def.family) : tt.color;
            t.instrument_standard_id = tt.instrument_standard_id;
            t.articulation_intents = tt.articulation_map;
            t.fallback_instruments = tt.fallback_instruments;
            t.plugin_requirements = tt.plugin_requirements;
            t.gain_db = tt.gain_db;
            t.pan = tt.pan;
            t.output_bus_id = tt.output_bus_id.empty()
                                  ? (default_buses ? map_bus("bus_" + planned[i].folder_key) : master)
                                  : map_bus(tt.output_bus_id);
            t.template_origin = tpl.template_id + "/" + tt.id;

            if (tt.midi_channel_policy == "drum" || (def.unpitched && tt.midi_channel_policy == "auto")) {
                t.midi_channel = 9;
            } else if (tt.midi_channel_policy.rfind("fixed:", 0) == 0) {
                t.midi_channel = std::stoi(tt.midi_channel_policy.substr(6)) - 1;
            } else {
                if (next_channel == 9) ++next_channel;
                t.midi_channel = next_channel % 16;
                next_channel = (next_channel + 1) % 16;
            }

            Resolution r = resolver.resolve(tt.instrument_standard_id, tt.articulation_map, tt.fallback_instruments);
            t.assignment.status = r.status;
            t.assignment.resolved_instrument_id = r.resolved_instrument_id;
            t.assignment.patch_id = r.patch ? r.patch->patch_id : std::string{};
            t.assignment.patch_display = r.patch ? r.patch->display : "GM placeholder";
            t.assignment.placeholder_program = r.placeholder_program;
            t.assignment.unsupported_articulations = r.unsupported_articulations;
            t.assignment.message = r.message;
            if (r.status != ResolutionStatus::Exact) warnings.push_back(t.name + ": " + r.message);
            else if (!r.unsupported_articulations.empty()) {
                std::string list;
                for (const auto& a : r.unsupported_articulations) list += (list.empty() ? "" : ", ") + a;
                warnings.push_back(t.name + ": patch has no mapping for " + list);
            }

            t.versions.push_back({track_ids[i].second, "Version 1", {}, score_for_instrument(def, t.name), {}, {}, {},
                                  std::nullopt, {}});
            t.active_version_id = track_ids[i].second;
            d.tracks.push_back(std::move(t));
        }

        if (opt.mode != InstantiateMode::Merge && opt.apply_project_defaults) {
            if (!tpl.meter_options.empty())
                if (auto sig = TimeSignature::parse(tpl.meter_options.front())) d.meter = MeterMap(*sig);
            d.tempo = TempoMap(std::round((tpl.tempo_low + tpl.tempo_high) / 2.0));
            for (const auto& km : tpl.recommended_key_modes) {
                if (km.mode == "custom") continue;
                d.key = km.tonic + " " + km.mode;
                break;
            }
            if (d.name == "Untitled") d.name = tpl.display_name;
        }
        d.applied_templates.push_back(tpl.template_id);
        return true;
    });
    if (!ok) {
        res.error = "template could not be applied";
        return res;
    }
    res.ok = true;
    for (const auto& t : track_ids) res.track_ids.push_back(t.first);
    for (const auto& k : folder_keys) res.folder_ids.push_back(folder_map[k]);
    for (const auto& b : buses) res.bus_ids.push_back(bus_map[b.id]);
    res.warnings = std::move(warnings);
    return res;
}

// --- Save as custom template ----------------------------------------------------------

Template save_user_template(const project::Project& proj, const std::string& template_id,
                            const std::string& display_name, const std::string& description)
{
    const auto& d = proj.data();
    Template t;
    t.template_id = template_id;
    t.content_version = "1.0.0";
    t.display_name = display_name;
    t.description = description.empty() ? "Custom template saved from " + d.name : description;
    t.category = "cinematic";
    t.subcategory = "custom";
    t.variant = "hybrid";
    t.tags = {"custom", "user"};
    t.built_in = false;
    t.meter_options = {d.meter.at_bar(1).to_string()};
    int bpm = static_cast<int>(std::lround(d.tempo.bpm_at(0)));
    t.tempo_low = std::max(20, bpm - 10);
    t.tempo_high = std::min(320, bpm + 10);
    t.license_metadata = json::Object{{"content_license", "user"}, {"author", "user"}, {"notes", ""}};

    auto space = d.key.find(' ');
    if (space != std::string::npos) t.recommended_key_modes.push_back({d.key.substr(0, space), d.key.substr(space + 1)});

    std::map<std::string, std::string> bus_ids;
    for (const auto& b : d.buses) {
        if (b.kind == "master") continue;
        std::string id = snake(b.name);
        while (std::any_of(bus_ids.begin(), bus_ids.end(), [&](const auto& kv) { return kv.second == id; })) id += "_x";
        bus_ids[b.id] = id;
    }
    for (const auto& b : d.buses) {
        if (b.kind == "master") continue;
        TemplateBus tb{bus_ids[b.id], b.name, b.kind, "master", {}, {}};
        if (bus_ids.count(b.output)) tb.output = bus_ids[b.output];
        for (const auto& s : b.sends)
            if (bus_ids.count(s.bus_id)) tb.sends.emplace_back(bus_ids[s.bus_id], s.level_db);
        for (const auto& i : b.inserts) tb.insert_suggestions.push_back(i.kind);
        t.buses.push_back(tb);
    }

    std::set<std::string> used;
    for (const auto& tr : d.tracks) {
        if (tr.kind != project::TrackKind::Instrument && tr.kind != project::TrackKind::MidiOnly) continue;
        if (tr.instrument_standard_id.empty()) continue;  // nothing to describe it by
        TemplateTrack tt;
        tt.id = snake(tr.name);
        while (!used.insert(tt.id).second) tt.id += "_2";
        tt.name = tr.name;
        tt.instrument_standard_id = tr.instrument_standard_id;
        tt.role = tr.role.empty() ? "melody" : tr.role;
        tt.articulation_map = tr.articulation_intents;
        tt.fallback_instruments = tr.fallback_instruments;
        tt.plugin_requirements = tr.plugin_requirements;
        tt.gain_db = tr.gain_db;
        tt.pan = tr.pan;
        tt.color = tr.color;
        if (bus_ids.count(tr.output_bus_id)) tt.output_bus_id = bus_ids[tr.output_bus_id];
        tt.midi_channel_policy = tr.midi_channel == 9 ? "drum" : "auto";
        t.tracks.push_back(tt);
    }
    std::size_t n = t.tracks.size();
    t.ensemble_size = n <= 1 ? "solo" : n <= 6 ? "small" : n <= 12 ? "medium" : "large";
    return t;
}

}  // namespace vcc::templates
