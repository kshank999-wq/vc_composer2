// Headless check of the desktop application (registered with CTest; run
// under a virtual display on Linux CI).
//
// It builds the real main window, creates a project from a template, writes
// notes through the Composer view exactly as typing would, opens every
// workspace and tool window, renders audio offline through the playback
// engine and checks it is not silent, saves and reopens, and writes a
// screenshot of each surface. Exit code 0 means every check passed.
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "MainWindow.h"
#include "PlatformFixes.h"
#include "audio/CueRenderer.h"
#include "ui/CueManager.h"
#include "ui/Dialogs.h"
#include "ui/VideoWindow.h"
#include "vcc/media/VideoDecoder.h"
#include "vcc/project/Render.h"

using namespace vcc;
using namespace vcc::desktop;

// --- Allocation counting -----------------------------------------------------------
// The playback engine promises the audio thread never allocates. That is
// measured here, not asserted: the global allocator is replaced and counts
// only while `counting` is set on the calling thread.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"  // false positive on replaced operators
#endif

namespace {
thread_local bool counting = false;
std::atomic<long> allocations{0};
}  // namespace

void* operator new(std::size_t n)
{
    if (counting) ++allocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n)
{
    if (counting) ++allocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

int failures = 0;

void check(bool ok, const juce::String& what)
{
    std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what.toRawUTF8());
    if (!ok) ++failures;
}

void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

void shoot(juce::Component& c, const juce::File& dir, const juce::String& name)
{
    pump(150);
    auto image = c.createComponentSnapshot(c.getLocalBounds(), true, 1.0f);
    juce::FileOutputStream out(dir.getChildFile(name + ".png"));
    if (out.openedOk()) {
        out.setPosition(0);
        out.truncate();
        juce::PNGImageFormat().writeImageToStream(image, out);
    }
    // A blank surface is a failed render, not a picture.
    std::map<juce::uint32, int> colours;
    for (int y = 0; y < image.getHeight(); y += 7)
        for (int x = 0; x < image.getWidth(); x += 7) ++colours[image.getPixelAt(x, y).getARGB()];
    check(colours.size() > 8, name + " screenshot has content (" + juce::String(colours.size()) + " colours)");
}

juce::Component* find_dialog(const juce::String& title)
{
    for (int i = juce::TopLevelWindow::getNumTopLevelWindows(); --i >= 0;)
        if (auto* w = juce::TopLevelWindow::getTopLevelWindow(i); w && w->getName() == title && w->isVisible()) return w;
    return nullptr;
}

// --- Console and recording (Main Spec §11, §12) ----------------------------------------
// Runs in a fresh, saved project so recordings land in its "Audio Files"
// folder. Signals go in through render_offline_with_input, exactly as a
// device's inputs would.

struct Measure {
    double rms = 0.0;  ///< Left output channel.
    float peak = 0.0f;
};

class Rig {
public:
    explicit Rig(PlaybackEngine& e) : engine(e) {}

    /// Renders `seconds` with `signal(channel, sample)` on two inputs.
    Measure run(double seconds, const std::function<float(int, std::int64_t)>& signal = {})
    {
        engine.rebuild_now();  // as the engine's timer would after an edit
        const int blocks = static_cast<int>(seconds * 48000.0 / 512.0 + 0.5);
        double sum = 0.0;
        Measure m;
        for (int b = 0; b < blocks; ++b) {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i) input.setSample(ch, i, signal ? signal(ch, clock + i) : 0.0f);
            engine.render_offline_with_input(input, out, 512);
            clock += 512;
            for (int i = 0; i < 512; ++i) sum += static_cast<double>(out.getSample(0, i)) * out.getSample(0, i);
            m.peak = std::max({m.peak, out.getMagnitude(0, 0, 512), out.getMagnitude(1, 0, 512)});
        }
        m.rms = blocks > 0 ? std::sqrt(sum / (blocks * 512.0)) : 0.0;
        return m;
    }
    static std::function<float(int, std::int64_t)> sine(double hz, float amp, int only_channel = -1)
    {
        return [=](int ch, std::int64_t i) {
            if (only_channel >= 0 && ch != only_channel) return 0.0f;
            return amp * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * hz * static_cast<double>(i) / 48000.0));
        };
    }

    PlaybackEngine& engine;
    juce::AudioBuffer<float> input{2, 512}, out{2, 512};
    std::int64_t clock = 0;
};

