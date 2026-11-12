// Copyright (c) VC Composer. All rights reserved.
//
// Notation markings that drive MIDI performance (spec 02 sections 5.3, 6, 7).
//
// Spec 3 is explicit that these are not decoration: "Notation markings such
// as dynamics, accents, articulations, crescendos and decrescendos should
// drive appropriate MIDI performance data and/or articulation mappings."
// So each marking here is defined by what it means musically, and the
// translation from meaning to MIDI lives in ExpressionMap - never hard-coded
// here, because spec 7 requires that a written mezzo-forte not be tied to
// one MIDI behaviour.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vcc/notation/MusicalTime.h"

namespace vcc::notation {

/// Written dynamic levels, quietest to loudest (spec 5.3: "ppp through fff
/// and extensible text dynamics").
///
/// The ordering is meaningful: comparison gives relative loudness, and a
/// hairpin interpolates between two of these.
enum class DynamicLevel : std::uint8_t {
    Pianississimo = 0,  ///< ppp
    Pianissimo    = 1,  ///< pp
    Piano         = 2,  ///< p
    MezzoPiano    = 3,  ///< mp
    MezzoForte    = 4,  ///< mf
    Forte         = 5,  ///< f
    Fortissimo    = 6,  ///< ff
    Fortississimo = 7,  ///< fff
};

constexpr std::size_t kDynamicLevelCount = 8;

/// Conventional abbreviation: "ppp", "mf", "fff".
const char* to_string(DynamicLevel level) noexcept;

/// Parses an abbreviation. Returns false for anything unrecognized, so a
/// text dynamic the model has no level for is not silently mapped to one.
bool dynamic_from_string(const std::string& text, DynamicLevel& out) noexcept;

/// A dynamic marking at a point in the score.
///
/// `text` carries extensible text dynamics such as "sotto voce" or
/// "morendo". When it is set, `level` is the nearest standard level used for
/// MIDI purposes, and the text is what gets rendered - so an unusual marking
/// still plays sensibly while displaying what was written.
struct DynamicMarking {
    Ticks tick = 0;
    DynamicLevel level = DynamicLevel::MezzoForte;
    std::uint8_t voice = 0;     ///< 0 applies to every voice on the staff.
    std::string text;           ///< Empty for a plain ppp..fff marking.

    /// The staff it is written on. A staff with markings of its own is
    /// governed by them; one with none follows the other staves' - so one
    /// marking between the staves of a piano part governs both hands,
    /// and a part with a marking per staff is read staff by staff.
    std::uint8_t staff = 0;

