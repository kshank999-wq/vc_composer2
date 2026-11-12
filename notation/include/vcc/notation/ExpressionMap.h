// Copyright (c) VC Composer. All rights reserved.
//
// Configurable notation-to-performance mapping (spec 02 section 7).
//
// Spec 7 states the requirement precisely: "Because orchestral sample
// libraries use different performance controls, VC Composer needs a
// configurable expression mapping layer. A written dynamic such as
// mezzo-forte must not be hard-coded to one MIDI behavior."
//
// So this is the only place that decides what a marking *does*. The score
// model records what the composer wrote; this decides how it sounds; the
// renderer applies it. A library that expresses dynamics through CC1 and one
// that uses velocity differ only by the map loaded, and spec 13 requires
// exactly that - "do not hard-code Kontakt or EastWest behavior into the
// core notation engine".
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vcc/notation/Markings.h"

namespace vcc::notation {

/// How a written dynamic reaches the instrument.
enum class DynamicTarget : std::uint8_t {
    /// Note-on velocity. Right for percussion and for libraries whose
    /// dynamic layers are velocity-switched.
    Velocity = 0,
    /// A continuous controller. Right for orchestral libraries that
    /// crossfade dynamic layers, where velocity chooses articulation
    /// instead.
    ControlChange,
    /// Both: velocity sets the attack, the controller the sustained level.
    VelocityAndControlChange,
};

const char* to_string(DynamicTarget target) noexcept;

/// Reads a target back from what `to_string` wrote. The writer's inverse,
/// so a map in a settings file cannot drift from what this build names.
bool dynamic_target_from_string(const std::string& text, DynamicTarget& out) noexcept;

/// What an articulation does to the performance.
enum class ArticulationAction : std::uint8_t {
    /// Scale note-on velocity, for accents.
    ScaleVelocity = 0,
    /// Scale sounding length, for staccato and tenuto.
    ScaleLength,
    /// Send a keyswitch note before the note, which is how most orchestral
    /// libraries select an articulation.
    Keyswitch,
    /// Set a controller value, for libraries that switch on CC instead.
    ControlChange,
    /// Both a velocity and a length change, for combinations like marcato.
    ScaleVelocityAndLength,
};

const char* to_string(ArticulationAction action) noexcept;

bool articulation_action_from_string(const std::string& text, ArticulationAction& out) noexcept;

/// One rule: what this articulation does, and with what values.
struct ArticulationRule {
    Articulation articulation = Articulation::Accent;
    ArticulationAction action = ArticulationAction::ScaleVelocity;

    /// Multiplier on velocity, for the velocity actions. 1.0 leaves it alone.
    double velocity_scale = 1.0;

    /// Multiplier on sounding length, for the length actions. Staccato is
    /// around 0.5, tenuto around 1.0.
    double length_scale = 1.0;

    /// Keyswitch note number, for `Keyswitch`. Usually well below the
    /// playable range.
    std::uint8_t keyswitch_note = 0;
    std::uint8_t keyswitch_velocity = 100;

    /// Controller and value, for `ControlChange`.
    std::uint8_t cc_number = 0;
    std::uint8_t cc_value = 0;

    bool is_valid() const noexcept;
};

/// A complete mapping for one patch (spec 7: "Ability to save the mapping
/// with the favorite instrument/patch definition"; spec 12: ExpressionMap).
struct ExpressionMap {
    std::string name = "Default";

    /// Which provider or library this was written for, purely informational.
    std::string library;

    // --- Dynamics -----------------------------------------------------------

    DynamicTarget dynamic_target = DynamicTarget::Velocity;

    /// Velocity for each written level, ppp through fff. Indexed by
    /// `DynamicLevel`. A per-library curve, as spec 7 requires.
    std::uint8_t dynamic_velocity[kDynamicLevelCount] = {16, 32, 48, 64, 80, 96, 112, 127};

    /// Controller used for dynamics when the target involves one. CC11
    /// (expression) is the common default; many orchestral libraries use
    /// CC1.
    std::uint8_t dynamics_cc = 11;

    /// Controller value for each written level.
    std::uint8_t dynamic_cc_value[kDynamicLevelCount] = {16, 32, 48, 64, 80, 96, 112, 127};

    // --- Hairpins -----------------------------------------------------------

    /// Default shape when a hairpin does not specify one.
    CurveShape default_hairpin_shape = CurveShape::Linear;

    /// Spacing of the controller messages a hairpin generates, in ticks.
    /// Finer is smoother but produces more data; a thirty-second note
    /// gives two messages per sixteenth.
    Ticks hairpin_resolution_ticks = kTicksPerQuarter / 8;

    // --- Articulations ------------------------------------------------------

    std::vector<ArticulationRule> articulations;

    // --- Note length --------------------------------------------------------

    /// Fraction of the written duration a note sounds for when it carries
    /// no length articulation. Slightly less than 1 keeps repeated notes
    /// from overlapping, which most instruments need.
    ///
    /// A written length articulation replaces this rather than compounding
    /// with it: tenuto means "held for its full value", and a note under it
    /// must reach 100% of the written length, which it could not if the
    /// default were still applied. Several length articulations on one note
    /// do multiply together.
    double default_length_scale = 0.95;

    /// Minimum sounding length in ticks, so a short note under an aggressive
    /// staccato does not become a zero-length note the instrument ignores.
    Ticks minimum_note_ticks = kTicksPerQuarter / 32;

    // --- Queries ------------------------------------------------------------

    /// Velocity for a written dynamic level.
    std::uint8_t velocity_for(DynamicLevel level) const noexcept;

    /// Controller value for a written dynamic level.
    std::uint8_t cc_value_for(DynamicLevel level) const noexcept;

    /// The rule for an articulation, or nullptr when unmapped - which is a
    /// normal state, not an error: a library with no pizzicato patch simply
    /// has no rule for it, and the renderer must then leave the note alone
    /// rather than invent a behaviour.
    const ArticulationRule* rule_for(Articulation articulation) const noexcept;

    /// Adds or replaces the rule for an articulation.
    void set_rule(const ArticulationRule& rule);

    bool is_valid() const noexcept;
};

/// A general-purpose starting map: dynamics on velocity, accents and
/// staccato handled by scaling. Suitable for a simple instrument and as the
/// base to edit from.
ExpressionMap make_default_expression_map();

/// A map shaped for a typical orchestral sustain patch: dynamics crossfaded
/// on a controller, with velocity kept moderate so the controller governs
/// the level, plus keyswitches for the common articulation changes.
///
/// The controller numbers and keyswitch notes are *defaults to edit*, not
/// claims about any particular product. Spec 13 requires no library be
/// hard-coded, and no library's real mapping can be asserted here without
/// its documentation in hand.
ExpressionMap make_orchestral_cc_expression_map();

}  // namespace vcc::notation
