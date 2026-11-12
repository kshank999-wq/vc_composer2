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
#include "audio/RemotePlugin.h"
#include "ui/StemExport.h"
#include "vcc/project/Stems.h"
#include "vcc/interchange/MusicXml.h"
#include "ui/Dialogs.h"
#include "ui/VideoWindow.h"
#include "vcc/media/VideoDecoder.h"
#include "vcc/project/Automation.h"
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
}

}  // namespace

int main(int argc, char** argv)
{
    vcc::desktop::prepare_display();
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File dir(argc > 1 ? juce::String(argv[1]) : juce::File::getCurrentWorkingDirectory().getChildFile("screenshots").getFullPathName());
    dir.createDirectory();
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
        hosting_checks(ctx, dir);

        for (int i = juce::TopLevelWindow::getNumTopLevelWindows(); --i >= 0;)
            if (auto* w = juce::TopLevelWindow::getTopLevelWindow(i); w && w != &window) w->setVisible(false);
        pump(100);
    }
    juce::DeletedAtShutdown::deleteAll();
    std::printf("%s: %d failure(s); screenshots in %s\n", failures ? "FAILED" : "PASSED", failures,
                dir.getFullPathName().toRawUTF8());
    return failures == 0 ? 0 : 1;
}
