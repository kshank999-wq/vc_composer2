#include <set>

#include "vcc/theory/Progression.h"
#include "vcc_test.h"

using namespace vcc;
using namespace vcc::theory;

static ProgressionCatalog load()
{
    ProgressionCatalog cat;
    std::string err;
    if (!cat.load_file(std::string(VCC_CONTENT_DIR) + "/progressions.json", &err))
        std::fprintf(stderr, "    %s\n", err.c_str());
    return cat;
}

TEST_CASE("shipped progression catalog loads and every numeral resolves in every key")
{
    auto cat = load();
    REQUIRE(cat.all().size() >= 16);
    for (const auto& p : cat.all()) {
        for (const char* tonic : {"C", "G", "F#", "Eb", "B", "Db"}) {
            Key k{*SpelledPitch::parse(tonic), p.default_mode};
            std::string err;
            auto chords = p.chords(k, &err);
            CHECK(chords.has_value());
            if (!chords) std::fprintf(stderr, "    %s in %s: %s\n", p.id.c_str(), tonic, err.c_str());
        }
    }
    for (const char* id : {"pop_uplift", "pop_introspective", "classical_cadence", "classical_authentic",
                           "jazz_turnaround", "jazz_ii_v_i", "minor_jazz", "blues_12bar", "minor_blues",
                           "romantic_cinematic", "suspense_pedal", "heroic_minor", "heroic_mixolydian",
                           "whimsical", "modal_folk", "neo_soul", "industrial_ambient"})
        CHECK(cat.find(id) != nullptr);
}

TEST_CASE("acceptance 12: 4-bar I-V-vi-IV in G at bar 5 writes G D Em C there")
{
    auto cat = load();
    const Progression* p = cat.find("pop_uplift");
    REQUIRE(p);
    ProgressionRenderOptions o;
    o.key = *Key::parse("G major");
    o.start_bar = 5;
    o.length_bars = 4;
    MeterMap meter;
    std::string err;
    auto r = render_progression(*p, meter, o, &err);
    REQUIRE(r);
    REQUIRE(r->chords.size() == 4);
    const char* expected[] = {"G", "D", "Em", "C"};
    const std::set<int> pcs[] = {{7, 11, 2}, {2, 6, 9}, {4, 7, 11}, {0, 4, 7}};
    for (int i = 0; i < 4; ++i) {
        CHECK_EQ(r->chords[i].chord.name(), std::string(expected[i]));
        CHECK_EQ(r->chords[i].start, meter.bar_start(5 + i));
        CHECK_EQ(r->chords[i].duration, kTicksPerWhole);
        std::set<int> got;
        for (int n : r->chords[i].voicing.notes) got.insert(n % 12);
        CHECK(got == pcs[i]);
    }
    CHECK_EQ(r->start, meter.bar_start(5));
    CHECK_EQ(r->end, meter.bar_start(9));
    CHECK(validate_notes(r->notes, r->start, r->end).empty());
    // Every chord's notes start on its bar.
    for (const auto& n : r->notes) CHECK((n.start - r->start) % kTicksPerWhole == 0);
}

TEST_CASE("8 bars stretches or repeats, exactly filling the region")
{
    auto cat = load();
    const Progression* p = cat.find("pop_uplift");
    REQUIRE(p);
    ProgressionRenderOptions o;
    o.key = *Key::parse("D major");
    o.length_bars = 8;
    MeterMap meter(TimeSignature{3, 4});
    auto stretch = render_progression(*p, meter, o);
    REQUIRE(stretch);
    CHECK_EQ(stretch->chords.size(), 4u);
    CHECK_EQ(stretch->chords[1].start, meter.bar_start(3));
    o.expand = ExpandMode::Repeat;
    auto repeat = render_progression(*p, meter, o);
    REQUIRE(repeat);
    CHECK_EQ(repeat->chords.size(), 8u);
    CHECK_EQ(repeat->end, meter.bar_start(9));
    CHECK(validate_notes(repeat->notes, repeat->start, repeat->end).empty());
}

TEST_CASE("12-bar blues refuses silent truncation")
{
    auto cat = load();
    const Progression* p = cat.find("blues_12bar");
    REQUIRE(p);
    ProgressionRenderOptions o;
    o.key = *Key::parse("A mixolydian");
    o.length_bars = 8;
    std::string err;
    CHECK(!render_progression(*p, MeterMap{}, o, &err));
    CHECK(err.find("12-bar") != std::string::npos);
    o.length_bars = 12;
    auto r = render_progression(*p, MeterMap{}, o, &err);
    REQUIRE(r);
    CHECK_EQ(r->chords[4].chord.name(), std::string("D7"));
    o.length_bars = 24;
    CHECK(!render_progression(*p, MeterMap{}, o, &err));  // stretching a fixed form
    o.expand = ExpandMode::Repeat;
    CHECK(render_progression(*p, MeterMap{}, o, &err).has_value());
}

TEST_CASE("half-bar chords and every rhythm stay inside the region in odd meters")
{
    auto cat = load();
    const Progression* p = cat.find("classical_cadence_64");
    REQUIRE(p);
    for (auto rhythm : {ChordRhythm::Block, ChordRhythm::BeatPulse, ChordRhythm::Arpeggio, ChordRhythm::Strum}) {
        for (TimeSignature sig : {TimeSignature{4, 4}, TimeSignature{7, 8}, TimeSignature{6, 8}, TimeSignature{5, 4}}) {
            ProgressionRenderOptions o;
            o.key = *Key::parse("Bb major");
            o.rhythm = rhythm;
            o.start_bar = 3;
            MeterMap meter(sig);
            auto r = render_progression(*p, meter, o);
            REQUIRE(r);
            CHECK_EQ(r->chords[2].start, meter.bar_start(5));
            CHECK_EQ(r->chords[3].start, meter.bar_start(5) + sig.ticks_per_bar() / 2);
            std::string fault = validate_notes(r->notes, r->start, r->end);
            CHECK(fault.empty());
            if (!fault.empty()) std::fprintf(stderr, "    %s %s: %s\n", to_string(rhythm), sig.to_string().c_str(), fault.c_str());
        }
    }
}

TEST_CASE("search finds progressions by tag")
{
    auto cat = load();
    CHECK(!cat.search("suspense").empty());
    CHECK(!cat.search("Jazz").empty());
}