    bool is_valid() const noexcept;
};

/// Articulations and accents (spec 5.3).
///
/// A bitmask rather than a single value, because a note can carry several at
/// once: staccato and accent together is ordinary notation.
enum class Articulation : std::uint32_t {
    None          = 0,
    Accent        = 1u << 0,   ///< >
    Staccato      = 1u << 1,   ///< .
    Staccatissimo = 1u << 2,   ///< wedge
    Tenuto        = 1u << 3,   ///< -
    Marcato       = 1u << 4,   ///< ^
    Legato        = 1u << 5,   ///< Slurred; usually implied by a slur span.
    Portato       = 1u << 6,   ///< Tenuto plus staccato.
    Spiccato      = 1u << 7,
    Pizzicato     = 1u << 8,
    Tremolo       = 1u << 9,
    Trill         = 1u << 10,
    Mordent       = 1u << 11,
    Turn          = 1u << 12,
    Fermata       = 1u << 13,
    UpBow         = 1u << 14,
    DownBow       = 1u << 15,
    Harmonic      = 1u << 16,
    Mute          = 1u << 17,
    Falloff       = 1u << 18,
    Doit          = 1u << 19,
    Plop          = 1u << 20,
    Scoop         = 1u << 21,
};

using ArticulationMask = std::uint32_t;

ArticulationMask to_mask(Articulation articulation) noexcept;
ArticulationMask operator|(Articulation a, Articulation b) noexcept;
bool has_articulation(ArticulationMask mask, Articulation articulation) noexcept;

/// Name for the toolbar, diagnostics and expression-map editing.
const char* to_string(Articulation articulation) noexcept;

/// Every articulation the model has, in a fixed order.
///
/// One list rather than one per caller: a new articulation added to the
/// enum and not to this is one that never appears in a diagnostic, a file
/// or an expression map, and the omission is invisible.
const std::vector<Articulation>& all_articulations();

/// Reads an articulation back from what `to_string` wrote. The writer's
/// inverse, so the two cannot drift apart. "none" is a name it writes and
/// so a name it reads.
bool articulation_from_string(const std::string& text, Articulation& out) noexcept;

/// Writes a comma-separated list of the articulations present. Always
/// NUL-terminates; writes "none" for an empty mask.
void format_articulations(ArticulationMask mask, char* out, std::size_t capacity) noexcept;

/// Direction of a hairpin.
enum class HairpinKind : std::uint8_t { Crescendo = 0, Diminuendo = 1 };

const char* to_string(HairpinKind kind) noexcept;

bool hairpin_kind_from_string(const std::string& text, HairpinKind& out) noexcept;

/// The shape a hairpin follows between its endpoints (spec 7: "hairpin curve
/// editor with default linear behavior and optional editable response
/// curves").
enum class CurveShape : std::uint8_t {
    Linear = 0,
    /// Slow start, fast finish. The usual choice for a long crescendo,
    /// because a linear ramp in MIDI value sounds front-loaded.
    Exponential,
    /// Fast start, slow finish.
    Logarithmic,
    /// Eased at both ends.
    SCurve,
};

const char* to_string(CurveShape shape) noexcept;

bool curve_shape_from_string(const std::string& text, CurveShape& out) noexcept;

/// Evaluates a curve shape at `position` in 0..1, returning 0..1.
/// Clamps rather than extrapolating outside that range.
double evaluate_curve(CurveShape shape, double position) noexcept;

/// A crescendo or diminuendo spanning a musical range (spec 5.3: "hairpins
/// that can be stretched across beats/measures"; acceptance criterion 6).
struct Hairpin {
    Ticks start_tick = 0;
    Ticks end_tick = 0;
    HairpinKind kind = HairpinKind::Crescendo;
    CurveShape shape = CurveShape::Linear;
    std::uint8_t voice = 0;

    /// The staff it is written on, read as a dynamic marking's is.
    std::uint8_t staff = 0;

    /// Endpoint dynamics. When unset, the renderer takes the start from the
    /// dynamic in force at `start_tick` and the end from the next dynamic
    /// marking - which is how a hairpin between two written dynamics
    /// behaves in practice.
    bool has_start_level = false;
    DynamicLevel start_level = DynamicLevel::MezzoForte;
    bool has_end_level = false;
    DynamicLevel end_level = DynamicLevel::Forte;

    Ticks length() const noexcept { return end_tick - start_tick; }
    bool is_valid() const noexcept;
};

/// A slur or phrase marking (spec 5.3).
struct Slur {
    Ticks start_tick = 0;
    Ticks end_tick = 0;
    std::uint8_t voice = 0;

    /// The staff it is drawn on. A slur joins notes, and notes live on a
    /// staff; without this a grand staff drew every slur twice, once over
    /// each staff, and the first picture of real notation showed it.
    std::uint8_t staff = 0;

    bool is_valid() const noexcept { return end_tick > start_tick; }
};

/// A tempo marking (spec 5.3, 6).
struct TempoMarking {
    Ticks tick = 0;
    double beats_per_minute = 120.0;
    /// The note value the marking is per, so "dotted quarter = 60" in 6/8 is
    /// representable rather than being converted and losing what was written.
    NoteValue beat_unit = NoteValue::Quarter;
    std::uint8_t beat_unit_dots = 0;
    std::string text;  ///< "Allegro", "poco rit." and similar.

    /// Tempo expressed in quarter notes per minute, which is what the
    /// timeline conversion needs.
    double quarter_notes_per_minute() const noexcept;

    bool is_valid() const noexcept;
};

/// Free text attached to a position: playing techniques, instructions
/// (spec 5.3: "text/playing-technique markings").
struct TextMarking {
    Ticks tick = 0;
    std::uint8_t voice = 0;
    std::string text;

    bool is_valid() const noexcept { return !text.empty(); }
};

}  // namespace vcc::notation
