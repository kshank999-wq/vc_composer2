// Exact musical time (spec 02 sections 5.1, 5.2, 13).
//
// These cases exist because notation software that is a tick out is
// unusable, and the errors accumulate silently. Every expected value here is
// derived by hand from 960 ticks per quarter.
#include "vcc/notation/MusicalTime.h"

#include <cstddef>

#include "vcc_test.h"

using namespace vcc::notation;

/// A quarter note in ticks, so the arithmetic reads as rhythm.
constexpr Ticks kQuarter = kTicksPerQuarter;

VCC_TEST(base_values_are_exact_powers_of_two)
{
    VCC_CHECK_EQ(base_ticks(NoteValue::Whole), static_cast<Ticks>((4 * kQuarter)));
    VCC_CHECK_EQ(base_ticks(NoteValue::Half), static_cast<Ticks>((2 * kQuarter)));
    VCC_CHECK_EQ(base_ticks(NoteValue::Quarter), static_cast<Ticks>(kQuarter));
    VCC_CHECK_EQ(base_ticks(NoteValue::Eighth), static_cast<Ticks>((kQuarter / 2)));
    VCC_CHECK_EQ(base_ticks(NoteValue::Sixteenth), static_cast<Ticks>((kQuarter / 4)));
    VCC_CHECK_EQ(base_ticks(NoteValue::ThirtySecond), static_cast<Ticks>(kQuarter / 8));
    VCC_CHECK_EQ(base_ticks(NoteValue::SixtyFourth), static_cast<Ticks>(kQuarter / 16));
    VCC_CHECK_EQ(base_ticks(NoteValue::Breve), static_cast<Ticks>((8 * kQuarter)));
}

VCC_TEST(dots_add_exact_fractions)
{
    // One dot is 3/2, two dots 7/4, three dots 15/8.
    VCC_CHECK_EQ(quarter(0).ticks(), static_cast<Ticks>(kQuarter));
    VCC_CHECK_EQ(quarter(1).ticks(), static_cast<Ticks>((3 * kQuarter / 2)));
    VCC_CHECK_EQ(quarter(2).ticks(), static_cast<Ticks>(7 * kQuarter / 4));
    VCC_CHECK_EQ(quarter(3).ticks(), static_cast<Ticks>(15 * kQuarter / 8));

    VCC_CHECK_EQ(half(1).ticks(), static_cast<Ticks>((3 * kQuarter)));
    VCC_CHECK_EQ(eighth(1).ticks(), static_cast<Ticks>((3 * kQuarter / 4)));
    VCC_CHECK_EQ(sixteenth(1).ticks(), static_cast<Ticks>((3 * kQuarter / 8)));

    // The shortest value with the most dots: a 64th is 180 ticks, and
    // 180 * 15 / 8 = 337.5, which is not an integer, so this combination
    // must be rejected rather than rounded.
    const Duration impossible{NoteValue::SixtyFourth, 3, {}};
    VCC_CHECK(!impossible.is_valid());
    VCC_CHECK_EQ(impossible.ticks(), static_cast<Ticks>(0));
}

VCC_TEST(triplets_divide_exactly)
{
    // Three triplet eighths fill a quarter note exactly.
    const Duration triplet_eighth = triplet(NoteValue::Eighth);
    VCC_CHECK_EQ(triplet_eighth.ticks(), static_cast<Ticks>((kQuarter / 3)));
    VCC_CHECK_EQ(triplet_eighth.ticks() * 3, static_cast<Ticks>(kQuarter));

    // Three triplet quarters fill a half note.
    VCC_CHECK_EQ(triplet(NoteValue::Quarter).ticks() * 3, static_cast<Ticks>((2 * kQuarter)));
    // Three triplet 16ths fill an eighth.
    VCC_CHECK_EQ(triplet(NoteValue::Sixteenth).ticks() * 3, static_cast<Ticks>((kQuarter / 2)));
}

VCC_TEST(quintuplets_and_septuplets_divide_exactly_where_representable)
{
    // The resolution is divisible by 5, so five quintuplet 16ths fill a
    // quarter.
    const Duration quintuplet{NoteValue::Sixteenth, 0, Tuplet{5, 4}};
    VCC_CHECK(quintuplet.is_valid());
    VCC_CHECK_EQ(quintuplet.ticks(), static_cast<Ticks>(kQuarter / 5));
    VCC_CHECK_EQ(quintuplet.ticks() * 5, static_cast<Ticks>(kQuarter));

    // It is not divisible by 7, so a septuplet 16th is not exact and must
    // be rejected rather than silently rounded.
    const Duration septuplet{NoteValue::Sixteenth, 0, Tuplet{7, 4}};
    VCC_CHECK(!septuplet.is_valid());
}

