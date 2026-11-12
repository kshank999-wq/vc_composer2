#include "vcc/theory/Pitch.h"
#include "vcc_test.h"

using namespace vcc::theory;

static std::string scale_names(const Key& k)
{
    std::string s;
    for (const auto& p : k.scale()) s += p.name() + " ";
    return s;
}

TEST_CASE("pitch spelling parses and prints")
{
    CHECK_EQ(SpelledPitch::parse("F#")->pitch_class(), 6);
    CHECK_EQ(SpelledPitch::parse("Bb")->name(), std::string("Bb"));
    CHECK_EQ(SpelledPitch::parse("B\xE2\x99\xAD")->name(), std::string("Bb"));
    CHECK_EQ(SpelledPitch::parse("Fx")->pitch_class(), 7);
    CHECK(!SpelledPitch::parse("H"));
    CHECK(!SpelledPitch::parse("C###"));
    // A major third above D is F#, above Db is F.
    CHECK_EQ(SpelledPitch::parse("D")->transposed(2, 4).name(), std::string("F#"));
    CHECK_EQ(SpelledPitch::parse("Db")->transposed(2, 4).name(), std::string("F"));
}

TEST_CASE("scales are spelled one letter per degree")
{
    CHECK_EQ(scale_names(*Key::parse("G major")), std::string("G A B C D E F# "));
    CHECK_EQ(scale_names(*Key::parse("E minor")), std::string("E F# G A B C D "));
    CHECK_EQ(scale_names(*Key::parse("E harmonic_minor")), std::string("E F# G A B C D# "));
    CHECK_EQ(scale_names(*Key::parse("Eb major")), std::string("Eb F G Ab Bb C D "));
    CHECK_EQ(scale_names(*Key::parse("C# minor")), std::string("C# D# E F# G# A B "));
    CHECK_EQ(scale_names(*Key::parse("D dorian")), std::string("D E F G A B C "));
    CHECK_EQ(scale_names(*Key::parse("A minor_pentatonic")), std::string("A C D E G "));
}

TEST_CASE("key signatures by mode")
{
    CHECK_EQ(Key::parse("G major")->fifths(), 1);
    CHECK_EQ(Key::parse("E minor")->fifths(), 1);
    CHECK_EQ(Key::parse("D minor")->fifths(), -1);
    CHECK_EQ(Key::parse("D dorian")->fifths(), 0);
    CHECK_EQ(Key::parse("F# major")->fifths(), 6);
    CHECK_EQ(Key::parse("E phrygian")->fifths(), 0);
    CHECK(Key::parse("Em")->mode == Mode::Minor);
    CHECK(Key::parse("Bb")->mode == Mode::Major);
    CHECK(!Key::parse("Q major"));
}

TEST_CASE("tuning keeps A4 at reference and collections realize with bends")
{
    auto meantone = TuningSystem::quarter_comma_meantone();
    CHECK_NEAR(meantone.frequency(69), 440.0, 1e-9);
    CHECK_NEAR(TuningSystem::equal_temperament().frequency(81), 880.0, 1e-9);
    PitchCollection rast{"Rast (illustrative)", {0, 200, 350, 500, 700, 900, 1050}, ""};
    auto third = rast.realize(60, 2);
    // 350 cents rounds to the nearer-above semitone with a -50 cent bend.
    CHECK_EQ(third.midi_note, 64);
    CHECK_NEAR(third.bend_cents, -50.0, 1e-9);
    auto octave = rast.realize(60, 7);
    CHECK_EQ(octave.midi_note, 72);
    auto below = rast.realize(60, -1);
    CHECK_EQ(below.midi_note, 58);  // 1050 cents an octave down = -150 cents
    CHECK_NEAR(below.bend_cents, 50.0, 1e-9);
}
