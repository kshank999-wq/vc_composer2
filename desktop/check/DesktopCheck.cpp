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
#include "ui/ScoreExchange.h"
#include "audio/StemRenderer.h"
#include "audio/PluginIpc.h"
#include "audio/RemotePlugin.h"
#include "ui/StemExport.h"
#include "vcc/project/Stems.h"
#include "vcc/interchange/MusicXml.h"
#include "ui/Dialogs.h"
#include "ui/hw/HwGallery.h"
#include "ui/composer/ComposerRuler.h"
#include "ui/composer/Inspector.h"
#include "ui/composer/LaneParts.h"
#include "ui/VideoWindow.h"
#include "vcc/media/VideoDecoder.h"
#include "vcc/project/Automation.h"
#include "vcc/notation/ScoreFile.h"
#include "vcc/project/Performance.h"
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

// --- Automation and plug-in delay compensation (Main Spec §12) --------------------------

/// Plays from the start for `seconds` and returns the whole output.
/// `before_block(seconds)` runs on this (message) thread between blocks, as
/// console gestures would.
juce::AudioBuffer<float> capture(Rig& rig, double seconds, const std::function<float(int, std::int64_t)>& signal = {},
                                 const std::function<void(double)>& before_block = {})
{
    rig.engine.rebuild_now();
    const int blocks = static_cast<int>(std::ceil(seconds * 48000.0 / 512.0));
    juce::AudioBuffer<float> all(2, blocks * 512);
    for (int b = 0; b < blocks; ++b) {
        if (before_block) before_block(b * 512 / 48000.0);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 512; ++i) rig.input.setSample(ch, i, signal ? signal(ch, rig.clock + i) : 0.0f);
        rig.engine.render_offline_with_input(rig.input, rig.out, 512);
        rig.clock += 512;
        for (int ch = 0; ch < 2; ++ch) all.copyFrom(ch, b * 512, rig.out, ch, 0, 512);
    }
    return all;
}

double rms_db(const juce::AudioBuffer<float>& b, double t0, double t1, int ch = 0)
{
    const int a = static_cast<int>(t0 * 48000.0), z = std::min(b.getNumSamples(), static_cast<int>(t1 * 48000.0));
    double sum = 0.0;
    for (int i = a; i < z; ++i) sum += static_cast<double>(b.getSample(ch, i)) * b.getSample(ch, i);
    const double rms = z > a ? std::sqrt(sum / (z - a)) : 0.0;
    return rms > 1.0e-9 ? 20.0 * std::log10(rms) : -180.0;
}

int loudest(const juce::AudioBuffer<float>& b, int ch)
{
    int at = -1;
    float best = 0.0f;
    for (int i = 0; i < b.getNumSamples(); ++i)
        if (std::abs(b.getSample(ch, i)) > best) {
            best = std::abs(b.getSample(ch, i));
            at = i;
        }
    return at;
}

std::optional<project::PluginRef> scan(AppContext& ctx, const char* path)
{
    juce::VST3PluginFormat vst3;
    juce::OwnedArray<juce::PluginDescription> found;
    vst3.findAllTypesForFile(found, path);
    if (found.isEmpty()) return std::nullopt;
    ctx.plugins.known().addType(*found[0]);
    return ctx.plugins.ref_for(*found[0]);
}

void automation_checks(AppContext& ctx, const juce::File& dir, const std::optional<project::PluginRef>& instrument_ref)
{
    auto& session = ctx.session;
    auto& engine = ctx.engine;
    engine.stop();
    session.new_empty();
    std::string error;
    session.save_as(dir.getChildFile("automation-check.vcc2").getFullPathName().toStdString(), &error);
    engine.prepare_offline(48000.0, 512);
    Rig rig(engine);
    auto T = [&](double seconds) { return session.data().tempo.tick_at_seconds(seconds); };
    auto set_lane = [&](const std::string& owner, const std::string& param, std::vector<project::AutomationPoint> points) {
        const std::string id = session.ensure_lane(owner, param);
        session.edit_lane(owner, id, "check " + param, [&](project::AutomationLane& l) { l.points = points; });
        return id;
    };
    auto drop_lane = [&](const std::string& owner, const std::string& param) {
        if (const auto* lanes = session.lanes_of(owner))
            if (const auto* l = project::find_lane(*lanes, param)) session.remove_lane(owner, std::string(l->id));
    };
    auto lane_value = [&](const std::string& owner, const std::string& param, double seconds) {
        const auto* lanes = session.lanes_of(owner);
        const auto* l = lanes ? project::find_lane(*lanes, param) : nullptr;
        return l ? project::value_at(*l, T(seconds)).value_or(-999.0) : -999.0;
    };
    juce::String written_message;
    auto previous_written = engine.on_automation_written;
    engine.on_automation_written = [&](const juce::String& m) { written_message = m; };
    auto play = [&](double seconds, const std::function<float(int, std::int64_t)>& signal,
                    const std::function<void(double)>& gestures = {}) {
        engine.stop();
        engine.locate(0);
        engine.play();
        auto out = capture(rig, seconds, signal, gestures);
        engine.stop();
        return out;
    };

    // Every strip parameter the project model offers can be automated by the engine.
    int unknown = 0;
    for (const auto& p : project::strip_params())
        if (p.id.rfind("strip.", 0) == 0 && strip_field_index(p.id) < 0) ++unknown;
    check(unknown == 0, "every strip.* parameter maps to an engine field (" + juce::String(unknown) + " unknown)");

    // A mono audio track monitoring a steady 1 kHz input.
    const std::string tr = session.add_audio_track("Auto", false);
    session.update_track_mix(tr, "monitor", [](project::Track& t) { t.monitor = "input"; });
    const auto tone = Rig::sine(1000.0, 0.5f);

    // Volume lane: 0 dB at 0.5 s ramping to -20 dB at 1.5 s.
    set_lane(tr, "volume", {{T(0.5), 0.0, "linear"}, {T(1.5), -20.0, "linear"}});
    {
        const auto out = play(2.0, tone);
        const double ref = rms_db(out, 0.1, 0.45);
        double worst = 0.0;
        juce::String detail;
        for (double t : {0.75, 1.0, 1.25, 1.75}) {
            const double expected = lane_value(tr, "volume", t);
            const double got = rms_db(out, t - 0.005, t + 0.005) - ref;
            worst = std::max(worst, std::abs(got - expected));
            detail << juce::String(expected, 1) << "/" << juce::String(got, 2) << " ";
        }
        check(worst < 0.5, "a volume lane ramp is heard (lane/measured dB: " + detail + "; worst " + juce::String(worst, 2) + " dB)");
    }
    drop_lane(tr, "volume");

    // A step mute lane: muted exactly from 1.0 s to 1.5 s.
    set_lane(tr, "mute", {{0, 0.0, "step"}, {T(1.0), 1.0, "step"}, {T(1.5), 0.0, "step"}});
    {
        const auto out = play(2.0, tone);
        const float inside = out.getMagnitude(0, 48000, 24000);
        const float edge = out.getMagnitude(0, 47900, 52);  // before the 1 ms close
        const double after = rms_db(out, 1.51, 1.9) - rms_db(out, 0.5, 0.9);
        check(juce::exactlyEqual(inside, 0.0f) && edge > 0.3f && std::abs(after) < 0.1,
              "a step mute lane silences exactly from its point to the next (peak inside " + juce::String(inside, 6) +
                  ", just before " + juce::String(edge, 3) + ", after " + juce::String(after, 2) + " dB)");
    }
    drop_lane(tr, "mute");

    // A strip EQ lane: the LF shelf jumps to +12 dB at 1 s on a 100 Hz tone.
    session.update_track_mix(tr, "eq", [](project::Track& t) {
        t.strip.eq_on = true;
        t.strip.lf.freq = 300.0;
    });
    set_lane(tr, "strip.lf.gain_db", {{0, 0.0, "step"}, {T(1.0), 12.0, "step"}});
    {
        const auto out = play(2.0, Rig::sine(100.0, 0.5f));
        const double boost = rms_db(out, 1.3, 1.9) - rms_db(out, 0.4, 0.9);
        check(boost > 9.0 && boost < 13.0, "a strip EQ lane changes the sound (+" + juce::String(boost, 1) + " dB at 100 Hz)");
    }
    drop_lane(tr, "strip.lf.gain_db");
    session.update_track_mix(tr, "eq", [](project::Track& t) { t.strip.eq_on = false; });

    // A built-in insert lane: compressor make-up 0 -> 12 dB at 1 s (below threshold).
    session.update_track_mix(tr, "inserts", [](project::Track& t) {
        project::InsertSlot c;
        c.kind = "compressor";
        c.suggestion = false;
        c.params["threshold_db"] = 0.0;
        t.inserts = {c};
    });
    set_lane(tr, "insert:0:makeup_db", {{0, 0.0, "step"}, {T(1.0), 12.0, "step"}});
    {
        const auto out = play(2.0, tone);
        const double gain = rms_db(out, 1.3, 1.9) - rms_db(out, 0.4, 0.9);
        check(std::abs(gain - 12.0) < 0.5, "an insert parameter lane reaches the insert (+" + juce::String(gain, 2) + " dB)");
    }
    drop_lane(tr, "insert:0:makeup_db");
    session.update_track_mix(tr, "inserts", [](project::Track& t) { t.inserts.clear(); });

    // A send lane: pre-fader send to a group bus opens at 1 s; the fader is down.
    const std::string bus = session.add_bus("Send Bus", "group");
    session.update_track_mix(tr, "send", [&](project::Track& t) {
        project::Send send(bus, -96.0);
        send.pre_fader = true;
        t.sends = {send};
        t.gain_db = -96.0;
    });
    set_lane(tr, "send:" + bus, {{0, -96.0, "step"}, {T(1.0), 0.0, "step"}});
    double ref_level = 0.0;
    {
        const auto out = play(2.0, tone);
        const double before = rms_db(out, 0.3, 0.9), after = rms_db(out, 1.3, 1.9);
        ref_level = after;
        // A mono 0.5 sine panned centre: 0.354 peak, -12.04 dB rms.
        check(before < -100.0 && std::abs(after + 12.04) < 0.5, "a send lane opens the send (" + juce::String(before, 0) + " dB -> " +
                                                  juce::String(after, 1) + " dB at the master)");
    }
    drop_lane(tr, "send:" + bus);
    session.update_track_mix(tr, "send", [](project::Track& t) {
        t.sends.clear();
        t.gain_db = 0.0;
    });

    // Plug-in parameter lanes: the instrument's Level and an effect's Gain.
    std::string synth;
    if (instrument_ref) {
        synth = session.add_track("Synth", "keys.piano");
        session.set_instrument_plugin(synth, *instrument_ref);
        session.insert_notes(synth, 0, kTicksPerWhole * 2, {{0, kTicksPerWhole, 69, 100, 0, {}}}, project::CollisionPolicy::Ask,
                             "check");
        session.set_solo(synth, true);
        set_lane(synth, "plugin:0", {{0, 0.25, "step"}, {T(1.0), 1.0, "step"}});
        const auto out = play(1.9, {});
        const double gain = rms_db(out, 1.2, 1.85) - rms_db(out, 0.3, 0.9);
        auto* instance = engine.plugin_for(synth);
        const float now = instance ? instance->getParameters()[0]->getValue() : -1.0f;
        check(std::abs(gain - 12.04) < 0.5 && std::abs(now - 1.0f) < 1.0e-3f,
              "a plug-in parameter lane reaches the plug-in (Level 0.25 -> 1: +" + juce::String(gain, 2) + " dB, value " +
                  juce::String(now, 3) + ")");
        session.set_solo(synth, false);
        session.set_mute(synth, true);  // out of the way of the following measurements
    }
    const auto effect_ref = scan(ctx, VCC_TEST_EFFECT);
    check(effect_ref.has_value(), "the test VST3 effect scans");
    if (effect_ref) {
        session.update_track_mix(tr, "inserts", [&](project::Track& t) {
            project::InsertSlot slot;
            slot.kind = "plugin";
            slot.suggestion = false;
            slot.plugin = *effect_ref;
            t.inserts = {slot};
        });
        set_lane(tr, "insert_plugin:0:1", {{0, 0.5, "step"}, {T(1.0), 1.0, "step"}});
        const auto out = play(2.0, tone);
        const double gain = rms_db(out, 1.3, 1.9) - rms_db(out, 0.4, 0.9);
        check(std::abs(gain - 6.02) < 0.3, "an effect plug-in's parameter lane reaches it (Gain 0.5 -> 1: +" + juce::String(gain, 2) + " dB)");
        drop_lane(tr, "insert_plugin:0:1");
        session.update_track_mix(tr, "inserts", [](project::Track& t) { t.inserts.clear(); });
    }

    // --- Writing -----------------------------------------------------------------------
    // Touch: the lane holds -20 dB; touched 1.0..2.0 s (-6, then -3 from 1.5 s).
    set_lane(tr, "volume", {{0, -20.0, "linear"}});
    session.set_automation_mode(tr, "touch");
    {
        const auto history = session.project().history_size();
        bool writes_while_playing = false, writes_stopped = true;
        bool began = false, mid = false, ended = false;
        written_message.clear();
        const auto out = play(3.0, tone, [&](double t) {
            if (!began && t >= 1.0) {
                began = true;
                writes_while_playing = engine.writes_automation(tr, "volume");
                engine.begin_touch(tr, "volume");
                engine.touch_value(tr, "volume", -6.0);
            } else if (!mid && t >= 1.5) {
                mid = true;
                engine.touch_value(tr, "volume", -3.0);
            } else if (!ended && t >= 2.0) {
                ended = true;
                engine.end_touch(tr, "volume");
            }
        });
        writes_stopped = engine.writes_automation(tr, "volume");
        const double base = ref_level;
        const double l0 = rms_db(out, 0.3, 0.95) - base, l1 = rms_db(out, 1.1, 1.45) - base,
                     l2 = rms_db(out, 1.6, 1.95) - base, l3 = rms_db(out, 2.2, 2.9) - base;
        check(std::abs(l0 + 20.0) < 0.5 && std::abs(l1 + 6.0) < 0.5 && std::abs(l2 + 3.0) < 0.5 && std::abs(l3 + 20.0) < 0.5,
              "touch: the written value is heard only while touched, then the lane (" + juce::String(l0, 1) + ", " +
                  juce::String(l1, 1) + ", " + juce::String(l2, 1) + ", " + juce::String(l3, 1) + " dB)");
        check(writes_while_playing && !writes_stopped, "writes_automation is true while playing in touch, false when stopped");
        const double v0 = lane_value(tr, "volume", 0.5), v1 = lane_value(tr, "volume", 1.25),
                     v2 = lane_value(tr, "volume", 1.75), v3 = lane_value(tr, "volume", 2.5);
        check(std::abs(v0 + 20) < 0.01 && std::abs(v1 + 6) < 0.3 && std::abs(v2 + 3) < 0.3 && std::abs(v3 + 20) < 0.01,
              "touch writes only the touched span into the lane (" + juce::String(v0, 1) + ", " + juce::String(v1, 1) +
                  ", " + juce::String(v2, 1) + ", " + juce::String(v3, 1) + ")");
        check(session.project().history_size() == history + 1 && session.undo_label() == "Write automation",
              "a written pass is one undo step \"Write automation\" (" + juce::String(session.undo_label()) + ")");
        check(written_message == "Wrote automation: Volume (Auto)", "the pass is reported (" + written_message + ")");
        session.undo();
        const auto* lane = project::find_lane(*session.lanes_of(tr), "volume");
        check(lane && lane->points.size() == 1 && juce::exactlyEqual(lane->points[0].value, -20.0),
              "undo restores the lane as it was before the pass");
    }

    // Latch: touched at 1.0 s (-6), released at 1.5 s, stopped at 2.5 s: holds -6 to the stop.
    session.set_automation_mode(tr, "latch");
    {
        bool began = false, ended = false;
        const auto out = play(2.5, tone, [&](double t) {
            if (!began && t >= 1.0) {
                began = true;
                engine.begin_touch(tr, "volume");
                engine.touch_value(tr, "volume", -6.0);
            } else if (!ended && t >= 1.5) {
                ended = true;
                engine.end_touch(tr, "volume");
            }
        });
        const double held = rms_db(out, 1.7, 2.45) - ref_level;
        const double v = lane_value(tr, "volume", 2.2), after = lane_value(tr, "volume", 2.7);
        check(std::abs(held + 6.0) < 0.5 && std::abs(v + 6.0) < 0.3 && std::abs(after + 20.0) < 0.01,
              "latch keeps writing the last value until stop (heard " + juce::String(held, 1) + " dB, lane " +
                  juce::String(v, 1) + " then " + juce::String(after, 1) + " after the pass)");
        session.undo();
    }

    // Write: every lane of the strip is written for the whole pass with the
    // control's value (static volume -10 dB, pan 0), whatever the lanes held.
    session.update_track_mix(tr, "fader", [](project::Track& t) {
        t.gain_db = -10.0;
        t.pan = 0.0;
    });
    set_lane(tr, "volume", {{0, 0.0, "linear"}, {T(2.0), -40.0, "linear"}});
    set_lane(tr, "pan", {{0, 0.5, "linear"}});
    session.set_automation_mode(tr, "write");
    {
        const auto history = session.project().history_size();
        written_message.clear();
        const auto out = play(2.0, tone);
        const double heard = rms_db(out, 0.9, 1.1) - ref_level;
        const double v = lane_value(tr, "volume", 1.0), p = lane_value(tr, "pan", 1.0);
        check(std::abs(heard + 10.0) < 0.5 && std::abs(v + 10.0) < 0.01 && std::abs(p) < 0.01 &&
                  session.project().history_size() == history + 1,
              "write mode writes every lane for the whole pass with the control value (heard " + juce::String(heard, 1) +
                  " dB; volume lane " + juce::String(v, 1) + ", pan lane " + juce::String(p, 2) + "; " + written_message + ")");
        session.undo();
    }
    session.set_automation_mode(tr, "read");
    drop_lane(tr, "pan");
    drop_lane(tr, "volume");
    session.update_track_mix(tr, "fader", [](project::Track& t) { t.gain_db = 0.0; });

    // A gesture in the plug-in's own editor is written (touch mode).
    if (!synth.empty()) {
        session.set_mute(synth, false);
        session.set_solo(synth, true);
        session.set_automation_mode(synth, "touch");
        auto* instance = engine.plugin_for(synth);
        auto* param = instance ? instance->getParameters()[0] : nullptr;
        bool began = false, ended = false;
        written_message.clear();
        play(1.9, {}, [&](double t) {
            if (!param) return;
            if (!began && t >= 0.5) {
                began = true;
                param->beginChangeGesture();
                param->setValueNotifyingHost(0.8f);
            } else if (!ended && t >= 1.0) {
                ended = true;
                param->endChangeGesture();
            }
        });
        const double inside = lane_value(synth, "plugin:0", 0.75), before = lane_value(synth, "plugin:0", 0.3),
                     after = lane_value(synth, "plugin:0", 1.5);
        check(std::abs(inside - 0.8) < 0.01 && std::abs(before - 0.25) < 0.01 && std::abs(after - 1.0) < 0.01,
              "a gesture in the plug-in's editor is written as plugin:0 (" + juce::String(before, 2) + ", " +
                  juce::String(inside, 2) + ", " + juce::String(after, 2) + "; " + written_message + ")");
        session.set_automation_mode(synth, "read");
        session.set_solo(synth, false);
    }
    engine.on_automation_written = previous_written;
}

