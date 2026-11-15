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

#include "vcc/theory/KeyReference.h"

TEST_CASE("key reference: the spec's G major, E-flat major and A minor examples")
{
    auto names = [](const std::vector<ScaleDegree>& s) {
        std::string out;
        for (const auto& d : s) out += (out.empty() ? "" : " ") + d.note.name();
        return out;
    };
    auto chords = [](const std::vector<DiatonicChord>& v, bool roman) {
        std::string out;
        for (const auto& c : v) out += (out.empty() ? "" : " ") + (roman ? c.roman : c.chord.name());
        return out;
    };
    const auto g = key_reference(*Key::parse("G major"));
    CHECK_EQ(names(g.scale), std::string("G A B C D E F#"));
    CHECK_EQ(g.fifths, 1);
    REQUIRE(g.signature.size() == 1u);
    CHECK_EQ(g.signature[0].name(), std::string("F#"));
    CHECK_EQ(chords(g.triads, false), std::string("G Am Bm C D Em F#dim"));
    CHECK_EQ(chords(g.triads, true), std::string("I ii iii IV V vi vii\xC2\xB0"));
    CHECK_EQ(chords(g.sevenths, true), std::string("Imaj7 ii7 iii7 IVmaj7 V7 vi7 vii\xC3\xB8" "7"));
    CHECK_EQ(g.relative.name(), std::string("E minor"));
    CHECK_EQ(g.parallel.name(), std::string("G minor"));
    CHECK_EQ(g.dominant.name(), std::string("D major"));
    CHECK_EQ(g.subdominant.name(), std::string("C major"));
    CHECK_EQ(g.scale[6].function, std::string("Leading tone"));
    CHECK_EQ(signature_text(g.fifths), std::string("1 sharp"));

    const auto eb = key_reference(*Key::parse("Eb major"));
    CHECK_EQ(names(eb.scale), std::string("Eb F G Ab Bb C D"));
    CHECK_EQ(signature_text(eb.fifths), std::string("3 flats"));
    REQUIRE(eb.signature.size() == 3u);
    CHECK_EQ(eb.signature[2].name(), std::string("Ab"));

    const auto am = key_reference(*Key::parse("A minor"));
    CHECK_EQ(names(am.scale), std::string("A B C D E F G"));
    CHECK_EQ(am.scale[6].function, std::string("Subtonic"));
    CHECK(am.altered_note.empty());
    const auto harm = key_reference(*Key::parse("A minor"), MinorForm::Harmonic);
    CHECK_EQ(names(harm.scale), std::string("A B C D E F G#"));
    CHECK(harm.scale[6].altered);
    CHECK(!harm.scale[5].altered);
    CHECK_EQ(harm.scale[6].function, std::string("Leading tone"));
    CHECK_EQ(harm.triads[4].chord.name(), std::string("E"));  // V, major: uses the raised 7th
    CHECK(harm.triads[4].uses_altered);
    CHECK(!harm.triads[3].uses_altered);
    CHECK_EQ(harm.triads[6].roman, std::string("vii\xC2\xB0"));
    CHECK(!harm.altered_note.empty());
    const auto mel = key_reference(*Key::parse("A minor"), MinorForm::MelodicAscending);
    CHECK_EQ(names(mel.scale), std::string("A B C D E F# G#"));
    const auto desc = key_reference(*Key::parse("A minor"), MinorForm::MelodicDescending);
    CHECK_EQ(names(desc.scale), std::string("A B C D E F G"));
    CHECK_EQ(am.relative.name(), std::string("C major"));
}

TEST_CASE("key reference: every major and minor key spelled one letter per degree, matching its signature")
{
    const auto& circle = circle_of_fifths();
    REQUIRE(circle.size() == 12u);
    CHECK_EQ(circle[6].major.name(), std::string("F#"));
    CHECK_EQ(circle[6].major_enharmonic->name(), std::string("Gb"));
    CHECK_EQ(circle[6].minor.name(), std::string("D#"));
    CHECK_EQ(circle[7].major.name(), std::string("Db"));
    CHECK_EQ(circle[7].major_enharmonic->name(), std::string("C#"));
    CHECK_EQ(circle[11].major.name(), std::string("F"));
    CHECK_EQ(circle[11].minor.name(), std::string("D"));
    // The fifteen major and fifteen minor keys that have key signatures.
    std::vector<std::string> keys;
    for (const char* t : {"C", "G", "D", "A", "E", "B", "F#", "C#", "F", "Bb", "Eb", "Ab", "Db", "Gb", "Cb"})
        keys.push_back(std::string(t) + " major");
    for (const char* t : {"A", "E", "B", "F#", "C#", "G#", "D#", "A#", "D", "G", "C", "F", "Bb", "Eb", "Ab"})
        keys.push_back(std::string(t) + " minor");
    for (const auto& name : keys) {
        {
            const auto key = Key::parse(name);
            REQUIRE(key);
            const auto r = key_reference(*key);
            CHECK_EQ(r.scale.size(), 7u);
            int letters = 0;
            for (const auto& d : r.scale) letters |= 1 << ((d.note.letter % 7 + 7) % 7);
            CHECK_EQ(letters, 0x7f);
            CHECK_EQ(static_cast<int>(r.signature.size()), std::abs(r.fifths));
            // Every degree carries exactly the signature's accidental for its letter.
            for (const auto& d : r.scale) {
                int expected = 0;
                for (const auto& s : r.signature)
                    if (s.letter % 7 == d.note.letter % 7) expected = s.alteration;
                CHECK_EQ(d.note.alteration, expected);
            }
            CHECK_EQ(circle_index(*key), circle_index(r.relative));
        }
    }
}

