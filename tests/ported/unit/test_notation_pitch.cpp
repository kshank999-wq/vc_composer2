// Pitch, key signatures and clefs (spec 02 sections 5.1, 5.3).
#include "vcc/notation/Pitch.h"

#include <cstring>
#include <set>

#include "vcc_test.h"

using namespace vcc::notation;

VCC_TEST(middle_c_is_midi_60)
{
    const Pitch middle_c{Step::C, 4, 0};
    VCC_CHECK_EQ(midi_note_number(middle_c), 60);
    VCC_CHECK_EQ(midi_note_number(Pitch{Step::A, 4, 0}), 69);   // A440.
    VCC_CHECK_EQ(midi_note_number(Pitch{Step::C, -1, 0}), 0);   // Lowest MIDI note.
    VCC_CHECK_EQ(midi_note_number(Pitch{Step::G, 9, 0}), 127);  // Highest.
}

VCC_TEST(enharmonic_spellings_are_distinct_notes_with_the_same_sound)
{
    // The reason pitch is stored diatonically rather than as a MIDI number:
    // these are the same key on a keyboard and different notes on a staff.
    const Pitch f_sharp{Step::F, 4, 1};
    const Pitch g_flat{Step::G, 4, -1};

    VCC_CHECK_EQ(midi_note_number(f_sharp), midi_note_number(g_flat));
    VCC_CHECK(f_sharp != g_flat);
    VCC_CHECK(diatonic_index(f_sharp) != diatonic_index(g_flat));

    // And they sit on different staff lines, which is the visible
    // consequence.
    VCC_CHECK(staff_position(f_sharp, Clef::Treble)
              != staff_position(g_flat, Clef::Treble));
}

VCC_TEST(double_accidentals_work)
{
    VCC_CHECK_EQ(midi_note_number(Pitch{Step::C, 4, 2}), 62);   // C double sharp = D.
    VCC_CHECK_EQ(midi_note_number(Pitch{Step::E, 4, -2}), 62);  // E double flat = D.
    VCC_CHECK(!Pitch{Step::C, 4, 3}.is_valid());                // Triple sharp.
    VCC_CHECK(!Pitch{Step::C, 4, -3}.is_valid());
}

VCC_TEST(pitches_outside_midi_range_report_it_rather_than_wrapping)
{
    // A wrapped note number would silently play the wrong pitch, which is
    // worse than reporting the note as unplayable.
    VCC_CHECK_EQ(midi_note_number(Pitch{Step::C, -1, -1}), -1);
    VCC_CHECK_EQ(midi_note_number(Pitch{Step::B, 9, 1}), -1);
}

VCC_TEST(intervals_are_correct_across_octaves_and_accidentals)
{
    VCC_CHECK_EQ(semitone_interval(Pitch{Step::C, 4, 0}, Pitch{Step::G, 4, 0}), 7);
    VCC_CHECK_EQ(semitone_interval(Pitch{Step::C, 4, 0}, Pitch{Step::C, 5, 0}), 12);
    VCC_CHECK_EQ(semitone_interval(Pitch{Step::C, 5, 0}, Pitch{Step::C, 4, 0}), -12);
    VCC_CHECK_EQ(semitone_interval(Pitch{Step::C, 4, 0}, Pitch{Step::E, 4, -1}), 3);

    // Intervals stay correct beyond MIDI's range, unlike note numbers.
    VCC_CHECK_EQ(semitone_interval(Pitch{Step::C, -1, 0}, Pitch{Step::C, 10, 0}), 132);
}