/// The test effect's saved state (as the host stores it) with a latency.
std::string state_with_latency(AppContext& ctx, const project::PluginRef& ref, int latency)
{
    juce::String error;
    auto instance = ctx.plugins.create(ref, 48000.0, 512, error);
    if (!instance) return {};
    instance->getParameters()[0]->setValue(static_cast<float>(latency) / 8192.0f);
    return PluginHost::state_of(*instance);
}

void delay_compensation_checks(AppContext& ctx, const juce::File& dir, const std::optional<project::PluginRef>& instrument_ref)
{
    auto& session = ctx.session;
    auto& engine = ctx.engine;
    const auto effect_ref = scan(ctx, VCC_TEST_EFFECT);
    if (!effect_ref) return;
    engine.stop();
    session.new_empty();
    std::string error;
    session.save_as(dir.getChildFile("pdc-check.vcc2").getFullPathName().toStdString(), &error);
    engine.prepare_offline(48000.0, 512);
    engine.set_delay_compensation(true);
    Rig rig(engine);

    // One click at 0.25 s in a mono file, played by two tracks: "Wet" through
    // a plug-in reporting 1000 samples of latency (panned left), "Dry" plain
    // (panned right).
    const auto file = dir.getChildFile("Audio Files").getChildFile("click.wav");
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
        auto writer = juce::WavAudioFormat().createWriterFor(
            stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000.0).withNumChannels(1).withBitsPerSample(24));
        juce::AudioBuffer<float> wave(1, 48000);
        wave.clear();
        wave.setSample(0, 12000, 0.9f);
        if (writer) writer->writeFromAudioSampleBuffer(wave, 0, 48000);
    }
    const std::string wet = session.add_audio_track("Wet", false);
    const std::string dry = session.add_audio_track("Dry", false);
    for (const auto& [id, pan] : {std::pair<std::string, double>{wet, -1.0}, {dry, 1.0}}) {
        project::AudioClip c;
        c.file = "Audio Files/click.wav";
        c.length = 1.0;
        c.channels = 1;
        c.file_sample_rate = 48000.0;
        session.add_audio_clips({{id, c}}, "Import");
        session.update_track_mix(id, "pan", [pan = pan](project::Track& t) { t.pan = pan; });
    }
    session.update_track_mix(wet, "inserts", [&](project::Track& t) {
        project::InsertSlot slot;
        slot.kind = "plugin";
        slot.suggestion = false;
        slot.plugin = *effect_ref;
        slot.plugin.state_base64 = state_with_latency(ctx, *effect_ref, 1000);
        t.inserts = {slot};
    });
    auto play = [&](double seconds, const std::function<float(int, std::int64_t)>& signal = {}) {
        engine.stop();
        engine.locate(0);
        engine.play();
        auto out = capture(rig, seconds, signal);
        engine.stop();
        return out;
    };

    engine.rebuild_now();
    {
        engine.locate(0);
        engine.play();
        const auto out = capture(rig, 1.0);
        const double heard = engine.position_seconds() * 48000.0;
        engine.stop();
        const int l = loudest(out, 0), r = loudest(out, 1);
        check(engine.latency_samples(wet) == 1000 && engine.latency_samples(dry) == 0 && engine.compensation_samples() == 1000,
              "latency is reported per strip and compensated (wet " + juce::String(engine.latency_samples(wet)) + ", dry " +
                  juce::String(engine.latency_samples(dry)) + ", compensation " + juce::String(engine.compensation_samples()) + ")");
        check(l == r && l == 12000 + 1000,
              "PDC on: the plug-in track and the dry track are sample-aligned at the master (wet " + juce::String(l) +
                  ", dry " + juce::String(r) + ")");
        check(std::abs(heard - (out.getNumSamples() - 1000)) < 0.5,
              "the playhead reports what is heard (render " + juce::String(out.getNumSamples()) + " - 1000 = " +
                  juce::String(heard, 0) + ")");
    }
    engine.set_delay_compensation(false);
    {
        const auto out = play(1.0);
        const int l = loudest(out, 0), r = loudest(out, 1);
        check(l - r == 1000 && engine.compensation_samples() == 0,
              "PDC off: misaligned by the plug-in's latency (wet " + juce::String(l) + ", dry " + juce::String(r) + ")");
    }
    // The switch belongs to the project: saved with it, and undoable.
    check(!session.data().delay_compensation && session.undo_label() == "Delay compensation off",
          "turning PDC off is a change to the project, with its own undo step");
    {
        std::string err;
        const auto path = dir.getChildFile("pdc-off.vcc2").getFullPathName().toStdString();
        session.save_as(path, &err);
        session.set_delay_compensation(true);
        check(session.open(path, &err) && !session.data().delay_compensation, "a project saved with PDC off reopens with it off");
        engine.rebuild_now();  // as the engine's timer would after the document changed
        check(!engine.delay_compensation() && engine.compensation_samples() == 0, "the engine follows the reopened project");
        session.set_delay_compensation(true);
        session.undo();
        engine.rebuild_now();
        check(!engine.delay_compensation(), "undo turns PDC back off, and the engine follows");
    }
    engine.set_delay_compensation(true);

    // The plug-in changes its latency while running: detected, re-aligned.
    if (auto* instance = engine.insert_plugin(wet, 0)) {
        instance->getParameters()[0]->setValueNotifyingHost(2000.0f / 8192.0f);
        engine.render_offline(rig.out, 512);  // the plug-in sees the change and reports it
        engine.render_offline(rig.out, 512);
        // The engine notices on its 30 Hz timer; allow for a busy machine.
        for (int waited = 0; waited < 1000 && engine.compensation_samples() != 2000; waited += 40) pump(40);
        const auto out = play(1.0);
        const int l = loudest(out, 0), r = loudest(out, 1);
        check(engine.compensation_samples() == 2000 && l == r && l == 12000 + 2000,
              "a run-time latency change re-aligns (compensation " + juce::String(engine.compensation_samples()) + ", wet " +
                  juce::String(l) + ", dry " + juce::String(r) + ")");
    }

    // Recording with compensation: the performer hears the mix (the click at
    // 0.25 s) and plays along; a loop-back from the output to input 1 stands
    // in for them. The take must put the click back at 0.25 s.
    {
        const auto heard = play(1.0);
        const std::string rec = session.add_audio_track("Rec", false);
        session.set_record_armed(rec, true);
        engine.set_record_enabled(true);
        engine.rebuild_now();
        const std::int64_t start = rig.clock;
        play(1.0, [&](int ch, std::int64_t i) {
            const std::int64_t k = i - start;
            return ch == 0 && k >= 0 && k < heard.getNumSamples() ? heard.getSample(0, static_cast<int>(k)) : 0.0f;
        });
        engine.set_record_enabled(false);
        session.set_record_armed(rec, false);
        const auto* take = lane_clip(session, rec, 0);
        double at = -1.0;
        if (take) {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader(
                formats.createReaderFor(juce::File(juce::String(session.resolve_audio_path(take->file)))));
            if (reader) {
                juce::AudioBuffer<float> wav(1, static_cast<int>(reader->lengthInSamples));
                reader->read(&wav, 0, wav.getNumSamples(), 0, true, false);
                at = take->start_seconds * 48000.0 + loudest(wav, 0) - take->source_offset * 48000.0;
            }
        }
        check(std::abs(at - 12000.0) < 1.0, "a take recorded with compensation lines up with what was heard (click at sample " +
                                                juce::String(at, 1) + ", expected 12000)");
    }

    // Everything at once on the audio thread, allocation-free: lanes on
    // volume, pan, mute, a send, the strip EQ, an insert plug-in and an
    // instrument plug-in, a touch being written, and delay lines in use.
    {
        auto T = [&](double seconds) { return session.data().tempo.tick_at_seconds(seconds); };
        auto set_lane = [&](const std::string& owner, const std::string& param, std::vector<project::AutomationPoint> points) {
            const std::string id = session.ensure_lane(owner, param);
            session.edit_lane(owner, id, "check " + param, [&](project::AutomationLane& l) { l.points = points; });
        };
        const std::string bus = session.add_bus("Verb", "fx");
        session.update_track_mix(dry, "all", [&](project::Track& t) {
            t.sends = {project::Send(bus, -12.0)};
            t.strip.eq_on = true;
        });
        set_lane(dry, "volume", {{0, 0.0, "linear"}, {T(3.0), -12.0, "smooth"}, {T(6.0), 0.0, "linear"}});
        set_lane(dry, "pan", {{0, -0.5, "linear"}, {T(6.0), 0.5, "linear"}});
        std::vector<project::AutomationPoint> mutes;
        for (int i = 0; i < 12; ++i) mutes.push_back({T(i * 0.25), static_cast<double>(i % 2), "step"});
        set_lane(dry, "mute", mutes);
        set_lane(dry, "send:" + bus, {{0, -30.0, "linear"}, {T(4.0), 0.0, "linear"}});
        set_lane(dry, "strip.lf.gain_db", {{0, -6.0, "linear"}, {T(4.0), 6.0, "linear"}});
        set_lane(wet, "insert_plugin:0:1", {{0, 0.2, "linear"}, {T(4.0), 1.0, "linear"}});
        std::string synth;
        if (instrument_ref) {
            synth = session.add_track("Synth", "keys.piano");
            session.set_instrument_plugin(synth, *instrument_ref);
            session.insert_notes(synth, 0, kTicksPerWhole * 4, {{0, kTicksPerWhole * 4, 60, 100, 0, {}}},
                                 project::CollisionPolicy::Ask, "check");
            set_lane(synth, "plugin:0", {{0, 0.0, "linear"}, {T(4.0), 1.0, "linear"}});
        }
        session.set_automation_mode(dry, "touch");
        engine.rebuild_now();
        engine.locate(0);
        engine.play();
        engine.begin_touch(dry, "volume");
        engine.touch_value(dry, "volume", -3.0);
        long before = allocations.load();
        for (int i = 0; i < 150; ++i) {
            if (i % 10 == 0) engine.touch_value(dry, "volume", -3.0 - i * 0.02);  // message thread, between blocks
            counting = true;
            engine.render_offline_with_input(rig.input, rig.out, 512);
            counting = false;
        }
        const long n = allocations.load() - before;
        const bool active = engine.compensation_samples() > 0 && engine.writes_automation(dry, "volume");
        engine.end_touch(dry, "volume");
        engine.stop();
        check(n == 0 && active, "automation playback + a pass being written + PDC delay lines allocate nothing (" +
                                    juce::String(n) + " allocations in 150 blocks)");
    }
}

