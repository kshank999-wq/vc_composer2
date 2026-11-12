#include "vcc/theory/Chord.h"
#include "vcc/theory/Voicing.h"
#include "vcc_test.h"

using namespace vcc::theory;

static std::string tone_names(const Chord& c)
{
    std::string s;
    for (const auto& t : c.tones()) s += (s.empty() ? "" : " ") + t.name();
    return s;
}

static Chord roman(const char* n, const char* key)
{
    std::string err;
    auto c = chord_from_roman(n, *Key::parse(key), &err);
    if (!c) std::fprintf(stderr, "    %s\n", err.c_str());
    return c.value_or(Chord{});
}

TEST_CASE("I-V-vi-IV in G major names G D Em C")
{
    CHECK_EQ(roman("I", "G major").name(), std::string("G"));
    CHECK_EQ(roman("V", "G major").name(), std::string("D"));
    CHECK_EQ(roman("vi", "G major").name(), std::string("Em"));
    CHECK_EQ(roman("IV", "G major").name(), std::string("C"));
}

TEST_CASE("T04 minor ii-V-i spells correctly in E minor")
{
    Chord ii = roman("ii\xC3\xB8" "7", "E minor");
    CHECK_EQ(ii.name(), std::string("F#m7b5"));
    CHECK_EQ(tone_names(ii), std::string("F# A C E"));
    Chord v = roman("V7alt", "E minor");
    CHECK_EQ(v.name(), std::string("B7alt"));
    CHECK_EQ(tone_names(v), std::string("B D# A C Cx G"));
    Chord i = roman("imaj7", "E minor");
    CHECK_EQ(i.name(), std::string("Em(maj7)"));
    CHECK_EQ(tone_names(i), std::string("E G B D#"));
    CHECK_EQ(roman("i7", "E minor").name(), std::string("Em7"));
    CHECK_EQ(roman("ii\xC3\xB8" "7", "E minor").display_name(), std::string("F\xE2\x99\xAF\xC3\xB8" "7"));
}

TEST_CASE("written accidentals alter the major-scale degree")
{
    CHECK_EQ(roman("bVII", "C major").name(), std::string("Bb"));
    CHECK_EQ(roman("bVII", "C minor").name(), std::string("Bb"));
    CHECK_EQ(roman("VII", "C minor").name(), std::string("Bb"));
    CHECK_EQ(roman("VI", "C minor").name(), std::string("Ab"));
    CHECK_EQ(roman("bII", "D minor").name(), std::string("Eb"));
    CHECK_EQ(roman("bIII", "A major").name(), std::string("C"));
    CHECK_EQ(roman("#iv\xC3\xB8" "7", "C major").name(), std::string("F#m7b5"));
}

TEST_CASE("figures, sevenths, pedals and applied chords")
{
    CHECK_EQ(roman("ii6", "C major").name(), std::string("Dm/F"));
    CHECK_EQ(roman("I64", "C major").name(), std::string("C/G"));
    CHECK_EQ(roman("V7", "C major").name(), std::string("G7"));
    CHECK_EQ(roman("V65", "C major").name(), std::string("G7/B"));
    CHECK_EQ(roman("V42", "C major").name(), std::string("G7/F"));
    CHECK_EQ(roman("viio7", "A harmonic_minor").name(), std::string("G#dim7"));
    CHECK_EQ(roman("I7", "A major").name(), std::string("A7"));
    CHECK_EQ(roman("Imaj9", "F major").name(), std::string("Fmaj9"));
    CHECK_EQ(roman("V13", "Bb major").name(), std::string("F13"));
    CHECK_EQ(roman("bVI@1", "C minor").name(), std::string("Ab/C"));
    CHECK_EQ(roman("V7/V", "C major").name(), std::string("D7"));
    CHECK_EQ(roman("V/vi", "C major").name(), std::string("E"));
    CHECK_EQ(roman("Isus4", "D major").name(), std::string("Dsus4"));
    std::string err;
    CHECK(!parse_roman("X", &err));
    CHECK(!parse_roman("v7alt", &err));
    CHECK(!parse_roman("Vfoo", &err));
}

TEST_CASE("chord symbols parse including inversions and slash basses")
{
    CHECK_EQ(parse_chord_symbol("Em7")->name(), std::string("Em7"));
    CHECK_EQ(parse_chord_symbol("C/E")->inversion, 1);
    CHECK(parse_chord_symbol("C/D")->bass.has_value());
    CHECK_EQ(parse_chord_symbol("Bbmaj9")->root.name(), std::string("Bb"));
    CHECK_EQ(parse_chord_symbol("C6/9")->quality.id, std::string("69"));
    CHECK(!parse_chord_symbol("Cfoo"));
}

TEST_CASE("diatonic chord matrix by size")
{
    auto triads = diatonic_chords(*Key::parse("C major"), 3);
    REQUIRE(triads.size() == 7);
    CHECK_EQ(triads[6].quality.id, std::string("dim"));
    auto sevenths = diatonic_chords(*Key::parse("A harmonic_minor"), 4);
    CHECK_EQ(sevenths[0].quality.id, std::string("mmaj7"));
    CHECK_EQ(sevenths[2].quality.id, std::string("augmaj7"));
    CHECK_EQ(sevenths[6].quality.id, std::string("dim7"));
    CHECK_EQ(roman_label(2, sevenths[1].quality), std::string("ii\xC3\xB8" "7"));
    auto ninths = diatonic_chords(*Key::parse("C major"), 5);
    CHECK_EQ(ninths[0].quality.id, std::string("maj9"));
    CHECK_EQ(ninths[2].quality.id, std::string("custom"));  // iii with b9
}

TEST_CASE("voicings keep the requested bass and stay in range")
{
    VoicingOptions o;
    Chord c = *parse_chord_symbol("C/E");
    Voicing v = voice_chord(c, o);
    REQUIRE(!v.notes.empty());
    CHECK_EQ(v.bass() % 12, 4);
    for (int n : v.notes) CHECK(n >= o.low && n <= o.high);
    o.style = VoicingStyle::Drop2;
    Voicing d = voice_chord(*parse_chord_symbol("Cmaj7"), o);
    CHECK_EQ(d.notes.size(), 4u);
    CHECK_EQ(d.bass() % 12, 0);
    o.style = VoicingStyle::Spread;
    Voicing s = voice_chord(*parse_chord_symbol("G"), o);
    CHECK(s.notes[1] - s.notes[0] >= 12);
}

TEST_CASE("voice leading moves less than independent voicing and is deterministic")
{
    std::vector<Chord> seq;
    for (const char* n : {"I", "vi", "IV", "V", "I"}) seq.push_back(roman(n, "C major"));
    VoicingOptions lead;
    VoicingOptions none;
    none.voice_lead = false;
    auto a = voice_progression(seq, lead);
    auto b = voice_progression(seq, none);
    int ma = 0, mb = 0;
    for (std::size_t i = 1; i < a.size(); ++i) {
        ma += voice_movement(a[i - 1], a[i]);
        mb += voice_movement(b[i - 1], b[i]);
    }
    CHECK(ma <= mb);
    auto again = voice_progression(seq, lead);
    for (std::size_t i = 0; i < a.size(); ++i) CHECK(a[i].notes == again[i].notes);
    // Bass follows the numerals when inversions are not free.
    CHECK_EQ(a[1].bass() % 12, 9);
    CHECK_EQ(a[3].bass() % 12, 7);
    // A lock forces an inversion.
    auto locked = voice_progression(seq, lead, {std::nullopt, std::nullopt, std::nullopt, 1});
    CHECK_EQ(locked[3].bass() % 12, 11);
}
