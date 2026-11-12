#include "vcc/core/Midi.h"
#include "vcc_test.h"

using namespace vcc;

TEST_CASE("note validation catches overlaps and range faults")
{
    std::vector<NoteEvent> ok = {{0, 100, 60, 80, 0, {}}, {100, 100, 60, 80, 0, {}}};
    CHECK(validate_notes(ok, 0, 200).empty());
    std::vector<NoteEvent> overlap = {{0, 150, 60, 80, 0, {}}, {100, 100, 60, 80, 0, {}}};
    CHECK(!validate_notes(overlap, 0, 400).empty());
    CHECK(!validate_notes(ok, 0, 199).empty());
    std::vector<NoteEvent> zero = {{0, 0, 60, 80, 0, {}}};
    CHECK(!validate_notes(zero, 0, 10).empty());
}

TEST_CASE("midi file has header, conductor and note tracks")
{
    MidiFileOptions o;
    o.tempo = TempoMap(96.0);
    MidiFileTrack t{"Strings", {{0, kTicksPerQuarter, 60, 90, 0, {}}}, 48};
    std::string f = write_midi_file({t}, o);
    REQUIRE(f.size() > 30);
    CHECK_EQ(f.substr(0, 4), std::string("MThd"));
    CHECK_EQ(static_cast<int>(static_cast<unsigned char>(f[11])), 2);  // two tracks
    CHECK_EQ((static_cast<unsigned char>(f[12]) << 8) | static_cast<unsigned char>(f[13]), 960);
    CHECK(f.find("Strings") != std::string::npos);
    // Note-on 0x90 60 90 present.
    CHECK(f.find(std::string("\x90\x3C\x5A", 3)) != std::string::npos);
}