/// The kernel wait the plug-in IPC uses (futex / os_sync / ulock / named
/// events): one block mapped twice, as two processes map it. A waiter
/// blocks on the shared word; another thread changes it 5 ms later and
/// wakes it. A wake that does not work shows as the waiter only noticing at
/// its 100 ms timeout.
void ipc_wait_checks(const juce::File& dir)
{
    const auto file = dir.getChildFile("ipc-wait-check.shm");
    ipc::SharedBlock host, plugin;
    const bool opened = host.open(file, true) && plugin.open(file, false);
    check(opened, juce::String("the shared block opens from both sides (wait: ") + ipc::wait_mechanism() + ")");
    if (!opened) return;
    std::vector<double> wake_ms;
    for (int round = 0; round < 20; ++round) {
        const std::uint32_t seen = host.get()->request.load();
        std::atomic<bool> ready{false};
        double waited = -1.0;
        std::thread waiter([&] {
            ready = true;
            const auto t0 = std::chrono::steady_clock::now();
            plugin.wait(ipc::Word::request, seen, 1000.0);
            waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        });
        while (!ready) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));  // the waiter is now blocked in the kernel
        host.get()->request.store(seen + 1, std::memory_order_release);
        host.wake(ipc::Word::request);
        waiter.join();
        wake_ms.push_back(waited - 5.0);
    }
    std::sort(wake_ms.begin(), wake_ms.end());
    const double median = wake_ms[wake_ms.size() / 2], worst = wake_ms.back();
    check(median < 2.0 && worst < 20.0,
          juce::String("a blocked waiter wakes as soon as the other side signals (") + ipc::wait_mechanism() +
              ": median " + juce::String(std::max(0.0, median), 3) + " ms after the 5 ms, worst " +
              juce::String(std::max(0.0, worst), 3) + " ms)");
    // And the other way, as the plug-in side signals a finished block.
    const std::uint32_t done = plugin.get()->done.load();
    std::thread signaller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        plugin.get()->done.store(done + 1, std::memory_order_release);
        plugin.wake(ipc::Word::done);
    });
    const auto t0 = std::chrono::steady_clock::now();
    const bool changed = host.wait(ipc::Word::done, done, 1000.0);
    const double back = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    signaller.join();
    check(changed && back < 25.0, "the finished-block signal wakes the host side too (" + juce::String(back, 2) + " ms)");
    file.deleteFile();
}

/// Insert windows from the console (Main Spec 12): an effect plug-in's own
/// window, and the controls of a built-in processor.
void insert_window_checks(AppContext& ctx, const juce::File& dir)
{
    auto& session = ctx.session;
    const auto effect = scan(ctx, VCC_TEST_EFFECT);
    if (!effect || !ctx.show_insert_window) {
        check(false, "insert windows can be opened (no test effect or no window handler)");
        return;
    }
    ctx.engine.stop();
    session.new_empty();
    const auto track = session.add_audio_track("Dialogue", false);
    session.update_track_mix(track, "inserts", [&](project::Track& t) {
        project::InsertSlot fx;
        fx.kind = "plugin";
        fx.suggestion = false;
        fx.plugin = *effect;
        project::InsertSlot comp;
        comp.kind = "compressor";
        comp.suggestion = false;
        t.inserts = {fx, comp};
    });
    ctx.engine.rebuild_now();

    // The effect's own window; a change made there is saved with the project.
    ctx.show_insert_window(track, 0);
    pump(400);
    auto* fx_window = find_dialog("VCC Test Effect - Dialogue");
    check(fx_window != nullptr, "an effect insert's window opens from its slot (" +
                                    juce::String(fx_window ? fx_window->getName() : juce::String("not found")) + ")");
    if (fx_window) {
        fx_window->setSize(520, 220);
        pump(200);
        shoot(*fx_window, dir, "13-insert-plugin");
    }
    if (auto* p = ctx.engine.insert_plugin(track, 0)) p->getParameters()[1]->setValueNotifyingHost(0.25f);
    const auto before = session.data().find_track(track)->inserts[0].plugin.state_base64;
    if (auto* dw = dynamic_cast<juce::DocumentWindow*>(fx_window)) dw->closeButtonPressed();
    pump(100);
    const auto after = session.data().find_track(track)->inserts[0].plugin.state_base64;
    check(!after.empty() && after != before, "closing the effect's window stores its settings in the project");

    // A built-in compressor: a window of controls, each drag one undo step.
    ctx.show_insert_window(track, 1);
    pump(300);
    auto* comp_window = find_dialog("Compressor - Dialogue (insert 2)");
    std::vector<juce::Slider*> sliders;
    std::function<void(juce::Component&)> collect = [&](juce::Component& c) {
        if (auto* sl = dynamic_cast<juce::Slider*>(&c)) sliders.push_back(sl);
        for (auto* child : c.getChildren()) collect(*child);
    };
    if (comp_window) collect(*comp_window);
    check(comp_window != nullptr && sliders.size() == 5,
          "a built-in compressor's window shows its controls (" + juce::String(static_cast<int>(sliders.size())) + ")");
    if (comp_window && sliders.size() >= 2) {
        shoot(*comp_window, dir, "14-insert-builtin");
        auto drag = [&](juce::Slider* sl, std::initializer_list<double> values) {
            if (sl->onDragStart) sl->onDragStart();
            for (double v : values) sl->setValue(v, juce::sendNotificationSync);
            if (sl->onDragEnd) sl->onDragEnd();
        };
        drag(sliders[0], {-30.0, -32.0, -34.0});  // threshold
        drag(sliders[1], {6.0, 8.0});             // ratio
        const auto* t = session.data().find_track(track);
        const auto& params = t->inserts[1].params;
        auto near = [](double a, double b) { return std::abs(a - b) < 1e-9; };
        const bool set = params.count("threshold_db") && near(params.at("threshold_db"), -34.0) && params.count("ratio") &&
                         near(params.at("ratio"), 8.0);
        check(set, "the compressor's controls change its settings in the project");
        session.undo();
        const auto* u = session.data().find_track(track);
        const bool one_step = near(u->inserts[1].params.at("threshold_db"), -34.0) &&
                              (!u->inserts[1].params.count("ratio") || !near(u->inserts[1].params.at("ratio"), 8.0));
        session.undo();
        const auto* u2 = session.data().find_track(track);
        check(one_step && !u2->inserts[1].params.count("threshold_db"),
              "each drag of a control is one undo step");
        pump(100);
        check(std::abs(sliders[0]->getValue() - (-18.0)) < 0.01, "the window follows an undo (threshold back to " +
                                                                      juce::String(sliders[0]->getValue(), 1) + " dB)");
    }
    // Replacing the insert closes its window.
    session.update_track_mix(track, "replace", [&](project::Track& t) { t.inserts[1].kind = "reverb"; });
    pump(100);
    check(find_dialog("Compressor - Dialogue (insert 2)") == nullptr, "a window closes when its insert is replaced");
}

juce::Component* find_by_id(juce::Component& root, const juce::String& id)
{
    if (root.getComponentID() == id) return &root;
    for (auto* c : root.getChildren())
        if (auto* f = find_by_id(*c, id)) return f;
    return nullptr;
}

juce::Component* find_named(juce::Component& root, const juce::String& name)
{
    if (root.getName() == name) return &root;
    for (auto* c : root.getChildren())
        if (auto* f = find_named(*c, name)) return f;
    return nullptr;
}

juce::MouseEvent mouse_event(juce::Component& c, juce::Point<float> p, juce::Point<float> down, bool dragged,
                             bool alt = false)
{
    const auto now = juce::Time::getCurrentTime();
    const int mods = juce::ModifierKeys::leftButtonModifier | (alt ? juce::ModifierKeys::altModifier : 0);
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), p, juce::ModifierKeys(mods),
                            juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, down, now, 1,
                            dragged);
}

void drag(juce::Component& c, juce::Point<float> from, juce::Point<float> to, bool alt = false)
{
    c.mouseDown(mouse_event(c, from, from, false, alt));
    c.mouseDrag(mouse_event(c, (from + to) / 2.0f, from, true, alt));
    c.mouseDrag(mouse_event(c, to, from, true, alt));
    c.mouseUp(mouse_event(c, to, from, true, alt));
    pump(100);
}

juce::TextButton* find_button(juce::Component& root, const juce::String& prefix)
{
    if (auto* b = dynamic_cast<juce::TextButton*>(&root); b && b->getButtonText().startsWith(prefix)) return b;
    for (auto* c : root.getChildren())
        if (auto* f = find_button(*c, prefix)) return f;
    return nullptr;
}

/// Score notes in the piano roll, found by hovering as the pointer would:
/// one entry per pitch row, {left edge, right edge, centre y}.
struct RollNote {
    float left, right, y;
};
std::vector<RollNote> find_roll_notes(juce::Component& roll)
{
    // The notes follow one another in time, a pixel apart: each run of
    // columns with a note under the pointer is one note.
    std::map<int, std::vector<float>> columns;
    const juce::MouseCursor normal(juce::MouseCursor::NormalCursor);
    for (int y = 40; y < roll.getHeight() - 60; y += 2)
        for (int x = 0; x < roll.getWidth() - 20; x += 1) {
            const juce::Point<float> p(static_cast<float>(x), static_cast<float>(y));
            roll.mouseMove(mouse_event(roll, p, p, false));
            if (roll.getMouseCursor() != normal) columns[x].push_back(p.y);
        }
    std::vector<RollNote> out;
    int prev = -10;
    std::vector<float> ys;
    auto flush = [&] {
        if (out.empty() || ys.empty()) return;
        std::sort(ys.begin(), ys.end());
        out.back().y = ys[ys.size() / 2];
        ys.clear();
    };
    for (const auto& [x, hits] : columns) {
        if (x != prev + 1) {
            flush();
            out.push_back({static_cast<float>(x), static_cast<float>(x), 0.0f});
        }
        out.back().right = static_cast<float>(x);
        ys.insert(ys.end(), hits.begin(), hits.end());
        prev = x;
    }
    flush();
    return out;
}

