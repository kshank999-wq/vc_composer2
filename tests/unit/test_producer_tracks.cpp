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

TEST_CASE("a print lands on the track's own audio lane as one undo step, the last print on a take lane")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto t = s.add_track("Flute", "woodwinds.flute");
    project::AudioClip clip;
    clip.file = "Audio Files/Flute_PRINT_01.wav";
    clip.name = "Flute_PRINT_01";
    clip.length = 4.0;
    clip.channels = 2;
    clip.file_sample_rate = 48000.0;
    const auto first = s.place_prints({{t, clip}}, false, true);
    REQUIRE(first.size() == 1);
    const auto* track = s.data().find_track(t);
    CHECK(track->playback == "audio");
    CHECK(!track->printed_fx);
    CHECK(track->active_version()->audio_clips.size() == 1);

    clip.file = "Audio Files/Flute_PRINT_02.wav";
    const auto second = s.place_prints({{t, clip}}, true, true);
    REQUIRE(second.size() == 1);
    track = s.data().find_track(t);
    CHECK(track->printed_fx);
    int on_main = 0, on_take = 0;
    for (const auto& c : track->active_version()->audio_clips) (c.lane == 0 ? on_main : on_take)++;
    CHECK(on_main == 1);
    CHECK(on_take == 1);

    s.undo();  // the second print, its FX flag and the take lane together
    track = s.data().find_track(t);
    CHECK(!track->printed_fx);
    CHECK(track->active_version()->audio_clips.size() == 1);
    CHECK(track->active_version()->audio_clips[0].lane == 0);
    s.undo();
    CHECK(s.data().find_track(t)->playback == "both");
    CHECK(s.data().find_track(t)->active_version()->audio_clips.empty());

    // Not onto an audio track.
    const auto a = s.add_audio_track("Vox", false);
    CHECK(s.place_prints({{a, clip}}, false, true).empty());
}

TEST_CASE("a MIDI track sends to an external device, saved and undoable")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto m = s.add_midi_track("Analog Synth");
    REQUIRE(!m.empty());
    CHECK(s.data().find_track(m)->kind == project::TrackKind::MidiOnly);
    CHECK(s.data().find_track(m)->midi_output.empty());
    CHECK(s.set_midi_output(m, "Hardware Synth Port 1"));
    CHECK(!s.set_midi_output(m, "Hardware Synth Port 1"));
    const std::string path = std::string(VCC_TEST_TMP_DIR) + "/midi-out.vcc2.json";
    std::string err;
    REQUIRE(s.save_as(path, &err));
    session::Session r(catalogs());
    REQUIRE(r.open(path, &err));
    CHECK(r.data().find_track(m)->midi_output == "Hardware Synth Port 1");
    s.undo();
    CHECK(s.data().find_track(m)->midi_output.empty());
    // Only MIDI tracks send to hardware.
    const auto f = s.add_track("Flute", "woodwinds.flute");
    CHECK(!s.set_midi_output(f, "Hardware Synth Port 1"));
}
