// Producer view tracks (docs/spec/ui, "Producer view"): an instrument track
// carries its own audio lane, and plays its MIDI, that lane, or both.
#include <filesystem>

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

    // On an audio track it is a bounce in place: the clips it covers move to
    // a take lane, and the track keeps playing audio.
    const auto a = s.add_audio_track("Vox", false);
    project::AudioClip original = clip;
    original.file = "Audio Files/Vox_01.wav";
    REQUIRE(s.add_audio_clips({{a, original}}, "Record", false).size() == 1u);
    CHECK(s.place_prints({{a, clip}}, true, true).size() == 1u);
    CHECK_EQ(s.undo_label(), std::string("Bounce in place"));
    const auto& clips = s.data().find_track(a)->active_version()->audio_clips;
    REQUIRE(clips.size() == 2u);
    CHECK_EQ(clips[0].lane, 0);
    CHECK_EQ(clips[0].file, clip.file);
    CHECK_EQ(clips[1].lane, 1);
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
    // Instrument tracks can send to hardware too (the track header's MIDI Out); audio tracks cannot.
    const auto f = s.add_track("Flute", "woodwinds.flute");
    CHECK(s.set_midi_output(f, "Hardware Synth Port 1"));
    const auto a = s.add_audio_track("Vox", false);
    CHECK(!s.set_midi_output(a, "Hardware Synth Port 1"));
}

TEST_CASE("comping: part of a take replaces the main lane there, splitting what it covers")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto a = s.add_audio_track("Vox", false);
    project::AudioClip c;
    c.file = "Audio Files/take1.wav";
    c.length = 8.0;
    c.channels = 1;
    c.file_sample_rate = 48000.0;
    s.add_audio_clips({{a, c}}, "Record");  // take 1
    c.file = "Audio Files/take2.wav";
    const auto take2 = s.add_audio_clips({{a, c}}, "Record");  // take 2 on lane 0, take 1 to lane 1
    const auto& clips = s.data().find_track(a)->active_version()->audio_clips;
    std::string take1;
    for (const auto& x : clips)
        if (x.lane == 1) take1 = x.id;
    REQUIRE(!take1.empty());
    CHECK(s.comp_take(a, take1, 2.0, 5.0));
    std::vector<project::AudioClip> main;
    for (const auto& x : s.data().find_track(a)->active_version()->audio_clips)
        if (x.lane == 0) main.push_back(x);
    REQUIRE(main.size() == 3);
    // take 2 [0,2), take 1 [2,5) read from 2 s into its file, take 2 [5,8) from 5 s in.
    CHECK(main[0].file == "Audio Files/take2.wav");
    CHECK(std::abs(main[0].length - 2.0) < 1e-9);
    CHECK(main[1].file == "Audio Files/take1.wav");
    CHECK(std::abs(main[1].start_seconds - 2.0) < 1e-9);
    CHECK(std::abs(main[1].source_offset - 2.0) < 1e-9);
    CHECK(std::abs(main[1].length - 3.0) < 1e-9);
    CHECK(main[2].file == "Audio Files/take2.wav");
    CHECK(std::abs(main[2].start_seconds - 5.0) < 1e-9);
    CHECK(std::abs(main[2].source_offset - 5.0) < 1e-9);
    CHECK(main[0].id != main[2].id);
    // The takes stay on their lanes; one undo puts the main lane back.
    s.undo();
    int on_main = 0;
    for (const auto& x : s.data().find_track(a)->active_version()->audio_clips) on_main += x.lane == 0 ? 1 : 0;
    CHECK(on_main == 1);
    CHECK(!s.comp_take(a, take2.front(), 1.0, 2.0));  // a main-lane clip is not a take
}

TEST_CASE("recorded MIDI lands as a region, earlier takes moving to take lanes; record source")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto t = s.add_track("Flute", "woodwinds.flute");
    project::MidiRegion r;
    r.start = 0;
    r.end = 4 * kTicksPerQuarter;
    r.source = "recorded";
    r.notes.push_back(NoteEvent{0, kTicksPerQuarter, 60, 90, 0, {}});
    REQUIRE(s.add_recorded_regions({{t, r}}, "Record MIDI").size() == 1);
    REQUIRE(s.add_recorded_regions({{t, r}}, "Record MIDI").size() == 1);
    int lane0 = 0, lane1 = 0;
    for (const auto& x : s.data().find_track(t)->active_version()->regions) (x.lane == 0 ? lane0 : lane1)++;
    CHECK(lane0 == 1);
    CHECK(lane1 == 1);
    CHECK(s.data().find_track(t)->record_source == "midi");
    CHECK(s.set_record_source(t, "audio"));
    CHECK(s.data().find_track(t)->record_source == "audio");
    const auto a = s.add_audio_track("Vox", false);
    CHECK(!s.set_record_source(a, "midi"));
}

