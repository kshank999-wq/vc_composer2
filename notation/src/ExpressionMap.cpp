#include "vcc/notation/ExpressionMap.h"

#include <algorithm>
#include <cmath>

namespace vcc::notation {

const char* to_string(DynamicTarget target) noexcept
{
    switch (target) {
        case DynamicTarget::Velocity:                 return "velocity";
        case DynamicTarget::ControlChange:            return "control change";
        case DynamicTarget::VelocityAndControlChange: break;
    }
    return "velocity + control change";
}

const char* to_string(ArticulationAction action) noexcept
{
    switch (action) {
        case ArticulationAction::ScaleVelocity:          return "scale velocity";
        case ArticulationAction::ScaleLength:            return "scale length";
        case ArticulationAction::Keyswitch:              return "keyswitch";
        case ArticulationAction::ControlChange:          return "control change";
        case ArticulationAction::ScaleVelocityAndLength: break;
    }
    return "scale velocity + length";
}

bool dynamic_target_from_string(const std::string& text, DynamicTarget& out) noexcept
{
    static const DynamicTarget targets[] = {DynamicTarget::Velocity, DynamicTarget::ControlChange,
                                            DynamicTarget::VelocityAndControlChange};
    for (const DynamicTarget target : targets) {
        if (text == to_string(target)) {
            out = target;
            return true;
        }
    }
    return false;
}

bool articulation_action_from_string(const std::string& text, ArticulationAction& out) noexcept
{
    static const ArticulationAction actions[] = {
        ArticulationAction::ScaleVelocity, ArticulationAction::ScaleLength,
        ArticulationAction::Keyswitch, ArticulationAction::ControlChange,
        ArticulationAction::ScaleVelocityAndLength};
    for (const ArticulationAction action : actions) {
        if (text == to_string(action)) {
            out = action;
            return true;
        }
    }
    return false;
}

bool ArticulationRule::is_valid() const noexcept
{
    if (!(velocity_scale > 0.0) || velocity_scale > 4.0)
        return false;
    if (!(length_scale > 0.0) || length_scale > 4.0)
        return false;
    if (keyswitch_note > 127 || keyswitch_velocity > 127)
        return false;
    if (cc_number > 127 || cc_value > 127)
        return false;

    // A keyswitch rule with velocity 0 would send a note-off, which selects
    // nothing and silently does the opposite of what was intended.
    if (action == ArticulationAction::Keyswitch && keyswitch_velocity == 0)
        return false;

    return true;
}

std::uint8_t ExpressionMap::velocity_for(DynamicLevel level) const noexcept
{
    const auto index = static_cast<std::size_t>(level);
    if (index >= kDynamicLevelCount)
        return dynamic_velocity[static_cast<std::size_t>(DynamicLevel::MezzoForte)];
    return dynamic_velocity[index];
}

std::uint8_t ExpressionMap::cc_value_for(DynamicLevel level) const noexcept
{
    const auto index = static_cast<std::size_t>(level);
    if (index >= kDynamicLevelCount)
        return dynamic_cc_value[static_cast<std::size_t>(DynamicLevel::MezzoForte)];
    return dynamic_cc_value[index];
}

const ArticulationRule* ExpressionMap::rule_for(Articulation articulation) const noexcept
{
    for (const ArticulationRule& rule : articulations) {
        if (rule.articulation == articulation)
            return &rule;
    }
    return nullptr;
}

void ExpressionMap::set_rule(const ArticulationRule& rule)
{
    const auto existing =
        std::find_if(articulations.begin(), articulations.end(),
                     [&rule](const ArticulationRule& other) {
                         return other.articulation == rule.articulation;
                     });

    if (existing != articulations.end())
        *existing = rule;
    else
        articulations.push_back(rule);
}

bool ExpressionMap::is_valid() const noexcept
{
    if (dynamics_cc > 127)
        return false;
    if (hairpin_resolution_ticks <= 0)
        return false;
    if (!(default_length_scale > 0.0) || default_length_scale > 2.0)
        return false;
    if (minimum_note_ticks < 0)
        return false;

    for (std::size_t index = 0; index < kDynamicLevelCount; ++index) {
        if (dynamic_velocity[index] > 127 || dynamic_cc_value[index] > 127)
            return false;
        // Velocity 0 is a note-off in MIDI, so it can never be a dynamic
        // level: the quietest ppp still has to sound.
        if (dynamic_velocity[index] == 0)
            return false;
    }

    for (const ArticulationRule& rule : articulations) {
        if (!rule.is_valid())
            return false;
    }
    return true;
}

ExpressionMap make_default_expression_map()
{
    ExpressionMap map;
    map.name = "Default";
    map.dynamic_target = DynamicTarget::Velocity;

    // Articulation behaviour that holds for almost any instrument, expressed
    // as proportions rather than absolute values so it scales with the
    // dynamic in force.
    ArticulationRule accent;
    accent.articulation = Articulation::Accent;
    accent.action = ArticulationAction::ScaleVelocity;
    accent.velocity_scale = 1.25;
    map.set_rule(accent);

    ArticulationRule marcato;
    marcato.articulation = Articulation::Marcato;
    marcato.action = ArticulationAction::ScaleVelocityAndLength;
    marcato.velocity_scale = 1.35;
    marcato.length_scale = 0.7;
    map.set_rule(marcato);

    ArticulationRule staccato;
    staccato.articulation = Articulation::Staccato;
    staccato.action = ArticulationAction::ScaleLength;
    staccato.length_scale = 0.5;
    map.set_rule(staccato);

    ArticulationRule staccatissimo;
    staccatissimo.articulation = Articulation::Staccatissimo;
    staccatissimo.action = ArticulationAction::ScaleLength;
    staccatissimo.length_scale = 0.3;
    map.set_rule(staccatissimo);

    ArticulationRule tenuto;
    tenuto.articulation = Articulation::Tenuto;
    tenuto.action = ArticulationAction::ScaleLength;
    tenuto.length_scale = 1.0;  // Full written value, unlike the default.
    map.set_rule(tenuto);

    ArticulationRule portato;
    portato.articulation = Articulation::Portato;
    portato.action = ArticulationAction::ScaleLength;
    portato.length_scale = 0.75;
    map.set_rule(portato);

    return map;
}

ExpressionMap make_orchestral_cc_expression_map()
{
    ExpressionMap map;
    map.name = "Orchestral (CC dynamics)";
    map.dynamic_target = DynamicTarget::VelocityAndControlChange;

    // With dynamics on a controller, velocity is not the level: it usually
    // selects attack or layer. Keeping it in a narrow band around the middle
    // stops it fighting the controller.
    for (std::size_t index = 0; index < kDynamicLevelCount; ++index)
        map.dynamic_velocity[index] = static_cast<std::uint8_t>(64 + index * 4);

    // CC1 (modulation) is the most common dynamics controller in orchestral
    // libraries. It is a default to edit, not a claim about any product.
    map.dynamics_cc = 1;

    // Slightly compressed at the quiet end, because a library's lowest layer
    // is rarely usable at a controller value near zero.
    const std::uint8_t cc_values[kDynamicLevelCount] = {12, 28, 44, 62, 80, 98, 114, 127};
    for (std::size_t index = 0; index < kDynamicLevelCount; ++index)
        map.dynamic_cc_value[index] = cc_values[index];

    // Long crescendos sound front-loaded on a linear controller ramp.
    map.default_hairpin_shape = CurveShape::Exponential;

    // Keyswitches are the usual way an orchestral library selects
    // articulation. These note numbers are placeholders to be edited per
    // patch; spec 13 forbids hard-coding any library's real layout, and
    // none can be asserted without its documentation.
    struct Switch { Articulation articulation; std::uint8_t note; };
    static const Switch switches[] = {
        {Articulation::Staccato,  24},
        {Articulation::Spiccato,  25},
        {Articulation::Pizzicato, 26},
        {Articulation::Tremolo,   27},
        {Articulation::Trill,     28},
        {Articulation::Harmonic,  29},
        {Articulation::Legato,    30},
        {Articulation::Marcato,   31},
    };

    for (const Switch& entry : switches) {
        ArticulationRule rule;
        rule.articulation = entry.articulation;
        rule.action = ArticulationAction::Keyswitch;
        rule.keyswitch_note = entry.note;
        rule.keyswitch_velocity = 100;
        map.set_rule(rule);
    }

    // Accents still work by velocity even when dynamics are on a controller,
    // since an accent is an attack, not a level change.
    ArticulationRule accent;
    accent.articulation = Articulation::Accent;
    accent.action = ArticulationAction::ScaleVelocity;
    accent.velocity_scale = 1.2;
    map.set_rule(accent);

    return map;
}

}  // namespace vcc::notation