/// Performance overrides and MIDI -> notation (Main Spec 8) in the piano
/// roll: an Alt-drag of a score note changes only how it is played; a plain
/// drag rewrites it in the score; an edit that can be read two ways asks;
/// a MIDI region is notated through its dialog.
void performance_checks(AppContext& ctx, MainComponent& main, const std::string& track, const juce::File& dir)
{
    auto& session = ctx.session;
    session.select_track(track);
    pump(200);
    auto* roll = find_named(main, "vcc-piano-roll");
    check(roll != nullptr && roll->isVisible() && roll->getHeight() > 100, "the piano roll is showing");
    if (!roll) return;
    const auto* t = session.data().find_track(track);
    const auto written = project::render_track(*t, false);
    const std::string score_before = json::write(notation::score_to_json(t->active_version()->score));
    const auto notes = find_roll_notes(*roll);
    check(notes.size() >= 4, "the score notes are found under the pointer in the piano roll (" +
                                 juce::String(static_cast<int>(notes.size())) + ")");
    if (notes.size() < 2) return;
    const auto& first = notes.front();
    const auto& last = notes.back();
    auto score_json = [&] {
        return json::write(notation::score_to_json(session.data().find_track(track)->active_version()->score));
    };

    // Alt-drag: a performance only.
    drag(*roll, {first.left + 2.0f, first.y}, {first.left + 16.0f, first.y}, true);
    const auto* perf = session.performance_of(track);
    check(perf && perf->size() == 1 && perf->begin()->second.onset > 0,
          "Alt-dragging a score note plays it later (" + juce::String(perf && !perf->empty() ? perf->begin()->second.onset : 0) +
              " ticks)");
    check(score_json() == score_before, "the Alt-drag leaves the written score untouched");
    const auto played = project::render_track(*session.data().find_track(track));
    bool moved = false, same_pitches = played.notes.size() == written.notes.size();
    for (std::size_t i = 0; same_pitches && i < played.notes.size(); ++i) {
        same_pitches = played.notes[i].pitch == written.notes[i].pitch;
        moved = moved || played.notes[i].start > written.notes[i].start;
    }
    check(moved && same_pitches, "playback and export hear the note later, at the written pitch");
    shoot(main, dir, "15-performance-drag");
    session.undo();
    check(session.performance_of(track)->empty(), "one undo returns the note to how it is written");

    // A plain drag of a note up a tone: a new pitch, written into the score.
    const auto rows_per_tone = (notes[0].y - notes[1].y);  // C to D is a tone
    const auto& sc0 = session.data().find_track(track)->active_version()->score;
    std::vector<notation::ScoreNote> line;
    for (const auto& n : sc0.notes(0))
        if (!n.is_rest) line.push_back(n);
    std::sort(line.begin(), line.end(), [](const auto& a, const auto& b) { return a.start_tick < b.start_tick; });
    const auto target = line.size() >= 4 ? line[3] : notation::ScoreNote{};
    drag(*roll, {last.left + 4.0f, last.y}, {last.left + 4.0f, last.y - rows_per_tone});
    if (auto* stray = find_dialog("Notate the edit"))
        if (auto* cancel = find_button(*stray, "Cancel")) cancel->triggerClick(), pump(100);
    const auto* rewritten = session.data().find_track(track)->active_version()->score.find_note(target.id);
    const int before_midi = notation::midi_note_number(target.pitch);
    const int after_midi = rewritten ? notation::midi_note_number(rewritten->pitch) : -1;
    check(rewritten && after_midi == before_midi + 2 && rewritten->start_tick == target.start_tick &&
              session.performance_of(track)->empty(),
          "dragging a score note up a tone rewrites its pitch in the score (" + juce::String(before_midi) + " -> " +
              juce::String(after_midi) + ")");
    session.undo();
    check(score_json() == score_before, "one undo puts the written note back");

    // Stretching the first note into the second: the reconciliation prompt.
    drag(*roll, {first.right - 1.0f, first.y}, {first.right + 20.0f, first.y});
    pump(200);
    auto* prompt = find_dialog("Notate the edit");
    check(prompt != nullptr, "an edit that overlaps the next note asks how to read it");
    if (prompt) {
        shoot(*prompt, dir, "17-notate-prompt");
        auto* voice = find_button(*prompt, "Write it in the score in voice");
        check(voice != nullptr, "the prompt offers a second voice");
        if (voice) voice->triggerClick();
        pump(300);
        bool in_voice_2 = false;
        for (const auto& n : session.data().find_track(track)->active_version()->score.notes(0))
            in_voice_2 = in_voice_2 || (n.voice == 1 && !n.is_rest);
        check(in_voice_2, "choosing the second voice writes the note there");
        session.undo();
    }

    project::HumanizeSettings h = project::humanize_preset("natural");
    h.seed = 7;
    check(session.humanize(track, {}, h) && session.performance_of(track)->size() == written.notes.size(),
          "humanize gives every score note a performance");
    pump(150);
    shoot(main, dir, "16-performance-humanized");
    session.undo();

    // A played MIDI region, notated through its dialog.
    constexpr Ticks Q = kTicksPerQuarter;
    const Ticks bar = 2 * 4 * Q;  // bar 3, after the typed phrase
    session.edit("Record MIDI", [&](project::ProjectData& d) {
        project::MidiRegion r;
        r.id = "rgn-check-played";
        r.start = bar;
        r.end = bar + 4 * Q;
        r.source = "recorded";
        r.notes = {{bar + 17, Q - 40, 62, 72, 0, {}}, {bar + Q - 25, Q / 2 + 10, 65, 90, 0, {}},
                   {bar + Q + Q / 2 + 9, Q * 2 - 30, 69, 84, 0, {}}};
        d.find_track(track)->active_version()->regions.push_back(r);
        return true;
    });
    const auto before_notation = project::render_track(*session.data().find_track(track)).notes;
    const auto count_before = session.data().find_track(track)->active_version()->score.total_note_count();
    main.sequencer().notate_region("rgn-check-played");
    pump(300);
    auto* dialog = find_dialog("Notate Region");
    check(dialog != nullptr, "Notate Region opens for a MIDI region");
    if (dialog) {
        shoot(*dialog, dir, "18-notate-region");
        if (auto* go = find_button(*dialog, "Notate the region")) go->triggerClick();
        pump(300);
        const auto* v = session.data().find_track(track)->active_version();
        check(v->regions.empty() && v->score.total_note_count() >= count_before + 3,
              "notating the region writes its notes into the score and removes the region");
        check(project::render_track(*session.data().find_track(track)).notes == before_notation,
              "the notated passage still plays exactly as recorded");
        check(find_dialog("Notate Region") == nullptr, "the dialog closes");
        main.show_workspace(MainComponent::Workspace::Composer);
        pump(200);
        shoot(main, dir, "19-notated-in-composer");
        main.show_workspace(MainComponent::Workspace::Sequencer);
        session.undo();
    }
    session.undo();
}

/// Pixels of `image` within `tolerance` (per channel) of `colour`.
int pixels_near(const juce::Image& image, juce::Colour colour, int tolerance = 18)
{
    int n = 0;
    const juce::Image::BitmapData data(image, juce::Image::BitmapData::readOnly);
    for (int y = 0; y < image.getHeight(); ++y)
        for (int x = 0; x < image.getWidth(); ++x) {
            const auto c = data.getPixelColour(x, y);
            if (std::abs(c.getRed() - colour.getRed()) <= tolerance &&
                std::abs(c.getGreen() - colour.getGreen()) <= tolerance &&
                std::abs(c.getBlue() - colour.getBlue()) <= tolerance)
                ++n;
        }
    return n;
}

/// UI spec step 1 (docs/spec/ui/SPEC.md): the theme, the fonts and the
/// hardware controls, in the Control Gallery.
void hardware_control_checks(MainComponent& main, const juce::File& dir)
{
    auto& theme = hw::Theme::get();
    check(hw::fonts::loaded(), "Barlow Condensed, Share Tech Mono and Inter are embedded and load");
    main.show_control_gallery();
    pump(300);
    auto* window = main.control_gallery();
    auto* gallery = window ? dynamic_cast<hw::Gallery*>(window->getContentComponent()) : nullptr;
    check(gallery != nullptr, "View > Control Gallery opens");
    if (!gallery) return;
    shoot(*gallery, dir, "20-controls-amber-cyan");
    const auto before = gallery->createComponentSnapshot(gallery->getLocalBounds());

    // Theme: switched live, everything repaints, remembered.
    theme.set_led(hw::Theme::led_choices()[1].colour);  // red
    theme.set_lcd(hw::Theme::lcd_choices()[1].colour);  // green
    pump(150);
    shoot(*gallery, dir, "21-controls-red-green");
    const auto after = gallery->createComponentSnapshot(gallery->getLocalBounds());
    const int amber_before = pixels_near(before, juce::Colour(0xffffb547));
    const int amber_after = pixels_near(after, juce::Colour(0xffffb547));
    const int red_after = pixels_near(after, juce::Colour(0xffff5a4e));
    const int green_after = pixels_near(after, juce::Colour(0xff9bf07a));
    check(amber_before > 200 && amber_after < amber_before / 4 && red_after > 200 && green_after > 200,
          "LED and LCD colours change everywhere at once (amber " + juce::String(amber_before) + " -> " +
              juce::String(amber_after) + " px; red " + juce::String(red_after) + ", green " + juce::String(green_after) +
              ")");
    int lit_theme_keys = 0;
    for (auto* c : gallery->getChildren())
        if (auto* k = dynamic_cast<hw::Keycap*>(c);
            k && k->getToggleState() && (k->getButtonText() == "Red" || k->getButtonText() == "Green"))
            ++lit_theme_keys;
    check(lit_theme_keys == 2, "the gallery's theme keys follow a change made elsewhere");
    auto& settings = hw::ui_settings();
    check(juce::Colour::fromString(settings.getValue("led_colour")) == hw::Theme::led_choices()[1].colour &&
              juce::Colour::fromString(settings.getValue("lcd_colour")) == hw::Theme::lcd_choices()[1].colour,
          "the theme is remembered in the settings");
    theme.set_paper(true);
    check(theme.staff_paper() == juce::Colour(0xfff1ece1) && theme.ink() == juce::Colour(0xff1b1a18),
          "paper staves switch the staff colours");
    theme.set_paper(false);
    theme.set_led(hw::Theme::led_choices()[0].colour);
    theme.set_lcd(hw::Theme::lcd_choices()[0].colour);

    // Keycaps: real buttons; a radio group lights exactly one.
    auto& step = gallery->step_key();
    check(step.getToggleState() && step.getTitle() == "Step", "a keycap is a button with an accessible name");
    if (auto* parent = step.getParentComponent())
        for (auto* c : parent->getChildren())
            if (auto* k = dynamic_cast<hw::Keycap*>(c); k && k->getButtonText() == "Insert") {
                k->triggerClick();
                pump(50);
                check(k->getToggleState() && !step.getToggleState(), "entry-mode keys light exactly one");
                step.triggerClick();
                pump(50);
                break;
            }

    // Ring knob: 15 LEDs over the range; drag up to raise, Shift for fine.
    auto& vel = gallery->velocity();
    vel.setValue(127.0, juce::sendNotificationSync);
    const int full = vel.lit_leds();
    vel.setValue(0.0, juce::sendNotificationSync);
    check(full == 15 && vel.lit_leds() == 0, "the ring knob lights 15 LEDs at the top, none at the bottom");
    auto drag = [&](juce::Component& c, float dy, bool shift) {
        const auto mods = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier |
                                             (shift ? juce::ModifierKeys::shiftModifier : 0));
        const juce::Point<float> p(40.0f, 40.0f);
        auto ev = [&](juce::Point<float> at) {
            const auto now = juce::Time::getCurrentTime();
            return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at, mods,
                                    juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, p,
                                    now, 1, true);
        };
        c.mouseDown(ev(p));
        c.mouseDrag(ev(p + juce::Point<float>(0.0f, dy)));
        c.mouseUp(ev(p + juce::Point<float>(0.0f, dy)));
    };
    drag(vel, -40.0f, false);
    const double coarse = vel.getValue();
    vel.setValue(0.0, juce::sendNotificationSync);
    drag(vel, -40.0f, true);
    const double fine = vel.getValue();
    check(coarse > 20.0 && fine > 0.0 && fine < coarse / 3.0,
          "dragging a knob up raises it, five times finer with Shift (" + juce::String(coarse, 1) + " vs " +
              juce::String(fine, 1) + ")");

    // Meters: fall 20 dB a second, hold the peak 1.5 s.
    auto& m = gallery->master_left();
    m.push(0.0f);
    const int top = m.lit_segments();
    m.advance(1.0f);
    const int after_1s = m.lit_segments();
    const int held = m.peak_segment();
    m.advance(1.0f);
    const float peak_db = m.ballistics().peak;
    check(top == 30 && after_1s == 20 && held == 30 && std::abs(peak_db - (-10.0f)) < 0.01f,
          "meters fall 20 dB a second and hold the peak for 1.5 s (" + juce::String(top) + " -> " +
              juce::String(after_1s) + " segments, peak " + juce::String(peak_db, 1) + " dB after 2 s)");

    // Fader law, and the pitch wheel springing back.
    auto& f = gallery->fader();
    f.set_db(0.0);
    const bool unity = std::abs(f.db()) < 1e-9;
    f.set_db(-std::numeric_limits<double>::infinity());
    check(unity && f.getValue() == 0.0 && hw::Fader::proportion_for(6.0) == 1.0,
          "the fader reads +6 at the top, 0 dB at its mark and off at the bottom");
    f.set_db(-4.5);
    auto& wheel = gallery->pitch_wheel();
    wheel.setValue(0.8, juce::sendNotificationSync);
    const auto at = juce::Point<float>(10.0f, 10.0f);
    const auto now = juce::Time::getCurrentTime();
    wheel.mouseUp(juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys(),
                                   juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f, &wheel, &wheel, now,
                                   at, now, 1, false));
    check(wheel.getValue() == 0.0, "the pitch wheel springs back to centre when let go");
    shoot(*gallery, dir, "22-controls-after-use");
    window->setVisible(false);
}

