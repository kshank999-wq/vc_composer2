// Copyright (c) VC Composer. All rights reserved.
//
// Exact musical time for the score model (spec 02 sections 5.1, 6, 13).
//
// Every position and duration in the notation engine is an integer tick
// count. Floating point is deliberately absent: a dotted triplet sixteenth
// inside a 7/8 bar must land on exactly the tick the composer wrote, and
// accumulated double rounding over a few hundred bars is how notation
// software ends up with notes a tick early.
#pragma once

#include <cstdint>
#include <string>

#include "vcc/music/MusicalTime.h"

namespace vcc::notation {

/// Ticks per quarter note for the score model.
///
/// 2880 = 2^6 x 3^2 x 5, so halves down to 64th notes, triplets,
/// quintuplets, their dotted forms, and a triplet inside a triplet are all
/// exact integers. It was 960 - the resolution the host and most notation
/// software use - until nested tuplets, which 960 cannot hold: a triplet
/// sixteenth inside a triplet eighth is a sixteenth at 4/9, and 240 is not
/// divisible by 9. Three times 960 is the smallest resolution that is.
///
/// The host, the bridge and the MIDI file keep their 960
/// (`music::kTicksPerQuarter`): a score tick is turned into a host tick
/// where it leaves the application, by `to_host_ticks`, and a host tick
/// into a score tick where one arrives, by `from_host_ticks`. Every tick
/// that was representable at 960 converts exactly both ways; only a nested
/// tuplet's can fall between two host ticks, and then it is rounded to the
/// nearer, a third of a host tick at most.
constexpr std::int32_t kTicksPerQuarter = 2880;

/// A duration or position in ticks. Signed so that differences and
/// "before the start" cases are representable without wrapping.
using Ticks = std::int64_t;

constexpr Ticks kTicksPerWhole = kTicksPerQuarter * 4;

/// Score ticks in one host tick: the scale between the model and the host.
constexpr Ticks kTicksPerHostTick = kTicksPerQuarter / music::kTicksPerQuarter;
static_assert(kTicksPerQuarter % music::kTicksPerQuarter == 0,
              "a host tick must be a whole number of score ticks");

/// A score tick as the host counts it, rounded to the nearest host tick
/// (halves away from zero) where it falls between two.
constexpr Ticks to_host_ticks(Ticks score_tick) noexcept
{
    const Ticks half = kTicksPerHostTick / 2;
    return score_tick >= 0 ? (score_tick + half) / kTicksPerHostTick
                           : -((-score_tick + half) / kTicksPerHostTick);
}

/// A host tick as the score counts it. Exact.
constexpr Ticks from_host_ticks(Ticks host_tick) noexcept
{
    return host_tick * kTicksPerHostTick;
}

/// True when a score tick lies on a host tick, so `to_host_ticks` loses
/// nothing of it.
constexpr bool on_host_tick(Ticks score_tick) noexcept
{
    return score_tick % kTicksPerHostTick == 0;
}

/// Written note values. `Maxima` through `SixtyFourth` covers everything
/// spec 5.2 requires plus the longer values used in older notation.
enum class NoteValue : std::uint8_t {
    Breve        = 0,  ///< Double whole.
    Whole        = 1,
    Half         = 2,
    Quarter      = 3,
    Eighth       = 4,
    Sixteenth    = 5,
    ThirtySecond = 6,
    SixtyFourth  = 7,
};

const char* to_string(NoteValue value) noexcept;

/// Reads a note value back from what `to_string` wrote. The writer's
/// inverse, so the two cannot drift apart.
bool note_value_from_string(const std::string& text, NoteValue& out) noexcept;

/// Undotted, untupleted length of a written value.
Ticks base_ticks(NoteValue value) noexcept;

/// A tuplet ratio: `actual` notes played in the time of `normal`.
///
/// 3:2 is a triplet, 5:4 a quintuplet, 2:3 a duplet in compound time. The
/// default 1:1 means "not a tuplet", which keeps the common case free of
/// special-casing.
struct Tuplet {
    std::uint16_t actual = 1;
    std::uint16_t normal = 1;

    bool is_tuplet() const noexcept { return actual != normal; }

    /// True when the ratio is usable: both sides non-zero and within the
    /// range real notation uses.
    bool is_valid() const noexcept;
};

bool operator==(const Tuplet& a, const Tuplet& b) noexcept;
bool operator!=(const Tuplet& a, const Tuplet& b) noexcept;

/// A written duration: a base value, dots, and an optional tuplet ratio.
///
/// Kept in written form rather than as a bare tick count because the score
/// has to render what the composer wrote. Two durations can share a tick
/// count and still be different notation - a dotted quarter and a quarter
/// tied to an eighth are both 1440 ticks but are not the same score.
struct Duration {
    NoteValue value = NoteValue::Quarter;
    std::uint8_t dots = 0;   ///< 0-3. Each dot adds half of the previous length.
    Tuplet tuplet{};

    /// The tuplet this tuplet sits inside, for a tuplet nested in another:
    /// a triplet of sixteenths inside a triplet eighth has `tuplet` 3:2 and
    /// `outer` 3:2, and sounds at 4/9 of a sixteenth. 1:1 when the tuplet
    /// is not nested, and not a duration at all without a tuplet of its
    /// own to sit inside. One level of nesting: a third is not notation
    /// anyone writes.
    Tuplet outer{};

    bool is_nested() const noexcept { return tuplet.is_tuplet() && outer.is_tuplet(); }

    /// Exact length in ticks.
    ///
    /// Dots are summed as a geometric series, then the tuplet ratio is
    /// applied. Integer arithmetic throughout: `is_valid` admits only a
    /// combination whose tick count divides exactly at kTicksPerQuarter,
    /// so nothing is rounded.
    Ticks ticks() const noexcept;

    bool is_valid() const noexcept;
};

bool operator==(const Duration& a, const Duration& b) noexcept;
bool operator!=(const Duration& a, const Duration& b) noexcept;

/// Convenience constructors, so call sites read like the notation they build.
Duration whole() noexcept;
Duration half(std::uint8_t dots = 0) noexcept;
Duration quarter(std::uint8_t dots = 0) noexcept;
Duration eighth(std::uint8_t dots = 0) noexcept;
Duration sixteenth(std::uint8_t dots = 0) noexcept;
Duration triplet(NoteValue value) noexcept;

/// Decomposes a tick count into the written durations that sum to it,
/// longest first, as a tie chain would be notated.
///
/// Used when MIDI is read back into notation (spec 16, phase 2) and when a
/// note has to be split across a barline. Writes at most `capacity`
/// durations and returns how many were produced; returns 0 if `ticks` cannot
/// be expressed within `capacity` values.
std::size_t decompose_ticks(Ticks ticks, Duration* out, std::size_t capacity) noexcept;

}  // namespace vcc::notation