const project::AudioClip* lane_clip(const session::Session& s, const std::string& track, int lane)
{
    const auto* t = s.data().find_track(track);
    if (!t || !t->active_version()) return nullptr;
    for (const auto& c : t->active_version()->audio_clips)
        if (c.lane == lane) return &c;
    return nullptr;
}

void console_and_recording_checks(AppContext& ctx, const juce::File& dir)
{
    auto& session = ctx.session;
    auto& engine = ctx.engine;
    session.new_empty();
    const auto project_file = dir.getChildFile("mix-check.vcc2");
    dir.getChildFile("Audio Files").deleteRecursively();
    std::string error;
    check(session.save_as(project_file.getFullPathName().toStdString(), &error), "the console check project saves");
    engine.stop();
    engine.prepare_offline(48000.0, 512);
    Rig rig(engine);

    juce::String last_message;
    bool last_ok = false;
    int finished = 0;
    auto previous = engine.on_recording_finished;
    engine.on_recording_finished = [&](const juce::String& m, bool ok) {
        last_message = m;
        last_ok = ok;
        ++finished;
        if (previous) previous(m, ok);
    };

    // A mono audio track listening to input 1, stopped: the strip processes it.
    const std::string tone = session.add_audio_track("Tone", false);
    session.update_track_mix(tone, "monitor", [](project::Track& t) { t.monitor = "input"; });
    rig.run(0.2, Rig::sine(100.0, 0.5f));
    const double flat = rig.run(0.3, Rig::sine(100.0, 0.5f)).rms;
    check(flat > 0.2 && flat < 0.3, "a mono input monitors through the strip at -3 dB centre (rms " + juce::String(flat, 3) + ")");

    session.update_track_mix(tone, "hpf", [](project::Track& t) {
        t.strip.hpf_on = true;
        t.strip.hpf_hz = 400.0;
    });
    rig.run(0.2, Rig::sine(100.0, 0.5f));
    const double filtered = rig.run(0.3, Rig::sine(100.0, 0.5f)).rms;
    check(filtered < flat * 0.15, "HPF at 400 Hz cuts a 100 Hz tone (" + juce::String(juce::Decibels::gainToDecibels(filtered / flat), 1) + " dB)");
    session.update_track_mix(tone, "eq", [](project::Track& t) {
        t.strip.hpf_on = false;
        t.strip.eq_on = true;
        t.strip.lf.gain_db = 12.0;
        t.strip.lf.freq = 300.0;
    });
    rig.run(0.2, Rig::sine(100.0, 0.5f));
    const double boosted = rig.run(0.3, Rig::sine(100.0, 0.5f)).rms;
    rig.run(0.2, Rig::sine(5000.0, 0.5f));
    const double high = rig.run(0.3, Rig::sine(5000.0, 0.5f)).rms;
    check(boosted > flat * 2.5 && high < flat * 1.15,
          "a +12 dB LF shelf boosts 100 Hz (" + juce::String(juce::Decibels::gainToDecibels(boosted / flat), 1) +
              " dB) and leaves 5 kHz alone (" + juce::String(juce::Decibels::gainToDecibels(high / flat), 1) + " dB)");

    session.update_track_mix(tone, "comp", [](project::Track& t) {
        t.strip.eq_on = false;
        t.strip.comp_on = true;
        t.strip.comp_threshold_db = -30.0;
        t.strip.comp_ratio = 4.0;
    });
    rig.run(0.2, Rig::sine(1000.0, 0.5f));
    engine.take_meter(tone);
    const double compressed = rig.run(0.3, Rig::sine(1000.0, 0.5f)).rms;
    const auto comp_meter = engine.take_meter(tone);
    check(comp_meter.gain_reduction_db < -10.0f && compressed < flat * 0.5,
          "the compressor reports gain reduction on a loud signal (" + juce::String(comp_meter.gain_reduction_db, 1) + " dB)");
    session.update_track_mix(tone, "gate", [](project::Track& t) {
        t.strip.comp_on = false;
        t.strip.gate_on = true;
        t.strip.gate_threshold_db = -40.0;
    });
    rig.run(0.3, Rig::sine(1000.0, 0.001f));
    check(engine.take_meter(tone).gate_closed, "the gate closes on a quiet signal");
    session.update_track_mix(tone, "gate", [](project::Track& t) { t.strip.gate_on = false; });

    // Sends: pre-fader still feeds the fx bus with the fader down.
    const std::string fx = session.add_bus("Verb", "fx");
    session.update_track_mix(tone, "send", [&](project::Track& t) {
        project::Send s(fx, 0.0);
        s.pre_fader = true;
        t.sends = {s};
        t.gain_db = -96.0;
    });
    rig.run(0.1, Rig::sine(440.0, 0.5f));
    engine.take_meter(tone);
    engine.take_meter(fx);
    rig.run(0.3, Rig::sine(440.0, 0.5f));
    const auto tone_meter = engine.take_meter(tone);
    const auto fx_meter = engine.take_meter(fx);
    check(tone_meter.left < 1.0e-4f && fx_meter.left > 0.01f,
          "a pre-fader send feeds the fx bus with the fader down (fx " + juce::String(fx_meter.left, 3) + ", channel " +
              juce::String(tone_meter.left, 5) + ")");

    // Reverb tail survives a document edit (persistent processors).
    rig.run(0.3);
    const double tail_before = rig.run(0.05).rms;
    session.rename_track(tone, "Tone 2");
    engine.rebuild_now();
    const double tail_after = rig.run(0.05).rms;
    check(tail_before > 1.0e-4 && tail_after > tail_before * 0.2,
          "the reverb tail survives a document edit (" + juce::String(tail_before, 5) + " -> " + juce::String(tail_after, 5) + ")");

    // Mute and solo (solo-in-place; a soloed strip keeps its buses audible).
    const std::string a = session.add_audio_track("A", false);
    const std::string b = session.add_audio_track("B", false);
    const std::string group = session.add_bus("Group", "group");
    session.update_track_mix(tone, "send", [](project::Track& t) { t.sends.clear(); t.monitor = "auto"; t.gain_db = 0.0; });
    session.update_track_mix(a, "monitor", [](project::Track& t) { t.monitor = "input"; });
    session.update_track_mix(b, "monitor", [&](project::Track& t) {
        t.monitor = "input";
        t.input = 1;
        t.output_bus_id = group;
    });
    auto levels = [&] {
        rig.run(0.05, Rig::sine(440.0, 0.5f));  // gain ramps settle
        engine.take_meter(a);
        engine.take_meter(b);
        engine.take_meter(group);
        rig.run(0.15, Rig::sine(440.0, 0.5f));
        return std::array<float, 3>{engine.take_meter(a).left, engine.take_meter(b).left, engine.take_meter(group).left};
    };
    auto l0 = levels();
    session.set_mute(a, true);
    auto l1 = levels();
    session.set_mute(a, false);
    session.set_solo(b, true);
    auto l2 = levels();
    session.set_solo(b, false);
    session.update_bus(group, "solo", [](project::Bus& x) { x.solo = true; });
    auto l3 = levels();
    session.update_bus(group, "solo", [](project::Bus& x) { x.solo = false; });
    check(l0[0] > 0.1f && l0[1] > 0.1f && l1[0] < 1.0e-4f && l1[1] > 0.1f, "mute silences only its own strip");
    check(l2[0] < 1.0e-4f && l2[1] > 0.1f && l2[2] > 0.1f, "solo silences the others and keeps the soloed track's bus audible");
    check(l3[0] < 1.0e-4f && l3[1] > 0.1f && l3[2] > 0.1f, "soloing a bus keeps the tracks feeding it audible");
    session.update_track_mix(a, "monitor", [](project::Track& t) { t.monitor = "auto"; });
    session.update_track_mix(b, "monitor", [](project::Track& t) { t.monitor = "auto"; });

    // Recording: an armed mono track, two seconds of 440 Hz.
    const std::string vox = session.add_audio_track("Vox", false);
    session.set_record_armed(vox, true);
    engine.set_record_enabled(true);
    const auto history = session.project().history_size();
    engine.locate(0);
    engine.play();
    rig.run(1.0, Rig::sine(440.0, 0.5f));
    check(engine.recording() && engine.record_seconds() > 0.9, "recording runs while playing (" + juce::String(engine.record_seconds(), 2) + " s)");
    rig.run(1.0, Rig::sine(440.0, 0.5f));
    engine.stop();
    const auto* take = lane_clip(session, vox, 0);
    check(session.project().history_size() == history + 1 && session.undo_label() == "Record",
          "a recording pass is one undo step \"Record\" (" + juce::String(session.undo_label()) + ")");
    check(finished == 1 && last_ok, "the recording reports back (" + last_message + ")");
    check(take && std::abs(take->length - 2.0) < 0.03 && take->start_seconds < 0.01 && take->channels == 1,
          "the recorded clip is placed at the start, 2 s long (" + juce::String(take ? take->length : 0.0, 3) + " s)");
    if (take) {
        check(juce::String(take->file).startsWith("Audio Files/Vox_"), "the take is stored relative to the project (" + juce::String(take->file) + ")");
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(
            formats.createReaderFor(juce::File(juce::String(session.resolve_audio_path(take->file)))));
        check(reader && reader->bitsPerSample == 24 && juce::approximatelyEqual(reader->sampleRate, 48000.0),
              "the take is a 24-bit WAV at the device rate");
        if (reader) {
            juce::AudioBuffer<float> wav(1, static_cast<int>(reader->lengthInSamples));
            reader->read(&wav, 0, wav.getNumSamples(), 0, true, false);
            int crossings = 0;
            for (int i = 1; i < wav.getNumSamples(); ++i)
                if (wav.getSample(0, i - 1) < 0.0f && wav.getSample(0, i) >= 0.0f) ++crossings;
            const double hz = crossings / (wav.getNumSamples() / 48000.0);
            const float peak = wav.getMagnitude(0, 0, wav.getNumSamples());
            check(std::abs(hz - 440.0) < 5.0 && peak > 0.45f && peak < 0.55f,
                  "the WAV holds the recorded sine (" + juce::String(hz, 1) + " Hz, peak " + juce::String(peak, 3) + ")");
        }
    }
    engine.set_record_enabled(false);
    session.set_record_armed(vox, false);
    engine.locate(0);
    engine.play();
    rig.run(0.2);
    engine.take_meter(vox);
    const Measure playback = rig.run(1.0);
    const auto vox_meter = engine.take_meter(vox);
    engine.stop();
    check(playback.rms > 0.1 && vox_meter.left > 0.3f, "the recorded clip plays back (rms " + juce::String(playback.rms, 3) + ")");

    // A second pass over it: the first becomes a take lane.
    session.set_record_armed(vox, true);
    engine.set_record_enabled(true);
    engine.locate(0);
    engine.play();
    rig.run(1.0, Rig::sine(880.0, 0.5f));
    engine.stop();
    const auto* first = lane_clip(session, vox, 1);
    const auto* second = lane_clip(session, vox, 0);
    check(first && second && first->file != second->file && session.undo_label() == "Record",
          "a second pass puts the first on a take lane");
    session.set_record_armed(vox, false);

    // Punch in/out: only 0.5 s .. 1.0 s is recorded.
    const std::string punch = session.add_audio_track("Punch", false);
    session.set_record_armed(punch, true);
    const auto& tempo = session.data().tempo;
    engine.set_punch(true, tempo.tick_at_seconds(0.5), tempo.tick_at_seconds(1.0));
    engine.locate(0);
    engine.play();
    rig.run(2.0, Rig::sine(440.0, 0.5f));
    engine.stop();
    const auto* punched = lane_clip(session, punch, 0);
    check(punched && std::abs(punched->start_seconds - 0.5) < 0.012 && std::abs(punched->length - 0.5) < 0.012,
          "punch records only inside the range (" + juce::String(punched ? punched->start_seconds : -1.0, 3) + " s + " +
              juce::String(punched ? punched->length : -1.0, 3) + " s)");
    engine.set_punch(false, 0, 0);
    session.set_record_armed(punch, false);

    // Everything at once on the audio thread, allocation-free: strip EQ,
    // dynamics, a send into a reverb, a clip playing, a track recording.
    session.update_track_mix(a, "all", [&](project::Track& t) {
        t.monitor = "input";
        t.strip.eq_on = t.strip.comp_on = t.strip.gate_on = t.strip.hpf_on = t.strip.lpf_on = true;
        t.strip.lf.gain_db = 3.0;
        t.sends = {project::Send(fx, -6.0)};
    });
    session.set_record_armed(vox, true);
    engine.rebuild_now();
    engine.locate(0);
    engine.play();
    long before = allocations.load();
    for (int i = 0; i < 150; ++i) {
        for (int ch = 0; ch < 2; ++ch)
            for (int k = 0; k < 512; ++k) rig.input.setSample(ch, k, Rig::sine(440.0, 0.3f)(ch, rig.clock + k));
        counting = true;
        engine.render_offline_with_input(rig.input, rig.out, 512);
        counting = false;
        rig.clock += 512;
    }
    const long console_allocations = allocations.load() - before;
    const bool was_recording = engine.recording();
    engine.stop();
    check(console_allocations == 0 && was_recording,
          "console + clip playback + recording allocates nothing (" + juce::String(console_allocations) + " allocations in 150 blocks)");
    engine.set_record_enabled(false);
    session.set_record_armed(vox, false);

    // A 44.1 kHz file plays at the right pitch on a 48 kHz engine.
    {
        const auto file = dir.getChildFile("Audio Files").getChildFile("tone44k.wav");
        file.deleteFile();
        {
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
            auto writer = juce::WavAudioFormat().createWriterFor(
                stream, juce::AudioFormatWriterOptions{}.withSampleRate(44100.0).withNumChannels(1).withBitsPerSample(24));
            juce::AudioBuffer<float> wave(1, 44100);
            for (int i = 0; i < 44100; ++i)
                wave.setSample(0, i, 0.5f * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * 1000.0 * i / 44100.0)));
            if (writer) writer->writeFromAudioSampleBuffer(wave, 0, 44100);
        }
        const std::string imported = session.add_audio_track("Imported", false);
        project::AudioClip c;
        c.file = "Audio Files/tone44k.wav";
        c.length = 1.0;
        c.channels = 1;
        c.file_sample_rate = 44100.0;
        session.add_audio_clips({{imported, c}}, "Import");
        session.set_solo(imported, true);
        engine.rebuild_now();
        engine.locate(0);
        engine.play();
        juce::AudioBuffer<float> played(1, 48000 / 2);
        for (int done = 0; done < played.getNumSamples(); done += 512) {
            engine.render_offline(rig.out, 512);
            played.copyFrom(0, done, rig.out, 0, 0, std::min(512, played.getNumSamples() - done));
        }
        engine.stop();
        int crossings = 0;
        for (int i = 2400; i < played.getNumSamples(); ++i)
            if (played.getSample(0, i - 1) < 0.0f && played.getSample(0, i) >= 0.0f) ++crossings;
        const double hz = crossings / ((played.getNumSamples() - 2400) / 48000.0);
        check(std::abs(hz - 1000.0) < 10.0 && engine.audio_status(imported).isEmpty(),
              "a 44.1 kHz file is resampled to the engine rate (" + juce::String(hz, 1) + " Hz)");
        session.set_solo(imported, false);
    }

    // A missing file: silence and a status, never a crash.
    session.add_audio_clips({{punch, [] {
                                  project::AudioClip c;
                                  c.file = "Audio Files/does-not-exist.wav";
                                  c.start_seconds = 0.0;
                                  c.length = 1.0;
                                  return c;
                              }()}},
                            "Import");
    engine.rebuild_now();
    check(engine.audio_status(punch).contains("does-not-exist"), "a missing audio file is reported (" + engine.audio_status(punch) + ")");
    engine.on_recording_finished = previous;
}

}  // namespace