/// UI spec step 2 (docs/spec/ui/SPEC.md, "Layout"): the Edit window shell
/// around both views, the Composer | Producer switch, the counters in 960
/// ticks, the stage bar, markers, new track, the channel and the status
/// strip, and the narrow-window layout.
void shell_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_from_template("cinematic.horror.psychological");
    pump(200);

    // Counters at 960 ticks to the quarter (the engine keeps 2880).
    {
        MeterMap m;
        const Ticks q = kTicksPerQuarter;
        const auto at = shell::bars_beats_ticks(m, 4 * q + 2 * q + 941 * 3);
        MeterMap six;
        six.set(1, TimeSignature{6, 8});
        const auto eighths = shell::bars_beats_ticks(six, 3 * q + q / 2 + 240 * 3);
        const auto len = shell::length_bbt(m, q, q + 4 * q + q + 480 * 3);
        check(at == "2|3|941" && eighths == "2|2|240" && len == "1|1|480",
              "the counters read bars|beats|ticks at 960 to the quarter (" + at + ", 6/8 " + eighths + ", length " + len +
                  ")");
        ctx.engine.locate(4 * q + q);
        main.deck().update_position();
        check(main.deck().main_counter().text() == "2|2|000",
              "the main counter follows the playhead (" + main.deck().main_counter().text() + ")");
    }

    // The switch: one project, two views; the playhead and selection stay.
    const Ticks here = ctx.engine.position();
    const auto selected = session.selected_track();
    main.deck().producer_key().triggerClick();
    pump(100);
    const bool producer = main.workspace() == MainComponent::Workspace::Producer && main.sequencer().isVisible() &&
                          !main.composer().isVisible() && main.channel_panel().isVisible() &&
                          main.new_track_panel().isVisible() && main.toolbar().key("Punch") != nullptr;
    check(producer && main.deck().producer_key().getToggleState() && !main.deck().composer_key().getToggleState(),
          "the Producer key shows the Producer view, its toolbar, new-track panel and channel");
    check(ctx.engine.position() == here && session.selected_track() == selected,
          "switching views keeps the playhead and the selection");
    check(main.edit_frame().stage_key(2).getToggleState(), "the stage bar shows Mix in the Producer view");
    main.setSize(1680, 1240);
    window.setSize(1680, 1240);
    pump(200);
    shoot(main, dir, "24-shell-producer-1680");
    ctx.commands.invokeDirectly(cmd::toggleView, false);
    pump(100);
    check(main.workspace() == MainComponent::Workspace::Composer && main.composer().isVisible() &&
              main.markers().isVisible() && main.selection_panel().isVisible(),
          "Ctrl+Tab (Switch Composer / Producer) flips back to the Composer view");
    check(main.edit_frame().stage_key(0).getToggleState() && !main.edit_frame().stage_key(2).getToggleState(),
          "the stage bar shows Compose in the Composer view");
    shoot(main, dir, "23-shell-composer-1680");

    // Markers: cues in time order, click to locate.
    session.edit("Cue", [](project::ProjectData& d) {
        project::Cue c;
        c.id = "cue-shell";
        c.code = "1M2";
        c.title = "Arrival";
        c.start = 8 * 4 * kTicksPerQuarter;
        c.end = c.start + 8 * 4 * kTicksPerQuarter;
        d.cues.push_back(c);
        return true;
    });
    pump(50);
    check(main.markers().rows() == 1, "the markers list shows the project's cues");
    main.markers().locate(0);
    check(ctx.engine.position() == 8 * 4 * kTicksPerQuarter, "clicking a marker locates the playhead to it");

    // The selection readout follows the selected track.
    const auto* t = session.data().find_track(session.selected_track());
    check(t && main.selection_panel().readout().text().contains(juce::String(t->name).toUpperCase()),
          "the selection panel shows the selected track");

    // New track (Producer view): two stereo audio tracks in one go.
    main.show_workspace(MainComponent::Workspace::Producer);
    pump(50);
    const auto before = session.data().tracks.size();
    main.new_track_panel().type_key(2).triggerClick();
    pump(20);
    main.new_track_panel().set_count(2);
    const int made = main.new_track_panel().create();
    check(made == 2 && session.data().tracks.size() == before + 2, "New track makes as many audio tracks as asked");
    session.undo();
    session.undo();

    // The channel: the fader and the automation keys write the selected track.
    session.select_track(selected);
    pump(50);
    main.channel_panel().fader().set_db(-6.0);
    main.channel_panel().mode_key(1).triggerClick();
    pump(50);
    const auto* sel = session.data().find_track(selected);
    check(sel && std::abs(sel->gain_db + 6.0) < 0.01 && sel->automation_mode == "touch",
          "the channel's fader and automation keys write the selected track");
    session.undo();
    session.undo();

    // The status strip carries messages.
    ctx.status("Shell check message");
    check(main.status_strip().message() == "Shell check message", "messages show in the status strip");

    // Narrow: the deck wraps and the right panel drops below the edit window.
    main.show_workspace(MainComponent::Workspace::Composer);
    window.setSize(1200, 800);
    main.setSize(1200, 800);
    pump(150);
    check(main.deck().getHeight() > 150 && main.selection_panel().getY() > main.edit_frame().getY() &&
              main.selection_panel().getRight() <= main.edit_frame().getRight() + 1,
          "under 1280 px the top deck wraps and the right panel drops below the edit window");
    shoot(main, dir, "25-shell-1200");
    window.setSize(1440, 900);
    main.setSize(1440, 900);
    pump(100);
}

/// UI spec step 3: one selection shared by both views, the counters and the
/// inspector; the shortcut table with Finale/Sibelius note values.
void selection_and_shortcut_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_from_template("cinematic.horror.psychological");
    main.show_workspace(MainComponent::Workspace::Composer);
    pump(200);
    const std::string track = session.selected_track();
    auto& composer = main.composer();
    composer.grabKeyboardFocus();

    // The default profile: Finale/Sibelius numbering (5 quarter, 4 eighth).
    check(ctx.shortcuts.name() == "Composer 2", "the default keyboard profile is Composer 2");
    for (const char c : {'5', 'c', 'd', '4', 'e', 'f', '6', 'g'}) composer.keyPressed(juce::KeyPress(c));
    pump(50);
    const auto* t = session.data().find_track(track);
    std::vector<notation::ScoreNote> line;
    if (t)
        for (const auto& n : t->active_version()->score.notes(0))
            if (!n.is_rest) line.push_back(n);
    std::sort(line.begin(), line.end(), [](const auto& a, const auto& b) { return a.start_tick < b.start_tick; });
    const bool values = line.size() == 5 && line[0].duration.value == notation::NoteValue::Quarter &&
                        line[2].duration.value == notation::NoteValue::Eighth &&
                        line[4].duration.value == notation::NoteValue::Half;
    check(values, "5, 4 and 6 choose a quarter, an eighth and a half, as in Finale and Sibelius");

    // Select everything in the Composer view: the shared selection, the
    // counters and the inspector follow.
    composer.perform(composer::Command::SelectAll);
    pump(50);
    const auto& sel = session.selection();
    check(sel.track == track && sel.notes.size() == 5 && sel.from == 0 && !line.empty() &&
              sel.to == line.back().end_tick(),
          "selecting notes in the Composer view is the shared selection, with their range (" +
              juce::String(static_cast<int>(sel.notes.size())) + " notes)");
    pump(120);  // the window's timer hands the range to the deck
    main.deck().update_position();
    const auto range = main.deck().range_lcd().text();
    check(range.contains("START   1|1|000") && range.contains("END     2|2|000"),
          "the start/end/length counter shows the selection (" + range.replace("\n", " / ") + ")");
    const auto inspector = main.selection_panel().readout().text();
    check(inspector.contains("NOTES     5") && inspector.contains("VALUE     MIXED") &&
              inspector.contains("VELOCITY") && !inspector.contains("VELOCITY  -"),
          "the inspector shows the selection's notes, value and velocity range");
    check(main.sequencer().range() == std::make_pair(sel.from, sel.to), "the ruler shows the selection's range");

    // The Producer view's piano roll has the same notes selected.
    main.show_workspace(MainComponent::Workspace::Producer);
    pump(200);
    shoot(main, dir, "26-selection-producer");
    // Selecting one note there selects it in the Composer view.
    if (auto* roll = find_named(main, "vcc-piano-roll")) {
        const auto notes = find_roll_notes(*roll);
        if (!notes.empty()) {
            const juce::Point<float> p(notes.front().left + 3.0f, notes.front().y);
            roll->mouseDown(mouse_event(*roll, p, p, false));
            roll->mouseUp(mouse_event(*roll, p, p, false));
            pump(50);
        }
        check(session.selection().notes.size() == 1, "clicking a note in the piano roll selects just that note");
        main.show_workspace(MainComponent::Workspace::Composer);
        pump(100);
        check(composer.selected_notes() == session.selection().notes,
              "the Composer view shows the note selected in the piano roll");
        shoot(main, dir, "27-selection-composer");
    }

    // A ruler range is the selection's range.
    main.sequencer().set_range(4 * kTicksPerQuarter, 8 * kTicksPerQuarter);
    check(session.selection().from == 4 * kTicksPerQuarter && session.selection().to == 8 * kTicksPerQuarter,
          "a range dragged on the ruler is the shared selection's range");

    // App shortcuts from the spec's table.
    auto* keys = ctx.commands.getKeyMappings();
    check(keys->containsMapping(cmd::rewind, juce::KeyPress(juce::KeyPress::returnKey)) &&
              keys->containsMapping(cmd::zoomIn, juce::KeyPress('=', juce::ModifierKeys::commandModifier, 0)) &&
              keys->containsMapping(cmd::zoomOut, juce::KeyPress('-', juce::ModifierKeys::commandModifier, 0)) &&
              keys->containsMapping(cmd::toggleView, juce::KeyPress(juce::KeyPress::tabKey, juce::ModifierKeys::ctrlModifier, 0)) &&
              !keys->containsMapping(cmd::showVideo, juce::KeyPress('3', juce::ModifierKeys::commandModifier, 0)),
          "Enter returns to zero, Cmd/Ctrl -/= zoom, Ctrl+Tab switches views, Cmd/Ctrl+1-4 are free for voices");
    ctx.commands.invokeDirectly(cmd::zoomIn, false);
    ctx.commands.invokeDirectly(cmd::zoomOut, false);

    // Switching profiles, remembered.
    check(main.use_shortcut_profile("Sibelius style") && ctx.shortcuts.name() == "Sibelius style" &&
              hw::ui_settings().getValue("shortcut_profile") == "Sibelius style",
          "the keyboard profile can be switched and is remembered");
    main.use_shortcut_profile("Composer 2");
}

/// Step 4 (docs/05): the Composer view's lanes, ruler rows, phrase blocks,
/// note-value keypad and inspector.
void composer_lane_checks(MainWindow& window, const juce::File& dir)
{
    using composer::Command;
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_from_template("cinematic.horror.psychological");
    main.show_workspace(MainComponent::Workspace::Composer);
    pump(200);
    const std::string track = session.selected_track();
    auto& composer = main.composer();
    composer.grabKeyboardFocus();
    const Ticks q = kTicksPerQuarter, bar = 4 * kTicksPerQuarter;

    // Geometry: 220 px headers, a 108 px single-staff row and a 120 px bar at 100%.
    check(composer.strip_count() > 1 && composer.header(0).getWidth() == composer_ui::TrackHeader::kWidth,
          "each track row starts with its 220 px header");
    const int row = composer.header(0).getHeight();
    check(row >= 104 && row <= 112, "a single-staff row is 108 px at 100% (" + juce::String(row) + ")");
    auto& lane0 = composer.lane(0);
    const float bar_px = lane0.x_of_tick(2 * bar) - lane0.x_of_tick(bar);
    check(std::abs(bar_px - 120.0f) < 1.5f, "a bar of 4/4 is 120 px at 100% (" + juce::String(bar_px, 1) + ")");
    bool aligned = true;
    for (int i = 1; i < composer.strip_count(); ++i)
        if (std::abs(composer.lane(i).x_of_tick(bar) - lane0.x_of_tick(bar)) > 0.5f) aligned = false;
    check(aligned, "bars line up across every lane, key signature or not");
    check(std::abs(composer.x_of_tick(bar) - (220.0f + lane0.x_of_tick(bar))) < 0.5f,
          "the ruler rows share the lanes' timeline");

    // A C chord in bar 1, then a line in bar 2; the 6 key (half) is lit after
    // choosing it from the toolbar for the next note.
    composer.keyPressed(juce::KeyPress('7'));
    composer.keyPressed(juce::KeyPress('c'));
    composer.keyPressed(juce::KeyPress('E', juce::ModifierKeys::shiftModifier, 'E'));
    composer.keyPressed(juce::KeyPress('G', juce::ModifierKeys::shiftModifier, 'G'));
    composer.keyPressed(juce::KeyPress('5'));
    for (const char ch : {'d', 'e', 'f', 'g'}) composer.keyPressed(juce::KeyPress(ch));
    pump(80);
    if (auto* k = main.toolbar().command_key(Command::SelectDurationHalf)) k->triggerClick();
    pump(150);
    const auto* half = main.toolbar().command_key(Command::SelectDurationHalf);
    const auto* quarter = main.toolbar().command_key(Command::SelectDurationQuarter);
    check(half && quarter && half->getToggleState() && !quarter->getToggleState() &&
              composer.cursor() && composer.cursor()->duration.value == notation::NoteValue::Half,
          "the note-value keypad chooses the next note's value and lights it");
    check(half && half->getButtonText() == "6" && quarter->getButtonText() == "5",
          "the keypad prints the Finale/Sibelius numbers (5 quarter, 6 half)");
    const auto& chords = composer.ruler().chords();
    // D Phrygian spells the E as E flat: a C minor chord.
    check(!chords.empty() && chords[0] == "Cm", "the Chords row reads the score (bar 1: " +
                                                    juce::String(chords.empty() ? "-" : chords[0]) + ")");

    // Header keys write the track.
    composer.header(0).solo_key().triggerClick();
    pump(50);
    check(session.data().find_track(track)->solo,
          "a header's S key solos its track");
    composer.header(0).solo_key().triggerClick();
    pump(50);

    // A phrase block on bar 2: dragging its name moves it a bar with its notes.
    const auto phrase = session.add_phrase(track, "Chorus melody", bar, 2 * bar);
    pump(80);
    auto bar2_notes = [&] {
        int n = 0;
        for (const auto& note : session.data().find_track(track)->active_version()->score.notes(0))
            if (!note.is_rest && note.start_tick >= 2 * bar && note.start_tick < 3 * bar) ++n;
        return n;
    };
    auto& layer = composer.phrases(0);
    const float x0 = lane0.x_of_tick(bar) + 20.0f;
    drag(layer, {x0, 8.0f}, {x0 + bar_px, 8.0f});
    pump(80);
    const auto* v = session.data().find_track(track)->active_version();
    check(!v->phrases.empty() && v->phrases.front().start == 2 * bar && bar2_notes() == 4,
          "dragging a phrase block's name moves it a bar, notes and all");
    shoot(main, dir, "28-composer-lanes");
    session.undo();
    pump(80);
    // Its right edge trims it on beats.
    const float right = lane0.x_of_tick(2 * bar) - 2.0f;
    drag(layer, {right, 40.0f}, {right - bar_px / 2.0f, 40.0f});
    pump(80);
    v = session.data().find_track(track)->active_version();
    check(!v->phrases.empty() && v->phrases.front().end == bar + 2 * q, "dragging a block's edge trims it on the beat");
    session.remove_phrase(track, phrase);

    // The inspector, on bar 2's notes.
    std::vector<notation::EventId> ids;
    for (const auto& note : session.data().find_track(track)->active_version()->score.notes(0))
        if (!note.is_rest && note.start_tick >= bar) ids.push_back(note.id);
    session.select_notes(track, ids);
    pump(80);
    auto& insp = main.inspector();
    insp.articulation_key(1).triggerClick();  // staccato
    pump(80);
    bool stacc = !ids.empty();
    for (auto id : ids)
        if (const auto* n = session.data().find_track(track)->active_version()->score.find_note(id))
            if (!notation::has_articulation(n->articulations, notation::Articulation::Staccato)) stacc = false;
    check(stacc && insp.articulation_key(1).getToggleState(),
          "the Staccato key marks every selected note and lights");
    insp.dynamic_key(3).triggerClick();  // mf
    pump(80);
    check(insp.dynamic_key(3).getToggleState(), "the mf key writes a dynamic where the selection starts and lights");
    insp.set_velocity(100);
    pump(50);
    check(insp.performed_velocity() == 100, "the Velocity knob sets the notes' performed velocity (" +
                                                juce::String(insp.performed_velocity()) + ")");
    const int before = notation::midi_note_number(
        session.data().find_track(track)->active_version()->score.find_note(ids.front())->pitch);
    insp.transpose_key(2).triggerClick();  // +1
    pump(80);
    const auto* moved = session.data().find_track(track)->active_version()->score.find_note(ids.front());
    check(moved && notation::midi_note_number(moved->pitch) == before + 1, "Transpose +1 raises the notes a semitone");
    insp.apply_humanize(60.0);
    pump(50);
    const auto* perf = session.performance_of(track);
    bool varied = false;
    if (perf)
        for (const auto& [id, o] : *perf)
            if (o.onset != 0) varied = true;
    check(varied && insp.performed_velocity() == 100,
          "Humanize writes timing overrides, keeps the velocity set, never the score");
    shoot(main, dir, "29-composer-inspector");
}