TEST_CASE("key reference: borrowed chords, labelled against the home key and never duplicating diatonic ones")
{
    auto list = [](const std::vector<BorrowedChord>& v) {
        std::string out;
        for (const auto& b : v) out += (out.empty() ? "" : " ") + b.roman + "=" + b.chord.name();
        return out;
    };
    // C major borrows from C minor, plus the Neapolitan from C Phrygian.
    const auto c = key_reference(*Key::parse("C major"));
    CHECK_EQ(list(c.borrowed_triads),
             std::string("i=Cm \xE2\x99\xADII=Db ii\xC2\xB0=Ddim \xE2\x99\xADIII=Eb iv=Fm v=Gm \xE2\x99\xADVI=Ab \xE2\x99\xADVII=Bb"));
    CHECK_EQ(c.borrowed_triads[1].name, std::string("Neapolitan"));
    CHECK_EQ(c.borrowed_triads[1].source, std::string("C Phrygian"));
    CHECK_EQ(c.borrowed_triads[4].source, std::string("C minor"));
    CHECK_EQ(c.borrowed_sevenths.size(), 8u);
    CHECK_EQ(c.borrowed_sevenths[4].roman, std::string("iv7"));
    CHECK_EQ(c.borrowed_sevenths[4].chord.name(), std::string("Fm7"));
    CHECK_EQ(c.borrowed_sevenths[6].chord.name(), std::string("Abmaj7"));
    // A harmonic minor borrows from A major what it does not already have
    // (V and vii° are its own); the raised 6th is written as the key's own.
    const auto am = key_reference(*Key::parse("A minor"), MinorForm::Harmonic);
    CHECK_EQ(list(am.borrowed_triads),
             std::string("I=A \xE2\x99\xADII=Bb ii=Bm \xE2\x99\xAFiii=C#m IV=D vi=F#m"));
    CHECK_EQ(am.borrowed_triads[0].name, std::string("Picardy third"));
    // Natural minor: V and vii° are borrowed too.
    const auto cm = key_reference(*Key::parse("C minor"));
    CHECK_EQ(list(cm.borrowed_triads),
             std::string("I=C \xE2\x99\xADII=Db ii=Dm \xE2\x99\xAFiii=Em IV=F V=G vi=Am vii\xC2\xB0=Bdim"));
    // Flat keys spell their borrowed roots with flats (Eb major: bVI is Cb).
    const auto eb = key_reference(*Key::parse("Eb major"));
    CHECK_EQ(eb.borrowed_triads[6].chord.name(), std::string("Cb"));
    // None duplicates a diatonic chord, in any key.
    for (const auto& p : circle_of_fifths())
        for (bool minor : {false, true}) {
            const auto r = key_reference(Key{minor ? p.minor : p.major, minor ? Mode::Minor : Mode::Major});
            for (const auto& b : r.borrowed_triads)
                for (const auto& d : r.triads)
                    CHECK(!(b.chord.root.pitch_class() == d.chord.root.pitch_class() && b.chord.quality.id == d.chord.quality.id));
        }
}

TEST_CASE("key reference: borrowing from other scales on the same tonic")
{
    auto list = [](const std::vector<BorrowedChord>& v) {
        std::string out;
        for (const auto& b : v) out += (out.empty() ? "" : " ") + b.roman + "=" + b.chord.name();
        return out;
    };
    const auto c = key_reference(*Key::parse("C major"));
    // The parallel source is what the reference already carries.
    CHECK_EQ(list(borrowed_chords(c, BorrowSource::Parallel, false)), list(c.borrowed_triads));
    // C Mixolydian lends the bVII and the minor v (and the diminished iii).
    CHECK_EQ(list(borrowed_chords(c, BorrowSource::Mixolydian, false)), std::string("iii\xC2\xB0=Edim v=Gm \xE2\x99\xADVII=Bb"));
    // C Lydian: the major II, #iv° and vii.
    CHECK_EQ(list(borrowed_chords(c, BorrowSource::Lydian, false)), std::string("II=D \xE2\x99\xAFiv\xC2\xB0=F#dim vii=Bm"));
    CHECK_EQ(borrowed_chords(c, BorrowSource::Lydian, false)[0].source, std::string("C Lydian"));
    // A minor from A Dorian: the major IV and the minor ii.
    const auto am = key_reference(*Key::parse("A minor"));
    CHECK_EQ(list(borrowed_chords(am, BorrowSource::Dorian, false)), std::string("ii=Bm IV=D vi\xC2\xB0=F#dim"));
    // Nothing new: A harmonic minor seen from itself.
    const auto ah = key_reference(*Key::parse("A minor"), MinorForm::Harmonic);
    CHECK(borrowed_chords(ah, BorrowSource::HarmonicMinor, false).empty());
    // Flat keys spell with double flats where the scale needs them (Eb Locrian's v is Bbb).
    const auto eb = key_reference(*Key::parse("Eb major"));
    bool double_flat = false;
    for (const auto& b : borrowed_chords(eb, BorrowSource::Locrian, false)) double_flat = double_flat || b.chord.name() == "Bbb";
    CHECK(double_flat);
    CHECK_EQ(borrow_source_name(BorrowSource::Parallel, true), std::string("Parallel major"));
}