VCC_TEST(key_signatures_alter_the_right_steps)
{
    // G major: one sharp, F#.
    const KeySignature g_major{1, false};
    VCC_CHECK_EQ(g_major.alteration_for(Step::F), 1);
    VCC_CHECK_EQ(g_major.alteration_for(Step::C), 0);

    // D major: F# and C#, in that order.
    const KeySignature d_major{2, false};
    VCC_CHECK_EQ(d_major.alteration_for(Step::F), 1);
    VCC_CHECK_EQ(d_major.alteration_for(Step::C), 1);
    VCC_CHECK_EQ(d_major.alteration_for(Step::G), 0);

    // F major: one flat, Bb.
    const KeySignature f_major{-1, false};
    VCC_CHECK_EQ(f_major.alteration_for(Step::B), -1);
    VCC_CHECK_EQ(f_major.alteration_for(Step::E), 0);

    // Eb major: Bb, Eb, Ab.
    const KeySignature e_flat{-3, false};
    VCC_CHECK_EQ(e_flat.alteration_for(Step::B), -1);
    VCC_CHECK_EQ(e_flat.alteration_for(Step::E), -1);
    VCC_CHECK_EQ(e_flat.alteration_for(Step::A), -1);
    VCC_CHECK_EQ(e_flat.alteration_for(Step::D), 0);

    // C major alters nothing.
    const KeySignature c_major{0, false};
    for (int step = 0; step < 7; ++step)
        VCC_CHECK_EQ(c_major.alteration_for(static_cast<Step>(step)), 0);
}

VCC_TEST(all_seven_sharps_and_flats_are_ordered_correctly)
{
    // C# major: every step sharpened, in the order F C G D A E B.
    const KeySignature c_sharp{7, false};
    for (int step = 0; step < 7; ++step)
        VCC_CHECK_EQ(c_sharp.alteration_for(static_cast<Step>(step)), 1);

    // Cb major: every step flattened.
    const KeySignature c_flat{-7, false};
    for (int step = 0; step < 7; ++step)
        VCC_CHECK_EQ(c_flat.alteration_for(static_cast<Step>(step)), -1);

    VCC_CHECK(!KeySignature{8, false}.is_valid());
    VCC_CHECK(!KeySignature{-8, false}.is_valid());
}

VCC_TEST(key_names_read_correctly)
{
    char buffer[24];
    VCC_CHECK(std::strcmp(key_name(KeySignature{0, false}, buffer, sizeof(buffer)),
                          "C major") == 0);
    VCC_CHECK(std::strcmp(key_name(KeySignature{2, false}, buffer, sizeof(buffer)),
                          "D major") == 0);
    VCC_CHECK(std::strcmp(key_name(KeySignature{-2, false}, buffer, sizeof(buffer)),
                          "Bb major") == 0);
    VCC_CHECK(std::strcmp(key_name(KeySignature{0, true}, buffer, sizeof(buffer)),
                          "A minor") == 0);
    VCC_CHECK(std::strcmp(key_name(KeySignature{-5, true}, buffer, sizeof(buffer)),
                          "Bb minor") == 0);
}

VCC_TEST(midi_notes_are_spelled_to_suit_the_key)
{
    // A flat key spells the black keys as flats, a sharp key as sharps,
    // which is what a composer expects to see.
    const Pitch in_flats = spell_midi_note(61, KeySignature{-3, false});
    VCC_CHECK_EQ(in_flats.step, Step::D);
    VCC_CHECK_EQ(in_flats.alteration, -1);       // Db
    VCC_CHECK_EQ(midi_note_number(in_flats), 61);

    const Pitch in_sharps = spell_midi_note(61, KeySignature{3, false});
    VCC_CHECK_EQ(in_sharps.step, Step::C);
    VCC_CHECK_EQ(in_sharps.alteration, 1);       // C#
    VCC_CHECK_EQ(midi_note_number(in_sharps), 61);

    // White keys need no accidental regardless of the key.
    const Pitch natural = spell_midi_note(60, KeySignature{-5, false});
    VCC_CHECK_EQ(natural.step, Step::C);
    VCC_CHECK_EQ(natural.alteration, 0);
}