/// Loop playback (L) and the keyboard deck (step 5).
void loop_and_keyboard_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_from_template("cinematic.horror.psychological");
    main.show_workspace(MainComponent::Workspace::Composer);
    pump(200);
    const Ticks bar = 4 * kTicksPerQuarter;

    // The engine loops sample-accurately: offline, one bar looped.
    {
        PlaybackEngine e(session, ctx.plugins);
        e.prepare_offline(48000.0, 512);
        e.set_loop(true, 0, bar);
        e.locate(0);
        e.play();
        juce::AudioBuffer<float> out(2, 512);
        double furthest = 0.0, last = 0.0;
        int wraps = 0;
        const double bar_seconds = session.data().tempo.seconds_at(bar);
        for (int b = 0; b < static_cast<int>(2.5 * bar_seconds * 48000.0 / 512.0); ++b) {
            e.render_offline(out, 512);
            const double s = e.position_seconds();
            if (s < last) ++wraps;
            furthest = std::max(furthest, s);
            last = s;
        }
        check(wraps == 2 && furthest <= bar_seconds + 1e-6,
              "loop playback jumps back at the loop's end, never past it (" + juce::String(wraps) + " wraps, furthest " +
                  juce::String(furthest, 3) + " s of " + juce::String(bar_seconds, 3) + " s)");
        e.stop();
    }
    // L and the deck's Loop key: with no range, the bar at the playhead.
    auto* keys = ctx.commands.getKeyMappings();
    check(keys->containsMapping(cmd::toggleLoop, juce::KeyPress('l')), "L toggles loop playback");
    ctx.engine.locate(bar + kTicksPerQuarter);
    ctx.commands.invokeDirectly(cmd::toggleLoop, false);
    pump(80);
    auto* loop_key = find_by_id(main, "transport_loop");
    check(ctx.engine.loop() && ctx.engine.loop_range() == std::make_pair(bar, 2 * bar) &&
              session.selection().from == bar && session.selection().to == 2 * bar && loop_key &&
              dynamic_cast<juce::Button*>(loop_key)->getToggleState(),
          "Loop with no range loops the playhead's bar, shows it as the range and lights the Loop key");
    session.set_selection_range(0, 2 * bar);
    pump(50);
    check(ctx.engine.loop_range() == std::make_pair(Ticks{0}, 2 * bar), "the loop follows the selection's range");
    ctx.commands.invokeDirectly(cmd::toggleLoop, false);
    pump(50);
    check(!ctx.engine.loop(), "L again turns the loop off");
    ctx.engine.locate(0);

    // The keyboard deck: 88 keys, 22 px each at 1x, 96 px tall; a 140 px deck.
    auto& deck = main.keyboard();
    auto& piano = deck.keys();
    deck.set_zoom(1.0);
    pump(50);
    check(piano.getWidth() == 1144 && piano.getHeight() == 96 && deck.getHeight() == 140,
          "88 keys, A0-C8, 1144 x 96 px at 1x in a 140 px deck (" + juce::String(piano.getWidth()) + " x " +
              juce::String(piano.getHeight()) + ", deck " + juce::String(deck.getHeight()) + ")");
    int whites = 0, blacks = 0;
    for (int n = shell::PianoKeys::kLowest; n <= shell::PianoKeys::kHighest; ++n) (shell::PianoKeys::is_black(n) ? blacks : whites)++;
    check(whites == 52 && blacks == 36, "52 white keys and 36 black keys");
    const int edit_before = main.composer().getHeight();
    deck.set_zoom(2.0);
    pump(80);
    const auto vr = deck.visible_range();
    check(piano.getHeight() == 144 && deck.getHeight() == 188 && main.composer().getHeight() < edit_before,
          "zooming to 2x makes the keys taller and the deck grows, shrinking the edit window");
    check(vr.first <= 60 && vr.second >= 60 &&
              std::abs(piano.centre_of(60) - (piano.centre_of(vr.first) + piano.centre_of(vr.second)) / 2.0f) <
                  static_cast<float>(piano.key_width()),
          "zoom keeps the anchor (middle C) centred");
    check(std::abs(session.data().key_zoom - 2.0) < 1e-9 && deck.zoom_lcd().text() == "2.00x",
          "the key zoom shows on its LCD and is remembered with the project");
    deck.zoom_slider().setValue(1.55, juce::sendNotificationSync);
    pump(30);
    check(std::abs(deck.zoom() - 1.55) < 1e-9, "the zoom slider zooms live in 0.05 steps");

    // Step input: a key played enters the note at the Composer view's cursor.
    auto& composer = main.composer();
    composer.grabKeyboardFocus();
    const auto track = session.selected_track();
    const auto count = [&] {
        int n = 0;
        for (const auto& note : session.data().find_track(track)->active_version()->score.notes(0))
            if (!note.is_rest) ++n;
        return n;
    };
    const int before = count();
    const juce::Point<float> d(piano.centre_of(62), static_cast<float>(piano.getHeight()) - 10.0f);
    piano.mouseDown(mouse_event(piano, d, d, false));
    pump(30);
    const bool held = ctx.engine.live_held(62) && piano.lit()[62];
    piano.mouseUp(mouse_event(piano, d, d, false));
    pump(80);
    const auto* t = session.data().find_track(track);
    bool entered_d = false;
    for (const auto& note : t->active_version()->score.notes(0))
        if (!note.is_rest && notation::midi_note_number(note.pitch) + t->active_version()->score.staff(0).total_transposition() == 62)
            entered_d = true;
    check(count() == before + 1 && entered_d, "a key played on the deck enters its note at the cursor (step input)");
    check(held, "a key held lights and sounds on the selected track");
    deck.refresh();
    check(piano.lit()[62] && deck.step_lcd().text().contains("NEXT") && deck.step_lcd().text().contains("VEL"),
          "the entered note stays lit as the selection; the step LCD shows the next position, value and velocity");
    deck.zoom_to_selection();
    pump(50);
    check(std::abs(deck.zoom() - 2.5) < 1e-9, "Zoom to selection sets 2.5x around the selected notes");
    deck.set_octave(1);
    check(ctx.engine.live_octave() == 1 && deck.octave_lcd().text() == "+1", "Octave + shifts MIDI keyboard input and shows +1");
    deck.set_octave(0);
    shoot(main, dir, "30-keyboard-deck");
    deck.set_zoom(1.0);
    pump(50);
}

