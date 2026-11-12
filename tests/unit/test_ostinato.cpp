#include <set>

#include "vcc/ostinato/Ostinato.h"
#include "vcc/theory/Progression.h"
#include "vcc_test.h"

using namespace vcc;
using namespace vcc::ostinato;

static PresetCatalog presets()
{
    PresetCatalog c;
    std::string err;
    if (!c.load_file(std::string(VCC_CONTENT_DIR) + "/ostinato_presets.json", &err))
        std::fprintf(stderr, "    %s\n", err.c_str());
    return c;
}

static OstinatoOptions strings_options()
{
    OstinatoOptions o;
    o.key = *theory::Key::parse("D minor");
    o.range_low = 48;  // viola-ish
    o.range_high = 88;
    o.apply_preset(*presets().find("urgent_string_spiccato"));
    return o;
}

TEST_CASE("rng is deterministic and unbiased enough")
{
    Rng a(42), b(42);
    for (int i = 0; i < 100; ++i) CHECK_EQ(a.next(), b.next());
    Rng r(7);
    int counts[6] = {};
    for (int i = 0; i < 60000; ++i) ++counts[r.range(0, 5)];
    for (int c : counts) CHECK(c > 9400 && c < 10600);
    CHECK(derive_seed(1, "rhythm") != derive_seed(1, "pitch"));
    // Known value pins the algorithm across platforms and compilers.
    CHECK_EQ(Rng(0).next(), 0x99EC5F36CB75F2B4ULL);
}

TEST_CASE("T07 same seed and options give identical output")
{
    Generator g(strings_options());
    auto a = g.generate(1234);
    auto b = g.generate(1234);
    CHECK(a.notes == b.notes);
    CHECK(!a.notes.empty());
    auto c = g.generate(1235);
    CHECK(a.notes != c.notes);
}

TEST_CASE("T06 16 bars in 7/8 fill exactly the meter-correct span")
{
    for (const auto& id : presets().ids()) {
        OstinatoOptions o = strings_options();
        o.apply_preset(*presets().find(id));
        o.meter = MeterMap(TimeSignature{7, 8});
        o.start_bar = 9;
        o.length_bars = 16;
        Generator g(o);
        auto v = g.generate(99);
        CHECK_EQ(v.start, o.meter.bar_start(9));
        CHECK_EQ(v.end - v.start, 16 * 7 * kTicksPerQuarter / 2);
        std::string fault = validate_notes(v.notes, v.start, v.end);
        CHECK(fault.empty());
        if (!fault.empty()) std::fprintf(stderr, "    %s: %s\n", id.c_str(), fault.c_str());
        CHECK(!v.notes.empty());
        if (!o.percussion)
            for (const auto& n : v.notes) CHECK(n.pitch >= o.range_low && n.pitch <= o.range_high);
    }
}

TEST_CASE("every preset in 3/4, 4/4, 5/4 and 6/8 produces valid, in-range notes")
{
    for (const auto& id : presets().ids()) {
        for (TimeSignature sig : {TimeSignature{3, 4}, TimeSignature{4, 4}, TimeSignature{5, 4}, TimeSignature{6, 8}}) {
            OstinatoOptions o = strings_options();
            o.apply_preset(*presets().find(id));
            o.meter = MeterMap(sig);
            o.evolve = true;
            o.call_response = true;
            auto v = Generator(o).generate(5);
            std::string fault = validate_notes(v.notes, v.start, v.end);
            CHECK(fault.empty());
            if (!fault.empty()) std::fprintf(stderr, "    %s %s: %s\n", id.c_str(), sig.to_string().c_str(), fault.c_str());
        }
    }
}

TEST_CASE("acceptance 14: three distinct auditionable variants for every preset")
{
    for (const auto& id : presets().ids()) {
        OstinatoOptions o = strings_options();
        o.apply_preset(*presets().find(id));
        auto vs = Generator(o).generate_variants(2026, 3);
        REQUIRE(vs.size() == 3);
        CHECK_EQ(vs[0].id, std::string("A"));
        CHECK_EQ(vs[2].id, std::string("C"));
        bool distinct = vs[0].notes != vs[1].notes && vs[1].notes != vs[2].notes && vs[0].notes != vs[2].notes;
        CHECK(distinct);
        if (!distinct) std::fprintf(stderr, "    preset %s produced duplicate variants\n", id.c_str());
    }
}