VCC_TEST(spelling_round_trips_for_every_midi_note_in_every_key)
{
    // Whatever spelling is chosen, it must sound as the note asked for.
    for (int fifths = -7; fifths <= 7; ++fifths) {
        const KeySignature key{static_cast<std::int8_t>(fifths), false};
        for (int note = 0; note <= 127; ++note) {
            const Pitch spelled = spell_midi_note(note, key);
            VCC_CHECK_MSG(spelled.is_valid(),
                          "invalid spelling for note " + std::to_string(note)
                              + " in key " + std::to_string(fifths));
            VCC_CHECK_MSG(midi_note_number(spelled) == note,
                          "note " + std::to_string(note) + " in key "
                              + std::to_string(fifths) + " spelled as something sounding "
                              + std::to_string(midi_note_number(spelled)));
        }
    }
}

VCC_TEST(staff_positions_match_the_clef)
{
    // Treble: bottom line is E4, middle C sits two positions below it.
    VCC_CHECK_EQ(staff_position(Pitch{Step::E, 4, 0}, Clef::Treble), 0);
    VCC_CHECK_EQ(staff_position(Pitch{Step::F, 4, 0}, Clef::Treble), 1);
    VCC_CHECK_EQ(staff_position(Pitch{Step::G, 4, 0}, Clef::Treble), 2);
    VCC_CHECK_EQ(staff_position(Pitch{Step::C, 4, 0}, Clef::Treble), -2);
    VCC_CHECK_EQ(staff_position(Pitch{Step::F, 5, 0}, Clef::Treble), 8);  // Top line.

    // Bass: bottom line is G2, top line A3, middle C two above the top.
    VCC_CHECK_EQ(staff_position(Pitch{Step::G, 2, 0}, Clef::Bass), 0);
    VCC_CHECK_EQ(staff_position(Pitch{Step::A, 3, 0}, Clef::Bass), 8);
    VCC_CHECK_EQ(staff_position(Pitch{Step::C, 4, 0}, Clef::Bass), 10);

    // Alto: middle C sits on the middle line, which is the whole point of
    // the clef.
    VCC_CHECK_EQ(staff_position(Pitch{Step::C, 4, 0}, Clef::Alto), 4);

    // Tenor: middle C sits one position above the middle line.
    VCC_CHECK_EQ(staff_position(Pitch{Step::C, 4, 0}, Clef::Tenor), 6);
}

VCC_TEST(accidentals_do_not_move_a_note_off_its_line)
{
    // F# and F natural are the same staff position; only the accidental
    // differs. A model that placed them differently would be unusable.
    VCC_CHECK_EQ(staff_position(Pitch{Step::F, 4, 0}, Clef::Treble),
                 staff_position(Pitch{Step::F, 4, 1}, Clef::Treble));
    VCC_CHECK_EQ(staff_position(Pitch{Step::B, 4, 0}, Clef::Treble),
                 staff_position(Pitch{Step::B, 4, -1}, Clef::Treble));
}

VCC_TEST(octave_clefs_are_written_like_their_base_clef)
{
    // The octave shift is a sounding transposition applied when rendering,
    // not a change to where the note appears on the staff.
    VCC_CHECK_EQ(staff_position(Pitch{Step::E, 4, 0}, Clef::TrebleOctaveDown),
                 staff_position(Pitch{Step::E, 4, 0}, Clef::Treble));
    VCC_CHECK_EQ(staff_position(Pitch{Step::G, 2, 0}, Clef::BassOctaveDown),
                 staff_position(Pitch{Step::G, 2, 0}, Clef::Bass));
}

VCC_TEST(pitches_sort_by_sound_then_spelling)
{
    std::set<Pitch> pitches;
    pitches.insert(Pitch{Step::G, 4, 0});
    pitches.insert(Pitch{Step::C, 4, 0});
    pitches.insert(Pitch{Step::E, 4, 0});

    // A chord reads bottom to top.
    auto it = pitches.begin();
    VCC_CHECK_EQ(it->step, Step::C);
    ++it;
    VCC_CHECK_EQ(it->step, Step::E);
    ++it;
    VCC_CHECK_EQ(it->step, Step::G);
}

VCC_TEST_MAIN("notation/Pitch")