/// Out-of-process hosting (Main Spec 20): plug-ins run in their own
/// processes; one that crashes while playing leaves the session playing.
void hosting_checks(AppContext& ctx, const juce::File& dir)
{
    auto& host = ctx.plugins;
    check(host.hosts_out_of_process(), "plug-ins run in their own processes (" + host.host_tool().getFileName() + ")");
    const auto crasher = scan(ctx, VCC_TEST_CRASHPLAY_PLUGIN);
    const auto effect = scan(ctx, VCC_TEST_EFFECT);
    if (!crasher || !effect) {
        check(false, "the hosting test plug-ins scan");
        return;
    }
    session::Session s(ctx.catalogs);
    s.new_empty();
    std::string error;
    s.save_as(dir.getChildFile("hosting-check.vcc2").getFullPathName().toStdString(), &error);
    // A steady tone on an audio track, through the plug-in that crashes when
    // its gain is turned all the way down.
    const auto file = dir.getChildFile("Audio Files").getChildFile("tone.wav");
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
        auto writer = juce::WavAudioFormat().createWriterFor(
            stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000.0).withNumChannels(1).withBitsPerSample(24));
        juce::AudioBuffer<float> wave(1, 48000 * 10);
        for (int i = 0; i < wave.getNumSamples(); ++i)
            wave.setSample(0, i, 0.3f * std::sin(2.0f * juce::MathConstants<float>::pi * 220.0f * static_cast<float>(i) / 48000.0f));
        if (writer) writer->writeFromAudioSampleBuffer(wave, 0, wave.getNumSamples());
    }
    const auto track = s.add_audio_track("Tone", false);
    project::AudioClip clip;
    clip.file = "Audio Files/tone.wav";
    clip.length = 10.0;
    clip.channels = 1;
    clip.file_sample_rate = 48000.0;
    s.add_audio_clips({{track, clip}}, "Import");
    s.update_track_mix(track, "inserts", [&](project::Track& t) {
        project::InsertSlot slot;
        slot.kind = "plugin";
        slot.suggestion = false;
        slot.plugin = *crasher;
        t.inserts = {slot};
    });
    PlaybackEngine e(s, host);
    e.prepare_offline(48000.0, 512);
    juce::AudioBuffer<float> out(2, 512);
    auto render = [&](int blocks) {
        double energy = 0.0;
        for (int b = 0; b < blocks; ++b) {
            e.render_offline(out, 512);
            energy += out.getRMSLevel(0, 0, 512);
        }
        return energy / blocks;
    };
    e.play();
    const double before = render(40);
    auto* plugin = e.insert_plugin(track, 0);
    const bool remote = dynamic_cast<RemotePluginInstance*>(plugin) != nullptr;
    check(remote && before > 0.05, "an effect runs in its own process and the track plays through it (level " +
                                       juce::String(before, 3) + ")");
    if (!plugin) return;

    // Crash it while playing: the session keeps going, the audio passes through.
    plugin->getParameters()[1]->setValue(0.0f);
    const double during = render(40);
    const auto crashed = e.take_newly_crashed_plugins();
    check(crashed.size() == 1 && e.take_newly_crashed_plugins().isEmpty() &&
              e.insert_status(track, 0).startsWith("Crashed"),
          "a plug-in that crashes while playing is reported once (" + e.insert_status(track, 0) + ")");
    const double after = render(40);
    check(after > 0.05, "the session plays on through the crash, audio passing the crashed insert (level " +
                            juce::String(during, 3) + " then " + juce::String(after, 3) + ")");
    juce::MemoryBlock state;
    plugin->getStateInformation(state);
    check(state.getSize() > 0, "a crashed plug-in still gives back its last state for saving");

    // Reload: a fresh process, from the last state.
    check(e.reload_crashed_plugins() == 1, "Reload Crashed Plug-ins starts it again");
    auto* again = e.insert_plugin(track, 0);
    auto* again_remote = dynamic_cast<RemotePluginInstance*>(again);
    const double reloaded = render(40);
    check(again_remote && !again_remote->crashed() && again != plugin && e.insert_status(track, 0).isEmpty() &&
              reloaded > 0.05,
          "the reloaded plug-in plays again (level " + juce::String(reloaded, 3) + ")");
    e.stop();

    // The real-time path: blocks driven from an audio-like thread must make
    // their deadlines (the offline path above waits; this one may not).
    s.update_track_mix(track, "inserts", [&](project::Track& t) {
        project::InsertSlot slot;
        slot.kind = "plugin";
        slot.suggestion = false;
        slot.plugin = *effect;
        t.inserts = {slot};
    });
    e.rebuild_now();
    e.take_late_plugin_blocks();
    std::atomic<double> rt_energy{0.0};
    std::thread audio([&] {
        juce::AudioBuffer<float> buffer(2, 512);
        double sum = 0.0;
        for (int b = 0; b < 200; ++b) {
            e.render_offline(buffer, 512);
            sum += buffer.getRMSLevel(0, 0, 512);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        rt_energy = sum / 200.0;
    });
    e.play();
    audio.join();
    e.stop();
    const int late = e.take_late_plugin_blocks();
    check(late == 0 && rt_energy.load() > 0.05, "a plug-in in its own process keeps up on the real-time path (" +
                                                    juce::String(late) + " late blocks, level " +
                                                    juce::String(rt_energy.load(), 3) + ")");
    auto* shown = dynamic_cast<RemotePluginInstance*>(e.insert_plugin(track, 0));
    check(shown && shown->show_editor(true) && shown->show_editor(false), "the plug-in's window opens in its own process");
    e.stop();

    // Hosting modes: how many processes carry an instrument and two effects.
    const auto instrument = scan(ctx, VCC_TEST_PLUGIN);
    if (!instrument) return;
    using Mode = PluginHost::HostingMode;
    const auto original = host.hosting_mode();
    host.clear_crash_isolation();
    // Counted against what other engines in this check already hold.
    auto census = [&] {
        int processes = 0, instances = 0;
        for (const auto& [label, n] : host.host_processes()) {
            ++processes;
            instances += n;
        }
        return std::pair<int, int>{processes, instances};
    };
    auto processes_for = [&](Mode mode) {
        host.set_hosting_mode(mode);
        const auto base = census();
        juce::String why;
        std::vector<std::unique_ptr<juce::AudioPluginInstance>> held;
        for (const auto* ref : {&*instrument, &*effect, &*effect}) held.push_back(host.create(*ref, 48000.0, 512, why));
        const auto now = census();
        int is_remote = 0;
        for (const auto& h : held) is_remote += dynamic_cast<RemotePluginInstance*>(h.get()) != nullptr;
        return std::tuple<int, int, int>{now.first - base.first, now.second - base.second, is_remote};
    };
    const auto [each_p, each_i, each_r] = processes_for(Mode::each_instance);
    const auto [plug_p, plug_i, plug_r] = processes_for(Mode::per_plugin);
    const auto [maker_p, maker_i, maker_r] = processes_for(Mode::per_manufacturer);
    const auto [shared_p, shared_i, shared_r] = processes_for(Mode::shared);
    const auto [app_p, app_i, app_r] = processes_for(Mode::in_app);
    check(each_p == 3 && plug_p == 2 && maker_p == 1 && shared_p == 1 && app_p == 0 && each_r == 3 && shared_i == 3 &&
              app_r == 0,
          "hosting modes put an instrument and two effects in the right processes (each " + juce::String(each_p) +
              ", per plug-in " + juce::String(plug_p) + ", per manufacturer " + juce::String(maker_p) + ", shared " +
              juce::String(shared_p) + ", in app " + juce::String(app_p) + ")");
    juce::ignoreUnused(each_i, plug_i, plug_r, maker_i, maker_r, shared_r, app_i);

    // Shared: two tracks, each through an effect at half gain, one process.
    host.set_hosting_mode(Mode::shared);
    const auto other = s.add_audio_track("Tone 2", false);
    s.add_audio_clips({{other, clip}}, "Import");
    for (const auto& id : {track, other})
        s.update_track_mix(id, "inserts", [&](project::Track& t) {
            project::InsertSlot slot;
            slot.kind = "plugin";
            slot.suggestion = false;
            slot.plugin = *effect;
            t.inserts = {slot};
        });
    check(e.restart_plugins() >= 2, "changing the hosting mode restarts the open project's plug-ins");
    for (const auto& id : {track, other})
        if (auto* p = e.insert_plugin(id, 0)) p->getParameters()[1]->setValue(0.5f);
    e.play();
    const double half = render(40);
    e.stop();
    int shared_instances = 0;
    for (const auto& [label, n] : host.host_processes())
        if (label == "All plug-ins") shared_instances = n;
    check(shared_instances == 2 && std::abs(half - before) < 0.02,
          "two effects sharing one process both process their tracks (level " + juce::String(half, 3) + " for two tracks at half gain)");

    // The settings survive a mode change, and insert state is saved.
    host.set_hosting_mode(Mode::per_plugin);
    e.restart_plugins();
    auto* moved = e.insert_plugin(track, 0);
    const auto* saved = s.data().find_track(track);
    check(moved && std::abs(moved->getParameters()[1]->getValue() - 0.5f) < 0.01f && saved &&
              !saved->inserts.empty() && !saved->inserts[0].plugin.state_base64.empty(),
          "a mode change keeps each plug-in's settings, and effect state is saved in the project");

    // A crash in a shared process silences what it carries; a reload isolates them.
    host.set_hosting_mode(Mode::shared);
    s.update_track_mix(other, "inserts", [&](project::Track& t) {
        project::InsertSlot slot;
        slot.kind = "plugin";
        slot.suggestion = false;
        slot.plugin = *crasher;
        t.inserts = {slot};
    });
    e.restart_plugins();
    e.take_newly_crashed_plugins();
    e.play();
    render(10);
    if (auto* p = e.insert_plugin(other, 0)) p->getParameters()[1]->setValue(0.0f);  // crash it
    render(20);
    const auto both = e.take_newly_crashed_plugins();
    check(both.size() == 2, "a crash in a shared process takes down what that process carried (" + both.joinIntoString(", ") + ")");
    check(e.reload_crashed_plugins() == 2, "both are reloaded");
    const double recovered = render(20);
    e.stop();
    auto* tone_fx = dynamic_cast<RemotePluginInstance*>(e.insert_plugin(track, 0));
    auto* crash_fx = dynamic_cast<RemotePluginInstance*>(e.insert_plugin(other, 0));
    check(tone_fx && crash_fx && &tone_fx->process() != &crash_fx->process() && tone_fx->process().instance_count() == 1 &&
              crash_fx->process().instance_count() == 1 && recovered > 0.05,
          "after a crash, the plug-ins that crashed run in processes of their own (level " + juce::String(recovered, 3) + ")");
    host.set_hosting_mode(original);
}

}  // namespace