TEST_CASE("chord-follow adapts pitches at chord boundaries without changing duration")
{
    theory::ProgressionCatalog cat;
    REQUIRE(cat.load_file(std::string(VCC_CONTENT_DIR) + "/progressions.json"));
    theory::ProgressionRenderOptions po;
    po.key = *theory::Key::parse("D minor");
    po.length_bars = 16;
    po.expand = theory::ExpandMode::Repeat;
    auto prog = theory::render_progression(*cat.find("romantic_cinematic"), MeterMap{}, po);
    REQUIRE(prog);
    OstinatoOptions o = strings_options();
    for (const auto& rc : prog->chords) o.harmony.push_back({rc.start, rc.start + rc.duration, rc.chord});
    auto followed = Generator(o).generate(77);
    OstinatoOptions plain = strings_options();
    auto unfollowed = Generator(plain).generate(77);
    CHECK_EQ(followed.end, unfollowed.end);
    CHECK_EQ(followed.notes.size(), unfollowed.notes.size());
    // Every note is a tone of the chord sounding at its onset.
    for (const auto& n : followed.notes) {
        for (const auto& h : o.harmony) {
            if (n.start >= h.start && n.start < h.end) {
                bool member = false;
                for (const auto& t : h.chord.tones()) member = member || t.pitch_class() == n.pitch % 12;
                CHECK(member);
            }
        }
    }
}

TEST_CASE("vary changes one aspect and honours locks")
{
    Generator g(strings_options());
    auto a = g.generate(10);
    Variant locked = a;
    locked.locks.rhythm = true;
    auto b = g.vary(locked, Aspect::Pitch, "B");
    // Same rhythm (onsets) because rhythm is locked and only pitch changed.
    REQUIRE(a.notes.size() == b.notes.size());
    bool pitch_changed = false;
    for (std::size_t i = 0; i < a.notes.size(); ++i) {
        CHECK_EQ(a.notes[i].start, b.notes[i].start);
        pitch_changed = pitch_changed || a.notes[i].pitch != b.notes[i].pitch;
    }
    CHECK(pitch_changed);
    // Locking step 0's pitch keeps every bar's first note.
    Variant step_locked = a;
    step_locked.locks.steps = {0};
    auto c = g.vary(step_locked, Aspect::Pitch, "C");
    CHECK_EQ(c.notes.front().pitch, a.notes.front().pitch);
}

TEST_CASE("variants persist as seeds and regenerate exactly")
{
    Generator g(strings_options());
    auto a = g.generate(31337);
    Variant locked = a;
    locked.locks.steps = {0, 3};
    auto b = g.vary(locked, Aspect::Rhythm, "B");
    auto restored = Variant::from_json(*json::parse(json::write(b.to_json())));
    REQUIRE(restored);
    CHECK(g.regenerate(*restored).notes == b.notes);
    CHECK(!Variant::from_json(*json::parse("{\"id\":\"x\",\"seed\":\"zz\"}")));
}

TEST_CASE("percussion mode uses drum mapping, not pitched keys")
{
    OstinatoOptions o = strings_options();
    o.apply_preset(*presets().find("syncopated_percussion"));
    auto v = Generator(o).generate(3);
    std::set<int> allowed(o.drum_notes.begin(), o.drum_notes.end());
    for (const auto& n : v.notes) CHECK(allowed.count(n.pitch) == 1);
    CHECK_EQ(v.notes.front().pitch, 36);  // kick on the downbeat
}

TEST_CASE("custom pitch collections drive scale-tone patterns")
{
    OstinatoOptions o = strings_options();
    o.pitch_mode = PitchMode::ScaleTones;
    o.key = *theory::Key::parse("D minor");
    o.custom_scale = theory::PitchCollection{"Hijaz-like (12-TET approximation)", {0, 100, 400, 500, 700, 800, 1000}, ""};
    auto v = Generator(o).generate(8);
    std::set<int> pcs = {2, 3, 6, 7, 9, 10, 0};
    for (const auto& n : v.notes) CHECK(pcs.count(n.pitch % 12) == 1);
}