TEST_CASE("clip editing: split, crossfade, nudge and replace, each one undo step")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto a = s.add_audio_track("Gtr", false);
    project::AudioClip c;
    c.file = "Audio Files/gtr.wav";
    c.length = 8.0;
    c.fade_in = 0.5;
    c.fade_out = 1.0;
    c.channels = 1;
    c.file_sample_rate = 48000.0;
    const auto ids = s.add_audio_clips({{a, c}}, "Record");
    REQUIRE(ids.size() == 1);
    auto clips = [&] { return s.data().find_track(a)->active_version()->audio_clips; };

    // Split at 3 s: [0,3) keeps the fade in, [3,8) the fade out, reading on from 3 s.
    const auto right = s.split_audio_clips({a}, 3.0);
    REQUIRE(right.size() == 1);
    CHECK(s.undo_label() == "Split clip");
    auto v = clips();
    REQUIRE(v.size() == 2);
    CHECK(v[0].id == ids[0]);
    CHECK(std::abs(v[0].length - 3.0) < 1e-9);
    CHECK(std::abs(v[0].fade_in - 0.5) < 1e-9);
    CHECK(v[0].fade_out == 0.0);
    CHECK(v[1].id == right[0]);
    CHECK(std::abs(v[1].start_seconds - 3.0) < 1e-9);
    CHECK(std::abs(v[1].source_offset - 3.0) < 1e-9);
    CHECK(std::abs(v[1].length - 5.0) < 1e-9);
    CHECK(v[1].fade_in == 0.0);
    CHECK(std::abs(v[1].fade_out - 1.0) < 1e-9);
    CHECK(s.split_audio_clips({a}, 3.002).empty());  // never a sliver

    // Crossfade where they meet: the right one starts 20 ms earlier into its file.
    CHECK(s.crossfade_audio_clips(a, ids[0], right[0], 0.02));
    CHECK(s.undo_label() == "Crossfade");
    v = clips();
    CHECK(std::abs(v[1].start_seconds - 2.98) < 1e-9);
    CHECK(std::abs(v[1].source_offset - 2.98) < 1e-9);
    CHECK(std::abs(v[0].fade_out - 0.02) < 1e-9);
    CHECK(std::abs(v[1].fade_in - 0.02) < 1e-9);
    // A clip with no audio before it cannot reach back.
    s.undo();
    s.update_audio_clip(a, right[0], "t", [](project::AudioClip& x) { x.source_offset = 0.0; });
    CHECK(!s.crossfade_audio_clips(a, ids[0], right[0], 0.02));

    // Nudge: both move; never before zero.
    CHECK(s.nudge_audio_clips(a, {ids[0], right[0]}, 0.25));
    CHECK(s.undo_label() == "Nudge clip");
    v = clips();
    CHECK(std::abs(v[0].start_seconds - 0.25) < 1e-9);
    CHECK(std::abs(v[1].start_seconds - 3.25) < 1e-9);
    CHECK(s.nudge_audio_clips(a, {ids[0]}, -1.0));
    CHECK(clips()[0].start_seconds == 0.0);

    // Replace (consolidate): two clips become one.
    project::AudioClip whole = c;
    whole.file = "Audio Files/gtr_CONSOL_01.wav";
    whole.fade_in = whole.fade_out = 0.0;
    const auto merged = s.replace_audio_clips(a, {ids[0], right[0]}, whole, "Consolidate");
    CHECK(!merged.empty());
    CHECK(s.undo_label() == "Consolidate");
    v = clips();
    REQUIRE(v.size() == 1);
    CHECK(v[0].id == merged);
    s.undo();
    CHECK(clips().size() == 2);
}

TEST_CASE("sidechain: strips and compressor/gate inserts keyed from another track or bus, saved and undoable")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto kick = s.add_audio_track("Kick", false);
    const auto bass = s.add_audio_track("Bass", false);
    const auto fx = s.add_bus("Verb", "fx");
    CHECK(s.set_sidechain(bass, -1, kick));
    CHECK(s.undo_label() == "Sidechain");
    CHECK(s.data().find_track(bass)->sidechain == kick);
    CHECK(!s.set_sidechain(bass, -1, kick));        // no change: no step
    CHECK(!s.set_sidechain(bass, -1, bass));        // not itself
    CHECK(!s.set_sidechain(bass, -1, "nope"));      // unknown source
    CHECK(s.set_sidechain(fx, -1, kick));           // a bus keyed from a track
    CHECK(s.data().find_bus(fx)->sidechain == kick);

    // A compressor insert keyed; other kinds cannot be.
    s.update_track_mix(bass, "inserts", [](project::Track& t) {
        project::InsertSlot comp;
        comp.kind = "compressor";
        comp.suggestion = false;
        project::InsertSlot verb;
        verb.kind = "reverb";
        verb.suggestion = false;
        t.inserts = {comp, verb};
    });
    CHECK(s.set_sidechain(bass, 0, fx));
    CHECK(!s.set_sidechain(bass, 1, kick));
    CHECK(s.data().find_track(bass)->inserts[0].sidechain == fx);

    const auto file = std::filesystem::temp_directory_path() / "vcc_sidechain_test.vcc2";
    std::string error;
    REQUIRE(s.save_as(file.string(), &error));
    session::Session back(catalogs());
    REQUIRE(back.open(file.string(), &error));
    CHECK(back.data().find_track(bass)->sidechain == kick);
    CHECK(back.data().find_track(bass)->inserts[0].sidechain == fx);
    CHECK(back.data().find_bus(fx)->sidechain == kick);
    std::filesystem::remove(file);

    CHECK(s.set_sidechain(bass, -1, ""));  // back to its own signal
    CHECK(s.data().find_track(bass)->sidechain.empty());
    s.undo();
    CHECK(s.data().find_track(bass)->sidechain == kick);
}

