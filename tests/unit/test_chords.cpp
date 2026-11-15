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

TEST_CASE("chord symbols: alterations, additions, omissions and power chords; contradictions refused")
{
    auto names = [](const char* text) {
        std::string out;
        auto c = vcc::theory::parse_chord_symbol(text);
        if (!c) return std::string("FAIL");
        for (const auto& t : c->tones()) out += (out.empty() ? "" : " ") + t.name();
        return c->name() + ": " + out;
    };
    CHECK_EQ(names("C7(b5,b9)"), std::string("C7(b5,b9): C E Bb Gb Db"));
    CHECK_EQ(names("C7b5b9"), std::string("C7(b5,b9): C E Bb Gb Db"));
    CHECK_EQ(names("C7#11"), std::string("C7(#11): C E G Bb F#"));
    CHECK_EQ(names("C7(#9,b13)"), std::string("C7(#9,b13): C E G Bb D# Ab"));
    CHECK_EQ(names("C(add9)"), std::string("C(add9): C E G D"));
    CHECK_EQ(names("C7(no3)"), std::string("C7(no3): C G Bb"));
    CHECK_EQ(names("C5"), std::string("C5: C G"));
    CHECK_EQ(names("F#m7(b13)/A"), std::string("F#m7(b13)/A: F# A C# E D"));
    // Round trip through the name.
    for (const char* s : {"C7(b5,b9)", "Eb7(#9,b13)", "Bbmaj7(#11)", "D(add9)", "G5", "A7(no3)"})
        CHECK_EQ(vcc::theory::parse_chord_symbol(s)->name(), std::string(s));
    // Contradictions: the same tone twice, an omitted fifth altered, nonsense.
    CHECK(!vcc::theory::parse_chord_symbol("C9(add9)"));
    CHECK(!vcc::theory::parse_chord_symbol("C7(b9,b9)"));
    CHECK(!vcc::theory::parse_chord_symbol("C7(no5,b5)"));
    CHECK(!vcc::theory::parse_chord_symbol("Cm7b5(b5)"));
    CHECK(!vcc::theory::parse_chord_symbol("Caug(#5)"));
    CHECK(!vcc::theory::parse_chord_symbol("C7(no7)"));
    CHECK(!vcc::theory::parse_chord_symbol("C7(b8)"));
    CHECK(!vcc::theory::parse_chord_symbol("C7xyz"));
}

TEST_CASE("chord entry voicing: bass in its octave, spelled as the chord spells it")
{
    using vcc::theory::VoicingStyle;
    auto voiced = [](const char* text, int octave, VoicingStyle style) {
        std::string out;
        auto c = vcc::theory::parse_chord_symbol(text);
        if (!c) return std::string("FAIL");
        for (const auto& t : vcc::theory::voice_for_entry(*c, octave, style))
            out += (out.empty() ? "" : " ") + t.pitch.name() + std::to_string(t.octave()) + "=" + std::to_string(t.midi);
        return out;
    };
    // Spec 3.4: C major, root position: C4 E4 G4.
    CHECK_EQ(voiced("C", 4, VoicingStyle::Closed), std::string("C4=60 E4=64 G4=67"));
    // G7/B: first inversion, B in the bass.
    CHECK_EQ(voiced("G7/B", 3, VoicingStyle::Closed), std::string("B3=59 D4=62 F4=65 G4=67"));
    // C7(b5,b9): every requested tone, spelled.
    CHECK_EQ(voiced("C7(b5,b9)", 3, VoicingStyle::Closed), std::string("C3=48 E3=52 Bb3=58 Gb4=66 Db5=73"));
    // Second inversion, open, drop 2, spread.
    CHECK_EQ(voiced("C/G", 3, VoicingStyle::Closed), std::string("G3=55 C4=60 E4=64"));
    CHECK_EQ(voiced("C", 4, VoicingStyle::Open), std::string("C4=60 G4=67 E5=76"));
    CHECK_EQ(voiced("Cmaj7", 4, VoicingStyle::Drop2), std::string("C3=48 G3=55 E4=64 B4=71"));
    CHECK_EQ(voiced("Cmaj7", 4, VoicingStyle::Spread), std::string("C3=48 E4=64 G4=67 B4=71"));
    // A slash bass that is not a chord tone sits under the chord.
    CHECK_EQ(voiced("D/C", 3, VoicingStyle::Closed), std::string("C3=48 D3=50 F#3=54 A3=57"));
    // Octave numbers follow the spelling: B#3 is MIDI 60.
    CHECK_EQ(voiced("G#/B#", 3, VoicingStyle::Closed), std::string("B#3=60 D#4=63 G#4=68"));
}