VCC_TEST(a_dotted_triplet_is_still_exact)
{
    // Dotted triplet eighth: 480 * 3/2 = 720, then 720 * 2/3 = 480.
    const Duration dotted_triplet{NoteValue::Eighth, 1, Tuplet{3, 2}};
    VCC_CHECK(dotted_triplet.is_valid());
    VCC_CHECK_EQ(dotted_triplet.ticks(), static_cast<Ticks>((kQuarter / 2)));
}

VCC_TEST(invalid_durations_are_rejected)
{
    VCC_CHECK(!Duration{NoteValue::Quarter, 4, {}}.is_valid());     // Too many dots.
    VCC_CHECK(!Duration{NoteValue::Quarter, 0, Tuplet{0, 2}}.is_valid());
    VCC_CHECK(!Duration{NoteValue::Quarter, 0, Tuplet{3, 0}}.is_valid());
    VCC_CHECK(!Duration{NoteValue::Quarter, 0, Tuplet{99, 2}}.is_valid());
}

VCC_TEST(a_plain_ratio_is_not_a_tuplet)
{
    const Tuplet none;
    VCC_CHECK(!none.is_tuplet());
    VCC_CHECK(none.is_valid());
    VCC_CHECK(Tuplet{3, 2}.is_tuplet());
}

VCC_TEST(decomposition_produces_the_conventional_tie_chain)
{
    Duration out[8];

    // An exact value is one duration.
    VCC_CHECK_EQ(decompose_ticks(kQuarter, out, 8), static_cast<std::size_t>(1));
    VCC_CHECK(out[0] == quarter());

    // A dotted value is preferred over a tie, since that is how it is
    // notated.
    VCC_CHECK_EQ(decompose_ticks((3 * kQuarter / 2), out, 8), static_cast<std::size_t>(1));
    VCC_CHECK(out[0] == quarter(1));

    // Five quarters: a whole note tied to a quarter.
    VCC_CHECK_EQ(decompose_ticks((5 * kQuarter), out, 8), static_cast<std::size_t>(2));
    VCC_CHECK_EQ(out[0].ticks() + out[1].ticks(), static_cast<Ticks>((5 * kQuarter)));
}

VCC_TEST(decomposition_always_sums_to_the_input)
{
    // The property that matters: whatever the decomposition, it must add up.
    Duration out[16];
    for (Ticks ticks = kQuarter / 16; ticks <= 9 * kQuarter; ticks += kQuarter / 16) {
        const std::size_t count = decompose_ticks(ticks, out, 16);
        VCC_CHECK_MSG(count > 0, "could not decompose " + std::to_string(ticks));

        Ticks total = 0;
        for (std::size_t i = 0; i < count; ++i)
            total += out[i].ticks();
        VCC_CHECK_MSG(total == ticks, "decomposition of " + std::to_string(ticks)
                                          + " summed to " + std::to_string(total));
    }
}

VCC_TEST(decomposition_refuses_what_it_cannot_notate)
{
    Duration out[8];
    // Shorter than a 64th note.
    VCC_CHECK_EQ(decompose_ticks(30, out, 8), static_cast<std::size_t>(0));
    VCC_CHECK_EQ(decompose_ticks(0, out, 8), static_cast<std::size_t>(0));
    VCC_CHECK_EQ(decompose_ticks(-kQuarter, out, 8), static_cast<std::size_t>(0));
    VCC_CHECK_EQ(decompose_ticks(kQuarter, nullptr, 8), static_cast<std::size_t>(0));
    VCC_CHECK_EQ(decompose_ticks(kQuarter, out, 0), static_cast<std::size_t>(0));
}

VCC_TEST(no_rounding_accumulates_over_a_long_score)
{
    // The failure this whole design exists to prevent: hundreds of bars of
    // mixed durations must land on exactly the tick arithmetic says.
    Ticks cursor = 0;
    for (int bar = 0; bar < 500; ++bar) {
        cursor += quarter().ticks();          // kQuarter
        cursor += eighth(1).ticks();          // (3 * kQuarter / 4)
        cursor += triplet(NoteValue::Eighth).ticks() * 3;  // kQuarter
        cursor += sixteenth().ticks();        // (kQuarter / 4)
        cursor += Duration{NoteValue::Sixteenth, 0, Tuplet{5, 4}}.ticks() * 5;  // kQuarter
    }

    // 500 * (1 + 3/4 + 1 + 1/4 + 1) quarters = 500 * 4 quarters.
    VCC_CHECK_EQ(cursor, static_cast<Ticks>(500) * 4 * kQuarter);
    // And that is exactly 500 whole notes, so nothing drifted.
    VCC_CHECK_EQ(cursor, static_cast<Ticks>(500) * kTicksPerWhole);
}

