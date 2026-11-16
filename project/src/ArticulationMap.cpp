#include "vcc/project/ArticulationMap.h"

#include <algorithm>

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

notation::ExpressionMap expression_map_for(const Track& track)
{
    if (track.articulation_map) return *track.articulation_map;
    return expression_map_named(track.expression_map);
}

std::string expression_map_key(const Track& track)
{
    const std::string drums = track.percussion_map.empty() ? std::string() : "|drums:" + track.percussion_map;
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
