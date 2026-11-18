#include "vcc/project/ArticulationMap.h"

#include <algorithm>
#include <cctype>

#include "vcc/project/Render.h"

namespace vcc::project {

namespace nt = notation;

namespace {

json::Value rule_to_json(const nt::ArticulationRule& r, bool with_articulation)
{
    json::Value o = json::Value::make_object();
    if (with_articulation) o.set("articulation", nt::to_string(r.articulation));
    o.set("action", nt::to_string(r.action));
    o.set("velocity_scale", r.velocity_scale);
    o.set("length_scale", r.length_scale);
    o.set("keyswitch_note", static_cast<int>(r.keyswitch_note));
    o.set("keyswitch_velocity", static_cast<int>(r.keyswitch_velocity));
    o.set("cc_number", static_cast<int>(r.cc_number));
    o.set("cc_value", static_cast<int>(r.cc_value));
    o.set("program", static_cast<int>(r.program));
    o.set("channel", static_cast<int>(r.channel));
    o.set("velocity_min", static_cast<int>(r.velocity_min));
    o.set("velocity_max", static_cast<int>(r.velocity_max));
    return o;
}

std::uint8_t byte(const json::Value& o, const char* key, int fallback)
{
    return static_cast<std::uint8_t>(std::clamp<std::int64_t>(o.get_int(key, fallback), 0, 127));
}

bool rule_from_json(const json::Value& o, nt::ArticulationRule& r, bool with_articulation)
{
    if (with_articulation && !nt::articulation_from_string(o.get_string("articulation"), r.articulation)) return false;
    if (!nt::articulation_action_from_string(o.get_string("action"), r.action)) return false;
    r.velocity_scale = o.get_double("velocity_scale", 1.0);
    r.length_scale = o.get_double("length_scale", 1.0);
    r.keyswitch_note = byte(o, "keyswitch_note", 0);
    r.keyswitch_velocity = byte(o, "keyswitch_velocity", 100);
    r.cc_number = byte(o, "cc_number", 0);
    r.cc_value = byte(o, "cc_value", 0);
    r.program = byte(o, "program", 0);
    r.channel = static_cast<std::uint8_t>(std::clamp<std::int64_t>(o.get_int("channel", 1), 0, 16));
    r.velocity_min = byte(o, "velocity_min", 1);
    r.velocity_max = byte(o, "velocity_max", 127);
    return true;
}

}  // namespace

std::vector<std::pair<std::string, std::string>> builtin_expression_maps()
{
    return {{"default", "Default (velocity dynamics)"}, {"orchestral_cc", "Orchestral (CC dynamics, keyswitches)"}};
}

json::Value expression_map_to_json(const nt::ExpressionMap& m)
{
    json::Value o = json::Value::make_object();
    o.set("name", m.name);
    o.set("library", m.library);
    o.set("dynamic_target", nt::to_string(m.dynamic_target));
    json::Value vel = json::Value::make_array(), cc = json::Value::make_array();
    for (std::size_t i = 0; i < nt::kDynamicLevelCount; ++i) {
        vel.push_back(static_cast<int>(m.dynamic_velocity[i]));
        cc.push_back(static_cast<int>(m.dynamic_cc_value[i]));
    }
    o.set("dynamic_velocity", std::move(vel));
    o.set("dynamics_cc", static_cast<int>(m.dynamics_cc));
    o.set("dynamic_cc_value", std::move(cc));
    o.set("hairpin_shape", nt::to_string(m.default_hairpin_shape));
    o.set("hairpin_resolution_ticks", static_cast<std::int64_t>(m.hairpin_resolution_ticks));
    o.set("default_length_scale", m.default_length_scale);
    o.set("minimum_note_ticks", static_cast<std::int64_t>(m.minimum_note_ticks));
    json::Value rules = json::Value::make_array();
    for (const auto& r : m.articulations) rules.push_back(rule_to_json(r, true));
    o.set("articulations", std::move(rules));
    if (m.has_natural) o.set("natural", rule_to_json(m.natural, false));
    return o;
}

std::optional<nt::ExpressionMap> expression_map_from_json(const json::Value& o)
{
    if (!o.is_object()) return std::nullopt;
    nt::ExpressionMap m;
    m.name = o.get_string("name", "Custom");
    m.library = o.get_string("library");
    if (!nt::dynamic_target_from_string(o.get_string("dynamic_target", "velocity"), m.dynamic_target)) return std::nullopt;
    const auto& vel = o["dynamic_velocity"];
    const auto& cc = o["dynamic_cc_value"];
    for (std::size_t i = 0; i < nt::kDynamicLevelCount; ++i) {
        if (i < vel.size()) m.dynamic_velocity[i] = static_cast<std::uint8_t>(std::clamp<std::int64_t>(vel.at(i).as_int(), 1, 127));
        if (i < cc.size()) m.dynamic_cc_value[i] = static_cast<std::uint8_t>(std::clamp<std::int64_t>(cc.at(i).as_int(), 0, 127));
    }
    m.dynamics_cc = byte(o, "dynamics_cc", 11);
    nt::curve_shape_from_string(o.get_string("hairpin_shape", nt::to_string(m.default_hairpin_shape)), m.default_hairpin_shape);
    m.hairpin_resolution_ticks = std::max<Ticks>(1, o.get_int("hairpin_resolution_ticks", m.hairpin_resolution_ticks));
    m.default_length_scale = o.get_double("default_length_scale", m.default_length_scale);
    m.minimum_note_ticks = std::max<Ticks>(0, o.get_int("minimum_note_ticks", m.minimum_note_ticks));
    for (const auto& j : o["articulations"].as_array()) {
        nt::ArticulationRule r;
        if (!rule_from_json(j, r, true)) return std::nullopt;
        m.set_rule(r);
    }
    if (const auto* n = o.get("natural")) {
        if (!rule_from_json(*n, m.natural, false)) return std::nullopt;
        m.has_natural = true;
    }
    if (!m.is_valid()) return std::nullopt;
    return m;
}

notation::ExpressionMap apply_accent_strength(notation::ExpressionMap map, int strength)
{
    strength = std::clamp(strength, 1, 100);
    if (strength == 50) return map;
    // The lift above the written dynamic, scaled: 50 is the map's own.
    for (auto& rule : map.articulations)
        if ((rule.articulation == notation::Articulation::Accent || rule.articulation == notation::Articulation::Marcato) &&
            (rule.action == notation::ArticulationAction::ScaleVelocity ||
             rule.action == notation::ArticulationAction::ScaleVelocityAndLength))
            rule.velocity_scale = std::clamp(1.0 + (rule.velocity_scale - 1.0) * strength / 50.0, 1.0, 4.0);
    return map;
}

std::vector<GmArticulation> gm_articulation_programs(int p)
{
    using A = notation::Articulation;
    std::vector<GmArticulation> out;
    const bool bowed = (p >= 40 && p <= 44) || (p >= 48 && p <= 51) || p == 110;
    if (bowed) {
        out.push_back({A::Pizzicato, 45});
        if (p != 44) out.push_back({A::Tremolo, 44});
    }
    if (p == 56 || p == 57 || p == 61) out.push_back({A::Mute, 59});
    if (p >= 24 && p <= 30) {
        out.push_back({A::Harmonic, 31});
        if (p != 28) out.push_back({A::Mute, 28});
    }
    return out;
}

notation::ExpressionMap with_gm_articulations(notation::ExpressionMap base, int program)
{
    const auto switches = gm_articulation_programs(program);
    if (switches.empty()) return base;
    for (const auto& s : switches) {
        notation::ArticulationRule r;
        r.articulation = s.articulation;
        r.action = notation::ArticulationAction::ProgramChange;
        r.program = static_cast<std::uint8_t>(s.program);
        base.set_rule(r);
    }
    // Back to the instrument's own sound for every other note.
    base.has_natural = true;
    base.natural = {};
    base.natural.action = notation::ArticulationAction::ProgramChange;
    base.natural.program = static_cast<std::uint8_t>(std::clamp(program, 0, 127));
    base.name += " + General MIDI articulations";
    return base;
}

notation::ExpressionMap expression_map_for(const Track& track)
{
    if (track.articulation_map) return apply_accent_strength(*track.articulation_map, track.accent_strength);
    return apply_accent_strength(expression_map_named(track.expression_map), track.accent_strength);
}

std::string expression_map_key(const Track& track)
{
    const std::string drums = (track.percussion_map.empty() ? std::string() : "|drums:" + track.percussion_map) +
                              (track.accent_strength == 50 ? std::string() : "|accent:" + std::to_string(track.accent_strength));
    if (!track.articulation_map) return track.expression_map + drums;
    return track.expression_map + "|" + expression_map_to_json(*track.articulation_map).to_text() + drums;
}

std::string MapHealth::summary() const
{
    if (ok()) return "Healthy: every written articulation is mapped";
    auto join = [](const std::vector<std::string>& v) {
        std::string s;
        for (const auto& x : v) s += (s.empty() ? "" : ", ") + x;
        return s;
    };
    std::string s;
    if (!unheard.empty()) s += join(unheard) + (unheard.size() == 1 ? " is" : " are") + " written but not heard";
    if (!unmapped_intents.empty())
        s += (s.empty() ? "" : "; ") + std::string("no rule for ") + join(unmapped_intents) + " (asked for by the template)";
    if (missing_natural) s += (s.empty() ? "" : "; ") + std::string("no natural switch back to sustain");
    return s;
}

MapHealth map_health(const Track& track)
{
    MapHealth h;
    const auto map = expression_map_for(track);
    auto mapped = [&](nt::Articulation a) { return map.rule_for(a) != nullptr; };
    if (const TrackVersion* v = track.active_version()) {
        nt::ArticulationMask used = 0;
        for (std::size_t s = 0; s < v->score.staff_count(); ++s)
            for (const auto& n : v->score.notes(s))
                if (!n.is_rest) used |= n.articulations;
        // Ornaments and bowings are notation that a player reads; only the
        // articulations that change a sound count as unheard.
        static const nt::Articulation kHeard[] = {
            nt::Articulation::Accent,   nt::Articulation::Staccato,  nt::Articulation::Staccatissimo,
            nt::Articulation::Tenuto,   nt::Articulation::Marcato,   nt::Articulation::Legato,
            nt::Articulation::Portato,  nt::Articulation::Spiccato,  nt::Articulation::Pizzicato,
            nt::Articulation::Tremolo,  nt::Articulation::Trill,     nt::Articulation::Harmonic,
            nt::Articulation::Mute};
        for (const auto a : kHeard)
            if (nt::has_articulation(used, a) && !mapped(a)) h.unheard.push_back(nt::to_string(a));
    }
    for (const auto& intent : track.articulation_intents) {
        nt::Articulation a;
        if (nt::articulation_from_string(intent, a) && !mapped(a)) h.unmapped_intents.push_back(intent);
    }
    const bool switches = std::any_of(map.articulations.begin(), map.articulations.end(),
                                      [](const nt::ArticulationRule& r) { return nt::selects_sound(r.action); });
    h.missing_natural = switches && !map.has_natural;
    return h;
}

}  // namespace vcc::project

