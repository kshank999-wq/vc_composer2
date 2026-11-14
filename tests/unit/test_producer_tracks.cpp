// Producer view tracks (docs/spec/ui, "Producer view"): an instrument track
// carries its own audio lane, and plays its MIDI, that lane, or both.
#include "vcc/session/Session.h"
#include "vcc_test.h"

using namespace vcc;

namespace {
const session::Catalogs& catalogs()
{
    static auto c = session::Catalogs::load_from_directory(VCC_CONTENT_DIR);
    return *c;
}
}  // namespace

TEST_CASE("an instrument track plays MIDI, its audio lane or both, saved and undoable")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto t = s.add_track("Flute", "woodwinds.flute");
    REQUIRE(!t.empty());
    CHECK(s.data().find_track(t)->playback == "both");
    CHECK(s.set_playback(t, "audio"));
    CHECK(s.data().find_track(t)->playback == "audio");
    CHECK(!s.set_playback(t, "audio"));    // unchanged
    CHECK(!s.set_playback(t, "loud"));     // not a choice
    s.undo();
    CHECK(s.data().find_track(t)->playback == "both");
    CHECK(s.set_playback(t, "midi"));

    // An audio clip on the instrument track's own lane.
    project::AudioClip clip;
    clip.file = "Audio Files/flute_PRINT_01.wav";
    clip.length = 2.0;
    clip.channels = 2;
    clip.file_sample_rate = 48000.0;
    CHECK(!s.add_audio_clips({{t, clip}}, "Print").empty());
    CHECK(s.data().find_track(t)->active_version()->audio_clips.size() == 1);

    // An audio track has no choice to make.
    const auto a = s.add_audio_track("Vox", false);
    CHECK(!s.set_playback(a, "midi"));

    const std::string path = std::string(VCC_TEST_TMP_DIR) + "/producer.vcc2.json";
    std::string err;
    REQUIRE(s.save_as(path, &err));
    session::Session r(catalogs());
    REQUIRE(r.open(path, &err));
    CHECK(r.data().find_track(t)->playback == "midi");
    CHECK(r.data().find_track(t)->active_version()->audio_clips.size() == 1);
}