int main(int argc, char** argv)
{
    vcc::desktop::prepare_display();
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File dir(argc > 1 ? juce::String(argv[1]) : juce::File::getCurrentWorkingDirectory().getChildFile("screenshots").getFullPathName());
    dir.createDirectory();

    {
        MainWindow window({}, false);
        window.setSize(1440, 900);
        auto& ctx = window.context();
        auto& session = ctx.session;
        pump(200);

        check(ctx.catalogs.error_count() == 0, "embedded content loads without errors");
        check(ctx.catalogs.templates->all().size() >= 200, "all template packs are embedded");
        check(music_font().loaded, "Bravura is embedded and loads");

        auto r = session.new_from_template("cinematic.horror.psychological");
        check(r.ok, "Horror template creates a project");
        pump(200);

        // Type a phrase into the selected track exactly as the keyboard would.
        auto& composer = window.main().composer();
        composer.grabKeyboardFocus();
        const std::string track = session.selected_track();
        for (char c : juce::String("cdefg")) composer.keyPressed(juce::KeyPress(c));
        composer.perform(composer::Command::ArticulationStaccato);
        composer.perform(composer::Command::DynamicPiano);
        const auto* t = session.data().find_track(track);
        check(t && t->active_version()->score.total_note_count() == 5, "five notes typed into the Composer view");
        check(session.can_undo(), "typing is undoable");
        check(!project::render_track(*t).notes.empty(), "the typed notes render to MIDI");
        shoot(window.main(), dir, "01-composer");

        // Undo goes through the project, not the editor's own history.
        composer.perform(composer::Command::Undo);
        check(session.data().find_track(track)->active_version()->score.notes(0).size() == 5,
              "undo removed the dynamic, not a note");

        window.main().show_workspace(MainComponent::Workspace::Sequencer);
        shoot(window.main(), dir, "02-sequencer");
        window.main().show_workspace(MainComponent::Workspace::Composer);

        window.main().show_mixer();
        if (auto* m = window.main().mixer_window()) {
            m->setSize(1100, 460);
            shoot(*m, dir, "03-mixer");
            m->setVisible(false);
        }

        show_template_browser(ctx, false);
        pump(200);
        if (auto* w = find_dialog("Templates")) shoot(*w, dir, "04-template-browser");
        show_chord_explorer(ctx);
        pump(200);
        if (auto* w = find_dialog("Chord Explorer")) shoot(*w, dir, "05-chord-explorer");
        show_ostinato_creator(ctx);
        pump(200);
        if (auto* w = find_dialog("Ostinato Creator")) shoot(*w, dir, "06-ostinato-creator");

        // Audio: render the opening bars offline through the real engine.
        ctx.engine.stop_preview();
        ctx.engine.prepare_offline(48000.0, 512);
        ctx.engine.locate(0);
        ctx.engine.play();
        juce::AudioBuffer<float> block(2, 512);
        double energy = 0.0;
        for (int i = 0; i < 48000 * 3 / 512; ++i) {
            ctx.engine.render_offline(block, 512);
            energy += block.getRMSLevel(0, 0, 512);
        }
        // Steady state: the next 400 blocks must not allocate at all.
        allocations = 0;
        for (int i = 0; i < 400; ++i) {
            counting = true;
            ctx.engine.render_offline(block, 512);
            counting = false;
        }
        check(allocations.load() == 0,
              "audio processing allocates nothing (" + juce::String(allocations.load()) + " allocations in 400 blocks)");
        // The counter itself must work, or "zero" proves nothing.
        counting = true;
        { auto probe = std::make_unique<int>(1); (void)probe; }
        counting = false;
        check(allocations.load() >= 1, "the allocation counter counts");
        ctx.engine.stop();
        check(energy > 0.01, "playback through the placeholder synth is audible (energy " + juce::String(energy, 3) + ")");
        check(ctx.engine.instrument_status(track).startsWith("Placeholder"), "the track is labelled as a placeholder sound");

        // VST3 hosting (Main Spec 20): scan, load, play, change state.
        std::string plugin_track;
        project::PluginRef plugin_ref;
        {
            juce::VST3PluginFormat vst3;
            juce::OwnedArray<juce::PluginDescription> found;
            vst3.findAllTypesForFile(found, VCC_TEST_PLUGIN);
            check(!found.isEmpty(), "the test VST3 instrument scans");
            if (!found.isEmpty()) {
                ctx.plugins.known().addType(*found[0]);
                plugin_ref = ctx.plugins.ref_for(*found[0]);
                plugin_track = session.add_track("Plug-in Test", "keys.piano");
                session.set_instrument_plugin(plugin_track, plugin_ref);
                session.insert_notes(plugin_track, 0, kTicksPerWhole,
                                     {{0, kTicksPerQuarter * 2, 69, 100, 0, {}}}, project::CollisionPolicy::Ask, "check");
                session.set_solo(plugin_track, true);
                ctx.engine.rebuild_now();
                check(ctx.engine.instrument_status(plugin_track) == "VCC Test Instrument",
                      "the track is played by the hosted plug-in (" + ctx.engine.instrument_status(plugin_track) + ")");
                ctx.engine.locate(0);
                ctx.engine.play();
                double plugin_energy = 0.0;
                for (int i = 0; i < 48000 / 512; ++i) {
                    ctx.engine.render_offline(block, 512);
                    plugin_energy += block.getRMSLevel(0, 0, 512);
                }
                ctx.engine.stop();
                check(plugin_energy > 0.01, "the hosted plug-in is audible (energy " + juce::String(plugin_energy, 3) + ")");
                if (auto* instance = ctx.engine.plugin_for(plugin_track)) {
                    instance->getParameters()[0]->setValueNotifyingHost(0.9f);
                    session.store_plugin_state(plugin_track, PluginHost::state_of(*instance));
                }
                session.set_solo(plugin_track, false);
            }
        }

        // Picture (Main Spec 13): import a generated movie with source
        // timecode, mark a hit, and park inside its streamer.
        std::string cue_id;
        {
            const auto movie = dir.getChildFile("check-picture.mov");
            juce::ChildProcess probe;
            const bool tool = probe.start("ffmpeg -version") && probe.readAllProcessOutput().contains("ffmpeg");
            if (media::available() && tool) {
                movie.deleteFile();
                juce::ChildProcess make;
                make.start(juce::StringArray{"ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                                             "testsrc2=size=640x360:rate=24:d=12", "-c:v", "mpeg4", "-q:v", "4",
                                             "-timecode", "00:59:58:00", movie.getFullPathName()});
                make.waitForProcessToFinish(60000);
                juce::String error;
                check(import_picture_file(ctx, movie, error), "a movie imports as the active cut " + error);
                const auto* pic = session.data().active_picture();
                check(pic && pic->start_timecode == "00:59:58:00" && pic->duration_frames == 12 * 24,
                      "the cut keeps the movie's source timecode and length");
                session.add_marker(4.0, "hit", "door slam");
                show_video_window(ctx);
                ctx.engine.locate(session.data().tempo.tick_at_seconds(3.5));  // mid-streamer
                pump(800);
                if (auto* w = video_window_if_open()) {
                    w->setSize(1024, 640);
                    shoot(*w, dir, "07-picture");
                }
            } else {
                std::printf("  skip  picture import (no FFmpeg in this build or no ffmpeg tool to make a test movie)\n");
            }
        }

        // Cues (Main Spec 14): a reel, an approved cue, the manager, render-by-cue.
        {
            const std::string reel = session.add_reel("Reel 1");
            cue_id = session.add_cue(0, 4 * kTicksPerWhole, reel, "Opening");
            check(session.data().find_cue(cue_id) && session.data().find_cue(cue_id)->code == "1M1", "the first cue is 1M1");
            session.update_cue(cue_id, [](project::Cue& c) { c.status = "approved"; });
            show_cue_manager(ctx);
            pump(300);
            if (auto* w = find_dialog("Cue Manager")) shoot(*w, dir, "08-cue-manager");

            CueRenderRequest request;
            request.cue_ids = {cue_id};
            request.folder = dir.getChildFile("bounces");
            request.tail_seconds = 1.0;
            request.folder.deleteRecursively();
            auto results = render_cues(ctx, request);
            check(results.size() == 1 && results[0].ok && results[0].file.getSize() > 1000,
                  "render-by-cue writes the cue's WAV (" + (results.empty() ? juce::String() : results[0].message) + ")");
            check(!results.empty() && results[0].peak > 1.0e-4f, "the bounced cue is not silent");
            auto again = render_cues(ctx, request);
            check(again.size() == 1 && again[0].skipped_protected, "an approved cue's deliverable is not overwritten unasked");
        }

        // Save and reopen.
        const auto file = dir.getChildFile("check.vcc2");
        std::string error;
        check(session.save_as(file.getFullPathName().toStdString(), &error), "project saves");
        check(session.open(file.getFullPathName().toStdString(), &error), "project reopens");
        check(session.data().find_track(track) != nullptr &&
                  session.data().find_track(track)->active_version()->score.total_note_count() == 5,
              "the typed notes survived save and reopen");

        check(session.data().find_cue(cue_id) != nullptr && session.data().find_cue(cue_id)->status == "approved",
              "cues survived save and reopen");
        if (!plugin_track.empty()) {
            const auto* pt = session.data().find_track(plugin_track);
            check(pt && pt->instrument_plugin.identifier == plugin_ref.identifier, "the plug-in assignment survived reopen");
            juce::String error_text;
            auto fresh = pt ? ctx.plugins.create(pt->instrument_plugin, 48000.0, 512, error_text) : nullptr;
            check(fresh != nullptr && std::abs(fresh->getParameters()[0]->getValue() - 0.9f) < 0.01f,
                  "a fresh plug-in instance restores the saved state");
        }

        console_and_recording_checks(ctx, dir);

        for (int i = juce::TopLevelWindow::getNumTopLevelWindows(); --i >= 0;)
            if (auto* w = juce::TopLevelWindow::getTopLevelWindow(i); w && w != &window) w->setVisible(false);
        pump(100);
    }
    juce::DeletedAtShutdown::deleteAll();
    std::printf("%s: %d failure(s); screenshots in %s\n", failures ? "FAILED" : "PASSED", failures,
                dir.getFullPathName().toRawUTF8());
    return failures == 0 ? 0 : 1;
}