VCC_TEST(a_tuplet_inside_a_tuplet_is_exact)
{
    // The case the resolution was raised for: a triplet of sixteenths
    // inside a triplet eighth is a sixteenth at 4/9, a ninth of a quarter.
    Duration nested{NoteValue::Sixteenth, 0, Tuplet{3, 2}};
    nested.outer = Tuplet{3, 2};
    VCC_CHECK(nested.is_valid());
    VCC_CHECK(nested.is_nested());
    VCC_CHECK_EQ(nested.ticks(), static_cast<Ticks>(kQuarter / 9));
    VCC_CHECK_EQ(nested.ticks() * 3, triplet(NoteValue::Eighth).ticks());

    // It is not the plain triplet it is written like.
    const Duration plain{NoteValue::Sixteenth, 0, Tuplet{3, 2}};
    VCC_CHECK(nested != plain);
    VCC_CHECK(!plain.is_nested());

    // A quintuplet of sixteenths inside a triplet divides too; a septuplet
    // does not, and is refused rather than rounded.
    Duration five{NoteValue::Sixteenth, 0, Tuplet{5, 4}};
    five.outer = Tuplet{3, 2};
    VCC_CHECK(five.is_valid());
    VCC_CHECK_EQ(five.ticks() * 5, triplet(NoteValue::Quarter).ticks());
    Duration seven{NoteValue::Sixteenth, 0, Tuplet{7, 4}};
    seven.outer = Tuplet{3, 2};
    VCC_CHECK(!seven.is_valid());
    VCC_CHECK_EQ(seven.ticks(), static_cast<Ticks>(0));

    // A quintuplet eighth divides on its own and not inside a quintuplet:
    // the nesting is what makes it inexact, and it is refused for that.
    Duration five_in_five{NoteValue::Eighth, 0, Tuplet{5, 4}};
    VCC_CHECK(five_in_five.is_valid());
    five_in_five.outer = Tuplet{5, 4};
    VCC_CHECK(!five_in_five.is_valid());
    VCC_CHECK_EQ(five_in_five.ticks(), static_cast<Ticks>(0));

    // An outer ratio with no tuplet inside it is not a duration at all.
    Duration hollow{NoteValue::Eighth, 0, Tuplet{}};
    hollow.outer = Tuplet{3, 2};
    VCC_CHECK(!hollow.is_valid());
    VCC_CHECK(!hollow.is_nested());
    VCC_CHECK_EQ(hollow.ticks(), static_cast<Ticks>(0));
}

VCC_TEST(a_score_tick_becomes_a_host_tick_and_back)
{
    // The host, the bridge and the MIDI file count 960 to the quarter; the
    // score counts three times that. Everything on the host's grid goes
    // both ways exactly, and a tick between two of the host's - only a
    // nested tuplet's - goes to the nearer, halves away from zero.
    VCC_CHECK_EQ(kTicksPerHostTick, static_cast<Ticks>(3));
    VCC_CHECK_EQ(to_host_ticks(0), static_cast<Ticks>(0));
    VCC_CHECK_EQ(to_host_ticks(kQuarter), static_cast<Ticks>(vcc::music::kTicksPerQuarter));
    VCC_CHECK_EQ(from_host_ticks(vcc::music::kTicksPerQuarter), static_cast<Ticks>(kQuarter));
    for (Ticks tick = -4 * kQuarter; tick <= 4 * kQuarter; tick += 3)
        VCC_CHECK_EQ(from_host_ticks(to_host_ticks(tick)), tick);

    VCC_CHECK(on_host_tick(kQuarter / 3));
    VCC_CHECK(!on_host_tick(kQuarter / 9));
    VCC_CHECK_EQ(to_host_ticks(1), static_cast<Ticks>(0));
    VCC_CHECK_EQ(to_host_ticks(2), static_cast<Ticks>(1));
    VCC_CHECK_EQ(to_host_ticks(4), static_cast<Ticks>(1));
    VCC_CHECK_EQ(to_host_ticks(5), static_cast<Ticks>(2));
    VCC_CHECK_EQ(to_host_ticks(-1), static_cast<Ticks>(0));
    VCC_CHECK_EQ(to_host_ticks(-2), static_cast<Ticks>(-1));
    VCC_CHECK_EQ(to_host_ticks(-4), static_cast<Ticks>(-1));
    VCC_CHECK_EQ(to_host_ticks(-5), static_cast<Ticks>(-2));

    // A triplet sixteenth inside a triplet eighth - the case the
    // resolution was raised for - is a whole number of score ticks and
    // not of the host's.
    const Ticks nested = kQuarter / 4 * 4 / 9;
    VCC_CHECK_EQ(nested * 9, kQuarter);
    VCC_CHECK(!on_host_tick(nested));
}

VCC_TEST_MAIN("notation/MusicalTime")