int main(int argc, char** argv)
{
    vcc::desktop::prepare_display();
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File dir(argc > 1 ? juce::String(argv[1]) : juce::File::getCurrentWorkingDirectory().getChildFile("screenshots").getFullPathName());
    dir.createDirectory();
    // `vcc_desktop_check <dir> --ipc-only`: just the plug-in IPC wait check
    // (fast; for checking the kernel wait on a new platform or machine).
    if (argc > 2 && juce::String(argv[2]) == "--ipc-only") {
        ipc_wait_checks(dir);
        std::printf("%s: %d failure(s)\n", failures == 0 ? "PASSED" : "FAILED", failures);
        return failures == 0 ? 0 : 1;
    }
    // The plug-in list and scan failures go to a throwaway settings folder,
    // never the user's own.
    const auto settings = dir.getChildFile("settings");
    settings.deleteRecursively();
    settings.createDirectory();
#if JUCE_WINDOWS
    _putenv_s("VCC_SETTINGS_DIR", settings.getFullPathName().toRawUTF8());
#else
    setenv("VCC_SETTINGS_DIR", settings.getFullPathName().toRawUTF8(), 1);
#endif

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
        performance_checks(ctx, window.main(), track, dir);
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

        // Out-of-process scanning (Main Spec 20): a plug-in that crashes or
        // hangs while it loads is blacklisted with the reason; the app lives on.
        {
            auto& host = ctx.plugins;
            host.set_scanner_tool(juce::File(VCC_PLUGIN_SCANNER_TOOL));
            host.set_scan_timeout_ms(4000);
            check(host.scans_out_of_process(), "plug-ins are scanned out of process (" +
                                                   host.scanner_tool().getFileName() + ")");
            juce::VST3PluginFormat vst3;
            auto& known = host.known();
            for (const char* p : {VCC_TEST_PLUGIN, VCC_TEST_CRASH_PLUGIN, VCC_TEST_HANG_PLUGIN}) {
                known.removeFromBlacklist(p);
                host.clear_scan_failure(p);
            }
            juce::OwnedArray<juce::PluginDescription> found;
            known.scanAndAddFile(VCC_TEST_PLUGIN, false, found, vst3);
            check(found.size() == 1 && found[0]->name == "VCC Test Instrument" && found[0]->isInstrument,
                  "a good plug-in scans in its own process (" +
                      (found.isEmpty() ? juce::String("nothing found") : found[0]->name) + ")");

            found.clear();
            known.scanAndAddFile(VCC_TEST_CRASH_PLUGIN, false, found, vst3);
            auto failures = host.scan_failures();
            const auto crash_reason = failures.count(VCC_TEST_CRASH_PLUGIN) ? failures[VCC_TEST_CRASH_PLUGIN] : juce::String();
            check(found.isEmpty() && known.getBlacklistedFiles().contains(VCC_TEST_CRASH_PLUGIN) &&
                      crash_reason.contains("crashed"),
                  "a plug-in that crashes while loading is blacklisted and the app carries on (" + crash_reason + ")");

            found.clear();
            const auto t0 = juce::Time::getMillisecondCounter();
            known.scanAndAddFile(VCC_TEST_HANG_PLUGIN, false, found, vst3);
            const auto waited = juce::Time::getMillisecondCounter() - t0;
            failures = host.scan_failures();
            const auto hang_reason = failures.count(VCC_TEST_HANG_PLUGIN) ? failures[VCC_TEST_HANG_PLUGIN] : juce::String();
            check(found.isEmpty() && known.getBlacklistedFiles().contains(VCC_TEST_HANG_PLUGIN) &&
                      hang_reason.contains("did not finish") && waited < 9000,
                  "a plug-in that hangs while loading is stopped after the time limit (" + juce::String(waited) +
                      " ms: " + hang_reason + ")");

            // A whole folder with all three: the scan completes, the good one is found.
            const auto folder = dir.getChildFile("plugin-folder");
            folder.deleteRecursively();
            folder.createDirectory();
            for (const char* p : {VCC_TEST_PLUGIN, VCC_TEST_CRASH_PLUGIN, VCC_TEST_HANG_PLUGIN}) {
                const juce::File src(p);
                src.copyDirectoryTo(folder.getChildFile(src.getFileName()));
            }
            juce::KnownPluginList folder_list;
            folder_list.setCustomScanner(nullptr);
            struct Forward final : juce::KnownPluginList::CustomScanner {
                explicit Forward(PluginHost& h) : host(h) {}
                bool findPluginTypesFor(juce::AudioPluginFormat& f, juce::OwnedArray<juce::PluginDescription>& r,
                                        const juce::String& id) override
                {
                    return host.scan_file(f, id, r);
                }
                PluginHost& host;
            };
            folder_list.setCustomScanner(std::make_unique<Forward>(host));
            juce::PluginDirectoryScanner scanner(folder_list, vst3, juce::FileSearchPath(folder.getFullPathName()), true,
                                                 juce::File(), false);
            juce::String name;
            int steps = 0;
            while (scanner.scanNextFile(true, name) && ++steps < 20) {}
            for (const auto& f : folder_list.getBlacklistedFiles()) host.clear_scan_failure(f);
            check(folder_list.getNumTypes() == 1 && folder_list.getBlacklistedFiles().size() == 2,
                  "a folder scan with a crasher and a hanger in it completes (" + juce::String(folder_list.getNumTypes()) +
                      " found, " + juce::String(folder_list.getBlacklistedFiles().size()) + " blacklisted)");

            // Without the tool, scanning still works (in process).
            host.set_scanner_tool(dir.getChildFile("no-such-scanner"));
            found.clear();
            juce::KnownPluginList fallback;
            fallback.setCustomScanner(std::make_unique<Forward>(host));
            fallback.scanAndAddFile(VCC_TEST_PLUGIN, false, found, vst3);
            check(!host.scans_out_of_process() && found.size() == 1,
                  "without the scanner tool, scanning falls back to in process");
            host.set_scanner_tool(juce::File(VCC_PLUGIN_SCANNER_TOOL));
            // The Plug-ins window shows how it scans and why each failure failed.
            ctx.commands.invokeDirectly(cmd::scanPlugins, false);
            pump(500);
            if (auto* w = find_dialog("Instrument and effect plug-ins")) {
                if (auto* top = w->getTopLevelComponent()) top->setSize(760, 620);
                pump(200);
                shoot(*w, dir, "12-plugins");
                w->exitModalState(0);
                pump(200);
            }
            for (const char* p : {VCC_TEST_CRASH_PLUGIN, VCC_TEST_HANG_PLUGIN}) host.clear_scan_failure(p);
            folder.deleteRecursively();
        }

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

        // Disk streaming (Main Spec 11): a long file plays sample-exactly far past
        // the read-ahead, after a locate into it, and with the real disk thread.
        {
            const auto file = dir.getChildFile("streaming-ramp.wav");
            file.deleteFile();
            constexpr int rate = 48000, seconds = 20;
            auto value_at = [](std::int64_t i) { return 0.05f + 0.4f * static_cast<float>(i % 4801) / 4800.0f; };
            {
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
                auto w = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                         .withSampleRate(rate)
                                                         .withNumChannels(1)
                                                         .withBitsPerSample(32)
                                                         .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
                juce::AudioBuffer<float> b(1, rate);
                for (int s2 = 0; s2 < seconds; ++s2) {
                    for (int i = 0; i < rate; ++i) b.setSample(0, i, value_at(static_cast<std::int64_t>(s2) * rate + i));
                    w->writeFromAudioSampleBuffer(b, 0, rate);
                }
            }
            session::Session solo(ctx.catalogs);
            solo.new_empty();
            const auto tid = solo.add_audio_track("Ramp", false);
            project::AudioClip c;
            c.file = file.getFullPathName().toStdString();
            c.start_seconds = 1.0;
            c.source_offset = 2.0;
            c.length = 15.0;
            c.channels = 1;
            c.file_sample_rate = rate;
            solo.add_audio_clips({{tid, c}}, "Import", false);
            PlaybackEngine e(solo, ctx.plugins);
            e.prepare_offline(rate, 512);
            juce::AudioBuffer<float> out(2, 512);
            // Expected left sample at timeline sample t (mono, centre pan: -3 dB).
            auto expected = [&](std::int64_t at) { return value_at(at - rate + 2 * rate) * 0.70710678f; };
            auto play_and_compare = [&](double from_seconds, int blocks, double& worst) {
                std::int64_t ts = static_cast<std::int64_t>(from_seconds * rate);
                for (int b = 0; b < blocks; ++b, ts += 512) {
                    e.render_offline(out, 512);
                    for (int i = 0; i < 512; ++i) {
                        const std::int64_t at = ts + i;
                        if (at < rate + 200 || at > 16 * rate - 200) continue;  // clip edges (de-click)
                        worst = std::max(worst, static_cast<double>(std::abs(out.getSample(0, i) - expected(at))));
                    }
                }
            };
            double worst = 0.0;
            e.locate(solo.data().tempo.tick_at_seconds(11.0));
            e.play();
            play_and_compare(11.0, 400, worst);  // 11 s to ~15.3 s: far past the 0.75 s read-ahead
            e.stop();
            check(worst < 1.0e-5, "a long clip streams sample-exactly after a locate into it (worst error " +
                                      juce::String(worst, 8) + ")");

            // The real disk thread: cue while stopped, then play: no dropouts.
            e.set_disk_synchronous(false);
            e.take_disk_underruns();
            e.locate(solo.data().tempo.tick_at_seconds(6.0));
            for (int b = 0; b < 20; ++b) {
                e.render_offline(out, 512);  // stopped: cues the stream
                juce::Thread::sleep(3);
            }
            e.play();
            double worst_rt = 0.0;
            for (int b = 0; b < 200; ++b) {
                std::int64_t t0 = static_cast<std::int64_t>(6.0 * rate) + static_cast<std::int64_t>(b) * 512;
                e.render_offline(out, 512);
                for (int i = 0; i < 512; ++i)
                    worst_rt = std::max(worst_rt, static_cast<double>(std::abs(out.getSample(0, i) - expected(t0 + i))));
                juce::Thread::sleep(2);  // faster than real time, but the disk keeps up
            }
            const int underruns = e.take_disk_underruns();
            check(underruns == 0 && worst_rt < 1.0e-5,
                  "the disk thread keeps a cued stream ahead of playback (" + juce::String(underruns) +
                      " dropouts, worst error " + juce::String(worst_rt, 8) + ")");
            // A jump while playing is a counted dropout, and the stream recovers.
            e.locate(solo.data().tempo.tick_at_seconds(13.0));
            std::int64_t ts = static_cast<std::int64_t>(13.0 * rate);
            double worst_after = 0.0;
            for (int b = 0; b < 60; ++b, ts += 512) {
                e.render_offline(out, 512);
                if (b >= 10)
                    for (int i = 0; i < 512; ++i)
                        worst_after = std::max(worst_after, static_cast<double>(std::abs(out.getSample(0, i) - expected(ts + i))));
                juce::Thread::sleep(3);
            }
            const int jump_underruns = e.take_disk_underruns();
            check(jump_underruns == 0 && worst_after < 1.0e-5,
                  "a jump while playing recovers within a few blocks and is not mistaken for a slow disk (" +
                      juce::String(jump_underruns) + " starved blocks, then worst error " + juce::String(worst_after, 8) + ")");
            e.stop();
        }

        // Stems (Main Spec 16): the mix and every stem, adding back up to the mix.
        {
            auto stems = session.stems();
            check(stems.size() >= 2 && project::stem_coverage(stems, session.data()).complete(),
                  "default stems cover every deliverable track once (" + juce::String(static_cast<int>(stems.size())) +
                      " stems)");
            // Music in two stems, one of them sending to the reverb.
            std::vector<std::string> playing;
            for (const auto& st : stems) {
                const auto tracks = project::stem_tracks(st, session.data());
                if (tracks.empty() || playing.size() == 2) continue;
                const std::string tid = tracks.front()->id;
                session.insert_notes(tid, 0, 2 * kTicksPerWhole,
                                     {{0, kTicksPerWhole, 52 + 7 * static_cast<int>(playing.size()), 100, 0, {}},
                                      {kTicksPerWhole, kTicksPerWhole, 55, 90, 0, {}}},
                                     project::CollisionPolicy::Merge, "check");
                playing.push_back(st.id);
            }
            StemRenderRequest rq;
            rq.folder = dir.getChildFile("stems");
            rq.folder.deleteRecursively();
            rq.ranges = {whole_project_range(ctx)};
            rq.ranges.front().end = 2 * kTicksPerWhole;
            rq.tail_seconds = 1.5;
            auto printed = render_stems(ctx, rq);
            int ok_files = 0;
            std::map<std::string, float> peak_of;
            for (const auto& f : printed.files) {
                ok_files += f.ok;
                peak_of[f.stem] = f.peak;
            }
            bool music_heard = peak_of["Mix"] > 1.0e-3f;
            for (const auto& st : stems)
                if (std::find(playing.begin(), playing.end(), st.id) != playing.end())
                    music_heard = music_heard && peak_of[st.name] > 1.0e-3f;
            check(ok_files == static_cast<int>(stems.size()) + 1,
                  "the mix and every stem print (" + juce::String(ok_files) + " files)");
            check(music_heard, "the mix and the stems given music are audible");
            check(printed.ranges.size() == 1 && printed.ranges[0].sum_checked && printed.ranges[0].sum_error_db < -60.0,
                  "the stems add back up to the mix (residual " +
                      (printed.ranges.empty() ? juce::String("?") : juce::String(printed.ranges[0].sum_error_db, 1)) + " dB)");
            for (const auto& w : printed.warnings) std::printf("  note %s\n", w.toRawUTF8());

            // A dry stem prints without its reverb share; a cue's stems tick its deliverable.
            auto edited = stems;
            for (auto& st : edited) st.include_effects = false;
            session.set_stems(edited);
            StemRenderRequest dry = rq;
            dry.include_mix = false;
            dry.folder = dir.getChildFile("stems-dry");
            dry.folder.deleteRecursively();
            dry.stem_ids = {playing.front()};
            auto rd = render_stems(ctx, dry);
            check(rd.files.size() == 1 && rd.files[0].ok && rd.files[0].peak > 1.0e-3f,
                  "a dry stem prints (" + (rd.files.empty() ? juce::String() : rd.files[0].message) + ")");
            // The last second is the release tail: the wet print carries reverb there, the dry one less.
            auto tail_rms = [](const juce::File& f) {
                juce::AudioFormatManager fm;
                fm.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(f));
                if (!reader) return -1.0f;
                const int n = static_cast<int>(reader->sampleRate);
                juce::AudioBuffer<float> b(2, n);
                reader->read(&b, 0, n, reader->lengthInSamples - n, true, true);
                return std::max(b.getRMSLevel(0, 0, n), b.getRMSLevel(1, 0, n));
            };
            juce::File wet_file;
            for (const auto& f : printed.files)
                for (const auto& st : stems)
                    if (st.id == playing.front() && f.stem == st.name) wet_file = f.file;
            const float wet_tail = tail_rms(wet_file), dry_tail = rd.files.empty() ? -1.0f : tail_rms(rd.files[0].file);
            check(wet_tail > 0.0f && dry_tail >= 0.0f && wet_tail > dry_tail * 1.5f,
                  "the dry stem leaves its reverb share out (tail rms wet " + juce::String(wet_tail, 5) + ", dry " +
                      juce::String(dry_tail, 5) + ")");
            session.undo();

            StemRenderRequest per_cue = rq;
            per_cue.folder = dir.getChildFile("stems-cues");
            per_cue.folder.deleteRecursively();
            per_cue.ranges = cue_ranges(ctx);
            per_cue.include_mix = false;
            auto rc = render_stems(ctx, per_cue);
            bool named = !rc.files.empty();
            for (const auto& f : rc.files) named = named && f.ok && f.file.getFileName().contains("1M1");
            const auto* c = session.data().find_cue(cue_id);
            bool ticked = false;
            for (const auto& del : c ? c->deliverables : std::vector<project::Deliverable>{})
                ticked = ticked || (del.name == "Stems" && del.done);
            check(named && ticked, "per-cue stems are named by cue and tick the cue's Stems deliverable");
            show_stem_export(ctx);
            pump(300);
            if (auto* w = find_dialog("Export Stems")) shoot(*w, dir, "11-export-stems");
            auto again = render_stems(ctx, per_cue);
            bool skipped = !again.files.empty();
            for (const auto& f : again.files) skipped = skipped && f.skipped_protected;
            check(skipped, "an approved cue's stems are not overwritten unasked");
        }

        // Notation out and in (Main Spec 16): printed parts as PDF, MusicXML.
        {
            const auto parts_dir = dir.getChildFile("parts");
            parts_dir.deleteRecursively();
            juce::String message;
            const int pages = export_parts_pdf(ctx, parts_dir, message);
            const auto pdfs = parts_dir.findChildFiles(juce::File::findFiles, false, "*.pdf");
            juce::MemoryBlock first;
            if (!pdfs.isEmpty()) pdfs[0].loadFileAsData(first);
            check(pages > 0 && !pdfs.isEmpty() && first.getSize() > 10000 &&
                      juce::String(static_cast<const char*>(first.getData()), 5) == "%PDF-",
                  "printed parts are written as PDF (" + message + ")");
            const auto all = dir.getChildFile("all-parts.pdf");
            all.deleteFile();
            const int all_pages = export_parts_pdf(ctx, all, message);
            juce::MemoryBlock all_bytes;
            all.loadFileAsData(all_bytes);
            const std::string all_text(static_cast<const char*>(all_bytes.getData()), all_bytes.getSize());
            check(all_pages == pages && all_text.find("/Outlines") != std::string::npos &&
                      all_text.find("/Title (") != std::string::npos,
                  "every part prints into one PDF with a bookmark per part (" + juce::String(pages) + " pages)");
            // Look at a printed page, when poppler is installed.
            const juce::File pdftoppm("/usr/bin/pdftoppm");
            if (pdftoppm.existsAsFile() && !pdfs.isEmpty()) {
                // The part with the typed notes, else the first.
                juce::File page = pdfs[0];
                const juce::String typed = juce::File::createLegalFileName(session.data().find_track(track)->name);
                for (const auto& f : pdfs)
                    if (f.getFileNameWithoutExtension().endsWith(typed)) page = f;
                juce::ChildProcess p;
                const auto out = dir.getChildFile("09-part-pdf");
                if (p.start(juce::StringArray{pdftoppm.getFullPathName(), "-r", "90", "-png", "-singlefile", "-f", "1",
                                              page.getFullPathName(), out.getFullPathName()})) {
                    p.waitForProcessToFinish(20000);
                    check(out.withFileExtension("png").getSize() > 5000, "a printed page renders (09-part-pdf)");
                }
            }

            // The full score, transposed and in C.
            for (const bool concert : {false, true}) {
                FullScoreSettings fs;
                fs.concert_pitch = concert;
                const auto score_file = dir.getChildFile(concert ? "score-in-c.pdf" : "full-score.pdf");
                score_file.deleteFile();
                const int score_pages = export_full_score_pdf(ctx, score_file, fs, message);
                check(score_pages > 0 && score_file.getSize() > 10000,
                      juce::String("the full score prints ") + (concert ? "in C" : "transposed") + " (" + message + ")");
                if (!concert && pdftoppm.existsAsFile()) {
                    juce::ChildProcess p;
                    const auto out = dir.getChildFile("10-full-score");
                    if (p.start(juce::StringArray{pdftoppm.getFullPathName(), "-r", "60", "-png", "-singlefile", "-f",
                                                  "1", score_file.getFullPathName(), out.getFullPathName()})) {
                        p.waitForProcessToFinish(20000);
                        check(out.withFileExtension("png").getSize() > 5000, "a full-score page renders (10-full-score)");
                    }
                }
            }

            const int parts_exported = static_cast<int>(
                interchange::musicxml_export_tracks(session.data()).size());
            for (const char* ext : {"musicxml", "mxl"}) {
                const auto xml_file = dir.getChildFile(juce::String("check.") + ext);
                xml_file.deleteFile();
                const bool exported = export_musicxml_file(ctx, xml_file, false, message);
                check(exported && xml_file.getSize() > 500,
                      juce::String("MusicXML exports as .") + ext + " (" + message + ")");
                std::string xml;
                juce::String why;
                check(read_musicxml_file(xml_file, xml, why) && xml.find("<score-partwise") != std::string::npos,
                      juce::String("the .") + ext + " file reads back");
                session::Session other(ctx.catalogs);
                other.new_empty();
                const auto imported = other.import_musicxml(xml, true);
                std::size_t notes = 0;
                for (const auto& id : imported.track_ids)
                    if (const auto* it = other.data().find_track(id)) notes += it->active_version()->score.total_note_count();
                check(imported.ok && static_cast<int>(imported.track_ids.size()) == parts_exported && notes >= 5,
                      juce::String("the .") + ext + " export imports back: " + juce::String(static_cast<int>(imported.track_ids.size())) +
                          " parts, " + juce::String(static_cast<int>(notes)) + " notes");
            }
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
        automation_checks(ctx, dir, plugin_track.empty() ? std::nullopt : std::optional<project::PluginRef>(plugin_ref));
        delay_compensation_checks(ctx, dir, plugin_track.empty() ? std::nullopt : std::optional<project::PluginRef>(plugin_ref));
        ipc_wait_checks(dir);
        hosting_checks(ctx, dir);
        insert_window_checks(ctx, dir);
        hardware_control_checks(window.main(), dir);
        shell_checks(window, dir);
        selection_and_shortcut_checks(window, dir);
        composer_lane_checks(window, dir);
        loop_and_keyboard_checks(window, dir);

        for (int i = juce::TopLevelWindow::getNumTopLevelWindows(); --i >= 0;)
            if (auto* w = juce::TopLevelWindow::getTopLevelWindow(i); w && w != &window) w->setVisible(false);
        pump(100);
    }
    juce::DeletedAtShutdown::deleteAll();
    std::printf("%s: %d failure(s); screenshots in %s\n", failures ? "FAILED" : "PASSED", failures,
                dir.getFullPathName().toRawUTF8());
    return failures == 0 ? 0 : 1;
}