namespace vcc::project {

std::string articulation_group(const notation::ExpressionMap& map, notation::ArticulationMask articulations)
{
    // As the renderer chooses: the map's rules in order, the first that
    // both matches a written articulation and selects a sound.
    for (const auto& rule : map.articulations)
        if (notation::has_articulation(articulations, rule.articulation) && notation::selects_sound(rule.action))
            return notation::to_string(rule.articulation);
    return "natural";
}

std::string articulation_route(const notation::ExpressionMap& map, notation::ArticulationMask articulations,
                               const std::vector<std::string>& groups)
{
    if (groups.empty()) return "natural";
    auto has = [&](const std::string& g) { return std::find(groups.begin(), groups.end(), g) != groups.end(); };
    const std::string chosen = articulation_group(map, articulations);
    if (chosen != "natural" && has(chosen)) return chosen;
    for (const auto& g : groups) {
        notation::Articulation a;
        if (g != "natural" && notation::articulation_from_string(g, a) && notation::has_articulation(articulations, a)) return g;
    }
    return has("natural") ? std::string("natural") : groups.front();
}

std::vector<Track> make_articulation_tracks(const Track& part, const std::vector<std::string>& groups, IdAllocator& ids)
{
    std::vector<Track> out;
    for (const auto& g : groups) {
        Track c = part;
        c.id = ids.next("trk");
        c.name = part.name + " - " + articulation_group_name(g);
        c.articulations_expanded = false;
        c.articulation_parent = part.id;
        c.articulation_group = g;
        c.derived_key.clear();
        c.automation.clear();
        c.print_signature.clear();
        c.instrument_unloaded = false;
        c.playback = "midi";
        c.record_armed = false;
        TrackVersion v;
        v.id = ids.next("ver");
        v.name = "Version 1";
        // Its own notes from now on: the part's notes that play this sound.
        if (const auto* pv = part.active_version()) {
            v.score = pv->score;
            v.performance = pv->performance;
            const auto map = expression_map_for(part);
            std::vector<notation::EventId> drop;
            for (std::size_t st = 0; st < v.score.staff_count(); ++st)
                for (const auto& n : v.score.notes(st))
                    if (!n.is_rest && articulation_route(map, n.articulations, groups) != g) drop.push_back(n.id);
            for (const auto id : drop) v.score.remove_note(id);
        }
        c.versions = {std::move(v)};
        c.active_version_id = c.versions.front().id;
        out.push_back(std::move(c));
    }
    return out;
}

std::string articulation_group_name(const std::string& group)
{
    if (group.empty()) return group;
    std::string s = group;
    s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

}  // namespace vcc::project