TEST_CASE("edit modes: Shuffle closes and opens gaps; a range deletes across clips")
{
    session::Session s(catalogs());
    s.new_empty();
    const auto a = s.add_audio_track("Gtr", false);
    // Three 2 s clips at 0, 2 and 5 s (a 1 s gap before the third).
    std::vector<std::pair<std::string, project::AudioClip>> in;
    for (double at : {0.0, 2.0, 5.0}) {
        project::AudioClip c;
        c.file = "Audio Files/gtr.wav";
        c.start_seconds = at;
        c.source_offset = at;
        c.length = 2.0;
        in.emplace_back(a, c);
    }
    const auto ids = s.add_audio_clips(in, "Record");
    REQUIRE(ids.size() == 3);
    auto clips = [&] { return s.data().find_track(a)->active_version()->audio_clips; };
    auto start_of = [&](const std::string& id) {
        for (const auto& c : clips())
            if (c.id == id) return c.start_seconds;
        return -1.0;
    };
    auto near = [](double x, double y) { return std::abs(x - y) < 1e-9; };

    // Shuffle delete: the second goes, the third moves 2 s earlier.
    CHECK(s.remove_audio_clips_shuffle({{a, ids[1]}}));
    CHECK(s.undo_label() == "Delete clip");
    CHECK(clips().size() == 2);
    CHECK(near(start_of(ids[2]), 3.0));
    s.undo();
    CHECK(near(start_of(ids[2]), 5.0));

    // Shuffle trim: the first ends 0.5 s sooner; everything after follows.
    CHECK(s.update_audio_clip_shuffle(a, ids[0], "Trim clip end", [](project::AudioClip& c) { c.length = 1.5; }));
    CHECK(s.update_audio_clip_shuffle(a, ids[0], "Trim clip end", [](project::AudioClip& c) { c.length = 1.0; }));
    CHECK(near(start_of(ids[1]), 1.0));
    CHECK(near(start_of(ids[2]), 4.0));
    s.undo();  // the drag was one step
    CHECK(near(start_of(ids[1]), 2.0));
    CHECK(near(start_of(ids[2]), 5.0));

    // Shuffle move: the third to the front pushes the others back by 2 s.
    CHECK(s.shuffle_move_audio_clip(a, ids[2], 0.3));
    CHECK(s.undo_label() == "Move clip");
    CHECK(near(start_of(ids[2]), 0.0));
    CHECK(near(start_of(ids[0]), 2.0));
    CHECK(near(start_of(ids[1]), 4.0));
    s.undo();
    // The first to just after the second: the second closes up to 0.
    CHECK(s.shuffle_move_audio_clip(a, ids[0], 2.1));
    CHECK(near(start_of(ids[1]), 0.0));
    CHECK(near(start_of(ids[0]), 2.0));
    CHECK(near(start_of(ids[2]), 5.0));
    CHECK(!s.shuffle_move_audio_clip(a, ids[0], 2.2));  // already there: no step
    s.undo();

    // A range across the first two (1 to 3 s): trimmed either side.
    CHECK(s.delete_audio_range({a}, 1.0, 3.0, false));
    CHECK(s.undo_label() == "Delete range");
    auto v = clips();
    REQUIRE(v.size() == 3);
    CHECK(near(v[0].length, 1.0));
    CHECK(near(v[1].start_seconds, 3.0));
    CHECK(near(v[1].source_offset, 3.0));
    CHECK(near(v[1].length, 1.0));
    CHECK(near(v[2].start_seconds, 5.0));
    s.undo();
    // Inside one clip with Shuffle: it splits, and everything after closes up.
    CHECK(s.delete_audio_range({a}, 2.5, 3.0, true));
    v = clips();
    REQUIRE(v.size() == 4);
    CHECK(near(v[1].length, 0.5));
    CHECK(near(v[2].start_seconds, 2.5));
    CHECK(near(v[2].source_offset, 3.0));
    CHECK(near(v[2].length, 1.0));
    CHECK(v[2].id != v[1].id);
    CHECK(near(v[3].start_seconds, 4.5));
}
