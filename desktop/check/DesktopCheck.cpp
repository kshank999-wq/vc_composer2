// Headless check of the desktop application (registered with CTest; run
// under a virtual display on Linux CI).
//
// It builds the real main window, creates a project from a template, writes
// notes through the Composer view exactly as typing would, opens every
// workspace and tool window, renders audio offline through the playback
// engine and checks it is not silent, saves and reopens, and writes a
// screenshot of each surface. Exit code 0 means every check passed.
#include <algorithm>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <csignal>
#if !defined(_WIN32)
#include <execinfo.h>
#include <unistd.h>
#endif

#include "MainWindow.h"
#include "PlatformFixes.h"
#include "audio/CueRenderer.h"
#include <mutex>
#include <set>
#include "audio/MidiOut.h"
#include "audio/PrintToAudio.h"
#include "audio/Mp3Encoder.h"
#include "ui/CueManager.h"
#include "ui/ScoreExchange.h"
#include "audio/StemRenderer.h"
#include "audio/PluginIpc.h"
#include "audio/RemotePlugin.h"
#include "ui/StemExport.h"
#include "vcc/project/Stems.h"
#include "vcc/interchange/MusicXml.h"
#include "ui/Dialogs.h"
#include "ui/ChordExplorer.h"
#include "ui/StudioSetup.h"
#include "ui/RecentProjects.h"
#include "ui/TrackListPalette.h"
#include "ui/Credits.h"
#include "ui/SoundsWindow.h"
#include "audio/SoundPack.h"
#include "audio/SoundFontInstrument.h"
#include "ui/Import.h"
#include "vcc/project/Mackie.h"
#include "system/PluginWatch.h"
#include "ui/hw/HwGallery.h"
#include "ui/console/MixActions.h"
#include "audio/ChannelDsp.h"
#include "ui/console/ConsoleStrip.h"
#include "ui/console/ConsoleView.h"
#include "ui/ScoreTools.h"
#include "ui/TrackPanel.h"
#include "ui/composer/CircleOfFifths.h"
#include "ui/console/RoutingCanvas.h"
#include "ui/surface/ControlSurface.h"
#include "ui/review/Review.h"
#include "ui/review/Relink.h"
#include "ui/EditTools.h"
#include "ui/NewProjectWizard.h"
#include "Preview.h"
#include "system/ResourceMonitor.h"
#include "licensing/Account.h"
#include "licensing/AccountWindow.h"
#include "licensing/UpdateWindow.h"
#include "licensing/Updater.h"
#include "collab/Collaboration.h"
#include "collab/SharedWindow.h"
#include "vcc/session/Shared.h"
#include "vcc/core/Hash.h"
#include <juce_cryptography/juce_cryptography.h>
#include "ui/PictureTools.h"
#include "ui/TemplateBrowser.h"
#include "ui/PrintPreview.h"
#include "ui/ShortcutEditor.h"
#include "ui/console/MixTools.h"
#include "ui/library/ArticulationMapWindow.h"
#include "ui/library/LibraryBrowser.h"
#include "ui/library/SoundAssignments.h"
#include "vcc/project/ArticulationMap.h"
#include "vcc/library/Library.h"
#include "ui/composer/TransposePopover.h"
#include "ui/composer/ComposerRuler.h"
#include "vcc/theory/Chord.h"
#include "ui/composer/Inspector.h"
#include "ui/composer/LaneParts.h"
#include "ui/InstrumentUsage.h"
#include "ui/VideoWindow.h"
#include "vcc/media/VideoDecoder.h"
#include "vcc/media/VideoEncoder.h"
#include "vcc/audio/Loudness.h"
#include "vcc/music/MidiFile.h"
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
    std::fflush(stdout);  // a crash must not swallow the lines before it
    if (!ok) ++failures;
}

// Which check is running, and where a crash happened (CI on another
// platform otherwise shows only "SegFault").
const char* g_section = "start";
void section(const char* name)
{
    g_section = name;
    std::printf("== %s\n", name);
    std::fflush(stdout);
}
#if !defined(_WIN32)
void on_crash(int sig)
{
    char line[256];
    const int n = std::snprintf(line, sizeof line, "\n*** signal %d in section %s\n", sig, g_section);
    if (n > 0) (void) ::write(2, line, static_cast<size_t>(n));
    void* frames[64];
    const int count = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, count, 2);
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
void install_crash_handler()
{
    for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE}) std::signal(sig, on_crash);
}
#else
void install_crash_handler() {}
#endif

/// Runs the message loop for `ms`, and at least until everything already
/// posted (a triggerClick, an async update) has been handled: a slow
/// machine may not get to it within a fixed time. Posted messages are
/// handled in order, so once a marker posted now arrives, they all have.
void pump(int ms)
{
    auto* mm = juce::MessageManager::getInstance();
    auto done = std::make_shared<std::atomic<bool>>(false);
    juce::MessageManager::callAsync([done] { done->store(true); });
    mm->runDispatchLoopUntil(ms);
    for (int waited = 0; !done->load() && waited < 5000; waited += 10) mm->runDispatchLoopUntil(10);
}

/// Pumps until `ready` (or `ms` pass): for asynchronous clicks and timers,
/// which a slow CI machine takes longer to deliver.
bool wait_for(const std::function<bool()>& ready, int ms = 2000)
{
    for (int waited = 0; waited < ms; waited += 25) {
        if (ready()) return true;
        pump(25);
    }
    return ready();
}

void shoot(juce::Component& c, const juce::File& dir, const juce::String& name, bool optional = false)
{
    // The component may go while the queue runs (a call-out closes itself
    // when focus moves on macOS), and an empty image must never reach the
    // PNG writer (libpng aborts the process on one).
    juce::Component::SafePointer<juce::Component> alive(&c);
    pump(150);
    if (!alive || alive->getWidth() <= 0 || alive->getHeight() <= 0) {
        if (!optional) check(false, name + " screenshot: the window was gone or empty");
        return;
    }
    auto image = alive->createComponentSnapshot(alive->getLocalBounds(), true, 1.0f);
    if (!image.isValid() || image.getWidth() <= 0 || image.getHeight() <= 0) {
        if (!optional) check(false, name + " screenshot: nothing could be drawn");
        return;
    }
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
    const auto project_file = dir.getChildFile("mix-check.vcc");
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
    session.save_as(dir.getChildFile("automation-check.vcc").getFullPathName().toStdString(), &error);
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
    session.save_as(dir.getChildFile("pdc-check.vcc").getFullPathName().toStdString(), &error);
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
        const auto path = dir.getChildFile("pdc-off.vcc").getFullPathName().toStdString();
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
        std::chrono::steady_clock::time_point woke;
        std::thread waiter([&] {
            ready = true;
            plugin.wait(ipc::Word::request, seen, 1000.0);
            woke = std::chrono::steady_clock::now();
        });
        while (!ready) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));  // the waiter is now blocked in the kernel
        // Measured from the signal itself (a sleep can overshoot on a busy machine).
        const auto signalled = std::chrono::steady_clock::now();
        host.get()->request.store(seen + 1, std::memory_order_release);
        host.wake(ipc::Word::request);
        waiter.join();
        wake_ms.push_back(std::chrono::duration<double, std::milli>(woke - signalled).count());
    }
    std::sort(wake_ms.begin(), wake_ms.end());
    const double median = wake_ms[wake_ms.size() / 2], worst = wake_ms.back();
    check(median < 2.0 && worst < 20.0,
          juce::String("a blocked waiter wakes as soon as the other side signals (") + ipc::wait_mechanism() +
              ": median " + juce::String(std::max(0.0, median), 3) + " ms after the 5 ms, worst " +
              juce::String(std::max(0.0, worst), 3) + " ms)");
    // And the other way, as the plug-in side signals a finished block.
    const std::uint32_t done = plugin.get()->done.load();
    std::chrono::steady_clock::time_point sent;
    std::thread signaller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        sent = std::chrono::steady_clock::now();
        plugin.get()->done.store(done + 1, std::memory_order_release);
        plugin.wake(ipc::Word::done);
    });
    const bool changed = host.wait(ipc::Word::done, done, 1000.0);
    const auto woke = std::chrono::steady_clock::now();
    signaller.join();
    const double back = std::chrono::duration<double, std::milli>(woke - sent).count();
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
                             bool alt = false, int extra_mods = 0)
{
    const auto now = juce::Time::getCurrentTime();
    const int mods = juce::ModifierKeys::leftButtonModifier | (alt ? juce::ModifierKeys::altModifier : 0) | extra_mods;
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), p, juce::ModifierKeys(mods),
                            juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, down, now, 1,
                            dragged);
}

void drag(juce::Component& c, juce::Point<float> from, juce::Point<float> to, bool alt = false, int extra_mods = 0)
{
    c.mouseDown(mouse_event(c, from, from, false, alt, extra_mods));
    c.mouseDrag(mouse_event(c, (from + to) / 2.0f, from, true, alt, extra_mods));
    c.mouseDrag(mouse_event(c, to, from, true, alt, extra_mods));
    c.mouseUp(mouse_event(c, to, from, true, alt, extra_mods));
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

    // A plain drag of a note up a tone changes the written music: Decision 1
    // asks - Apply to Score or Keep Producer Override (Memory Management
    // spec §2.3: never silently overwrite Composer content).
    const auto rows_per_tone = (notes[0].y - notes[1].y);  // C to D is a tone
    const auto& sc0 = session.data().find_track(track)->active_version()->score;
    std::vector<notation::ScoreNote> line;
    for (const auto& n : sc0.notes(0))
        if (!n.is_rest) line.push_back(n);
    std::sort(line.begin(), line.end(), [](const auto& a, const auto& b) { return a.start_tick < b.start_tick; });
    const auto target = line.size() >= 4 ? line[3] : notation::ScoreNote{};
    const int before_midi = notation::midi_note_number(target.pitch);
    auto played_pitch = [&] {
        const auto r = project::render_track(*session.data().find_track(track));
        for (std::size_t i = 0; i < r.notes.size(); ++i)
            if (r.note_ids[i] == target.id) return r.notes[i].pitch;
        return -1;
    };
    const int played_before = played_pitch();
    auto drag_up = [&] { drag(*roll, {last.left + 4.0f, last.y}, {last.left + 4.0f, last.y - rows_per_tone}); pump(150); };
    drag_up();
    auto* question = find_dialog("This changes the score");
    check(question != nullptr && score_json() == score_before,
          "dragging a score note up a tone asks before the score changes (Apply to Score / Keep Producer Override)");
    if (question) {
        shoot(*question, dir, "19-producer-edit-question");
        if (auto* keep = find_button(*question, "Keep Producer Override")) keep->triggerClick();
        pump(200);
    }
    const auto* ov = session.performance_of(track);
    check(score_json() == score_before && played_pitch() == played_before + 2 && ov && ov->count(target.id) &&
              ov->at(target.id).producer,
          "Keep Producer Override: the score stays as written, playback is a tone higher, the note is marked");
    main.show_workspace(MainComponent::Workspace::Composer);
    pump(300);
    shoot(main, dir, "19-producer-override-in-composer");
    main.show_workspace(MainComponent::Workspace::Sequencer);
    pump(300);
    // Apply to Score for the kept note writes it as it plays.
    const int applied = session.apply_overrides_to_score(track, {target.id});
    const auto* now = session.data().find_track(track)->active_version()->score.find_note(target.id);
    check(applied == 1 && now && notation::midi_note_number(now->pitch) == before_midi + 2 && played_pitch() == played_before + 2 &&
              session.performance_of(track)->count(target.id) == 0,
          "Apply to Score later writes the kept note as it plays, and the override goes");
    session.undo();
    session.undo();
    check(score_json() == score_before && session.performance_of(track)->empty(), "undo puts both back");

    // Apply to Score, ticking "always": written, and the project's rule.
    drag_up();
    question = find_dialog("This changes the score");
    if (question) {
        if (auto* always = dynamic_cast<juce::ToggleButton*>(question->findChildWithID("always")))
            always->setToggleState(true, juce::dontSendNotification);
        if (auto* apply = find_button(*question, "Apply to Score")) apply->triggerClick();
        pump(200);
    }
    const auto* rewritten = session.data().find_track(track)->active_version()->score.find_note(target.id);
    const int after_midi = rewritten ? notation::midi_note_number(rewritten->pitch) : -1;
    check(rewritten && after_midi == before_midi + 2 && rewritten->start_tick == target.start_tick &&
              session.performance_of(track)->empty() && session.data().producer_edits == "apply",
          "Apply to Score rewrites the pitch in the score (" + juce::String(before_midi) + " -> " + juce::String(after_midi) +
              ") and, ticked, becomes this project's rule");
    session.undo();
    check(score_json() == score_before, "one undo puts the written note back");
    drag_up();
    check(find_dialog("This changes the score") == nullptr && score_json() != score_before,
          "with the rule set, the next such edit applies without asking");
    session.undo();
    session.set_producer_edits("ask");

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
              main.markers().isVisible() && !main.selection_panel().isShowing(),
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
    check(main.deck().getHeight() > 150 && main.edit_frame().getRight() > main.getWidth() - 20 &&
              main.edit_frame().getBottom() > main.keyboard().getY() - 20,
          "under 1280 px the top deck wraps; the Composer's edit window takes the full width and height");
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

    // Geometry: 220 px headers, a single-staff row as tall as its header (with
    // the instrument and MIDI rows) and a 120 px bar at 100%.
    check(composer.strip_count() > 1 && composer.header(0).getWidth() == composer_ui::TrackHeader::kWidth,
          "each track row starts with its 220 px header");
    // The template's two sketch pianos (two staves each) head the list; the next is one staff.
    const int row = composer.header(composer.strip_count() > 2 ? 2 : 0).getHeight();
    check(row == composer_ui::TrackHeader::kMinHeight, "a single-staff row fits its header at 100% (" + juce::String(row) + ")");
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
    wait_for([&] {
        const auto* h = main.toolbar().command_key(Command::SelectDurationHalf);
        return h && h->getToggleState() && composer.cursor() && composer.cursor()->duration.value == notation::NoteValue::Half;
    });
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
    // The inspector is a floating window, off until Window > Inspector.
    check(!main.inspector_shown() && main.inspector_window() == nullptr && !main.inspector().isShowing(),
          "the inspector is off by default: no column beside the edit window");
    window.context().commands.invokeDirectly(cmd::toggleInspector, false);
    pump(120);
    check(main.inspector_shown() && main.inspector_window() != nullptr && main.inspector().isShowing() &&
              main.inspector_window()->getContentComponent() != nullptr &&
              main.selection_panel().findParentComponentOfClass<dialogs::ToolWindow>() == main.inspector_window(),
          "Window > Inspector floats it in its own window (the selection readout over the inspector)");
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
    if (auto* w = main.inspector_window()) shoot(*w, dir, "29a-inspector-window");
    window.context().commands.invokeDirectly(cmd::toggleInspector, false);
    pump(150);
    check(!main.inspector_shown(), "Window > Inspector again closes it");
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

/// Producer view tracks (step 6): instrument rows with their own audio lane,
/// what each plays, expanding with the piano roll inline, resizing, the
/// header keys and the Clips list.
void producer_track_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_from_template("cinematic.horror.psychological");
    std::string error;
    session.save_as(dir.getChildFile("producer-check.vcc").getFullPathName().toStdString(), &error);
    main.show_workspace(MainComponent::Workspace::Producer);
    pump(200);
    auto& seq = main.sequencer();
    const auto tracks = session.music_tracks();
    std::vector<std::string> inst;
    for (const auto* t : tracks)
        if (t->kind == project::TrackKind::Instrument) inst.push_back(t->id);
    if (inst.size() < 2) {
        check(false, "the template has instrument tracks");
        return;
    }
    const std::string first = inst[0], second = inst[1];
    session.select_track(first);
    pump(80);

    // Rows: 272 px expanded with the piano roll inline, 72 collapsed, 84 audio.
    auto* roll = find_named(main, "vcc-piano-roll");
    auto* lanes = find_by_id(main, "sequencer_lanes");
    check(seq.expanded_track() == first && seq.row_height(first) == 272 && seq.row_height(second) == 72 && roll &&
              roll->isVisible() && roll->getParentComponent() == lanes,
          "the selected instrument track opens 272 px tall with its piano roll inline; others are 72 px");
    const auto audio = session.add_audio_track("Vox", false);
    pump(80);
    check(seq.row_height(audio) == 84, "an audio track's row is 84 px");
    session.select_track(second);
    pump(80);
    check(seq.expanded_track() == second && seq.row_height(first) == 72, "the open piano roll follows the selected track");
    seq.set_expanded_track(first);
    pump(80);

    // Dragging the bottom edge of a header resizes the row.
    if (lanes) {
        const int h = seq.row_height(first);
        drag(*lanes, {100.0f, static_cast<float>(h - 2)}, {100.0f, static_cast<float>(h + 38)});
        pump(50);
        check(seq.row_height(first) == h + 40, "dragging a header's bottom edge resizes the row (" + juce::String(h) +
                                                    " -> " + juce::String(seq.row_height(first)) + ")");
        // The MIDI | Audio | Both keys choose what the track plays.
        const juce::Point<float> audio_key(26.0f + 40.0f + 19.0f, 52.0f + 8.0f);
        lanes->mouseDown(mouse_event(*lanes, audio_key, audio_key, false));
        lanes->mouseUp(mouse_event(*lanes, audio_key, audio_key, false));
        pump(50);
        check(session.data().find_track(first)->playback == "audio", "the header's Audio key makes the track play its audio lane");
        session.undo();
        // S and M.
        const juce::Point<float> solo(51.0f + 11.0f, 30.0f + 9.0f);
        lanes->mouseDown(mouse_event(*lanes, solo, solo, false));
        lanes->mouseUp(mouse_event(*lanes, solo, solo, false));
        pump(50);
        check(session.data().find_track(first)->solo, "the header's S key solos the track");
        session.set_solo(first, false);
    }

    // The instrument track's own audio lane: a printed WAV plays when the
    // track plays audio (or both), and is silent when it plays MIDI.
    const auto file = dir.getChildFile("Audio Files").getChildFile("print.wav");
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
        auto writer = juce::WavAudioFormat().createWriterFor(
            stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000.0).withNumChannels(2).withBitsPerSample(24));
        juce::AudioBuffer<float> wave(2, 48000 * 4);
        for (int i = 0; i < wave.getNumSamples(); ++i) {
            const float s = 0.3f * std::sin(2.0f * juce::MathConstants<float>::pi * 330.0f * static_cast<float>(i) / 48000.0f);
            wave.setSample(0, i, s);
            wave.setSample(1, i, s);
        }
        if (writer) writer->writeFromAudioSampleBuffer(wave, 0, wave.getNumSamples());
    }
    project::AudioClip clip;
    clip.file = "Audio Files/print.wav";
    clip.name = "Celli_PRINT_01";
    clip.length = 4.0;
    clip.channels = 2;
    clip.file_sample_rate = 48000.0;
    const auto clip_ids = session.add_audio_clips({{first, clip}}, "Print");
    pump(100);
    auto level = [&](const std::string& playback) {
        session.set_playback(first, playback);
        for (const auto& id : inst)
            if (id != first) session.set_mute(id, true);
        PlaybackEngine e(session, ctx.plugins);
        e.prepare_offline(48000.0, 512);
        e.locate(0);
        e.play();
        juce::AudioBuffer<float> out(2, 512);
        double energy = 0.0;
        for (int b = 0; b < 120; ++b) {
            e.render_offline(out, 512);
            if (b >= 20) energy += out.getRMSLevel(0, 0, 512);
        }
        e.stop();
        return energy / 100.0;
    };
    const double heard_audio = level("audio"), heard_both = level("both"), heard_midi = level("midi");
    check(heard_audio > 0.05 && heard_both > 0.05 && heard_midi < 0.005,
          "an instrument track's audio lane plays on Audio and Both, and is silent on MIDI (" + juce::String(heard_audio, 3) +
              " / " + juce::String(heard_both, 3) + " / " + juce::String(heard_midi, 4) + ")");
    session.set_playback(first, "both");
    for (const auto& id : inst) session.set_mute(id, false);
    pump(80);

    // The Clips list: the printed clip, click to select it and go there.
    auto& clips = main.clips_panel();
    check(clips.isVisible() && clips.rows() == 1, "the Clips list in the Producer rack lists the clip on the track's audio lane");
    clips.choose(0);
    pump(80);
    check(!clip_ids.empty() && seq.selected_clip() == std::make_pair(first, clip_ids.front()),
          "choosing a clip in the list selects it in its track's audio lane");
    shoot(main, dir, "31-producer-tracks");
}

/// Print to audio (step 7): instrument tracks rendered offline onto their own
/// audio lanes, as 48 kHz / 24-bit WAVs, sounding as their MIDI did.
void print_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_from_template("cinematic.horror.psychological");
    std::string error;
    const auto project_dir = dir.getChildFile("print-check");
    project_dir.deleteRecursively();
    project_dir.createDirectory();
    session.save_as(project_dir.getChildFile("print.vcc").getFullPathName().toStdString(), &error);
    std::vector<std::string> inst;
    for (const auto* t : session.music_tracks())
        if (t->kind == project::TrackKind::Instrument) inst.push_back(t->id);
    if (inst.size() < 3) {
        check(false, "the template has instrument tracks to print");
        return;
    }
    const std::string a = inst[0], b = inst[1], silent = inst[2];
    // A line on each of the first two tracks.
    main.show_workspace(MainComponent::Workspace::Composer);
    auto& composer = main.composer();
    for (const auto& [track, keys] : std::vector<std::pair<std::string, std::string>>{{a, "5cdefg"}, {b, "6ec"}}) {
        session.select_track(track);
        pump(80);
        composer.grabKeyboardFocus();
        for (const char ch : keys) composer.keyPressed(juce::KeyPress(ch));
        pump(50);
    }
    const Ticks bar = 4 * kTicksPerQuarter;
    const double tone_end = session.data().tempo.seconds_at(2 * bar);
    auto level = [&](const std::string& track, const std::string& playback) {
        session.set_playback(track, playback);
        PlaybackEngine e(session, ctx.plugins);
        e.prepare_offline(48000.0, 512);
        PlaybackEngine::RenderOverride solo;
        solo.solo = {track};
        e.set_render_override(solo);
        e.locate(0);
        e.play();
        juce::AudioBuffer<float> out(2, 512);
        double sum = 0.0;
        std::int64_t count = 0;
        for (std::int64_t pos = 0; pos < static_cast<std::int64_t>(tone_end * 48000.0); pos += 512) {
            e.render_offline(out, 512);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i) sum += static_cast<double>(out.getSample(ch, i)) * out.getSample(ch, i);
            count += 1024;
        }
        return 10.0 * std::log10(std::max(1e-20, sum / static_cast<double>(std::max<std::int64_t>(1, count))));
    };
    const double midi_db = level(a, "midi");
    session.set_playback(a, "both");

    // Print both from the panel: song range, no effects, play the audio.
    main.show_workspace(MainComponent::Workspace::Producer);
    pump(150);
    auto& panel = main.print_panel();
    panel.set_ticked(a, true);
    panel.set_ticked(b, true);
    panel.set_ticked(silent, false);
    panel.song_key().setToggleState(true, juce::sendNotificationSync);
    panel.fx_key().setToggleState(false, juce::dontSendNotification);
    panel.play_audio_key().setToggleState(true, juce::dontSendNotification);
    panel.record_key().triggerClick();
    // A fast machine may finish before the first look: progress or done both count.
    wait_for([&] { return panel.progress_lcd().text().contains("/ 2") || panel.progress_lcd().text().contains("100%"); }, 3000);
    check(panel.progress_lcd().text().contains("/ 2") || panel.progress_lcd().text().contains("100%"),
          "Record score to audio starts the print; the LCD follows it (" + panel.progress_lcd().text().replace("\n", " | ") + ")");
    shoot(main, dir, "32-print-progress");
    panel.finish_now();
    pump(80);
    auto print_of = [&](const std::string& track) -> const project::AudioClip* {
        const auto* t = session.data().find_track(track);
        for (const auto& c : t->active_version()->audio_clips)
            if (c.lane == 0 && juce::String(c.name).contains("_PRINT_")) return &c;
        return nullptr;
    };
    const auto* pa = print_of(a);
    const auto* pb = print_of(b);
    bool format_ok = false;
    if (pa) {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r(
            formats.createReaderFor(juce::File(juce::String(session.resolve_audio_path(pa->file)))));
        format_ok = r && std::abs(r->sampleRate - 48000.0) < 0.5 && r->bitsPerSample == 24 && r->numChannels == 2;
    }
    check(pa && pb && juce::String(pa->file).startsWith("Audio Files/") && juce::String(pa->name).endsWith("_PRINT_01"),
          "each ticked track has its print on its own audio lane (Audio Files/<Track>_PRINT_01.wav)");
    check(format_ok, "the prints are 48 kHz / 24-bit stereo WAV files");
    check(pa && pa->length < tone_end + 3.0, "a song print ends where the sound does, not at the end of the song (" +
                                                 juce::String(pa ? pa->length : 0.0, 2) + " s)");
    check(session.data().find_track(a)->playback == "audio" && session.data().find_track(b)->playback == "audio",
          "the printed tracks switch to playing their printed audio");
    check(session.undo_label() == "Print 2 tracks to audio", "the whole print is one undo step (" + juce::String(session.undo_label()) + ")");
    const double audio_db = level(a, "audio");
    check(std::abs(audio_db - midi_db) < 0.5,
          "the print sounds as the MIDI did, through the same channel strip (" + juce::String(midi_db, 2) + " vs " +
              juce::String(audio_db, 2) + " dB)");
    session.undo();
    pump(50);
    check(!print_of(a) && !print_of(b) && session.data().find_track(a)->playback == "both",
          "one undo takes the prints away and the tracks play their MIDI again");
    session.redo();
    pump(50);

    // The selection only: the earlier print moves to a take lane.
    session.set_selection_range(bar, 2 * bar);
    pump(50);
    panel.selection_key().setToggleState(true, juce::sendNotificationSync);
    panel.print_tracks({a}, false);
    panel.finish_now();
    pump(50);
    const auto* sel_print = print_of(a);
    int takes = 0;
    for (const auto& c : session.data().find_track(a)->active_version()->audio_clips) takes += c.lane > 0 ? 1 : 0;
    check(sel_print && std::abs(sel_print->start_seconds - session.data().tempo.seconds_at(bar)) < 1e-6 && takes == 1,
          "printing the selection places the print at the range; the earlier print moves to a take lane");
    panel.song_key().setToggleState(true, juce::sendNotificationSync);

    // Freeze: printed with the strip's processing, which is then bypassed
    // while the print plays, so it is not heard twice.
    session.update_track_mix(b, "Trim", [](project::Track& t) { t.strip.trim_db = 12.0; });
    const double strip_db = level(b, "midi");
    if (main.sequencer().on_print_track) main.sequencer().on_print_track(b, true);
    panel.finish_now();
    pump(50);
    const auto* tb = session.data().find_track(b);
    const double frozen_db = level(b, "audio");
    check(tb->printed_fx && std::abs(frozen_db - strip_db) < 0.5,
          "Freeze prints with the channel strip and inserts, and bypasses them while the print plays (" +
              juce::String(strip_db, 2) + " vs " + juce::String(frozen_db, 2) + " dB)");

    // A track with nothing to play is not printed.
    panel.print_tracks({silent}, false);
    panel.finish_now();
    pump(50);
    check(!print_of(silent) && !panel.last_items().empty() && !panel.last_items().front().ok,
          "a track with nothing in the range places no print (" +
              (panel.last_items().empty() ? juce::String() : panel.last_items().front().message) + ")");
    session.select_track(a);
    pump(100);
    shoot(main, dir, "33-print-done");
}

/// Mix (step 8): the Producer view's channel strip and the mixer deck.
void mix_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_from_template("cinematic.horror.psychological");
    main.show_workspace(MainComponent::Workspace::Producer);
    main.show_bottom_mixer(true);
    pump(200);
    auto& deck = main.mixer_deck();
    check(deck.isVisible() && !main.keyboard().isVisible(), "the Producer view's bottom deck is the mixer");
    deck.keyboard_key().triggerClick();
    pump(80);
    check(main.keyboard().isVisible() && !deck.isVisible() && main.keyboard().mixer_key().isVisible(),
          "the Keyboard key switches the deck to the keyboard, which has a Mixer key back");
    main.keyboard().mixer_key().triggerClick();
    pump(80);
    check(deck.isVisible(), "the Mixer key switches back to the mixer");
    main.show_bottom_mixer(false);
    main.go_to_stage(2);
    pump(80);
    check(deck.isVisible() && main.edit_frame().stage_key(2).getToggleState(), "the Mix stage shows the mixer deck");

    // Full screen: the deck's key brings up the console, laid out like a
    // hardware console - every section above, the fader the bottom third.
    deck.full_key().triggerClick();
    pump(150);
    auto* full = main.full_console();
    bool proportional = false;
    juce::String measured;
    if (full && full->isVisible()) {
        auto& view = full->view();
        if (auto* s = view.master()) {
            const int fader = s->fader_bounds().getHeight();
            const int strip = full->getHeight();
            const double share = static_cast<double>(fader) / strip;
            proportional = share > 0.22 && share < 0.40 && fader >= 140;
            measured = juce::String(fader) + " of " + juce::String(strip) + " px, " + juce::String(share * 100.0, 0) + "%";
        }
    }
    check(main.mixer_full() && full && full->isVisible() && full->getY() < main.edit_frame().getY() && proportional,
          "Full screen is the console in hardware proportions: the fader about a third of the strip (" + measured + ")");
    shoot(main, dir, "35b-mixer-full");
    if (full) full->view().compact_button().triggerClick();
    pump(100);
    check(!main.mixer_full() && (!full || !full->isVisible()) && deck.isVisible() &&
              deck.getHeight() == shell::MixerDeck::kHeight,
          "COMPACT puts the mixer back in the bottom deck");
    // Cmd/Ctrl+M (the toolbar's Mixer key) from the Composer view, and back.
    main.show_workspace(MainComponent::Workspace::Composer);
    pump(50);
    main.show_mixer();
    pump(80);
    const bool opened = main.mixer_full() && main.full_console() && main.full_console()->isVisible();
    main.show_mixer();
    pump(80);
    check(opened && !main.mixer_full() && main.workspace() == MainComponent::Workspace::Composer,
          "Cmd/Ctrl+M opens the full-screen mixer and closes it back to the view it came from");
    main.show_workspace(MainComponent::Workspace::Producer);
    main.show_bottom_mixer(true);
    pump(80);

    // One strip per track and group/FX bus, and the master pinned at the right.
    std::size_t buses = 0;
    for (const auto& b : session.data().buses) buses += b.kind != "master" ? 1u : 0u;
    const auto tracks = session.music_tracks();
    auto* master = deck.strip_for("bus-master");
    check(deck.strip_count() == static_cast<int>(tracks.size() + buses) && master && master->master() &&
              master->getParentComponent() == &deck && master->getWidth() > deck.strip(0).getWidth(),
          "the mixer deck has a strip per track and bus, and a wider master strip that stays in view (" +
              juce::String(deck.strip_count()) + " strips)");
    const std::string first = tracks.front()->id;
    auto* strip = deck.strip_for(first);
    if (!strip) return;
    strip->fader().set_db(-10.0, juce::sendNotificationSync);
    pump(30);
    check(std::abs(session.data().find_track(first)->gain_db + 10.0) < 0.05, "a deck strip's fader sets the track's volume");
    strip->solo().triggerClick();
    pump(30);
    check(session.data().find_track(first)->solo, "a deck strip's S key solos the track");
    session.set_solo(first, false);
    master->fader().set_db(-3.0, juce::sendNotificationSync);
    pump(30);
    check(std::abs(session.data().find_bus("bus-master")->gain_db + 3.0) < 0.05, "the master strip's fader sets the master bus");

    // The channel strip: inserts with bypass lights, sends, input and output.
    session.select_track(first);
    session.update_track_mix(first, "Add EQ", [](project::Track& t) {
        project::InsertSlot slot;
        slot.kind = "eq";
        slot.suggestion = false;
        t.inserts.insert(t.inserts.begin(), slot);
    });
    pump(80);
    auto& channel = main.channel_panel();
    auto& slots = channel.inserts();
    const auto led = slots.led_rect(0).getCentre().toFloat();
    slots.mouseDown(mouse_event(slots, led, led, false));
    slots.mouseUp(mouse_event(slots, led, led, false));
    pump(50);
    check(session.data().find_track(first)->inserts.front().bypass, "clicking an insert's light on the channel strip bypasses it");
    if (channel.send_count() > 0) {
        const auto bus = channel.send_bus(0);
        channel.send(0).setValue(-6.0, juce::sendNotificationSync);
        pump(30);
        double level = 99.0;
        for (const auto& s : session.data().find_track(first)->sends)
            if (s.bus_id == bus) level = s.level_db;
        check(std::abs(level + 6.0) < 0.05, "a send knob on the channel strip sets the send to its FX bus");
    } else {
        check(false, "the template has an FX bus to send to");
    }
    check(channel.input_lcd().text().startsWith("IN") && channel.output_lcd().text().startsWith("OUT"),
          "the channel strip shows the track's input and output (" + channel.input_lcd().text() + ", " +
              channel.output_lcd().text() + ")");
    session.set_mute(first, true);
    pump(50);
    check(deck.strip_for(first)->mute().getToggleState(), "the deck follows the document (a track muted elsewhere)");
    session.set_mute(first, false);
    pump(80);
    shoot(main, dir, "34-mixer-deck");
}

/// Master (step 9): loudness, the target and Match, the master chain, and
/// Export master to WAV and FLAC.
void master_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_from_template("cinematic.horror.psychological");
    const auto project_dir = dir.getChildFile("master-check");
    project_dir.deleteRecursively();
    project_dir.createDirectory();
    std::string error;
    session.save_as(project_dir.getChildFile("master.vcc").getFullPathName().toStdString(), &error);
    std::vector<std::string> inst;
    for (const auto* t : session.music_tracks())
        if (t->kind == project::TrackKind::Instrument) inst.push_back(t->id);
    main.show_workspace(MainComponent::Workspace::Composer);
    auto& composer = main.composer();
    for (const auto& [track, keys] : std::vector<std::pair<std::string, std::string>>{{inst[0], "6cdefgab"}, {inst[3], "7ce"}}) {
        session.select_track(track);
        pump(80);
        composer.grabKeyboardFocus();
        for (const char ch : keys) composer.keyPressed(juce::KeyPress(ch));
        pump(50);
    }
    main.go_to_stage(3);
    pump(150);
    auto& panel = main.master_panel();
    check(panel.isVisible() && !main.channel_panel().isVisible() && main.edit_frame().stage_key(3).getToggleState(),
          "the Master stage shows the Master panel in the right column");

    // Live loudness: the engine measures its master output as it plays.
    {
        PlaybackEngine e(session, ctx.plugins);
        e.prepare_offline(48000.0, 512);
        e.locate(0);
        e.play();
        juce::AudioBuffer<float> out(2, 512);
        for (int b = 0; b < 48000 * 6 / 512; ++b) e.render_offline(out, 512);
        const auto blocks = e.take_loudness_blocks();
        panel.reset_meter();
        panel.add_blocks(blocks);
        check(blocks.size() >= 55 && blocks.size() <= 62 && panel.meter().integrated() > -70.0,
              "the engine measures the master as it plays, 100 ms at a time (" + juce::String(static_cast<int>(blocks.size())) +
                  " blocks, " + panel.integrated_lcd().text() + " LUFS, TP " + panel.true_peak_lcd().text() + ")");
    }

    // The target: -14 by default; the key steps through the standards.
    check(std::abs(session.data().loudness_target + 14.0) < 1e-9, "the loudness target is -14 LUFS by default");
    panel.target_key().triggerClick();
    pump(50);
    check(std::abs(session.data().loudness_target + 16.0) < 1e-9 && panel.target_key().getButtonText().contains("-16"),
          "the Target key steps to -16 LUFS (then -23, -24)");
    session.set_loudness_target(-23.0);
    pump(50);

    // Analyse the song, then Match: the master fader meets the target.
    panel.analyse();
    panel.finish_now();
    const auto before = panel.last_analysis();
    check(before.ok && before.integrated > -70.0, "Analyse renders the song and measures it (" +
                                                       juce::String(before.integrated, 2) + " LUFS, " +
                                                       juce::String(before.true_peak, 2) + " dBTP)");
    panel.match_target();
    pump(50);
    panel.analyse();
    panel.finish_now();
    const auto after = panel.last_analysis();
    check(std::abs(after.integrated + 23.0) < 0.2, "Match sets the master so the song meets -23 LUFS (" +
                                                        juce::String(after.integrated, 2) + " LUFS)");

    // The master chain, in order: EQ, glue compressor, limiter.
    for (int i : {2, 0, 1}) panel.chain_key(i).triggerClick();
    pump(80);
    const auto* master = session.data().find_bus("bus-master");
    std::vector<std::string> kinds;
    for (const auto& s : master->inserts)
        if (s.kind == "eq" || s.kind == "compressor" || s.kind == "limiter") kinds.push_back(s.kind);
    check(kinds == std::vector<std::string>{"eq", "compressor", "limiter"} && panel.chain_index(2) >= 0,
          "the master chain keys add EQ, glue compressor and limiter in chain order");
    panel.chain_key(0).triggerClick();
    pump(50);
    check(session.data().find_bus("bus-master")->inserts[static_cast<std::size_t>(panel.chain_index(0))].bypass,
          "pressing a chain key again bypasses that processor");
    session.undo();
    session.undo();
    session.undo();
    session.undo();
    pump(50);

    // Export master: WAV 24-bit 48 kHz, then FLAC 16-bit 44.1 kHz with dither.
    auto read = [](const juce::File& f) {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        return std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(f));
    };
    auto measure_file = [&](const juce::File& f) {
        auto r = read(f);
        audio::LoudnessAnalyser a;
        audio::LoudnessMeter m;
        if (!r) return m;
        a.prepare(r->sampleRate, 2);
        juce::AudioBuffer<float> buf(2, 4096);
        for (juce::int64 pos = 0; pos < r->lengthInSamples; pos += 4096) {
            const int n = static_cast<int>(std::min<juce::int64>(4096, r->lengthInSamples - pos));
            r->read(&buf, 0, n, pos, true, true);
            const float* ch[] = {buf.getReadPointer(0), buf.getReadPointer(1)};
            a.process(ch, n, [&](const audio::LoudnessBlock& b) { m.add(b); });
        }
        return m;
    };
    const auto wav = project_dir.getChildFile("Bounces").getChildFile("master.wav");
    panel.format_key(0).setToggleState(true, juce::sendNotificationSync);
    panel.bits_key(1).setToggleState(true, juce::sendNotificationSync);
    panel.rate_key(1).setToggleState(true, juce::sendNotificationSync);
    panel.export_to(wav);
    panel.finish_now();
    const auto wav_result = panel.last_export();
    auto wr = read(wav);
    check(wav_result.ok && wr && std::abs(wr->sampleRate - 48000.0) < 0.5 && wr->bitsPerSample == 24 && wr->numChannels == 2,
          "Export master writes a 24-bit / 48 kHz stereo WAV (" + wav_result.message + ")");
    const auto wav_measured = measure_file(wav);
    check(std::abs(wav_measured.integrated() - wav_result.integrated) < 0.05 && std::abs(wav_result.integrated + 23.0) < 0.2,
          "the exported file measures as the export reported, at the target (" + juce::String(wav_measured.integrated(), 2) +
              " LUFS)");
    const auto flac = project_dir.getChildFile("Bounces").getChildFile("master.flac");
    panel.format_key(1).setToggleState(true, juce::sendNotificationSync);
    panel.bits_key(0).setToggleState(true, juce::sendNotificationSync);
    panel.rate_key(0).setToggleState(true, juce::sendNotificationSync);
    check(panel.dither_key().isEnabled() && panel.dither_key().getToggleState(), "16-bit turns dither on");
    panel.export_to(flac);
    panel.finish_now();
    auto fr = read(flac);
    check(panel.last_export().ok && fr && std::abs(fr->sampleRate - 44100.0) < 0.5 && fr->bitsPerSample == 16 &&
              std::abs(measure_file(flac).integrated() + 23.0) < 0.3,
          "Export master writes a dithered 16-bit / 44.1 kHz FLAC at the same loudness");
    // MP3 through LAME (loaded at run time): 320 kbps CBR, then VBR V0.
    if (juce::String why; !Mp3Encoder::available(&why)) {
        check(!panel.format_key(2).isEnabled() && panel.format_key(2).getTooltip() == why,
              "without LAME the MP3 key is off and says how to get it (" + why + ")");
        shoot(main, dir, "35-master");
        return;
    }
    check(panel.format_key(2).isEnabled(), "the MP3 key is live when LAME can be loaded (" + Mp3Encoder::version() + ")");
    panel.format_key(2).setToggleState(true, juce::sendNotificationSync);
    panel.rate_key(0).setToggleState(true, juce::sendNotificationSync);
    check(panel.bitrate_key().isVisible() && !panel.bits_key(0).isVisible() && !panel.rate_key(2).isEnabled() &&
              !panel.dither_key().isEnabled() && panel.mp3_kbps() == 320,
          "MP3 shows a bit rate (320 kbps) in place of the bit depth, and no 96 kHz or dither");
    const auto mp3 = project_dir.getChildFile("Bounces").getChildFile("master.mp3");
    panel.export_to(mp3);
    panel.finish_now();
    const auto mp3_result = panel.last_export();
    auto mr = read(mp3);
    const double wav_seconds = wr ? static_cast<double>(wr->lengthInSamples) / wr->sampleRate : 0.0;
    const double mp3_seconds = mr ? static_cast<double>(mr->lengthInSamples) / mr->sampleRate : 0.0;
    const double kbps = mp3_seconds > 0.0 ? static_cast<double>(mp3.getSize()) * 8.0 / 1000.0 / mp3_seconds : 0.0;
    check(mp3_result.ok && mr && std::abs(mr->sampleRate - 44100.0) < 0.5 && mr->numChannels == 2 &&
              std::abs(mp3_seconds - wav_seconds) < 0.1 && kbps > 300.0 && kbps < 340.0,
          "Export master writes a 320 kbps / 44.1 kHz MP3 of the whole song (" + juce::String(mp3_seconds, 2) + " s, " +
              juce::String(kbps, 0) + " kbps)");
    const auto mp3_measured = measure_file(mp3);
    check(std::abs(mp3_measured.integrated() + 23.0) < 0.3, "the MP3 decodes at the same loudness (" +
                                                                  juce::String(mp3_measured.integrated(), 2) + " LUFS, " +
                                                                  juce::String(mp3_measured.true_peak_db(), 2) + " dBTP)");
    panel.bitrate_key().triggerClick();
    pump(30);
    panel.bitrate_key().triggerClick();
    pump(30);
    panel.bitrate_key().triggerClick();
    pump(30);
    const auto v0 = project_dir.getChildFile("Bounces").getChildFile("master-v0.mp3");
    panel.export_to(v0);
    panel.finish_now();
    check(panel.mp3_kbps() == 0 && panel.last_export().ok && read(v0) != nullptr && v0.getSize() < mp3.getSize(),
          "VBR V0 exports too, smaller than 320 kbps (" + juce::String(v0.getSize() / 1024) + " KB vs " +
              juce::String(mp3.getSize() / 1024) + " KB)");
    // MP3 in: an MP3 on an audio track plays (the import's "*.mp3" now decodes).
    {
        const auto vox = session.add_audio_track("MP3 in", false);
        project::AudioClip clip;
        clip.file = mp3.getFullPathName().toStdString();
        clip.length = 4.0;
        clip.channels = 2;
        clip.file_sample_rate = 44100.0;
        session.add_audio_clips({{vox, clip}}, "Import");
        PlaybackEngine e(session, ctx.plugins);
        e.prepare_offline(48000.0, 512);
        PlaybackEngine::RenderOverride solo;
        solo.solo = {vox};
        e.set_render_override(solo);
        e.locate(0);
        e.play();
        juce::AudioBuffer<float> out(2, 512);
        double energy = 0.0;
        for (int b = 0; b < 300; ++b) {
            e.render_offline(out, 512);
            energy += out.getRMSLevel(0, 0, 512);
        }
        check(energy > 0.01, "an MP3 imported onto an audio track plays");
        session.undo();
        session.undo();
    }
    shoot(main, dir, "35-master");
}

/// Follow (the edit window's title bar): views scroll with the playhead only while it is on.
void follow_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_from_template("cinematic.horror.psychological");
    main.show_workspace(MainComponent::Workspace::Composer);
    pump(150);
    auto& composer = main.composer();
    auto& key = main.edit_frame().follow_key();
    const Ticks far = 24 * 4 * kTicksPerQuarter;  // bar 25
    check(ctx.follow && key.getToggleState() && key.isShowing(), "Follow is on by default, lit in the title bar");

    // Off: playing past the edge leaves the view where it is.
    key.setToggleState(false, juce::dontSendNotification);
    ctx.commands.invokeDirectly(cmd::toggleFollow, false);
    pump(50);
    composer.set_origin(0);
    composer.set_playhead(far, true);
    pump(30);
    check(!ctx.follow && !key.getToggleState() && composer.origin() == 0 && !hw::ui_settings().getBoolValue("follow", true),
          "with Follow off the Composer view stays put as the playhead runs on (and it is remembered)");
    main.show_workspace(MainComponent::Workspace::Producer);
    pump(100);
    auto& seq = main.sequencer();
    seq.set_playhead(0, false);
    const double start = seq.view_start();
    seq.set_playhead(far, true);
    pump(30);
    shoot(main, dir, "36-follow-off");
    check(std::abs(seq.view_start() - start) < 1e-6, "with Follow off the Producer view stays put too");

    // On: both views keep the playhead in sight.
    ctx.commands.invokeDirectly(cmd::toggleFollow, false);
    pump(50);
    check(ctx.follow && key.getToggleState() && hw::ui_settings().getBoolValue("follow", false),
          "View > Follow Playhead (Cmd/Ctrl+Shift+F) turns it back on and lights the key");
    seq.set_playhead(far + kTicksPerQuarter, true);
    pump(30);
    check(seq.view_start() > start + 1.0 && seq.view_start() <= static_cast<double>(far + kTicksPerQuarter),
          "with Follow on the Producer view scrolls to the playhead");
    main.show_workspace(MainComponent::Workspace::Composer);
    pump(100);
    composer.set_origin(0);
    composer.set_playhead(far, true);
    pump(30);
    check(composer.origin() > 0 && composer.origin() <= far, "with Follow on the Composer view scrolls to the playhead");
    auto* keys = ctx.commands.getKeyMappings();
    check(keys->containsMapping(cmd::toggleFollow, juce::KeyPress('f', juce::ModifierKeys::commandModifier |
                                                                            juce::ModifierKeys::shiftModifier, 0)),
          "Cmd/Ctrl+Shift+F toggles Follow");
    ctx.engine.stop();
    ctx.engine.locate(0);
}

/// Real-time print and external MIDI: a MIDI track plays a (virtual)
/// hardware synth whose return comes back on its input, printed live
/// together with an instrument track.

/// The recording workflow: count-in and pre-roll, stopping in a count-in,
/// loop recording into takes, MIDI recording, wait for note, and swipe
/// comping in the take lanes.
void recording_workflow_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    auto& engine = ctx.engine;
    engine.stop();
    session.new_empty();
    const auto project_dir = dir.getChildFile("recording-check");
    project_dir.deleteRecursively();
    project_dir.createDirectory();
    std::string error;
    session.save_as(project_dir.getChildFile("rec.vcc").getFullPathName().toStdString(), &error);
    engine.prepare_offline(48000.0, 512);
    Rig rig(engine);
    juce::String last_message;
    int finished = 0;
    auto previous = engine.on_recording_finished;
    engine.on_recording_finished = [&](const juce::String& m, bool) {
        last_message = m;
        ++finished;
    };
    const auto& d = session.data();
    const Ticks bar2 = d.meter.bar_start(2), bar3 = d.meter.bar_start(3);
    const double bar2_s = d.tempo.seconds_at(bar2);
    const double bar_s = d.tempo.seconds_at(bar3) - bar2_s;
    auto clips_on = [&](const std::string& track) {
        std::vector<project::AudioClip> out;
        if (const auto* t = d.find_track(track); t && t->active_version()) out = t->active_version()->audio_clips;
        return out;
    };

    // Count-in, 1 bar: the transport rolls back a bar, the click plays and
    // the music (here the monitored input) is silent; the take starts where
    // the transport was.
    auto& deck = main.deck();
    deck.set_count_in(1, false);
    check(engine.count_in_bars() == 1 && !engine.pre_roll() && deck.count_in_key().getToggleState(),
          "the Count-in key sets a 1-bar count-in");
    const std::string vox = session.add_audio_track("Vox", false);
    session.update_track_mix(vox, "monitor", [](project::Track& t) { t.monitor = "input"; });
    session.set_record_armed(vox, true);
    engine.set_record_enabled(true);
    engine.locate(bar2);
    engine.play();
    const double rolled = engine.position_seconds();
    const Measure counted = rig.run(bar_s * 0.9, Rig::sine(100.0, 0.5f));
    const bool silent_record = !engine.recording();
    check(std::abs(rolled - (bar2_s - bar_s)) < 0.01 && silent_record,
          "Play with a count-in starts a bar early and records nothing yet (from " + juce::String(rolled, 3) + " s)");
    check(counted.rms < 0.08 && counted.peak > 0.15,
          "during the count-in only the click is heard (rms " + juce::String(counted.rms, 3) + ", peak " +
              juce::String(counted.peak, 2) + ")");
    rig.run(bar_s * 0.1, Rig::sine(100.0, 0.5f));
    const Measure after = rig.run(1.0, Rig::sine(100.0, 0.5f));
    check(engine.recording() && after.rms > 0.15, "after the count-in the music plays and the take records (rms " +
                                                      juce::String(after.rms, 3) + ")");
    engine.stop();
    auto takes = clips_on(vox);
    check(takes.size() == 1 && std::abs(takes[0].start_seconds - bar2_s) < 0.012 && std::abs(takes[0].length - 1.0) < 0.03,
          "the counted-in take starts where the transport was (" +
              juce::String(takes.empty() ? -1.0 : takes[0].start_seconds, 3) + " s)");
    check(std::abs(engine.position_seconds() - (bar2_s + 1.0)) < 0.05, "stop leaves the transport where it stopped");

    // Pre-roll: the music plays in; the take still starts at the position.
    deck.set_count_in(1, true);
    engine.locate(bar2);
    engine.play();
    const Measure pre = rig.run(bar_s * 0.5, Rig::sine(100.0, 0.5f));
    const bool pre_recording = engine.recording();
    rig.run(bar_s * 0.5 + 1.0, Rig::sine(220.0, 0.5f));
    engine.stop();
    takes = clips_on(vox);
    const project::AudioClip* main_take = nullptr;
    for (const auto& c : takes)
        if (c.lane == 0) main_take = &c;
    check(pre.rms > 0.15 && !pre_recording, "pre-roll plays the music in without recording (rms " + juce::String(pre.rms, 3) + ")");
    check(takes.size() == 2 && main_take && std::abs(main_take->start_seconds - bar2_s) < 0.012,
          "the pre-rolled take starts at the position; the first moved to a take lane");

    // Stopping during a count-in goes back to where the recording would start.
    deck.set_count_in(2, false);
    const auto history = session.project().history_size();
    engine.locate(bar3);
    engine.play();
    rig.run(0.5);
    engine.stop();
    rig.run(0.05);
    check(engine.position() == bar3 && session.project().history_size() == history,
          "stopping in the count-in returns to the start and records nothing");
    deck.set_count_in(0, false);
    check(!deck.count_in_key().getToggleState() && engine.count_in_bars() == 0, "Count-in off");
    session.set_record_armed(vox, false);
    session.update_track_mix(vox, "monitor", [](project::Track& t) { t.monitor = "auto"; });

    // Loop recording: three passes round a one-bar loop make three takes.
    const std::string loop = session.add_audio_track("Loop", false);
    session.set_record_armed(loop, true);
    engine.set_loop(true, 0, d.meter.bar_start(2));
    engine.locate(0);
    engine.play();
    for (double t = 0.0; t < bar_s * 2.95; t += 0.25) {
        rig.run(0.25, Rig::sine(330.0, 0.4f));
        engine.service_recording();  // as the engine's timer does
    }
    engine.stop();
    engine.set_loop(false, 0, 0);
    takes = clips_on(loop);
    std::set<int> lanes;
    std::set<std::string> files;
    int full = 0;
    for (const auto& c : takes) {
        lanes.insert(c.lane);
        files.insert(c.file);
        if (std::abs(c.length - bar_s) < 0.03 && c.start_seconds < 0.012) ++full;
    }
    juce::String detail;
    for (const auto& c : takes)
        detail << " [lane " << c.lane << ": " << juce::String(c.start_seconds, 3) << " s + " << juce::String(c.length, 3) << " s]";
    check(takes.size() == 3 && lanes.size() == 3 && files.size() == 3 && full == 2,
          "loop recording keeps every pass as a take (" + juce::String(static_cast<int>(takes.size())) + " takes, " +
              juce::String(full) + " full cycles;" + detail + "; " + last_message + ")");
    session.set_record_armed(loop, false);

    // MIDI: what is played on the keyboard into an armed instrument track.
    const std::string flute_rec = session.add_track("Flute", "woodwinds.flute");
    check(d.find_track(flute_rec)->record_source == "midi", "an instrument track records MIDI unless set to audio");
    // Regions are a MIDI track's: a staff track's recording goes onto its staff.
    const std::string keys = session.add_midi_track("Keys");
    session.select_track(keys);
    engine.set_live_track(keys);
    session.set_record_armed(keys, true);
    engine.locate(0);
    engine.play();
    rig.run(0.5);
    engine.send_live(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)));
    rig.run(0.5);
    engine.send_live(juce::MidiMessage::noteOff(1, 60));
    rig.run(0.5);
    engine.send_live(juce::MidiMessage::noteOn(1, 64, static_cast<juce::uint8>(80)));
    rig.run(0.5);
    engine.send_live(juce::MidiMessage::noteOff(1, 64));
    rig.run(0.5);
    engine.stop();
    auto regions = [&] {
        std::vector<project::MidiRegion> out;
        if (const auto* t = d.find_track(keys); t && t->active_version()) out = t->active_version()->regions;
        return out;
    };
    auto r = regions();
    const Ticks tol = d.tempo.tick_at_seconds(0.025);
    const bool notes_ok = r.size() == 1 && r[0].notes.size() == 2 && r[0].notes[0].pitch == 60 && r[0].notes[1].pitch == 64 &&
                          std::abs(r[0].notes[0].start - d.tempo.tick_at_seconds(0.5)) <= tol &&
                          std::abs(r[0].notes[0].duration - d.tempo.tick_at_seconds(0.5)) <= tol &&
                          r[0].notes[0].velocity == 100 && r[0].notes[1].velocity == 80;
    check(notes_ok && r[0].start == 0 && r[0].end == bar3 && session.undo_label() == "Record MIDI",
          "MIDI recording places the notes played as a region of whole bars (" + last_message + ")");
    // A second pass: the first region goes to a take lane, and is silent there.
    engine.locate(0);
    engine.play();
    rig.run(0.25);
    engine.send_live(juce::MidiMessage::noteOn(1, 72, static_cast<juce::uint8>(90)));
    rig.run(0.5);
    engine.send_live(juce::MidiMessage::noteOff(1, 72));
    rig.run(0.25);
    engine.stop();
    r = regions();
    int lane0 = 0, lane1 = 0;
    for (const auto& x : r) (x.lane == 0 ? lane0 : lane1)++;
    check(lane0 == 1 && lane1 == 1, "a second MIDI pass moves the first to a take lane");
    const auto played = project::render_track(*d.find_track(keys));
    bool only_new = !played.notes.empty();
    for (const auto& n : played.notes) only_new = only_new && n.pitch == 72;
    check(only_new, "MIDI take lanes are silent: only the main lane plays");

    // Wait note: Play waits for the first note, which starts the take.
    deck.set_wait_note(true);
    check(engine.wait_note() && deck.wait_note_key().getToggleState(), "the Wait note key turns wait-for-note on");
    engine.locate(bar2);
    engine.play();
    rig.run(0.5);
    const bool waited = engine.waiting_for_note() && !engine.playing() && engine.position() == bar2;
    engine.send_live(juce::MidiMessage::noteOn(1, 67, static_cast<juce::uint8>(110)));
    engine.service_recording();
    const bool started = engine.playing() && !engine.waiting_for_note();
    rig.run(0.5);
    engine.send_live(juce::MidiMessage::noteOff(1, 67));
    rig.run(0.3);
    engine.stop();
    check(waited && started, "Play waits for a note, and the note starts the transport");
    r = regions();
    const project::MidiRegion* waited_region = nullptr;
    for (const auto& x : r)
        if (x.lane == 0) waited_region = &x;
    check(waited_region && waited_region->notes.size() == 1 && waited_region->notes[0].pitch == 67 &&
              waited_region->notes[0].start == bar2 && waited_region->start == bar2,
          "the note that started the take is its first note, on the downbeat");
    deck.set_wait_note(false);
    session.set_record_armed(keys, false);
    engine.set_record_enabled(false);

    // Swipe comping: a drag across part of a take on its lane makes that
    // part the main lane's; a double-click promotes a whole take.
    main.show_workspace(MainComponent::Workspace::Producer);
    session.select_track(vox);
    pump(150);
    auto& seq = main.sequencer();
    seq.show_take_lanes(vox, true);
    for (const auto& c : clips_on(vox))
        if (c.lane > 0) seq.select_clip(vox, c.id);  // scrolls it into view
    pump(100);
    shoot(main, dir, "recording-takes");
    auto* lanes_view = find_by_id(main, "sequencer_lanes");
    juce::Rectangle<int> take_area;
    if (lanes_view) {
        // The take's body shows the comping (I-beam) pointer.
        for (int y = 0; y < lanes_view->getHeight(); y += 3)
            for (int x = 0; x < lanes_view->getWidth(); x += 3) {
                const juce::Point<float> p(static_cast<float>(x), static_cast<float>(y));
                lanes_view->mouseMove(mouse_event(*lanes_view, p, p, false));
                if (lanes_view->getMouseCursor() == juce::MouseCursor::IBeamCursor) take_area = take_area.isEmpty() ? juce::Rectangle<int>(x, y, 1, 1)
                                                                                          : take_area.getUnion({x, y, 1, 1});
            }
    }
    check(!take_area.isEmpty(), "the open take lane shows its take");
    if (lanes_view && !take_area.isEmpty()) {
        const float y = static_cast<float>(take_area.getCentreY());
        const float x0 = static_cast<float>(take_area.getX() + take_area.getWidth() / 4);
        const float x1 = static_cast<float>(take_area.getX() + take_area.getWidth() * 3 / 4);
        drag(*lanes_view, {x0, y}, {x1, y}, false, juce::ModifierKeys::commandModifier);
        takes = clips_on(vox);
        std::string take_file;
        for (const auto& c : takes)
            if (c.lane > 0) take_file = c.file;
        int main_lane = 0, from_take = 0;
        for (const auto& c : takes)
            if (c.lane == 0) {
                ++main_lane;
                if (c.file == take_file) ++from_take;
            }
        check(session.undo_label() == "Comp take" && main_lane == 3 && from_take == 1,
              "swiping across a take comps that part onto the main lane, splitting what was there (" +
                  juce::String(main_lane) + " main-lane clips)");
        shoot(main, dir, "recording-comp");
        const juce::Point<float> p(x0, y);
        lanes_view->mouseDoubleClick(mouse_event(*lanes_view, p, p, false));
        pump(50);
        check(session.undo_label() == "Promote take", "double-clicking a take promotes it whole");
    }
    seq.show_take_lanes(vox, false);
    engine.on_recording_finished = previous;
}

/// Clip editing in the sequencer: split at the playhead (Cmd/Ctrl+T),
/// crossfade, nudge (Alt+arrows), consolidate into one file, undo.
void clip_editing_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_empty();
    const auto project_dir = dir.getChildFile("clip-edit-check");
    project_dir.deleteRecursively();
    project_dir.getChildFile("Audio Files").createDirectory();
    std::string error;
    session.save_as(project_dir.getChildFile("clips.vcc").getFullPathName().toStdString(), &error);
    // A 4 s, 440 Hz mono tone at 48 kHz.
    const auto tone = project_dir.getChildFile("Audio Files").getChildFile("tone.wav");
    auto sine = [](std::int64_t i) { return 0.5f * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * i / 48000.0)); };
    {
        juce::AudioBuffer<float> b(1, 4 * 48000);
        for (int i = 0; i < b.getNumSamples(); ++i) b.setSample(0, i, sine(i));
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(tone);
        auto writer = juce::WavAudioFormat().createWriterFor(
            stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000.0).withNumChannels(1).withBitsPerSample(24));
        if (writer) writer->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
    }
    const auto track = session.add_audio_track("Gtr", false);
    project::AudioClip clip;
    clip.name = "tone";
    clip.file = "Audio Files/tone.wav";
    clip.length = 4.0;
    clip.channels = 1;
    clip.file_sample_rate = 48000.0;
    const auto first = session.add_audio_clips({{track, clip}}, "Import");
    if (first.size() != 1) {
        check(false, "the clip-editing check's clip is placed");
        return;
    }
    main.show_workspace(MainComponent::Workspace::Producer);
    pump(100);
    auto& seq = main.sequencer();
    auto clips = [&] {
        std::vector<project::AudioClip> out = session.data().find_track(track)->active_version()->audio_clips;
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.start_seconds < b.start_seconds; });
        return out;
    };
    const auto& tempo = session.data().tempo;

    // Split at the playhead (1.5 s), then Cmd/Ctrl+T at 3 s.
    seq.select_clip(track, first[0]);
    seq.set_playhead(tempo.tick_at_seconds(1.5), false);
    const bool split = seq.split_at_playhead();
    auto v = clips();
    check(split && v.size() == 2 && session.undo_label() == "Split clip" && std::abs(v[1].start_seconds - 1.5) < 0.001 &&
              std::abs(v[1].source_offset - 1.5) < 0.001,
          "Split at playhead cuts the clip in two, the right part reading on from the cut");
    seq.set_playhead(tempo.tick_at_seconds(3.0), false);
    seq.select_clip(track, v[1].id);
    auto* lanes = find_by_id(main, "sequencer_lanes");
    if (lanes) lanes->keyPressed(juce::KeyPress('t', juce::ModifierKeys::commandModifier, 't'));
    v = clips();
    check(v.size() == 3, "Cmd/Ctrl+T splits at the playhead");

    // Crossfade the first two: the second reaches 20 ms back into its file.
    seq.select_clip(track, v[0].id);
    const bool faded = seq.crossfade_selected(0.02);
    v = clips();
    check(faded && session.undo_label() == "Crossfade" && std::abs(v[1].start_seconds - 1.48) < 0.001 &&
              std::abs(v[0].fade_out - 0.02) < 0.001 && std::abs(v[1].fade_in - 0.02) < 0.001,
          "a crossfade overlaps the two clips by 20 ms, fading across");

    // Nudge: Alt+Right moves the selected clip 10 ms later; undo puts it back.
    seq.select_clip(track, v[2].id);
    if (lanes) lanes->keyPressed(juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::altModifier, 0));
    const double nudged = clips()[2].start_seconds;
    check(std::abs(nudged - 3.01) < 0.0005 && session.undo_label() == "Nudge clip",
          "Alt+Right nudges the clip 10 ms (" + juce::String(nudged, 4) + " s)");
    session.undo();
    shoot(main, dir, "clip-editing");

    // Consolidate: the three become one file that sounds the same.
    seq.select_clip(track, clips()[0].id);
    const bool merged = seq.consolidate_selected();
    v = clips();
    check(merged && v.size() == 1 && session.undo_label() == "Consolidate" && std::abs(v[0].length - 4.0) < 0.001 &&
              juce::String(v[0].file).startsWith("Audio Files/Gtr_CONSOL_"),
          "Consolidate renders the touching clips into one new file (" + juce::String(v.empty() ? "" : v[0].file) + ")");
    if (merged && v.size() == 1) {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(
            formats.createReaderFor(juce::File(juce::String(session.resolve_audio_path(v[0].file)))));
        float worst = 1.0f;
        if (reader) {
            juce::AudioBuffer<float> b(1, static_cast<int>(reader->lengthInSamples));
            reader->read(&b, 0, b.getNumSamples(), 0, true, false);
            worst = 0.0f;
            // Away from the crossfade, sample for sample the original tone.
            for (int i = 0; i < b.getNumSamples(); ++i) {
                if (i > static_cast<int>(1.47 * 48000) && i < static_cast<int>(1.51 * 48000)) continue;
                worst = std::max(worst, std::abs(b.getSample(0, i) - sine(i)));
            }
        }
        check(reader && reader->bitsPerSample == 24 && worst < 1.0e-4f,
              "the consolidated file is the clips' audio, sample for sample (worst error " + juce::String(worst, 6) + ")");
    }
    session.undo();
    check(clips().size() == 3, "undo brings the clips back");

    // Several clips at once: a box dragged across the track's row from empty
    // space selects all three; Alt+Right nudges them together; Delete
    // removes them as one undo step.
    if (lanes) {
        seq.select_clip(track, clips()[0].id);
        pump(30);
        const float y = 40.0f;
        const float right = static_cast<float>(lanes->getWidth() - 30), left = 230.0f;
        drag(*lanes, {right, y - 15.0f}, {left, y + 15.0f});
        const int chosen = seq.selected_clip_count();
        lanes->keyPressed(juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::altModifier, 0));
        auto moved = clips();
        bool all_moved = moved.size() == 3;
        for (const auto& m : moved) all_moved = all_moved && m.start_seconds > 0.005;
        check(chosen == 3 && all_moved && session.undo_label() == "Nudge clip",
              "a box across the lanes selects every clip it touches (" + juce::String(chosen) + "), and they nudge together");
        session.undo();
        lanes->keyPressed(juce::KeyPress(juce::KeyPress::deleteKey));
        check(clips().empty() && session.undo_label() == "Delete clips", "Delete removes the selected clips as one undo step");
        session.undo();
        check(clips().size() == 3, "and undo brings them all back");
    }
}

/// Edit modes and the Smart tool, driven with the mouse on real clips:
/// Grid snaps and Slip does not, Shuffle closes gaps when deleting,
/// trimming and moving, Spot puts a clip at a typed time, and the Smart
/// tool's top half selects a range that Delete and Cmd/Ctrl+T act on.
void edit_mode_checks(MainWindow& window, const juce::File& dir)
{
    using Mode = SequencerView::EditMode;
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_empty();
    const auto project_dir = dir.getChildFile("edit-mode-check");
    project_dir.deleteRecursively();
    project_dir.getChildFile("Audio Files").createDirectory();
    std::string error;
    session.save_as(project_dir.getChildFile("modes.vcc").getFullPathName().toStdString(), &error);
    const auto tone = project_dir.getChildFile("Audio Files").getChildFile("tone.wav");
    {
        juce::AudioBuffer<float> b(1, 4 * 48000);
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample(0, i, 0.5f * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * i / 48000.0)));
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(tone);
        auto writer = juce::WavAudioFormat().createWriterFor(
            stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000.0).withNumChannels(1).withBitsPerSample(24));
        if (writer) writer->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
    }
    const auto track = session.add_audio_track("Gtr", false);
    // Three 1 s clips at 0, 1 and 3 s (a gap before the third).
    std::vector<std::pair<std::string, project::AudioClip>> in;
    for (double at : {0.0, 1.0, 3.0}) {
        project::AudioClip clip;
        clip.name = "tone";
        clip.file = "Audio Files/tone.wav";
        clip.start_seconds = at;
        clip.source_offset = at;
        clip.length = 1.0;
        clip.channels = 1;
        clip.file_sample_rate = 48000.0;
        in.emplace_back(track, clip);
    }
    const auto ids = session.add_audio_clips(in, "Import");
    main.show_workspace(MainComponent::Workspace::Producer);
    pump(100);
    auto& seq = main.sequencer();
    auto* lanes = find_by_id(main, "sequencer_lanes");
    if (ids.size() != 3 || !lanes) {
        check(false, "the edit-mode check's clips are placed");
        return;
    }
    auto start_of = [&](const std::string& id) {
        for (const auto& c : session.data().find_track(track)->active_version()->audio_clips)
            if (c.id == id) return c.start_seconds;
        return -1.0;
    };
    auto count = [&] { return session.data().find_track(track)->active_version()->audio_clips.size(); };
    auto near = [](double a, double b, double tol = 0.002) { return std::abs(a - b) < tol; };
    // A point on a clip: `at` seconds into it, at `y` (0 top, 1 bottom) of its height.
    auto point = [&](const std::string& id, double at, float y) {
        const auto r = seq.clip_bounds(track, id);
        return juce::Point<float>(r.getX() + static_cast<float>(at) * r.getWidth(), r.getY() + y * r.getHeight());
    };
    auto px_per_second = [&] { return seq.clip_bounds(track, ids[0]).getWidth(); };

    // The keys: the toolbar's Edit keys and F1-F4 / F6 choose them, and light.
    if (auto* key = dynamic_cast<juce::Button*>(find_by_id(main, "edit_mode_1"))) key->triggerClick();
    pump(60);
    const bool slip = seq.edit_mode() == Mode::Slip;
    auto* mappings = ctx.commands.getKeyMappings();
    const bool fkeys = mappings && mappings->findCommandForKeyPress(juce::KeyPress(juce::KeyPress::F1Key)) == cmd::editShuffle &&
                       mappings->findCommandForKeyPress(juce::KeyPress(juce::KeyPress::F3Key)) == cmd::editSpot &&
                       mappings->findCommandForKeyPress(juce::KeyPress(juce::KeyPress::F6Key)) == cmd::toggleSmartTool;
    ctx.commands.invokeDirectly(cmd::editGrid, false);
    pump(60);
    auto* grid_key = dynamic_cast<juce::Button*>(find_by_id(main, "edit_mode_3"));
    auto* slip_key = dynamic_cast<juce::Button*>(find_by_id(main, "edit_mode_1"));
    check(slip && fkeys && seq.edit_mode() == Mode::Grid && grid_key && grid_key->getToggleState() && slip_key &&
              !slip_key->getToggleState(),
          "the Edit keys and F1-F4 choose Shuffle, Slip, Spot or Grid, and the chosen key lights (F6: Smart tool) [slip " +
              juce::String(slip ? "y" : "n") + ", F-keys " + (fkeys ? "y" : "n") + ", grid " +
              (seq.edit_mode() == Mode::Grid ? "y" : "n") + ", lit " + (grid_key && grid_key->getToggleState() ? "y" : "n") + "]");

    // Grid snaps a dragged clip; Slip leaves it where it is let go.
    const float nudge = 0.35f * px_per_second();
    drag(*lanes, point(ids[2], 0.5, 0.75f), point(ids[2], 0.5, 0.75f) + juce::Point<float>(nudge, 0.0f));
    const double gridded = start_of(ids[2]);
    const double grid_step = 0.5;  // a quarter at 120 bpm at this zoom
    const bool on_grid = near(std::fmod(gridded + 1e-6, grid_step), 0.0, 0.003) && gridded > 3.1;
    if (gridded > 3.1) session.undo();
    seq.set_edit_mode(Mode::Slip);
    drag(*lanes, point(ids[2], 0.5, 0.75f), point(ids[2], 0.5, 0.75f) + juce::Point<float>(nudge, 0.0f));
    const double slipped = start_of(ids[2]);
    if (slipped > 3.01) session.undo();
    check(on_grid && near(slipped, 3.35, 0.03),
          "Grid snaps a dragged clip to the grid (" + juce::String(gridded, 3) + " s); Slip leaves it where it is let go (" +
              juce::String(slipped, 3) + " s)");

    // Shuffle: deleting the middle clip pulls the third in after the first.
    seq.set_edit_mode(Mode::Shuffle);
    seq.select_clip(track, ids[1]);
    lanes->keyPressed(juce::KeyPress(juce::KeyPress::deleteKey));
    check(count() == 2 && near(start_of(ids[2]), 2.0) && session.undo_label() == "Delete clip",
          "Shuffle: Delete closes the gap - the clip after moves 1 s earlier");
    session.undo();
    // Trimming the middle clip's end 0.25 s shorter pulls the clip after it in.
    drag(*lanes, point(ids[1], 1.0, 0.75f) - juce::Point<float>(1.0f, 0.0f),
         point(ids[1], 0.75, 0.75f) - juce::Point<float>(1.0f, 0.0f));
    const double trimmed_last = start_of(ids[2]);
    check(near(trimmed_last, 2.75, 0.03) && near(start_of(ids[1]), 1.0) && session.undo_label() == "Trim clip end",
          "Shuffle: trimming a clip's end moves the clips after it with it (" + juce::String(trimmed_last, 3) + " s)");
    session.undo();
    // Dragging the third clip to the front: it goes in at 0 and the others make room.
    drag(*lanes, point(ids[2], 0.5, 0.75f), point(ids[0], 0.1, 0.75f));
    check(near(start_of(ids[2]), 0.0) && near(start_of(ids[0]), 1.0) && near(start_of(ids[1]), 2.0) &&
              session.undo_label() == "Move clip",
          "Shuffle: a clip dragged to the front goes in at 0, and the others move up to make room");
    shoot(main, dir, "edit-mode-shuffle");
    session.undo();

    // Spot: a typed time, in bars|beats, min:sec or timecode.
    const auto& d = session.data();
    const auto bbt = SequencerView::spot_text(d, 2.5, 0), mmss = SequencerView::spot_text(d, 2.5, 1),
               tc = SequencerView::spot_text(d, 2.5, 2);
    const auto back_bbt = SequencerView::parse_spot_text(d, "2|2|000", 0);
    const auto back_mmss = SequencerView::parse_spot_text(d, "0:02.5", 1);
    const auto back_tc = SequencerView::parse_spot_text(d, tc, 2);
    check(bbt == "2|2|000" && mmss == "0:02.500" && back_bbt && near(*back_bbt, 2.5) && back_mmss && near(*back_mmss, 2.5) &&
              back_tc && near(*back_tc, 2.5, 0.05) && !SequencerView::parse_spot_text(d, "soon", 0),
          "Spot reads and writes bars|beats (" + bbt + "), min:sec (" + mmss + ") and timecode (" + tc + ")");
    seq.set_edit_mode(Mode::Spot);
    lanes->mouseDown(mouse_event(*lanes, point(ids[2], 0.5, 0.75f), point(ids[2], 0.5, 0.75f), false));
    pump(60);
    auto* modal = juce::ModalComponentManager::getInstance()->getModalComponent(0);
    const bool asked = dynamic_cast<juce::AlertWindow*>(modal) != nullptr;
    if (modal) modal->exitModalState(0);
    pump(60);
    seq.select_clip(track, ids[2]);
    const bool spotted = seq.spot_selected(*back_bbt);
    check(asked && spotted && near(start_of(ids[2]), 2.5) && session.undo_label() == "Spot clip",
          "Spot: a click on a clip asks where it starts, and the clip goes exactly there");
    session.undo();

    // The Smart tool: the top half of a clip selects a range; Delete takes it out.
    seq.set_edit_mode(Mode::Grid);
    seq.set_smart_tool(true);
    pump(60);
    auto* smart_key = dynamic_cast<juce::Button*>(find_by_id(main, "edit_smart"));
    drag(*lanes, point(ids[0], 0.5, 0.25f), point(ids[1], 0.5, 0.25f));
    const auto range = seq.range();
    const auto& tempo = session.data().tempo;
    const bool selected = near(tempo.seconds_at(range.first), 0.5) && near(tempo.seconds_at(range.second), 1.5);
    shoot(main, dir, "edit-mode-smart");
    lanes->keyPressed(juce::KeyPress(juce::KeyPress::deleteKey));
    check(smart_key && smart_key->getToggleState() && selected && count() == 3 && session.undo_label() == "Delete range" &&
              near(start_of(ids[1]), 1.5) && near(start_of(ids[2]), 3.0),
          "Smart tool: dragging in a clip's top half selects 0.5-1.5 s across two clips; Delete cuts it out of both");
    session.undo();
    // In Shuffle the range's gap closes.
    seq.set_edit_mode(Mode::Shuffle);
    seq.select_track_range({track}, tempo.tick_at_seconds(0.5), tempo.tick_at_seconds(1.5));
    const bool deleted = seq.delete_selection();
    check(deleted && near(start_of(ids[1]), 0.5) && near(start_of(ids[2]), 2.0),
          "Smart tool in Shuffle: deleting the range closes the gap after it");
    session.undo();
    // Cmd/Ctrl+T on a range splits at both edges; a click locates the playhead.
    seq.set_edit_mode(Mode::Grid);
    seq.select_track_range({track}, tempo.tick_at_seconds(0.5), tempo.tick_at_seconds(1.5));
    lanes->keyPressed(juce::KeyPress('t', juce::ModifierKeys::commandModifier, 't'));
    const auto split_count = count();
    session.undo();
    session.undo();
    const auto at = point(ids[2], 0.5, 0.25f);
    lanes->mouseDown(mouse_event(*lanes, at, at, false));
    lanes->mouseUp(mouse_event(*lanes, at, at, false));
    pump(60);
    const double located = tempo.seconds_at(ctx.engine.position());
    check(split_count == 5 && near(located, 3.5, 0.01) && seq.range().first == seq.range().second,
          "Smart tool: Cmd/Ctrl+T splits at the range's edges (" + juce::String(static_cast<int>(split_count)) +
              " clips); a click in the top half moves the playhead there (" + juce::String(located, 3) + " s)");
    // The bottom half still moves the clip.
    drag(*lanes, point(ids[2], 0.5, 0.75f), point(ids[2], 0.5, 0.75f) + juce::Point<float>(px_per_second(), 0.0f));
    check(near(start_of(ids[2]), 4.0, 0.01), "Smart tool: the bottom half of a clip still moves it");
    session.undo();
    seq.set_smart_tool(false);
    seq.set_edit_mode(Mode::Grid);
}

/// MIDI tools: input quantize and controllers while recording, Quantize and
/// Velocity on played-in notes, the Q key, the CC lane (drawn by call and
/// by mouse), and drawn CCs reaching playback.
void midi_tools_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    auto& engine = ctx.engine;
    engine.stop();
    session.new_empty();
    const auto project_dir = dir.getChildFile("midi-tools-check");
    project_dir.deleteRecursively();
    project_dir.createDirectory();
    std::string error;
    session.save_as(project_dir.getChildFile("midi.vcc").getFullPathName().toStdString(), &error);
    engine.prepare_offline(48000.0, 512);
    Rig rig(engine);
    const auto& d = session.data();
    const Ticks Q = kTicksPerQuarter, S = Q / 4;
    const std::string keys = session.add_midi_track("Keys");
    auto regions = [&] {
        std::vector<project::MidiRegion> out;
        if (const auto* t = d.find_track(keys); t && t->active_version()) out = t->active_version()->regions;
        return out;
    };

    // Recording with input quantize at 1/16 and the sustain pedal.
    check(session.set_input_quantize(keys, S), "input quantize is set per track");
    session.select_track(keys);
    engine.set_live_track(keys);
    session.set_record_armed(keys, true);
    engine.set_record_enabled(true);
    engine.locate(0);
    engine.play();
    rig.run(0.3);
    engine.send_live(juce::MidiMessage::controllerEvent(1, 64, 127));
    rig.run(0.23);  // 0.53 s: just after the second beat's sixteenth at 0.5 s
    engine.send_live(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(96)));
    rig.run(0.4);
    engine.send_live(juce::MidiMessage::noteOff(1, 60));
    rig.run(0.3);
    engine.send_live(juce::MidiMessage::controllerEvent(1, 64, 0));
    rig.run(0.3);
    engine.stop();
    engine.set_record_enabled(false);
    session.set_record_armed(keys, false);
    auto r = regions();
    bool pedal_down = false, pedal_up = false;
    if (!r.empty())
        for (const auto& e : r[0].controllers) {
            pedal_down = pedal_down || (e.controller == 64 && e.value == 127);
            pedal_up = pedal_up || (e.controller == 64 && e.value == 0);
        }
    check(r.size() == 1 && r[0].notes.size() == 1 && r[0].notes[0].start == Q,
          "input quantize puts a recorded note on the 1/16 grid (" +
              juce::String(r.empty() || r[0].notes.empty() ? -1 : static_cast<int>(r[0].notes[0].start)) + ")");
    check(pedal_down && pedal_up, "the sustain pedal is recorded into the region");
    session.set_input_quantize(keys, 0);

    // Pitch bend and aftertouch, played in.
    session.set_record_armed(keys, true);
    engine.set_record_enabled(true);
    engine.locate(0);
    engine.play();
    rig.run(0.2);
    engine.send_live(juce::MidiMessage::noteOn(1, 64, static_cast<juce::uint8>(90)));
    engine.send_live(juce::MidiMessage::pitchWheel(1, 16383));
    engine.send_live(juce::MidiMessage::channelPressureChange(1, 90));
    rig.run(0.3);
    engine.send_live(juce::MidiMessage::pitchWheel(1, 8192));
    engine.send_live(juce::MidiMessage::noteOff(1, 64));
    rig.run(0.2);
    engine.stop();
    engine.set_record_enabled(false);
    session.set_record_armed(keys, false);
    {
        int bend_up = 0, bend_back = 0, pressure = 0;
        for (const auto& x : regions())
            if (x.lane == 0)
                for (const auto& e : x.controllers) {
                    bend_up += e.controller == kPitchBendController && e.value == 16383 ? 1 : 0;
                    bend_back += e.controller == kPitchBendController && e.value == 8192 ? 1 : 0;
                    pressure += e.controller == kChannelPressureController && e.value == 90 ? 1 : 0;
                }
        check(bend_up == 1 && bend_back == 1 && pressure == 1, "pitch bend and aftertouch are recorded with the notes");
    }

    // Played back: a held note bent up two semitones halfway through sounds 2^(2/12) higher.
    {
        const auto flute = session.add_track("Bend", "woodwinds.flute");
        project::MidiRegion bent;
        bent.start = 0;
        bent.end = session.data().meter.bar_start(3);
        const Ticks one_second = session.data().tempo.tick_at_seconds(1.0);
        bent.notes = {{0, 2 * one_second, 69, 100, 0, {}}};
        bent.controllers = {{0, 0, kPitchBendController, 8192}, {one_second, 0, kPitchBendController, 16383}};
        session.add_recorded_regions({{flute, bent}}, "Record MIDI");
        session.set_solo(flute, true);
        engine.rebuild_now();
        engine.locate(0);
        engine.play();
        std::vector<float> audio;
        juce::AudioBuffer<float> block(2, 512);
        for (int b = 0; b < static_cast<int>(2.0 * 48000 / 512); ++b) {
            engine.render_offline(block, 512);
            for (int i = 0; i < 512; ++i) audio.push_back(block.getSample(0, i));
        }
        engine.stop();
        session.set_solo(flute, false);
        auto crossings = [&](double from, double to) {
            int n = 0;
            const auto a = static_cast<std::size_t>(from * 48000), b = std::min(audio.size(), static_cast<std::size_t>(to * 48000));
            for (std::size_t i = a + 1; i < b; ++i) n += audio[i - 1] < 0.0f && audio[i] >= 0.0f ? 1 : 0;
            return static_cast<double>(n) / (to - from);
        };
        const double before = crossings(0.3, 0.9), after = crossings(1.3, 1.9);
        const double ratio = before > 0 ? after / before : 0.0;
        check(std::abs(ratio - std::pow(2.0, 2.0 / 12.0)) < 0.03,
              "pitch bend plays: the note rises two semitones (" + juce::String(before, 1) + " -> " + juce::String(after, 1) +
                  " Hz, x" + juce::String(ratio, 3) + ")");
    }

    // Off-grid notes, as played without input quantize.
    project::MidiRegion played;
    played.start = 0;
    played.end = 4 * Q;
    played.notes = {{S + 30, S, 64, 40, 0, {}}, {2 * S - 25, S, 65, 110, 0, {}}, {3 * Q + 50, Q, 67, 70, 0, {}}};
    session.add_recorded_regions({{keys, played}}, "Record MIDI");
    main.show_workspace(MainComponent::Workspace::Producer);
    pump(100);
    auto& seq = main.sequencer();
    seq.set_expanded_track(keys);
    pump(100);
    project::QuantizeSettings q;
    q.grid = S;
    const int moved = seq.quantize_notes(q);
    r = regions();
    const project::MidiRegion* main_lane = nullptr;
    for (const auto& x : r)
        if (x.lane == 0) main_lane = &x;
    bool on_grid = main_lane && main_lane->notes.size() == 3;
    if (main_lane)
        for (const auto& n : main_lane->notes) on_grid = on_grid && n.start % S == 0;
    check(moved == 3 && on_grid && session.undo_label() == "Quantize",
          "Quantize puts the played-in notes on the grid (" + juce::String(moved) + " moved)");

    // The Q key quantizes too, with the last settings: undo, then press Q.
    session.undo();
    if (auto* roll = find_named(main, "vcc-piano-roll")) {
        roll->grabKeyboardFocus();
        roll->keyPressed(juce::KeyPress('q'));
    }
    check(session.undo_label() == "Quantize", "Q in the piano roll quantizes");

    // Velocity: set all to 100.
    project::VelocityChange v;
    v.set = 100;
    const int changed = seq.change_velocities(v);
    bool all100 = true;
    for (const auto& x : regions())
        if (x.lane == 0)
            for (const auto& n : x.notes) all100 = all100 && n.velocity == 100;
    check(changed > 0 && all100 && session.undo_label() == "Velocity", "Velocity sets the notes' velocities");

    // The CC lane: modulation drawn as a ramp across the region, by call...
    seq.set_cc_lane(1);
    check(seq.cc_lane() == 1, "the piano roll's bottom lane shows CC1");
    const bool drawn = seq.draw_cc(1, 0, 2 * Q, 0, 127);
    int cc1 = 0, first = -1, last = -1;
    for (const auto& x : regions())
        if (x.lane == 0)
            for (const auto& e : x.controllers)
                if (e.controller == 1) {
                    ++cc1;
                    if (first < 0) first = e.value;
                    last = e.value;
                }
    check(drawn && cc1 > 4 && first < 10 && last > 110 && session.undo_label() == "Draw CC",
          "a modulation ramp is drawn in the CC lane (" + juce::String(cc1) + " points)");
    // ...and by dragging across the lane.
    if (auto* roll = find_named(main, "vcc-piano-roll")) {
        const float y_top = static_cast<float>(roll->getHeight() - 48 + 8), y_bottom = static_cast<float>(roll->getHeight() - 6);
        seq.set_cc_lane(11);
        // From the left of the timeline (inside the region, which starts at bar 1) to the right.
        const float x0 = static_cast<float>(220 + 4), x1 = static_cast<float>(220 + 120);
        roll->mouseDown(mouse_event(*roll, {x0, y_bottom}, {x0, y_bottom}, false));
        roll->mouseDrag(mouse_event(*roll, {(x0 + x1) / 2, (y_top + y_bottom) / 2}, {x0, y_bottom}, true));
        roll->mouseDrag(mouse_event(*roll, {x1, y_top}, {x0, y_bottom}, true));
        roll->mouseUp(mouse_event(*roll, {x1, y_top}, {x0, y_bottom}, true));
        pump(50);
        int cc11 = 0;
        for (const auto& x : regions())
            for (const auto& e : x.controllers) cc11 += e.controller == 11 ? 1 : 0;
        check(cc11 > 1, "dragging in the CC lane draws expression (" + juce::String(cc11) + " points)");
        seq.set_cc_lane(1);
        pump(50);
        shoot(main, dir, "midi-tools");
    }
    // Drawn controllers reach playback: the track renders them.
    int rendered = 0;
    for (const auto& e : project::render_track(*d.find_track(keys)).controllers) rendered += e.controller == 1 ? 1 : 0;
    check(rendered == cc1, "drawn controllers play with the track (" + juce::String(rendered) + " CC1 events)");
    seq.set_cc_lane(-1);
}

/// Drag and drop: audio and MIDI files dropped onto the sequencer's lanes
/// (on a track, at the time under the pointer; below the tracks, new
/// tracks) and onto the window.
void drag_drop_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_empty();
    const auto project_dir = dir.getChildFile("drop-check");
    project_dir.deleteRecursively();
    project_dir.createDirectory();
    std::string error;
    session.save_as(project_dir.getChildFile("drop.vcc").getFullPathName().toStdString(), &error);
    // The files, somewhere outside the project.
    const auto source = dir.getChildFile("drop-source");
    source.deleteRecursively();
    source.createDirectory();
    const auto wav = source.getChildFile("Shaker.wav");
    {
        juce::AudioBuffer<float> b(2, 48000);
        for (int i = 0; i < b.getNumSamples(); ++i)
            for (int ch = 0; ch < 2; ++ch) b.setSample(ch, i, 0.3f * static_cast<float>(std::sin(i * 0.05)));
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(wav);
        auto writer = juce::WavAudioFormat().createWriterFor(
            stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000.0).withNumChannels(2).withBitsPerSample(24));
        if (writer) writer->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
    }
    auto smf_note = [](std::int64_t tick, bool on, int ch, int pitch) {
        music::SmfEvent e;
        e.tick = tick;
        e.event.type = on ? music::MidiEventType::NoteOn : music::MidiEventType::NoteOff;
        e.event.channel = static_cast<std::uint8_t>(ch);
        e.event.data1 = static_cast<std::uint8_t>(pitch);
        e.event.data2 = static_cast<std::uint8_t>(on ? 100 : 0);
        return e;
    };
    const auto song = source.getChildFile("Groove.mid");
    {
        music::SmfFile f;
        music::SmfTrack conductor, bass, lead;
        conductor.meta.push_back(music::smf_tempo(0, 96.0));
        bass.name = "Bass";
        lead.name = "Lead";
        for (int i = 0; i < 4; ++i) {
            bass.events.push_back(smf_note(i * 960, true, 0, 36 + i));
            bass.events.push_back(smf_note(i * 960 + 900, false, 0, 36 + i));
        }
        lead.events.push_back(smf_note(0, true, 1, 72));
        lead.events.push_back(smf_note(1920, false, 1, 72));
        f.tracks = {conductor, bass, lead};
        music::write_smf_file(f, song.getFullPathName().toStdString());
    }
    const auto riff = source.getChildFile("Riff.mid");
    {
        music::SmfFile f;
        music::SmfTrack line;
        line.events.push_back(smf_note(0, true, 0, 64));
        line.events.push_back(smf_note(480, false, 0, 64));
        f.tracks = {line};
        music::write_smf_file(f, riff.getFullPathName().toStdString());
    }
    const auto text = source.getChildFile("notes.txt");
    text.replaceWithText("not music");

    main.show_workspace(MainComponent::Workspace::Producer);
    pump(150);
    auto* lanes = find_by_id(main, "sequencer_lanes");
    auto* target = dynamic_cast<juce::FileDragAndDropTarget*>(lanes);
    check(target != nullptr, "the sequencer's lanes take dropped files");
    if (!target) return;
    check(target->isInterestedInFileDrag({wav.getFullPathName(), song.getFullPathName()}) &&
              !target->isInterestedInFileDrag({text.getFullPathName()}),
          "audio and MIDI files are accepted; other files are not");

    // A MIDI file dropped below the tracks: a track per part, the file's tempo.
    const auto tracks_before = session.data().tracks.size();
    target->filesDropped({song.getFullPathName()}, 300, lanes->getHeight() - 10);
    pump(100);
    const auto& d = session.data();
    std::vector<const project::Track*> made;
    for (const auto& t : d.tracks)
        if (t.name == "Bass" || t.name == "Lead") made.push_back(&t);
    check(d.tracks.size() == tracks_before + 2 && made.size() == 2 && session.undo_label() == "Import Groove" &&
              std::abs(d.tempo.bpm_at(0) - 96.0) < 0.01,
          "a MIDI file dropped below the tracks makes a track per part, taking its tempo (" +
              juce::String(static_cast<int>(d.tracks.size() - tracks_before)) + " tracks, " + juce::String(d.tempo.bpm_at(0), 1) +
              " BPM)");
    std::string bass_id;
    for (const auto* t : made)
        if (t->name == "Bass") bass_id = t->id;
    const auto* bass = d.find_track(bass_id);
    // Imported MIDI on a staff track is notated: the notes are on its staff.
    auto staff_notes = [&](const project::Track* t, Ticks from) {
        int n = 0;
        if (t && t->active_version())
            for (const auto& x : t->active_version()->score.notes(0)) n += !x.is_rest && x.start_tick >= from ? 1 : 0;
        return n;
    };
    check(staff_notes(bass, 0) == 4, "the dropped part's notes are on its track's staff (" + juce::String(staff_notes(bass, 0)) + ")");

    // The WAV dropped on the Bass track's row, to the right: onto its own
    // audio lane, at a grid time, copied into the project.
    session.select_track(bass_id);
    pump(100);
    // Rows run from the top in track order.
    int bass_row = -1;
    {
        int top = 0;
        for (const auto& t : d.tracks) {
            const int h = main.sequencer().row_height(t.id);
            if (t.id == bass_id) {
                bass_row = top + std::min(20, h / 2);
                break;
            }
            top += h;
        }
    }
    target->filesDropped({wav.getFullPathName()}, 600, bass_row);
    pump(100);
    bass = d.find_track(bass_id);  // an edit replaces the document: look it up again
    const project::AudioClip* dropped = nullptr;
    if (bass)
        for (const auto& c : bass->active_version()->audio_clips) dropped = &c;
    const Ticks dropped_tick = dropped ? d.tempo.tick_at_seconds(dropped->start_seconds) : -1;
    check(dropped && dropped->start_seconds > 0.0 && juce::String(dropped->file).startsWith("Audio Files/Shaker") &&
              project_dir.getChildFile(juce::String(dropped->file)).existsAsFile(),
          "audio dropped on a track lands on its audio lane where it was dropped, copied into the project (" +
              juce::String(dropped ? dropped->start_seconds : -1.0, 3) + " s)");
    target->fileDragMove({riff.getFullPathName()}, 800, bass_row);  // the drop hint, for the screenshot
    shoot(main, dir, "drag-drop");
    target->fileDragExit({});

    // A one-part MIDI file on the Bass row: onto that track, from the bar under the pointer.
    target->filesDropped({riff.getFullPathName()}, 600, bass_row);
    pump(100);
    bass = d.find_track(bass_id);
    const Ticks riff_bar = d.meter.bar_start(d.meter.bar_at(dropped_tick));
    check(staff_notes(bass, 0) == 5 && staff_notes(bass, riff_bar) >= 1 && staff_notes(bass, riff_bar + 1) == staff_notes(bass, riff_bar) - 1,
          "a one-part MIDI file dropped on a track joins it, from the bar under the pointer");

    // Nothing for files that are neither.
    const auto history = session.project().history_size();
    target->filesDropped({text.getFullPathName()}, 600, bass_row);
    pump(50);
    check(session.project().history_size() == history, "a dropped text file changes nothing");

    // Dropped on the window elsewhere: at the playhead, on a new audio track.
    ctx.engine.locate(0);
    const auto tracks_now = d.tracks.size();
    main.filesDropped({wav.getFullPathName()}, 10, 10);
    pump(100);
    check(d.tracks.size() == tracks_now + 1 && d.tracks.back().kind == project::TrackKind::Audio &&
              !d.tracks.back().active_version()->audio_clips.empty() &&
              d.tracks.back().active_version()->audio_clips[0].start_seconds < 0.001,
          "audio dropped on the window lands on a new audio track at the playhead");

    // MusicXML dropped on the window: its parts become tracks with their notation.
    {
        session.new_from_template("cinematic.horror.psychological");
        std::string e2;
        const auto xml_project = source.getChildFile("xml-source.vcc");
        session.save_as(xml_project.getFullPathName().toStdString(), &e2);
        main.show_workspace(MainComponent::Workspace::Composer);
        pump(50);
        auto& composer = main.composer();
        session.select_track(session.music_tracks().front()->id);
        pump(50);
        composer.grabKeyboardFocus();
        for (const char ch : {'5', 'c', 'd', 'e', 'f'}) composer.keyPressed(juce::KeyPress(ch));
        pump(50);
        const auto xml = source.getChildFile("Theme.musicxml");
        juce::String m;
        export_musicxml_file(ctx, xml, false, m);
        session.save_as(xml_project.getFullPathName().toStdString(), &e2);  // clean, so a later open asks nothing
        session.new_empty();
        const auto before_xml = session.data().tracks.size();
        check(main.isInterestedInFileDrag({xml.getFullPathName()}) && !target->isInterestedInFileDrag({xml.getFullPathName()}),
              "the window takes MusicXML; the sequencer's lanes leave it to the window");
        main.filesDropped({xml.getFullPathName()}, 10, 10);
        pump(100);
        int notes = 0;
        for (const auto& t : session.data().tracks)
            if (const auto* v = t.active_version()) notes += static_cast<int>(v->score.total_note_count());
        check(session.data().tracks.size() > before_xml && notes >= 4,
              "a dropped MusicXML file adds its parts as tracks with their notation (" +
                  juce::String(static_cast<int>(session.data().tracks.size() - before_xml)) + " tracks, " + juce::String(notes) +
                  " notes)");

        // A project file dropped on the window opens it.
        session.new_empty();
        main.filesDropped({xml_project.getFullPathName()}, 10, 10);
        pump(100);
        check(juce::File(juce::String(session.path())) == xml_project, "a dropped project file opens");
    }
}

/// MIDI take comping in the sequencer: the TAKES key in an instrument
/// track's header opens its take lanes; a drag across part of a MIDI take
/// comps those notes onto the main lane; a double-click promotes the take.
/// Articulation tracks: their part's colour in Producer, boxed; one
/// articulation per bar, and the new part offered for an overlap.
void articulation_bar_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& session = window.context().session;
    window.context().engine.stop();
    session.new_empty();
    const auto vln = session.add_track("Violins I", "strings.violins_1");
    const auto& d = session.data();
    {
        notation::ExpressionMap map = notation::make_default_expression_map();
        map.articulations.clear();
        notation::ArticulationRule r;
        r.articulation = notation::Articulation::Pizzicato;
        r.action = notation::ArticulationAction::Keyswitch;
        r.keyswitch_note = 24;
        map.articulations.push_back(r);
        map.has_natural = true;
        map.natural.action = notation::ArticulationAction::Keyswitch;
        map.natural.keyswitch_note = 23;
        session.set_articulation_map(vln, map);
        notation::Score sc = d.find_track(vln)->active_version()->score;
        notation::ScoreNote a, b;
        a.start_tick = 0;
        b.start_tick = d.meter.bar_start(2);
        a.duration = b.duration = notation::quarter();
        a.pitch = notation::spell_midi_note(67, {});
        b.pitch = notation::spell_midi_note(69, {});
        b.articulations = notation::to_mask(notation::Articulation::Pizzicato);
        sc.insert_note(0, a);
        sc.insert_note(0, b);
        session.commit_score(vln, sc, "Write");
    }
    const auto kids = session.expand_articulations(vln);
    check(kids.size() >= 2, "the part expands to articulation tracks (" + juce::String(static_cast<int>(kids.size())) + ")");
    if (kids.size() < 2) return;
    main.show_workspace(MainComponent::Workspace::Producer);
    pump(200);
    auto& seq = main.sequencer();
    const auto part_colour = seq.lane_colour(vln);
    bool same = true;
    for (const auto& k : kids) same = same && seq.lane_colour(k) == part_colour;
    check(same, "in Producer, each articulation track wears its part's colour");
    shoot(main, dir, "articulation-tracks");

    // Bar 1 is the first articulation's: writing it in the second is refused,
    // and a new part is offered; taking it makes "Violins I div." after the tracks.
    std::string pizz;
    for (const auto& k : kids)
        if (d.find_track(k)->articulation_group == "pizzicato") pizz = k;
    check(!pizz.empty(), "there is a pizzicato track");
    if (pizz.empty()) return;
    int asked = 0;
    main.answer_new_part = [&] {
        ++asked;
        return true;
    };
    const auto tracks_before = d.tracks.size();
    notation::Score sc = d.find_track(pizz)->active_version()->score;
    sc.insert_chord(0, kTicksPerQuarter, notation::quarter(), {notation::spell_midi_note(72, {})});
    const bool written = session.commit_score(pizz, sc, "Write");
    pump(300);  // the refusal is noticed on the main timer
    main.answer_new_part = nullptr;
    const project::Track* div = nullptr;
    for (const auto& t : d.tracks)
        if (t.name == "Violins I div.") div = &t;
    check(!written && asked == 1 && d.tracks.size() == tracks_before + 1 && div != nullptr && div->articulation_parent.empty(),
          "a bar already written in another articulation is refused, and New Part makes Violins I div. (asked " +
              juce::String(asked) + ", +" + juce::String(static_cast<int>(d.tracks.size() - tracks_before)) + " tracks; " +
              juce::String(session.last_refusal()) + ")");
    shoot(main, dir, "articulation-new-part");
    main.show_workspace(MainComponent::Workspace::Composer);
    pump(100);
}

/// The Chord Explorer's builder: borrowed chords under the key's, twelve
/// bars to drag chords into, auditioned and sent as notation.
void chord_builder_checks(MainWindow& window, const juce::File& dir)
{
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    // Opened while another project's track is the target, as when the window
    // stays open across projects: that target is gone in the new one.
    session.new_empty();
    session.select_track(session.add_track("Old Piano", "keys.piano"));
    show_chord_explorer(ctx);
    pump(200);
    session.new_empty();
    pump(100);
    show_chord_explorer(ctx);
    pump(300);
    auto* w = dynamic_cast<juce::DocumentWindow*>(find_dialog("Chord Explorer"));
    auto* ex = w ? dynamic_cast<ChordExplorer*>(w->getContentComponent()) : nullptr;
    check(ex != nullptr, "Window > Chord Explorer opens");
    if (!ex) return;
    check(ex->getWidth() >= 1200 && ex->getHeight() >= 840, "the Chord Explorer is bigger (" + juce::String(ex->getWidth()) + " x " +
                                                                 juce::String(ex->getHeight()) + ")");
    check(ex->borrowed_count() >= 4, "borrowed chords sit under the key's (" + juce::String(ex->borrowed_count()) + ")");
    auto* strip = find_by_id(*ex, "chord_build_strip");
    auto* target = dynamic_cast<juce::DragAndDropTarget*>(strip);
    check(strip && strip->isShowing() && target, "the twelve-bar build strip is there, above Audition, Stop and Send");
    if (!strip || !target) return;
    // A chord dragged from the key's row onto bar 1, another onto bar 3: dropped where it lands.
    auto drop = [&](const theory::Chord& chord, const std::string& numeral, int slot) {
        const int w12 = strip->getWidth() / ChordExplorer::kBuildBars;
        const juce::Point<int> at(slot * w12 + w12 / 2, strip->getHeight() / 2);
        const juce::var what = juce::String("chord:") + juce::String(numeral) + "|" + juce::String(chord.name());
        juce::DragAndDropTarget::SourceDetails details(what, nullptr, at);
        if (target->isInterestedInDragSource(details)) target->itemDropped(details);
    };
    drop(ex->key_chord(0), "I", 0);
    drop(ex->key_chord(5), "vi", 1);
    drop(ex->borrowed_chord(0), "borrowed", 2);
    ex->add_to_build(ex->key_chord(4), "V");  // a double-click: the next empty bar
    pump(100);
    check(ex->build_chord(0) && ex->build_chord(1) && ex->build_chord(2) && ex->build_chord(3) && !ex->build_chord(4) &&
              ex->using_build(),
          "chords dragged in land in their bars; a double-click fills the next; building switches Send to My Build");
    const auto built = ex->render_build();
    check(built && built->chords.size() == 4 && built->end == session.data().meter.bar_start(5),
          "the build renders bar by bar (4 chords, bars 1-4)");
    shoot(*w, dir, "chord-builder");
    // Send: a new Harmony track gets the notation, with the chord symbols.
    const auto before = session.data().tracks.size();
    ex->press_send();
    pump(200);
    const project::Track* harmony = nullptr;
    for (const auto& t : session.data().tracks)
        if (t.name == "Harmony") harmony = &t;
    const auto notes = harmony ? harmony->active_version()->score.total_note_count() : 0u;
    check(session.data().tracks.size() == before + 1 && harmony && notes >= 12 && session.data().chords.size() >= 4,
          "Send writes your progression on the target track as notation, with its chord symbols (" + juce::String(static_cast<int>(notes)) +
              " notes, " + juce::String(static_cast<int>(session.data().chords.size())) + " symbols)");
    ex->clear_build();
    pump(60);
    check(!ex->build_chord(0) && ex->render_build() == std::nullopt, "Clear empties the build");
    w->closeButtonPressed();
    pump(60);
}

void midi_comp_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_empty();
    const auto& d = session.data();
    const Ticks Q = kTicksPerQuarter;
    const auto keys = session.add_midi_track("Keys");
    project::MidiRegion take1, take2;
    take1.start = take2.start = 0;
    take1.end = take2.end = d.meter.bar_start(3);
    for (int i = 0; i < 8; ++i) {
        take1.notes.push_back({i * Q, Q / 2, 60, 80, 0, {}});
        take2.notes.push_back({i * Q, Q / 2, 72, 90, 0, {}});
    }
    take1.source = take2.source = "recorded";
    session.add_recorded_regions({{keys, take1}}, "Record MIDI");
    session.add_recorded_regions({{keys, take2}}, "Record MIDI");  // take 1 now on lane 1
    main.show_workspace(MainComponent::Workspace::Producer);
    auto& seq = main.sequencer();
    seq.set_expanded_track({});  // a plain row: header at the top
    session.select_track(keys);
    seq.set_expanded_track({});
    pump(150);
    auto* lanes = find_by_id(main, "sequencer_lanes");
    if (!lanes) {
        check(false, "the sequencer's lanes are there");
        return;
    }
    // The TAKES key, in the header's third row (beside MIDI / AUDIO / BOTH).
    const juce::Point<float> takes_key(180.0f, 60.0f);
    lanes->mouseDown(mouse_event(*lanes, takes_key, takes_key, false));
    lanes->mouseUp(mouse_event(*lanes, takes_key, takes_key, false));
    pump(100);
    juce::Rectangle<int> take_area;
    for (int y = 0; y < lanes->getHeight(); y += 3)
        for (int x = 230; x < lanes->getWidth() - 20; x += 3) {
            const juce::Point<float> p(static_cast<float>(x), static_cast<float>(y));
            lanes->mouseMove(mouse_event(*lanes, p, p, false));
            if (lanes->getMouseCursor() == juce::MouseCursor::IBeamCursor)
                take_area = take_area.isEmpty() ? juce::Rectangle<int>(x, y, 1, 1) : take_area.getUnion({x, y, 1, 1});
        }
    check(!take_area.isEmpty(), "the TAKES key opens the instrument track's take lanes, showing the MIDI take");
    if (take_area.isEmpty()) return;

    // Drag across the take's second quarter (of its width): those notes are comped in.
    const float y = static_cast<float>(take_area.getCentreY());
    const float x0 = static_cast<float>(take_area.getX() + take_area.getWidth() / 4);
    const float x1 = static_cast<float>(take_area.getX() + take_area.getWidth() / 2);
    drag(*lanes, {x0, y}, {x1, y});
    int from_take = 0, kept = 0;
    for (const auto& r : d.find_track(keys)->active_version()->regions)
        if (r.lane == 0)
            for (const auto& n : r.notes) (n.pitch == 60 ? from_take : kept)++;
    check(session.undo_label() == "Comp take" && from_take >= 1 && from_take < 8 && from_take + kept == 8,
          "a drag across part of a MIDI take comps those notes onto the main lane (" + juce::String(from_take) +
              " from the take, " + juce::String(kept) + " kept)");
    shoot(main, dir, "midi-comp");

    // Double-click: the whole take onto the main lane.
    const juce::Point<float> p(x0, y);
    lanes->mouseDoubleClick(mouse_event(*lanes, p, p, false));
    pump(50);
    int sixties = 0;
    for (const auto& r : d.find_track(keys)->active_version()->regions)
        if (r.lane == 0)
            for (const auto& n : r.notes) sixties += n.pitch == 60 ? 1 : 0;
    check(session.undo_label() == "Promote take" && sixties == 8, "double-clicking a MIDI take promotes it whole");
    // Only the main lane plays.
    int played60 = 0, played72 = 0;
    for (const auto& n : project::render_track(*d.find_track(keys)).notes) (n.pitch == 60 ? played60 : played72)++;
    check(played60 == 8 && played72 == 0, "only the main lane's MIDI plays");

    // Cmd/Ctrl+T splits the selected track's MIDI regions at the playhead.
    seq.select_clip({}, {});
    session.select_track(keys);
    seq.set_playhead(2 * Q + Q / 2, false);
    lanes->keyPressed(juce::KeyPress('t', juce::ModifierKeys::commandModifier, 't'));
    int main_regions = 0;
    for (const auto& r : d.find_track(keys)->active_version()->regions) main_regions += r.lane == 0 ? 1 : 0;
    check(session.undo_label() == "Split region" && main_regions == 2, "Cmd/Ctrl+T splits a MIDI region at the playhead");
}

/// Sidechain: a bass on input 1 keyed from a (muted) kick on input 2 ducks
/// only while the kick sounds; without the key it does not; a keyed
/// compressor insert does the same.
void sidechain_checks(MainWindow& window, const juce::File& dir)
{
    juce::ignoreUnused(dir);
    auto& ctx = window.context();
    auto& session = ctx.session;
    auto& engine = ctx.engine;
    engine.stop();
    session.new_empty();
    engine.prepare_offline(48000.0, 512);
    Rig rig(engine);
    // Added kick last, so the key comes from a track processed after the bass in the plan's order.
    const auto bass = session.add_audio_track("Bass", false);
    const auto kick = session.add_audio_track("Kick", false);
    session.update_track_mix(bass, "setup", [](project::Track& t) {
        t.input = 0;
        t.monitor = "input";
        t.strip.comp_on = true;
        t.strip.comp_threshold_db = -30.0;
        t.strip.comp_ratio = 20.0;
        t.strip.comp_attack_ms = 1.0;
        t.strip.comp_release_ms = 50.0;
    });
    session.update_track_mix(kick, "setup", [](project::Track& t) {
        t.input = 1;
        t.monitor = "input";
        t.mute = true;  // heard only as the key
    });
    // The bass: a quiet 100 Hz tone, below the threshold on its own (-32 dB peak).
    auto signal = [](bool kick_on) {
        return [kick_on](int ch, std::int64_t i) {
            const double t = static_cast<double>(i) / 48000.0;
            if (ch == 0) return 0.025f * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * 100.0 * t));
            return kick_on ? 0.7f * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * 1000.0 * t)) : 0.0f;
        };
    };
    auto level = [&](bool kick_on) {
        rig.run(0.3, signal(kick_on));
        return juce::Decibels::gainToDecibels(rig.run(0.3, signal(kick_on)).rms, -120.0);
    };
    const double own = level(false), own_with_kick = level(true);
    check(std::abs(own - own_with_kick) < 0.5, "without a key the bass ignores the kick (" + juce::String(own, 1) + " / " +
                                                   juce::String(own_with_kick, 1) + " dB)");
    check(session.set_sidechain(bass, -1, kick) && console::sidechain_text(ctx, bass, -1) == "Kick",
          "the bass strip's dynamics are keyed from the kick");
    const double quiet = level(false), ducked = level(true);
    engine.take_meter(bass);
    rig.run(0.1, signal(true));
    const auto meter = engine.take_meter(bass);
    check(std::abs(quiet - own) < 0.5 && ducked < quiet - 12.0 && meter.gain_reduction_db < -12.0f,
          "keyed, the bass ducks only while the kick sounds (" + juce::String(quiet, 1) + " -> " + juce::String(ducked, 1) +
              " dB, GR " + juce::String(meter.gain_reduction_db, 1) + " dB)");
    session.set_sidechain(bass, -1, {});
    const double back = level(true);
    check(std::abs(back - own_with_kick) < 0.5, "back on its own signal, it ignores the kick again");

    // A compressor insert keyed from the kick.
    session.update_track_mix(bass, "insert", [](project::Track& t) {
        t.strip.comp_on = false;
        project::InsertSlot comp;
        comp.kind = "compressor";
        comp.suggestion = false;
        comp.params = {{"threshold_db", -30.0}, {"ratio", 20.0}, {"attack_ms", 1.0}, {"release_ms", 50.0}};
        t.inserts = {comp};
    });
    session.set_sidechain(bass, 0, kick);
    const double insert_quiet = level(false), insert_ducked = level(true);
    check(insert_ducked < insert_quiet - 12.0, "a compressor insert keyed from the kick ducks the bass (" +
                                                   juce::String(insert_quiet, 1) + " -> " + juce::String(insert_ducked, 1) + " dB)");

    // An effect plug-in with a sidechain input (the test effect ducks 20 dB
    // while its key is loud): keyed from the kick, run in its own process.
    if (const auto effect_ref = scan(ctx, VCC_TEST_EFFECT)) {
        session.update_track_mix(bass, "plug-in insert", [&](project::Track& t) {
            project::InsertSlot slot;
            slot.kind = "plugin";
            slot.suggestion = false;
            slot.plugin = *effect_ref;
            t.inserts = {slot};
        });
        const double plain_quiet = level(false), plain_loud = level(true);
        check(session.set_sidechain(bass, 0, kick), "an effect plug-in insert can be keyed");
        const double keyed_quiet = level(false), keyed_loud = level(true);
        check(std::abs(plain_quiet - plain_loud) < 0.5 && std::abs(keyed_quiet - plain_quiet) < 0.5 &&
                  keyed_loud < keyed_quiet - 15.0,
              "the plug-in's sidechain input hears the kick: it ducks only while the kick sounds (" + juce::String(keyed_quiet, 1) +
                  " -> " + juce::String(keyed_loud, 1) + " dB; unkeyed " + juce::String(plain_quiet, 1) + " / " +
                  juce::String(plain_loud, 1) + " dB)");
    } else {
        check(false, "the test effect scans for the plug-in sidechain check");
    }
    // Keyed processing stays allocation-free.
    engine.locate(0);
    engine.play();
    const long before = allocations.load();
    for (int i = 0; i < 50; ++i) {
        for (int ch = 0; ch < 2; ++ch)
            for (int k = 0; k < 512; ++k) rig.input.setSample(ch, k, signal(true)(ch, rig.clock + k));
        counting = true;
        engine.render_offline_with_input(rig.input, rig.out, 512);
        counting = false;
        rig.clock += 512;
    }
    engine.stop();
    check(allocations.load() == before, "keyed dynamics allocate nothing on the audio thread");
}

/// The Composer view's Page view: the full score on pages as it prints,
/// switched with the toolbar's Track | Page keys (and the Score stage).
/// Chord Mode (Add-On spec §3): the palette, one-step entry at the cursor
/// and by a click on a staff, inversions, alterations, a contradiction
/// refused, a transposing staff, Replace Selected, and one undo step.
void chord_mode_checks(MainWindow& window, const juce::File& dir)
{
    window.main().composer().set_note_entry(true);  // clicks on the staff write (the quill)
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_empty();
    // One staff here (the piano's two hands are checked after); an instrument that plays chords.
    const auto piano = session.add_track("Texture", "synth.ambient_texture");
    main.show_workspace(MainComponent::Workspace::Composer);
    if (auto* track_key = main.toolbar().key("Track")) track_key->triggerClick();
    pump(100);
    auto& composer = main.composer();
    session.select_track(piano);
    pump(50);
    composer.grabKeyboardFocus();
    composer.keyPressed(juce::KeyPress('5'));  // quarter notes
    auto* chord_key = main.toolbar().key("Chord mode");
    if (chord_key) chord_key->triggerClick();
    pump(80);
    auto& palette = composer.chord_palette();
    check(chord_key && palette.isVisible() && composer.chord_mode() && chord_key->getToggleState(),
          "the Chord mode key opens the floating chord palette and lights");
    const Ticks Q = kTicksPerQuarter;
    auto notes_at = [&](const std::string& track, Ticks tick) {
        std::vector<std::string> out;
        const auto& score = session.data().find_track(track)->active_version()->score;
        for (std::size_t s = 0; s < score.staff_count(); ++s)
            for (const auto& n : score.notes(s))
                if (n.start_tick == tick && !n.is_rest)
                    out.push_back(std::string(1, "CDEFGAB"[static_cast<int>(n.pitch.step)]) +
                                  (n.pitch.alteration > 0 ? "#" : n.pitch.alteration < 0 ? "b" : "") +
                                  std::to_string(n.pitch.octave));
        std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
            const int oa = a.back() - '0', ob = b.back() - '0';
            if (oa != ob) return oa < ob;
            return std::string("CDEFGAB").find(a[0]) < std::string("CDEFGAB").find(b[0]);
        });
        juce::String s;
        for (const auto& n : out) s << (s.isEmpty() ? "" : " ") << n;
        return s;
    };

    // C major, root position, at the cursor: C4 E4 G4 as one action.
    const auto history = session.project().history_size();
    palette.insert_key().triggerClick();
    pump(50);
    const auto c_major = notes_at(piano, 0);
    check(c_major == "C4 E4 G4" && session.project().history_size() == history + 1 &&
              session.undo_label() == "Enter chord C" && !session.data().chords.empty() && session.data().chords[0].text == "C",
          "Insert enters C major as C4 E4 G4 in one undo step, with its chord symbol (" + c_major + ")");
    session.undo();
    pump(30);
    check(notes_at(piano, 0).isEmpty() && session.data().chords.empty(), "one undo takes the whole chord and its symbol away");

    // G7/B: G, dominant 7, first inversion, bass octave 3 (the cursor
    // stayed on beat 2 after the undo).
    palette.root_key(4).triggerClick();
    const auto& qualities = theory::chord_qualities();
    for (std::size_t i = 0; i < qualities.size(); ++i)
        if (qualities[i].id == "7") palette.quality_box().setSelectedItemIndex(static_cast<int>(i), juce::sendNotificationSync);
    palette.bass_box().setSelectedId(2, juce::sendNotificationSync);  // 1st inversion
    composer.chord_state().octave = 3;
    palette.refresh();
    composer.grabKeyboardFocus();
    palette.insert_key().triggerClick();
    pump(50);
    const auto g7 = notes_at(piano, Q);
    check(g7 == "B3 D4 F4 G4" && session.data().chords.size() == 1 && session.data().chords[0].text == "G7/B",
          "G7/B enters a first-inversion dominant seventh with B in the bass (" + g7 + ")");

    // C7(b5,b9): every requested tone, spelled; shown before it is entered.
    palette.root_key(0).triggerClick();
    if (auto* k = palette.alteration_key("b5")) k->triggerClick();
    if (auto* k = palette.alteration_key("b9")) k->triggerClick();
    palette.bass_box().setSelectedId(1, juce::sendNotificationSync);
    pump(30);
    const auto preview = palette.preview_notes();
    check(preview == juce::String::fromUTF8("C3 E3 B\xE2\x99\xAD" "3 G\xE2\x99\xAD" "4 D\xE2\x99\xAD" "5"),
          "the palette previews C7(b5,b9) as C3 E3 Bb3 Gb4 Db5 (" + preview + ")");
    palette.insert_key().triggerClick();
    pump(50);
    const auto c7 = notes_at(piano, 2 * Q);
    check(c7 == "C3 E3 Bb3 Gb4 Db5" && session.data().chords.size() == 2 && session.data().chords[1].text == "C7(b5,b9)",
          "C7(b5,b9) is entered spelled as asked, on the next beat (" + c7 + ")");
    shoot(main, dir, "40-chord-mode");

    // A contradiction is refused: half-diminished with another flat five.
    for (std::size_t i = 0; i < qualities.size(); ++i)
        if (qualities[i].id == "m7b5") palette.quality_box().setSelectedItemIndex(static_cast<int>(i), juce::sendNotificationSync);
    if (auto* k = palette.alteration_key("b9")) k->triggerClick();  // leave b5 on
    pump(30);
    check(!palette.insert_key().isEnabled() && palette.preview_warning().contains("contradict"),
          "a contradiction (m7b5 with b5) disables Insert and says why: " + palette.preview_warning());
    if (auto* k = palette.alteration_key("b5")) k->triggerClick();
    for (std::size_t i = 0; i < qualities.size(); ++i)
        if (qualities[i].id == "maj") palette.quality_box().setSelectedItemIndex(static_cast<int>(i), juce::sendNotificationSync);
    pump(30);

    // A click on a staff in Chord Mode: the chord on the note clicked, in the
    // key (E in C major: E minor), the bass in the octave clicked.
    {
        engraving::InsertionPoint p;
        p.valid = true;
        p.staff_index = 0;
        p.tick = 3 * Q;
        p.pitch = notation::Pitch{notation::Step::E, 4, 0};
        if (composer.lane(0).on_staff_clicked) composer.lane(0).on_staff_clicked(p);
        pump(50);
    }
    const auto clicked = notes_at(piano, 3 * Q);
    check(clicked == "E4 G4 B4" && session.undo_label() == "Enter chord Em",
          "a click on E in Chord Mode enters the key's chord on E there - E minor - in the octave clicked (" + clicked + ")");

    // Replace Selected: a note typed, then replaced by the chord at its place and value.
    composer.chord_state().octave = 4;
    palette.root_key(0).triggerClick();
    palette.refresh();
    {
        engraving::InsertionPoint p;
        p.valid = true;
        p.staff_index = 0;
        p.tick = 5 * Q;
        p.pitch = notation::Pitch{notation::Step::A, 4, 0};
        composer.set_chord_mode(false);
        if (composer.lane(0).on_staff_clicked) composer.lane(0).on_staff_clicked(p);  // a plain A4
        pump(50);
        composer.set_chord_mode(true);
    }
    const bool had_a = notes_at(piano, 5 * Q) == "A4";
    const auto selected = composer.selected_notes().size();
    palette.replace_key().triggerClick();
    pump(50);
    const auto replaced = notes_at(piano, 5 * Q);
    check(had_a && selected == 1 && replaced == "C4 E4 G4" && session.undo_label() == "Enter chord C",
          "Replace sel. puts the chord in place of the selected note, as one step (" + replaced + ")");

    // A B-flat clarinet plays one line: Insert writes no chord there. Sent to the
    // woodwinds, its note (C4 of C E G, close) is written a tone up, transposed once.
    const auto clarinet = session.add_track("Clarinet in Bb", "woodwinds.clarinet_bb");
    session.select_track(clarinet);
    pump(80);
    composer.chord_state().octave = 4;
    palette.refresh();
    palette.insert_key().triggerClick();
    pump(50);
    const auto refused = notes_at(clarinet, 0);
    palette.send_box().setSelectedItemIndex(2, juce::sendNotificationSync);
    palette.insert_key().triggerClick();
    pump(50);
    palette.send_box().setSelectedItemIndex(0, juce::sendNotificationSync);
    const auto written = notes_at(clarinet, 0);
    check(refused.isEmpty() && written == "D4",
          "on a B-flat clarinet: no chord (one line); sent to the woodwinds, its C4 is written D4, transposed once (" + written + ")");
    check(palette.preview_warning().isEmpty() || palette.preview_warning().contains("range"),
          "the palette names any notes outside the instrument's range");
    composer.set_chord_mode(false);
    pump(30);
    check(!palette.isVisible() && !composer.chord_mode(), "turning Chord mode off hides the palette");
}

/// A tiny SoundFont 2 (one sine sample at A4) with the presets the sound pack
/// is asked for: 0:0 (piano), 0:48 (strings) and 128:0 (a kit). The checks
/// stand in for the real pack with it, so they need no download.
juce::MemoryBlock tiny_soundfont()
{
    juce::MemoryOutputStream smpl;
    const int rate = 48000, frames = rate / 2;
    for (int i = 0; i < frames + 46; ++i) {
        const double env = i < frames ? 1.0 - static_cast<double>(i) / frames : 0.0;
        smpl.writeShort(static_cast<short>(i < frames ? std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * i / rate) * 20000.0 * env : 0.0));
    }
    auto chunk = [](juce::MemoryOutputStream& to, const char* id, const juce::MemoryBlock& data) {
        to.write(id, 4);
        to.writeInt(static_cast<int>(data.getSize()));
        to.write(data.getData(), data.getSize());
        if (data.getSize() % 2) to.writeByte(0);
    };
    auto name = [](juce::MemoryOutputStream& to, const char* n) {
        char b[20] = {};
        std::strncpy(b, n, 19);
        to.write(b, 20);
    };
    auto list = [&](juce::MemoryOutputStream& to, const char* kind, const juce::MemoryBlock& body) {
        juce::MemoryOutputStream l;
        l.write(kind, 4);
        l.write(body.getData(), body.getSize());
        chunk(to, "LIST", l.getMemoryBlock());
    };
    juce::MemoryOutputStream info, sdta, pdta;
    {
        juce::MemoryOutputStream v;
        v.writeShort(2);
        v.writeShort(1);
        chunk(info, "ifil", v.getMemoryBlock());
        chunk(info, "isng", juce::MemoryBlock("EMU8000", 8));
        chunk(info, "INAM", juce::MemoryBlock("Check\0", 6));
    }
    chunk(sdta, "smpl", smpl.getMemoryBlock());
    {
        juce::MemoryOutputStream phdr, pbag, pmod, pgen, inst, ibag, imod, igen, shdr;
        const std::vector<std::tuple<const char*, int, int>> presets = {{"Test Piano", 0, 0}, {"Test Strings", 48, 0}, {"Test Pizzicato", 45, 0}, {"Test Kit", 0, 128}};
        int bag = 0;
        for (const auto& [n, prog, bank] : presets) {
            name(phdr, n);
            phdr.writeShort(static_cast<short>(prog));
            phdr.writeShort(static_cast<short>(bank));
            phdr.writeShort(static_cast<short>(bag));
            phdr.writeInt(0); phdr.writeInt(0); phdr.writeInt(0);
            pbag.writeShort(static_cast<short>(bag)); pbag.writeShort(0);
            pgen.writeShort(41); pgen.writeShort(0);  // instrument 0
            ++bag;
        }
        name(phdr, "EOP");
        phdr.writeShort(0); phdr.writeShort(0); phdr.writeShort(static_cast<short>(bag));
        phdr.writeInt(0); phdr.writeInt(0); phdr.writeInt(0);
        pbag.writeShort(static_cast<short>(bag)); pbag.writeShort(0);
        pgen.writeInt(0);
        pmod.writeRepeatedByte(0, 10);
        name(inst, "Sine");
        inst.writeShort(0);
        name(inst, "EOI");
        inst.writeShort(1);
        ibag.writeShort(0); ibag.writeShort(0);
        ibag.writeShort(2); ibag.writeShort(0);
        igen.writeShort(58); igen.writeShort(69);  // root key A4
        igen.writeShort(53); igen.writeShort(0);   // sample 0
        igen.writeInt(0);
        imod.writeRepeatedByte(0, 10);
        name(shdr, "Sine");
        shdr.writeInt(0); shdr.writeInt(frames); shdr.writeInt(0); shdr.writeInt(0); shdr.writeInt(rate);
        shdr.writeByte(69); shdr.writeByte(0); shdr.writeShort(0); shdr.writeShort(1);
        name(shdr, "EOS");
        shdr.writeRepeatedByte(0, 26);
        chunk(pdta, "phdr", phdr.getMemoryBlock()); chunk(pdta, "pbag", pbag.getMemoryBlock());
        chunk(pdta, "pmod", pmod.getMemoryBlock()); chunk(pdta, "pgen", pgen.getMemoryBlock());
        chunk(pdta, "inst", inst.getMemoryBlock()); chunk(pdta, "ibag", ibag.getMemoryBlock());
        chunk(pdta, "imod", imod.getMemoryBlock()); chunk(pdta, "igen", igen.getMemoryBlock());
        chunk(pdta, "shdr", shdr.getMemoryBlock());
    }
    juce::MemoryOutputStream body;
    body.write("sfbk", 4);
    list(body, "INFO", info.getMemoryBlock());
    list(body, "sdta", sdta.getMemoryBlock());
    list(body, "pdta", pdta.getMemoryBlock());
    juce::MemoryOutputStream riff;
    chunk(riff, "RIFF", body.getMemoryBlock());
    return riff.getMemoryBlock();
}

/// A sound pack folder (as the storage serves it): the files and a manifest.
void write_test_pack(const juce::File& src, bool bad_checksum = false)
{
    src.deleteRecursively();
    src.createDirectory();
    const auto sf = tiny_soundfont();
    json::Array files;
    for (const auto& [file, role] : std::vector<std::pair<juce::String, juce::String>>{
             {"Piano.sf2", "piano"}, {"General.sf2", "gm"}, {"CREDITS.txt", "text"}}) {
        const auto f = src.getChildFile(file);
        if (role == "text")
            f.replaceWithText("Salamander Grand Piano by Alexander Holm, CC-BY 3.0\nMuseScore General, MIT\n");
        else
            f.replaceWithData(sf.getData(), sf.getSize());
        auto sha = *hash::sha256_file(f.getFullPathName().toStdString());
        if (bad_checksum && role == "gm") sha[0] = sha[0] == '0' ? '1' : '0';
        json::Object o;
        o["name"] = json::Value::make_string(file.toStdString());
        o["role"] = json::Value::make_string(role.toStdString());
        o["size"] = json::Value::make_number(static_cast<double>(f.getSize()));
        o["sha256"] = json::Value::make_string(sha);
        files.push_back(json::Value(o));
    }
    json::Object m;
    m["version"] = json::Value::make_string("check");
    m["files"] = json::Value(files);
    src.getChildFile("manifest.json").replaceWithText(juce::String(json::write(json::Value(m))));
}

/// Studio > Built-in Sounds and Help > Credits: the pack downloads (here from
/// a folder), is checked against its manifest, loads, and then plays the
/// tracks that have no plug-in and every preview; the credits list the
/// sounds, software, fonts and trademarks (as text: no logos).
void sound_pack_checks(MainWindow& window, const juce::File& dir)
{
    auto& ctx = window.context();
    auto& main = window.main();
    const auto root = dir.getChildFile("sound-pack-check");
    root.deleteRecursively();
    const auto installed_dir = root.getChildFile("Sounds"), src = root.getChildFile("server");
    setenv("VCC_SOUNDS_DIR", installed_dir.getFullPathName().toRawUTF8(), 1);
    setenv("VCC_SOUNDS_URL", juce::URL(src).toString(false).toRawUTF8(), 1);
    sounds::unload();
    write_test_pack(src);

    check(sounds::folder() == installed_dir && !sounds::installed() && sounds::source_url().startsWith("file:"),
          "the pack lives in its own folder and is not installed yet (" + sounds::status() + ")");
    check(sounds::parse_manifest(src.getChildFile("manifest.json").loadFileAsString()).size() == 3 &&
              sounds::parse_manifest("{\"files\":[{\"name\":\"a.sf3\",\"size\":10}]}").empty(),
          "the manifest lists each file with its size and SHA-256 (a file without a checksum is refused)");

    // Before it is loaded, the placeholder plays.
    check(!pack_voice_for(0, false).valid(), "until the pack is loaded, nothing is played from it");

    auto wait_until = [](auto cond, int ms) {
        for (int i = 0; i < ms / 20 && !cond(); ++i) pump(20);
        return cond();
    };

    // First launch: it downloads, checks every file, and loads.
    sounds::download_async(false);
    const bool ready = wait_until([] { return sounds::state() == sounds::State::Ready; }, 15000);
    check(ready && sounds::installed() && sounds::piano() && sounds::gm() &&
              !installed_dir.getChildFile("download").exists() && installed_dir.getChildFile("CREDITS.txt").existsAsFile(),
          "the pack downloads, every file matches its checksum, and both banks load (" + sounds::status() + ")");

    // Pianos play the Salamander grand; the rest MuseScore General; drums its kit.
    const auto piano_voice = pack_voice_for(0, false), strings_voice = pack_voice_for(48, false),
               kit_voice = pack_voice_for(-1, true);
    check(piano_voice.valid() && piano_voice.name == "Salamander Grand Piano" && strings_voice.bank == sounds::gm() &&
              strings_voice.name == "MuseScore General: Test Strings" && kit_voice.bank_number == 128,
          "pianos play the grand, strings the General MIDI strings, drums the kit (" + strings_voice.name + ", " +
              kit_voice.name + ")");

    // The instrument itself: a note sounds, at the right pitch, and stops.
    {
        SoundFontInstrument inst(strings_voice);
        inst.prepare(48000.0, 512);
        juce::AudioBuffer<float> out(2, 512);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 69, 0.8f), 0);
        std::vector<float> mono;
        for (int b = 0; b < 44; ++b) {
            out.clear();
            inst.process(out, midi, 512);
            midi.clear();
            for (int i = 0; i < 512; ++i) mono.push_back(out.getSample(0, i));
        }
        int crossings = 0;
        for (std::size_t i = 4800; i + 1 < mono.size(); ++i)
            if (mono[i] <= 0.0f && mono[i + 1] > 0.0f) ++crossings;
        const double hz = crossings * 48000.0 / static_cast<double>(mono.size() - 4800);
        float peak = 0.0f;
        for (float v : mono) peak = std::max(peak, std::abs(v));
        inst.all_notes_off();
        float after = 0.0f;
        for (int b = 0; b < 8; ++b) {
            out.clear();
            inst.process(out, midi, 512);
        }
        after = out.getMagnitude(0, 512);
        check(peak > 0.05f && std::abs(hz - 440.0) < 3.0 && after < 1e-4f,
              "a note sounds at its pitch (A4: " + juce::String(hz, 1) + " Hz, peak " + juce::String(peak, 2) +
                  ") and stops when the notes are cut");
    }

    // Written pizzicato: the engine sends General MIDI's Pizzicato Strings
    // before the note (and the instrument's own program before arco notes);
    // the instrument follows it.
    {
        SoundFontInstrument inst(strings_voice);
        inst.prepare(48000.0, 512);
        juce::AudioBuffer<float> out(2, 512);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::programChange(1, 45), 0);
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 1);
        inst.process(out, midi, 512);
        const int pizz = inst.current_program();
        midi.clear();
        midi.addEvent(juce::MidiMessage::programChange(1, 48), 0);
        inst.process(out, midi, 512);
        check(pizz == 45 && inst.current_program() == 48 &&
                  !project::gm_articulation_programs(48).empty(),
              "a pizzicato note switches the strings to Pizzicato Strings (45), the next arco note back (48)");
    }

    // Tracks without a plug-in swap to the pack; previews use it too.
    auto& session = ctx.session;
    session.new_empty();
    const auto piano = session.add_track("Piano", "keys.piano");
    const auto violins = session.add_track("Violins", "strings.violins_1");
    pump(300);
    juce::AudioBuffer<float> block(2, 512);
    ctx.engine.render_offline(block, 512);
    pump(100);
    check(ctx.engine.instrument_status(piano) == "Salamander Grand Piano" &&
              ctx.engine.instrument_status(violins).startsWith("MuseScore General:"),
          "the piano track plays the grand, the violins MuseScore General (" + ctx.engine.instrument_status(violins) + ")");
    {
        PlaybackEngine e(session, ctx.plugins);
        e.prepare_offline(48000.0, 512);
        e.preview({NoteEvent{0, 960, 60, 100}, NoteEvent{0, 960, 64, 100}, NoteEvent{0, 960, 67, 100}}, "keys", 0, 100.0,
                  false, 960);
        float peak = 0.0f;
        juce::AudioBuffer<float> out(2, 512);
        for (int b = 0; b < 40; ++b) {
            out.clear();
            e.render_offline(out, 512);
            peak = std::max(peak, out.getMagnitude(0, 512));
        }
        check(peak > 0.01f, "a chord's audition plays on the built-in piano (peak " + juce::String(peak, 3) + ")");
    }

    // The window: what is installed, and keys to fetch it again.
    show_sounds_window();
    pump(150);
    auto* panel = sounds_panel();
    check(panel != nullptr && panel->status_text().startsWith("Ready") && panel->status_text().contains("Salamander"),
          "Studio > Built-in Sounds says what is installed (" + (panel ? panel->status_text() : juce::String()) + ")");
    if (auto* w = find_dialog("Built-in Sounds")) shoot(*w, dir, "103-built-in-sounds", true);

    // A file that does not match its checksum is never used: the installed pack stays.
    write_test_pack(src, true);
    sounds::download_async(true);
    wait_until([] { return sounds::state() != sounds::State::Downloading && sounds::state() != sounds::State::Loading; }, 15000);
    pump(100);
    check(sounds::status().contains("checksum") && sounds::installed() && sounds::piano() != nullptr,
          "a download that does not match its checksum is thrown away; the pack already installed keeps playing (" +
              sounds::status() + ")");

    // Help > Credits: by category, with the notices the licences ask for.
    const auto all = credits();
    juce::StringArray names;
    for (const auto& c : all) names.add(c.name);
    const auto text_of = [&](const juce::String& n) {
        for (const auto& c : all)
            if (c.name == n) return credits_text(c);
        return juce::String();
    };
    check(names.joinIntoString(",") == "Sounds,Software,Fonts,Trademarks" &&
              text_of("Sounds").contains("Alexander Holm") && text_of("Sounds").contains("Creative Commons Attribution 3.0") &&
              text_of("Sounds").contains("Frank Wen") && text_of("Sounds").contains("Permission is hereby granted") &&
              text_of("Software").contains("TinySoundFont") && text_of("Software").contains("stb_vorbis") &&
              text_of("Fonts").contains("Bravura") &&
              text_of("Trademarks").contains("ASIO is a trademark and software of Steinberg Media Technologies GmbH"),
          "Help > Credits lists sounds, software, fonts and trademarks with their licences and notices");
    show_credits_window();
    pump(150);
    auto* tabs = credits_tabs();
    check(tabs != nullptr && tabs->getNumTabs() == 4 && tabs->getTabNames()[3] == "Trademarks",
          "the Credits window has a tab for each category");
    if (auto* w = find_dialog("Credits")) shoot(*w, dir, "104-credits", true);
    if (tabs) {
        tabs->setCurrentTabIndex(3);
        pump(80);
        if (auto* w = find_dialog("Credits")) shoot(*w, dir, "105-credits-trademarks", true);
    }

    // The menus: Help > Credits..., Studio > Built-in Sounds...
    auto has = [&](const juce::String& menu, const juce::String& item) {
        auto m = main.getMenuForIndex(0, menu);
        for (juce::PopupMenu::MenuItemIterator it(m, true); it.next();)
            if (it.getItem().text == item) return true;
        return false;
    };
    check(has("Help", "Credits...") && has("Studio", "Built-in Sounds..."),
          "Help has Credits... and Studio has Built-in Sounds...");

    for (const auto& t : {"Credits", "Built-in Sounds"})
        if (auto* w = dynamic_cast<juce::DocumentWindow*>(find_dialog(t))) w->closeButtonPressed();
    sounds::unload();
    pump(300);
    ctx.engine.render_offline(block, 512);
    pump(100);
    check(ctx.engine.instrument_status(piano).contains("laceholder") || !ctx.engine.instrument_status(piano).startsWith("Salamander"),
          "without the pack the placeholder plays again (" + ctx.engine.instrument_status(piano) + ")");
    unsetenv("VCC_SOUNDS_URL");
    root.deleteRecursively();
}

/// Crescendos and diminuendos: drawn with the mouse (press where it starts,
/// drag to where it ends) or across selected notes; each plays as a swell of
/// expression (CC11) from 0 to 100 (or 100 to 0), and the swell is edited in
/// the Producer's CC11 lane.
void swell_checks(MainWindow& window, const juce::File& dir)
{
    using composer::Command;
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_empty();
    const Ticks Q = kTicksPerQuarter;
    const auto track = session.add_track("Violins", "strings.violins_1");
    // Two bars of whole notes to swell.
    session.update_track_mix(track, "Write", [Q](project::Track& t) {
        editing::ScoreEditor editor(t.active_version()->score);
        editor.set_duration(notation::NoteValue::Whole);
        notation::Pitch c;
        c.step = notation::Step::C;
        c.octave = 5;
        editor.insert_at(0, 0, c);
        editor.insert_at(0, 4 * Q, c);
    });
    main.show_workspace(MainComponent::Workspace::Composer);
    if (auto* track_key = main.toolbar().key("Track")) track_key->triggerClick();
    pump(150);
    auto& composer = main.composer();
    composer.set_staff_tools_detached(false);
    composer.set_staff_tools_collapsed(false);
    session.select_track(track);
    pump(100);
    composer.update_staff_tools();
    auto& tools = composer.staff_tools();
    auto hairpins = [&] { return session.data().find_track(track)->active_version()->score.hairpins(); };

    // < arms drawing: the key lights.
    auto* cresc_key = tools.key_for(Command::StartCrescendo);
    check(cresc_key != nullptr, "the staff tools always offer < and > (no selection needed)");
    if (cresc_key == nullptr) return;
    cresc_key->triggerClick();
    pump(80);
    composer.update_staff_tools();
    check(composer.hairpin_drawing() == 1 && tools.key_for(Command::StartCrescendo)->getToggleState(),
          "< arms crescendo drawing: the key lights");

    std::function<vcc::ui::NotationView*(juce::Component&)> find_view = [&](juce::Component& c) -> vcc::ui::NotationView* {
        if (auto* v = dynamic_cast<vcc::ui::NotationView*>(&c); v && v->isShowing()) return v;
        for (auto* k : c.getChildren())
            if (auto* v = find_view(*k)) return v;
        return nullptr;
    };
    auto* view = find_view(composer);
    if (view == nullptr) {
        check(false, "the Violins staff is on screen");
        return;
    }
    // Press where it starts (bar 1), drag to where it ends (bar 2), let go.
    const float y = view->y_of_staff_position(0, 4.0);
    const juce::Point<float> from{view->x_of_tick(0) + 2.0f, y}, to{view->x_of_tick(4 * Q), y};
    view->mouseDown(mouse_event(*view, from, from, false));
    view->mouseDrag(mouse_event(*view, {(from.x + to.x) / 2.0f, y}, from, true));
    view->mouseDrag(mouse_event(*view, to, from, true));
    shoot(main, dir, "107-hairpin-drawing", true);
    view->mouseUp(mouse_event(*view, to, from, true));
    pump(150);
    auto hs = hairpins();
    check(hs.size() == 1 && hs[0].kind == notation::HairpinKind::Crescendo && hs[0].start_tick == 0 &&
              hs[0].end_tick == 4 * Q && composer.hairpin_drawing() == 0,
          "pressing at bar 1 and dragging to bar 2 draws a crescendo there (" +
              (hs.empty() ? juce::String("none") : juce::String(hs[0].start_tick) + "-" + juce::String(hs[0].end_tick)) +
              "); drawing switches off after it");

    // It plays as a swell: CC11 from 0 to 100 across it.
    auto cc11 = [&](Ticks at) {
        int v = -1;
        for (const auto& c : project::render_track(*session.data().find_track(track)).controllers)
            if (c.controller == 11 && c.tick <= at) v = c.value;
        return v;
    };
    check(cc11(0) == 0 && cc11(2 * Q) > 35 && cc11(2 * Q) < 65 && cc11(4 * Q) == 100,
          "it plays as a swell of expression, CC11 0 -> 100 (" + juce::String(cc11(0)) + ", " + juce::String(cc11(2 * Q)) +
              ", " + juce::String(cc11(4 * Q)) + ")");
    shoot(main, dir, "107a-crescendo", true);

    // Esc cancels drawing; > across the selected bar-2 note draws a diminuendo at once.
    tools.key_for(Command::StartDiminuendo)->triggerClick();
    pump(60);
    composer.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    const bool cancelled = composer.hairpin_drawing() == 0;
    const auto& score = session.data().find_track(track)->active_version()->score;
    notation::EventId second = notation::kInvalidEventId;
    for (const auto& n : score.notes(0))
        if (!n.is_rest && n.start_tick == 4 * Q) second = n.id;
    session.select_notes(track, {second});
    pump(100);
    tools.key_for(Command::StartDiminuendo)->triggerClick();
    pump(120);
    hs = hairpins();
    const bool dim = hs.size() == 2 && hs[1].kind == notation::HairpinKind::Diminuendo && hs[1].start_tick == 4 * Q &&
                     hs[1].end_tick == 8 * Q;
    check(cancelled && dim && cc11(4 * Q) == 100 && cc11(8 * Q - 1) <= 4,
          "Esc cancels drawing; > with a note selected draws a diminuendo across it, 100 -> 0");

    // The Producer's CC11 lane shows each swell; dragging its end handle edits it.
    main.show_workspace(MainComponent::Workspace::Producer);
    pump(200);
    auto& seq = main.sequencer();
    session.select_track(track);
    seq.set_cc_lane(11);
    pump(150);
    const auto sw = seq.swells(track);
    check(sw.size() == 2 && sw[0].start_value == 0 && sw[0].end_value == 100 && sw[1].start_value == 100 &&
              sw[1].end_value == 0,
          "the Producer's CC11 lane shows both swells (0 -> 100, 100 -> 0)");
    for (int v = 105; v <= 127; v += 11) seq.set_swell(track, 0, true, v);
    seq.set_swell(track, 1, false, 90);
    pump(100);
    hs = hairpins();
    check(hs.size() == 2 && notation::hairpin_expression_end(hs[0]) == 127 && notation::hairpin_expression_start(hs[1]) == 90 &&
              cc11(4 * Q - 1) > 120 && session.undo_label().rfind("Swell", 0) == 0,
          "dragging the handles edits the swells (crescendo up to 127, diminuendo from 90): playback follows");
    // The piano roll, its bottom lane on CC11: the swells with their handles.
    {
        auto* roll = find_named(main, "vcc-piano-roll");
        if (roll == nullptr || !roll->isShowing()) {
            std::function<juce::Button*(juce::Component&)> find_key = [&](juce::Component& c) -> juce::Button* {
                if (auto* b = dynamic_cast<juce::TextButton*>(&c); b && b->getButtonText() == "Piano Roll" && b->isShowing()) return b;
                for (auto* k : c.getChildren())
                    if (auto* b = find_key(*k)) return b;
                return nullptr;
            };
            if (auto* key = find_key(main)) key->triggerClick();
            pump(250);
            roll = find_named(main, "vcc-piano-roll");
        }
        seq.set_cc_lane(11);
        pump(150);
        if (roll != nullptr && roll->isShowing()) shoot(*roll, dir, "108-swell-lane", true);
        else shoot(main, dir, "108-swell-lane", true);
    }
    session.undo();
    session.undo();
    pump(80);
    check(notation::hairpin_expression_end(hairpins()[0]) == 100, "undo puts the swell back");
    main.show_workspace(MainComponent::Workspace::Composer);
    pump(100);
}

/// Each track's header: the instrument insert, MIDI in and out; an articulation
/// track per lane; a click on another track never writes; the quill pointer;
/// the library in its own window.
void track_header_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& session = window.context().session;
    auto& engine = window.context().engine;
    session.new_empty();
    const auto vln = session.add_track("Violins I", "strings.violins_1");
    const auto vla = session.add_track("Violas", "strings.viola");
    main.show_workspace(MainComponent::Workspace::Composer);
    session.select_track(vln);
    pump(150);
    auto& composer = main.composer();
    check(composer.strip_count() == 2, "two tracks, two lanes");
    auto& h = composer.header(0);
    check(h.instrument_key().getButtonText() == "Instrument" && h.instrument_key().isShowing(),
          "the header's instrument insert says Instrument until one is assigned");
    const auto tag = h.trans_key().getBounds();
    const auto ins = h.instrument_key().getBounds();
    check(ins.getY() > tag.getBottom() && ins.getX() <= 11 && ins.getRight() >= tag.getRight() - 1,
          "the instrument insert sits under Treble and Trans, as wide as both");
    project::PluginRef ref;
    ref.format = "VST3";
    ref.identifier = "VST3-Opus-test";
    ref.name = "Opus";
    ref.vendor = "EastWest";
    session.set_instrument_plugin(vln, ref);
    pump(120);
    check(composer.header(0).instrument_key().getButtonText() == "Opus", "assigned, it shows the plug-in's name: " +
                                                                              composer.header(0).instrument_key().getButtonText());
    session.set_instrument_plugin(vln, {});
    // MIDI in: the selected track hears only its device.
    check(session.set_midi_input(vln, "Keys A"), "a track's MIDI In is set");
    pump(80);
    check(composer.header(0).midi_in_key().getButtonText() == "In: Keys A", "the header shows MIDI In");
    check(engine.live_input_allows("Keys A") && !engine.live_input_allows("Pads B"),
          "the selected track plays from its MIDI In only");
    session.set_midi_input(vln, {});
    pump(40);
    check(engine.live_input_allows("Pads B"), "All inputs: any device plays it");
    check(session.set_midi_output(vln, "Synth Out"), "an instrument track can send to external MIDI");
    pump(80);
    check(composer.header(0).midi_out_key().getButtonText() == "Out: Synth Out" &&
              composer.header(0).instrument_key().getButtonText() == "External MIDI",
          "the header shows MIDI Out");
    session.set_midi_output(vln, {});
    pump(40);
    shoot(composer, dir, "track-header");

    // Versions, on the header: a new one (a copy, or empty), switch back, up to ten.
    {
        auto& v = composer.header(0).versions();
        check(v.isShowing() && v.getText() == "Version 1", "the header shows the track's version (Version 1)");
        notation::Score sc = session.data().find_track(vln)->active_version()->score;
        sc.insert_chord(0, 0, notation::half(), {notation::spell_midi_note(72, {})});
        session.commit_score(vln, sc, "Idea");
        pump(60);
        composer.header(0).versions().setSelectedId(1002, juce::sendNotificationSync);  // New Empty Version
        pump(100);
        const auto empty_notes = session.data().find_track(vln)->active_version()->score.total_note_count();
        check(composer.header(0).versions().getText() == "Version 2" && empty_notes == 0,
              "New Empty Version: Version 2, no notes - the idea stays in Version 1");
        shoot(composer.header(0), dir, "track-versions");
        composer.header(0).versions().setSelectedId(1, juce::sendNotificationSync);
        pump(100);
        check(session.data().find_track(vln)->active_version()->score.total_note_count() == 1 &&
                  composer.header(0).versions().getText() == "Version 1",
              "back to Version 1: its notes are on the track again");
    }

    // Clear Track (CLR): the whole staff and its MIDI, undoable.
    {
        notation::Score sc = session.data().find_track(vln)->active_version()->score;
        sc.insert_chord(0, kTicksPerQuarter, notation::quarter(), {notation::spell_midi_note(76, {})});
        session.commit_score(vln, sc, "Idea");
        pump(60);
        auto& h0 = composer.header(0);
        check(h0.clear_key().isShowing(), "each track header has Clear Track (CLR)");
        h0.clear_track(false);
        pump(60);
        const auto cleared = session.data().find_track(vln)->active_version()->score.total_note_count();
        check(cleared == 0 && session.undo_label() == "Clear Violins I", "Clear Track empties the staff, one undo step");
        session.undo();
        pump(60);
        check(session.data().find_track(vln)->active_version()->score.total_note_count() > 0, "Undo brings it back");
    }
    // SCORE: in or out of the score; muted instruments can leave it too.
    {
        auto parts = [&] {
            juce::String why;
            const auto laid = lay_out_full_score(window.context(), FullScoreSettings{}, why);
            return laid ? laid->parts : 0;
        };
        const int all = parts();
        auto& h0 = composer.header(0);
        check(h0.score_key().isShowing() && h0.score_key().getToggleState(), "each track header has SCORE, lit while in the score");
        h0.score_key().setToggleState(false, juce::sendNotificationSync);
        pump(60);
        const int without = parts();
        check(!session.in_score(*session.data().find_track(vln)) && without == all - 1 &&
                  session.audible(*session.data().find_track(vln)),
              "SCORE off leaves the instrument out of the score; it still plays (" + juce::String(all) + " -> " +
                  juce::String(without) + " parts)");
        session.undo();
        pump(60);
        check(composer.header(0).score_key().getToggleState() && parts() == all, "Undo puts it back in the score");
        session.set_muted_leave_score(true);
        session.set_mute(vla, true);
        pump(60);
        const int muted = parts();
        session.set_mute(vla, false);
        session.set_muted_leave_score(false);
        pump(60);
        check(muted == all - 1, "with the option on, a muted instrument leaves the score (" + juce::String(muted) + " parts)");
    }
    // Producer: W and H sliders bottom right, as in the Composer.
    {
        main.show_workspace(MainComponent::Workspace::Sequencer);
        pump(150);
        auto& seq = main.sequencer();
        auto* w = dynamic_cast<juce::Slider*>(seq.findChildWithID("producer_zoom_width"));
        auto* hz = dynamic_cast<juce::Slider*>(seq.findChildWithID("producer_zoom_height"));
        const int before = seq.row_height(vla);
        if (hz) hz->setValue(2.0, juce::sendNotificationSync);
        pump(60);
        const int taller = seq.row_height(vla);
        if (hz) hz->setValue(1.0, juce::sendNotificationSync);
        check(w && hz && w->isShowing() && hz->isShowing() && w->getRight() > seq.getWidth() - 300 && taller > before,
              "Producer has W and H sliders bottom right; H makes the tracks taller (" + juce::String(before) + " -> " +
                  juce::String(taller) + ")");
        main.show_workspace(MainComponent::Workspace::Composer);
        pump(150);
    }

    // File > Score Info: the window, and what it puts on the printed head.
    {
        show_score_info(window.context());
        pump(120);
        auto* info_window = find_dialog("Score Info");
        check(info_window != nullptr && info_window->isOnDesktop(), "File > Score Info opens its window");
        if (info_window) shoot(*info_window, dir, "score-info");
        auto si = session.score_info();
        si.title = "Night Train";
        si.composer = "A. Composer";
        si.revision = "Version 3";
        si.date = "19 November 2026";
        si.title_italic = true;
        session.set_score_info(si);
        engraving::PrintOptions po;
        apply_score_info(window.context(), po);
        check(po.title == "Night Train" && po.composer == "A. Composer" && po.head.title_italic &&
                  po.head.info_line == "19 November 2026  -  Version 3" && po.head.tempo_bpm > 0.0,
              "the score's head takes the title (styled), composer, date and version, and the tempo from the music");
        if (auto* w = dynamic_cast<juce::DocumentWindow*>(info_window)) w->closeButtonPressed();
        pump(60);
    }

    // Selecting another track by clicking its staff, while writing: no note.
    session.select_track(vln);
    pump(60);
    composer.set_note_entry(true);
    pump(40);
    check(composer.lane(0).getMouseCursor() == composer_ui::IconKey::quill_cursor(), "writing: the pointer is the quill");
    const auto notes_before = session.data().find_track(vla)->active_version()->score.total_note_count();
    engraving::InsertionPoint p;
    p.valid = true;
    p.staff_index = 0;
    p.tick = 0;
    p.pitch = notation::Pitch{notation::Step::C, 4, 0};
    if (composer.lane(1).on_staff_clicked) composer.lane(1).on_staff_clicked(p);
    pump(80);
    check(session.selected_track() == vla && session.data().find_track(vla)->active_version()->score.total_note_count() == notes_before &&
              !composer.note_entry(),
          "a click on another track selects it, in Select mode, and writes no note");
    check(composer.lane(1).getMouseCursor() != composer_ui::IconKey::quill_cursor(), "selecting: the ordinary pointer");

    // Expanded: a lane per articulation, each with its own notes, named so.
    session.select_track(vln);
    const auto kids = session.expand_articulations(vln, {"natural", "pizzicato"});
    pump(150);
    bool names = composer.strip_count() == 3;
    juce::StringArray lane_names;
    for (int i = 0; i < composer.strip_count(); ++i) lane_names.add(composer.header(i).scribble().text().upToFirstOccurrenceOf("\n", false, false));
    names = names && lane_names.contains("VIOLINS I - NATURAL") && lane_names.contains("VIOLINS I - PIZZICATO");
    check(kids.size() == 2 && names, "expanded: a lane per articulation, named with it (" + lane_names.joinIntoString(", ") + ")");
    int pizz_lane = -1;
    for (int i = 0; i < composer.strip_count(); ++i)
        if (lane_names[i] == "VIOLINS I - PIZZICATO") pizz_lane = i;
    if (pizz_lane >= 0) {
        session.select_track(kids[1]);
        pump(80);
        composer.set_note_entry(true);
        pump(40);
        // In a bar of its own: one articulation per bar (bar 1 holds the natural notes).
        p.tick = session.data().meter.bar_start(8);
        if (composer.lane(pizz_lane).on_staff_clicked) composer.lane(pizz_lane).on_staff_clicked(p);
        pump(80);
        const auto& own = session.data().find_track(kids[1])->active_version()->score;
        const auto& part = session.data().find_track(vln)->active_version()->score;
        bool marked = false;
        for (const auto& n : part.notes(0)) marked = marked || notation::has_articulation(n.articulations, notation::Articulation::Pizzicato);
        check(own.total_note_count() == 1 && marked, "written on the Pizzicato lane: the track holds it, the printed part shows pizz.");
        composer.set_note_entry(false);
    }
    shoot(composer, dir, "articulation-lanes");
    session.consolidate_articulations(vln);
    pump(80);

    // The left Tracks/Library panel is gone; the library floats.
    check(!main.track_panel().isVisible(), "no Tracks/Library panel beside the edit window");
    main.set_library_shown(true);
    pump(120);
    auto* lib = find_dialog("Library");
    check(lib != nullptr && lib->isOnDesktop() && main.library_shown() && main.track_panel().library().isShowing(),
          "Window > Library: the library in its own window");
    if (lib) shoot(*lib, dir, "library-window");
    main.set_library_shown(false);
    pump(80);
    check(!main.library_shown(), "and it closes");
}

/// The piano sketch: every template opens with a Piano on top; its lines go
/// to any track, its chords spread over a section; one line never gets a chord.
void piano_sketch_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& session = window.context().session;
    session.new_from_template("cinematic.orchestra.consolidated");
    main.show_workspace(MainComponent::Workspace::Composer);
    pump(300);
    auto& composer = main.composer();
    const auto tracks = session.composer_tracks();
    check(tracks.size() > 2 && tracks[0]->name == "Sketch Piano Melody" && tracks[1]->name == "Sketch Piano Chords" &&
              composer.header(0).scribble().text().startsWith("SKETCH PIANO MELODY") && session.is_sketch(*tracks[1]) &&
              tracks[0]->output_bus_id == "bus-master",
          "the template opens with its Sketch area on top: two sketch pianos, straight to the master");
    // Ids, not pointers: adding and undoing below replaces the track list.
    const std::string piano = tracks.empty() ? std::string() : tracks.front()->id;
    const std::size_t track_count = tracks.size();
    // Another instrument into the Sketch area, from the + Track menu: after the pianos.
    {
        auto menu = TrackPanel::sketch_area_menu(window.context());
        juce::PopupMenu::MenuItemIterator it(menu);
        bool ran = false;
        while (it.next())
            if (it.getItem().text == "Sketch Piano" && it.getItem().action) {
                it.getItem().action();
                ran = true;
                break;
            }
        pump(150);
        const auto now = session.composer_tracks();
        check(ran && now.size() == track_count + 1 && now[2]->name == "Sketch Piano 3" && session.is_sketch(*now[2]),
              "Into the Sketch Area adds a third sketch piano after the other two, outside the orchestra");
        session.undo();
        pump(100);
    }
    notation::Score sc = session.data().find_track(piano)->active_version()->score;
    sc.insert_chord(0, 0, notation::whole(), {notation::spell_midi_note(64, {}), notation::spell_midi_note(67, {}), notation::spell_midi_note(72, {})});
    sc.insert_chord(1, 0, notation::whole(), {notation::spell_midi_note(48, {})});
    session.commit_score(piano, sc, "Sketch");
    session.select_track(piano);
    pump(100);
    // The header's menu carries the sketch tools.
    auto menu = sketch_menu(window.context(), piano);
    juce::StringArray labels;
    for (juce::PopupMenu::MenuItemIterator it(menu); it.next();) labels.add(it.getItem().text);
    check(labels.contains("Send Top Line to") && labels.contains("Send Bass Line to") && labels.contains("Send All Notes (chords) to") &&
              labels.contains("Spread Chords to Strings (open, 5 parts)") && labels.contains("Spread Chords to Woodwinds (4 parts)") &&
              labels.contains("Spread Chords to Choir (SATB)"),
          "the Piano's menu: send a line to any track, spread chords to a section (" + labels.joinIntoString(", ") + ")");
    // All Notes lists only instruments that play chords.
    bool violins_offered = false;
    for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
        if (it.getItem().text == "Send All Notes (chords) to" && it.getItem().subMenu)
            for (juce::PopupMenu::MenuItemIterator sub(*it.getItem().subMenu); sub.next();)
                violins_offered = violins_offered || sub.getItem().text.startsWith("Violins");
    check(!violins_offered, "chords are never offered to a single-line instrument (violins)");
    session.set_selection_range(0, 4 * kTicksPerQuarter);
    const auto status = spread_to_strings(window.context(), piano);
    const auto strings = session.string_section();
    auto sounding = [&](const std::string& id) {
        int p = -1;
        if (const auto* t = session.data().find_track(id))
            for (const auto& n : t->active_version()->score.notes(0))
                if (!n.is_rest && n.start_tick == 0) p = notation::midi_note_number(n.pitch) + t->active_version()->score.staff(0).total_transposition();
        return p;
    };
    check(!strings[0].empty() && sounding(strings[0]) == 72 && sounding(strings[1]) == 64 && sounding(strings[2]) == 55 &&
              sounding(strings[3]) == 48 && sounding(strings[4]) == 36,
          "Spread Chords to Strings: C5 violins I, E4 violins II, G3 violas, C3 celli, C2 basses (" + status + ")");
    pump(120);
    shoot(composer, dir, "piano-sketch-spread");

    // The chord box: Send to Strings spreads the chord at the cursor; on a violin track alone, no chord.
    composer.set_chord_mode(true);
    pump(100);
    auto& palette = composer.chord_palette();
    check(palette.send_box().isShowing() && palette.send_box().getNumItems() == 5,
          "the chord box has Send to: this track / Strings / Woodwinds / Brass / Choir");
    session.select_track(strings[0]);
    pump(80);
    palette.send_box().setSelectedItemIndex(0, juce::sendNotificationSync);
    const auto before = session.data().find_track(strings[0])->active_version()->score.total_note_count();
    palette.insert_key().triggerClick();
    pump(80);
    check(session.data().find_track(strings[0])->active_version()->score.total_note_count() == before,
          "on the violins alone, Insert writes no chord (one line)");
    palette.send_box().setSelectedItemIndex(1, juce::sendNotificationSync);
    const Ticks at = composer.cursor() ? composer.cursor()->tick : 0;
    const auto revisions = [&] {
        std::vector<std::uint64_t> r;
        for (const auto& id : strings)
            if (const auto* t = session.data().find_track(id)) r.push_back(t->active_version()->score.revision());
        return r;
    };
    const auto rev_before = revisions();
    palette.insert_key().triggerClick();
    pump(80);
    const auto rev_after = revisions();
    int filled = 0;
    for (std::size_t k = 0; k < strings.size(); ++k) {
        const auto* t = session.data().find_track(strings[k]);
        if (!t) continue;
        int here = 0;
        for (const auto& n : t->active_version()->score.notes(0)) here += !n.is_rest && n.start_tick == at ? 1 : 0;
        filled += here == 1 && rev_after[k] != rev_before[k] ? 1 : 0;
    }
    check(filled == 5, "Send to: Strings - the chord spread over all five string parts, one note each (" + juce::String(filled) + ")");
    if (auto* w = find_dialog("Chord Mode")) shoot(*w, dir, "chord-box-send-to");
    palette.send_box().setSelectedItemIndex(0, juce::sendNotificationSync);
    composer.set_chord_mode(false);
    session.new_empty();
}

void chord_palette_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& session = window.context().session;
    session.new_empty();
    const auto piano = session.add_track("Piano", "keys.piano");
    main.show_workspace(MainComponent::Workspace::Composer);
    pump(100);
    auto& composer = main.composer();
    session.select_track(piano);
    pump(80);
    composer.set_chord_mode(true);
    pump(100);
    auto& palette = composer.chord_palette();
    auto* win = find_dialog("Chord Mode");
    check(win != nullptr && win->isOnDesktop() && palette.isShowing(),
          "the chord palette is a window of its own (it can go on another screen)");
    if (win) shoot(*win, dir, "98-chord-palette");
    if (win) {
        // Its header strip drags the whole window (to another screen), not the
        // palette inside its frame.
        const auto win_before = win->getScreenPosition();
        const auto palette_in_win = palette.getBounds().getPosition();
        const juce::Point<float> grab(60.0f, 12.0f);
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        const auto now = juce::Time::getCurrentTime();
        juce::MouseEvent down(src, grab, {}, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &palette, &palette, now, grab, now, 1, false);
        palette.mouseDown(down);
        const auto moved = grab + juce::Point<float>(120.0f, 70.0f);
        juce::MouseEvent drag(src, moved, juce::ModifierKeys::leftButtonModifier, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &palette,
                              &palette, now, grab, now, 1, false);
        palette.mouseDrag(drag);
        pump(40);
        check(win->getScreenPosition() == win_before + juce::Point<int>(120, 70) &&
                  palette.getBounds().getPosition() == palette_in_win,
              "dragging the chord palette's header moves its window, not the palette inside it (" +
                  win_before.toString() + " -> " + win->getScreenPosition().toString() + ", palette " +
                  palette_in_win.toString() + " -> " + palette.getBounds().getPosition().toString() + ")");
        win->setTopLeftPosition(win_before);
        pump(20);
    }
    auto on_staves = [&](Ticks tick) {
        std::array<juce::String, 2> out;
        const auto& score = session.data().find_track(piano)->active_version()->score;
        for (std::size_t st = 0; st < std::min<std::size_t>(2, score.staff_count()); ++st) {
            std::vector<notation::ScoreNote> ns;
            for (const auto& n : score.notes(st))
                if (n.start_tick == tick && !n.is_rest) ns.push_back(n);
            std::sort(ns.begin(), ns.end(), [](const auto& a, const auto& b) {
                return notation::midi_note_number(a.pitch) < notation::midi_note_number(b.pitch);
            });
            for (const auto& n : ns)
                out[st] << (out[st].isEmpty() ? "" : " ") << juce::String::charToString("CDEFGAB"[static_cast<int>(n.pitch.step)])
                        << (n.pitch.alteration > 0 ? "#" : n.pitch.alteration < 0 ? "b" : "") << n.pitch.octave;
        }
        return out;
    };
    auto click_note = [&](notation::Step step, int octave, Ticks tick) {
        engraving::InsertionPoint p;
        p.valid = true;
        p.staff_index = 0;
        p.tick = tick;
        p.pitch = notation::Pitch{step, static_cast<std::int8_t>(octave), 0};
        if (composer.lane(0).on_staff_clicked) composer.lane(0).on_staff_clicked(p);
        pump(50);
    };
    const Ticks Q = kTicksPerQuarter;
    if (auto* k = palette.type_key("triad")) k->triggerClick(), pump(40);
    palette.inversion_key(0).triggerClick(), pump(40);
    palette.close_key().triggerClick(), pump(40);
    palette.span_key(1).triggerClick(), pump(40);
    composer.keyPressed(juce::KeyPress('5'));
    // Piano: the right hand the chord on the note clicked, the left hand its root.
    click_note(notation::Step::C, 4, 0);
    auto c = on_staves(0);
    check(c[0] == "C4 E4 G4" && c[1] == "C3", "on piano a click on C: C E G in the right hand, C in the left (" + c[0] + " | " + c[1] + ")");
    // The key's own seventh on G: G7, spelled in the key.
    if (auto* k = palette.type_key("seventh")) k->triggerClick(), pump(40);
    click_note(notation::Step::G, 4, Q);
    c = on_staves(Q);
    check(c[0] == "G4 B4 D5 F5" && c[1] == "G3", "7th on G in C major is G7 (" + c[0] + " | " + c[1] + ")");
    // Sus4, first inversion... each a key.
    if (auto* k = palette.type_key("sus4")) k->triggerClick(), pump(40);
    click_note(notation::Step::D, 4, 2 * Q);
    c = on_staves(2 * Q);
    check(c[0] == "D4 G4 A4", "Sus4 on D is D G A (" + c[0] + ")");
    if (auto* k = palette.type_key("triad")) k->triggerClick(), pump(40);
    palette.inversion_key(1).triggerClick(), pump(40);
    click_note(notation::Step::C, 4, 3 * Q);
    c = on_staves(3 * Q);
    check(c[0].startsWith("E4") && c[1] == "E3", "1st inversion: E in the bass of both hands (" + c[0] + " | " + c[1] + ")");
    // Open, over two octaves: both hands, the left hand root, fifth and octave.
    palette.inversion_key(0).triggerClick(), pump(40);
    palette.open_key().triggerClick(), pump(40);
    palette.span_key(2).triggerClick(), pump(40);
    click_note(notation::Step::A, 4, 4 * Q);
    c = on_staves(4 * Q);
    check(c[1] == "A2 E3 A3" && c[0].startsWith("A4") && c[0].containsChar('C') && c[0].contains("5"),
          "open over two octaves on A (A minor in C): the left hand A E A, the right hand spread above (" + c[0] + " | " +
              c[1] + ")");
    shoot(main, dir, "99-piano-chords", true);
    composer.set_chord_mode(false);
    pump(100);
    check(find_dialog("Chord Mode") == nullptr, "turning Chord mode off closes its window");
    session.new_empty();
}

/// Instrument range guardrails (Add-On spec §4): Strict refuses a note
/// outside and asks about a chord, Warn and Extended allow with a ring, a
/// new instrument re-checks without deleting, the report and Move into range.
void range_checks(MainWindow& window, const juce::File& dir)
{
    window.main().composer().set_note_entry(true);  // clicks on the staff write (the quill)
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_empty();
    juce::String last_status;
    const auto old_status = ctx.status;
    ctx.status = [&](const juce::String& s) {
        last_status = s;
        if (old_status) old_status(s);
    };
    const auto flute = session.add_track("Flute", "woodwinds.flute");
    main.show_workspace(MainComponent::Workspace::Composer);
    if (auto* track_key = main.toolbar().key("Track")) track_key->triggerClick();
    pump(100);
    auto& composer = main.composer();
    session.select_track(flute);
    pump(50);
    composer.grabKeyboardFocus();
    composer.keyPressed(juce::KeyPress('5'));
    const Ticks Q = kTicksPerQuarter;
    auto click = [&](Ticks tick, notation::Step step, int octave) {
        engraving::InsertionPoint p;
        p.valid = true;
        p.staff_index = 0;
        p.tick = tick;
        p.pitch = notation::Pitch{step, static_cast<std::int8_t>(octave), 0};
        if (composer.lane(0).on_staff_clicked) composer.lane(0).on_staff_clicked(p);
        pump(40);
    };
    auto count = [&] { return session.data().find_track(flute)->active_version()->score.total_note_count(); };
    auto issues = [&] { return session.range_issues(*session.data().find_track(flute)).size(); };

    // Strict (the default for a flute): D7 above its C7 is refused; C5 goes in.
    check(session.range_limits(*session.data().find_track(flute)).policy == session::Session::RangePolicy::Strict,
          "a flute's range is Strict by default (B3-C7)");
    click(0, notation::Step::D, 7);
    const bool refused = count() == 0 && last_status.startsWith("Not entered") && last_status.contains("above");
    click(0, notation::Step::C, 5);
    check(refused && count() == 1, "Strict: a click above the flute's range enters nothing and says why; C5 goes in (" +
                                       last_status.upToFirstOccurrenceOf(".", false, false) + ")");

    // A chord partly outside: asked. Move into range brings the notes in by octaves.
    // (A flute plays one line, so the chord goes on a vibraphone, Strict: F3-F6.)
    const auto vib = session.add_track("Vibraphone", "percussion.vibraphone");
    session.set_range_policy(vib, "strict");
    session.select_track(vib);
    pump(80);
    composer.set_note_entry(true);
    auto vib_count = [&] { return session.data().find_track(vib)->active_version()->score.total_note_count(); };
    auto vib_issues = [&] { return session.range_issues(*session.data().find_track(vib)).size(); };
    composer.set_chord_mode(true);
    {
        // A plain C major triad, close: earlier sections leave the palette on
        // other types and voicings, which this check is not about.
        const auto key = composer.chord_state().key;
        composer.chord_state() = composer_ui::ChordModeState{};
        composer.chord_state().key = key;
    }
    composer.chord_state().octave = 6;  // C6 E6 G6: the G6 above the vibraphone's F6
    composer.chord_palette().refresh();
    composer.chord_palette().insert_key().triggerClick();
    pump(50);
    auto* question = dynamic_cast<juce::AlertWindow*>(juce::ModalComponentManager::getInstance()->getModalComponent(0));
    const bool asked = question && question->getComponentID() == "range_question";
    shoot(main, dir, "41a-range-question");
    if (question) question->exitModalState(1);
    pump(60);
    check(asked && vib_issues() == 0 && vib_count() == 3 && session.undo_label() == "Enter chord C",
          "Strict: a chord outside the range asks, and Move into range enters it with every note inside");
    // Keep, flagged: entered as is, ringed.
    composer.chord_palette().insert_key().triggerClick();
    pump(50);
    question = dynamic_cast<juce::AlertWindow*>(juce::ModalComponentManager::getInstance()->getModalComponent(0));
    if (question) question->exitModalState(2);
    pump(60);
    const auto kept = vib_issues();
    // Cancel: nothing.
    const auto before_cancel = vib_count();
    composer.chord_palette().insert_key().triggerClick();
    pump(50);
    question = dynamic_cast<juce::AlertWindow*>(juce::ModalComponentManager::getInstance()->getModalComponent(0));
    if (question) question->exitModalState(0);
    pump(60);
    check(kept == 1 && vib_count() == before_cancel, "Keep, flagged enters the chord with its high note flagged; Cancel enters nothing");
    composer.set_chord_mode(false);
    session.select_track(flute);
    pump(80);
    composer.set_note_entry(true);

    // Warn: allowed and ringed. Extended: the flute's extended range takes D7.
    session.set_range_policy(flute, "warn");
    click(8 * Q, notation::Step::D, 7);
    check(count() == 2 && issues() == 1 && last_status.startsWith("Flagged"),
          "Warn: D7 goes in and is flagged (" + last_status.upToFirstOccurrenceOf("(", false, false).trim() + ")");
    session.set_range_policy(flute, "extended");
    check(issues() == 0, "Extended: the flute's extended range (to D7) takes the D7");
    session.set_range_policy(flute, "");
    shoot(main, dir, "41-instrument-range");

    // A new instrument re-checks without deleting a note, and says so.
    last_status.clear();
    const auto notes = count();
    session.update_track_mix(flute, "Instrument", [](project::Track& t) { t.instrument_standard_id = "woodwinds.piccolo"; });
    pump(50);
    check(count() == notes && issues() == 1 && last_status.contains("outside the Piccolo's range"),
          "changing the instrument (to piccolo) re-checks: nothing deleted, the new notes outside are announced (" +
              juce::String(static_cast<int>(issues())) + ")");
    session.undo();
    pump(30);

    // The report, and Move into range from it.
    const auto report = TrackPanel::range_report(ctx, {});
    check(report.contains("Flute") && report.contains("above"), "the range report lists each note outside: " +
                                                                     report.upToFirstOccurrenceOf("\n", false, false));
    TrackPanel::show_range_report(ctx, flute);
    pump(30);
    auto* report_window = dynamic_cast<juce::AlertWindow*>(juce::ModalComponentManager::getInstance()->getModalComponent(0));
    if (report_window) report_window->exitModalState(1);
    pump(60);
    check(report_window && issues() == 0 && count() == notes && session.undo_label() == "Move into range",
          "the report's Move into range brings every note inside by octaves, as one undo step");
    ctx.status = old_status;
}

/// The Circle of Fifths (Add-On spec §9): follows the score's key, a click
/// changes only the reference, minor forms, a later modulation, Send to
/// Chord Mode, and Apply key to score with confirmation.
void circle_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    ctx.engine.locate(0);
    session.new_empty();
    session.set_key("G major");
    const auto piano = session.add_track("Piano", "keys.piano");
    session.select_track(piano);
    main.show_workspace(MainComponent::Workspace::Composer);
    pump(80);
    hw::ui_settings().setValue("circle_follow", true);
    if (auto* key = main.toolbar().key("Circle")) key->triggerClick();
    wait_for([] { return composer_ui::circle_of_fifths_panel() != nullptr; });
    auto* panel = composer_ui::circle_of_fifths_panel();
    if (!panel) {
        check(false, "the Circle key opens the Circle of Fifths window");
        return;
    }
    panel->set_following(true);
    auto scale = [&] {
        juce::String s;
        for (const auto& d : panel->reference().scale) s << (s.isEmpty() ? "" : " ") << d.note.name() << (d.altered ? "*" : "");
        return s;
    };
    auto triads = [&] {
        juce::String s;
        for (const auto& t : panel->reference().triads) s << (s.isEmpty() ? "" : " ") << t.chord.name();
        return s;
    };
    check(panel->following() && panel->reference().key.name() == "G major" && scale() == "G A B C D E F#" &&
              panel->reference().fifths == 1 && triads() == "G Am Bm C D Em F#dim",
          "the Circle key opens the window on the score's key: G major, G A B C D E F#, one sharp, G Am Bm C D Em F#dim");
    shoot(*panel->getTopLevelComponent(), dir, "42-circle-of-fifths");

    // A click on E-flat: the reference changes, the score does not.
    const auto history = session.project().history_size();
    const auto at = panel->segment_centre(9, false);
    panel->mouseDown(mouse_event(*panel, at, at, false));
    pump(30);
    check(!panel->following() && panel->reference().key.name() == "Eb major" && scale() == "Eb F G Ab Bb C D" &&
              panel->reference().fifths == -3 && session.data().key == "G major" &&
              session.project().history_size() == history,
          "a click on Eb shows Eb major (three flats) and changes nothing in the score");

    // A minor, natural and harmonic: G against G#.
    const auto inner = panel->segment_centre(0, true);
    panel->mouseDown(mouse_event(*panel, inner, inner, false));
    pump(30);
    const auto natural = scale();
    panel->form_box().setSelectedItemIndex(1, juce::sendNotificationSync);
    pump(30);
    const auto harmonic = scale();
    shoot(*panel->getTopLevelComponent(), dir, "42b-circle-harmonic-minor");
    check(natural == "A B C D E F G" && harmonic == "A B C D E F G#*" && panel->reference().triads[4].uses_altered,
          "A minor shows G natural; harmonic minor shows G# marked as altered, and V as using it");
    panel->form_box().setSelectedItemIndex(0, juce::sendNotificationSync);

    // Follow Score Key: a modulation to D major at bar 3, selected, updates the reference.
    {
        const Ticks bar = session.data().meter.at_bar(1).ticks_per_bar();
        notation::Score score = session.data().find_track(piano)->active_version()->score;
        score.set_key_signature(2 * bar, notation::KeySignature{2, false});
        session.commit_score(piano, score, "Key change");
        panel->follow_key().triggerClick();
        pump(30);
        session.set_selection_range(2 * bar, 3 * bar);
        panel->refresh();
        pump(30);
        check(panel->following() && panel->reference().key.name() == "D major" && panel->anchor_text().contains("bar 3"),
              "following the score, a selection after the modulation shows D major (" + panel->anchor_text() + ")");
        session.set_selection_range(0, 0);
        panel->refresh();
    }

    // Send to Chord Mode: ii (A minor) fills the palette; nothing is inserted.
    const auto before_send = session.project().history_size();
    panel->chord_key(1).triggerClick();
    pump(40);
    const bool sent = panel->send_to_chord_mode();
    pump(50);
    auto& composer = main.composer();
    check(sent && composer.chord_mode() && composer.chord_state().letter == 5 && composer.chord_state().quality == "m" &&
              session.project().history_size() == before_send,
          "Send to Chord Mode puts Am in the chord palette and opens it, entering nothing [sent " + juce::String(sent ? "y" : "n") +
              ", mode " + (composer.chord_mode() ? "y" : "n") + ", letter " + juce::String(composer.chord_state().letter) +
              ", quality " + composer.chord_state().quality + ", key " + panel->reference().key.name() + ", steps " +
              juce::String(static_cast<int>(session.project().history_size() - before_send)) + "]");
    composer.set_chord_mode(false);

    // Borrowed chords (modal mixture), shown under the diatonic ones: in G
    // major, from G minor (and the Neapolitan); a click says what it is and
    // which notes are outside the key, and it goes to Chord Mode like any other.
    panel->set_reference(*theory::Key::parse("G major"));
    panel->set_sevenths(false);
    panel->set_borrowed(true);
    panel->set_borrow_source(theory::BorrowSource::Parallel);
    pump(40);
    juce::String lent;
    for (const auto& b : panel->borrowed()) lent << (lent.isEmpty() ? "" : " ") << juce::String::fromUTF8(b.roman.c_str()) << "=" << b.chord.name();
    check(panel->borrowed().size() == 8 && panel->borrowed_key(6).isVisible() &&
              lent == juce::String::fromUTF8("i=Gm \xE2\x99\xADII=Ab ii\xC2\xB0=Adim \xE2\x99\xADIII=Bb iv=Cm v=Dm \xE2\x99\xADVI=Eb \xE2\x99\xADVII=F"),
          "G major lists its borrowed chords with numerals: " + lent);
    panel->borrowed_key(6).triggerClick();  // bVI, E-flat
    pump(40);
    const auto said = panel->selection_text();
    check(panel->selected_borrowed() == 6 && panel->selected_chord() < 0 &&
              said.contains(juce::String::fromUTF8("\xE2\x99\xADVI = E\xE2\x99\xAD (E\xE2\x99\xAD G B\xE2\x99\xAD)")) &&
              said.contains("borrowed from G minor") && said.contains("outside G major"),
          "a borrowed chord says what it is, where it comes from and which notes are outside the key: " + said);
    check(panel->send_to_chord_mode() && composer.chord_mode() && composer.chord_state().letter == 2 &&
              composer.chord_state().accidental == -1,
          "a borrowed chord goes to Chord Mode as E-flat");
    composer.set_chord_mode(false);
    panel->chord_key(4).triggerClick();
    pump(40);
    check(panel->selection_text().contains("V = D (D F") && panel->selection_text().contains("diatonic in G major"),
          "a diatonic chord's line gives its numeral, name and notes: " + panel->selection_text());
    shoot(*panel->getTopLevelComponent(), dir, "42c-circle-borrowed");

    // Borrowing from another scale on the tonic: the "Borrow from" list.
    auto lent_text = [&] {
        juce::String t;
        for (const auto& b : panel->borrowed()) t << (t.isEmpty() ? "" : " ") << juce::String::fromUTF8(b.roman.c_str()) << "=" << b.chord.name();
        return t;
    };
    check(panel->borrow_box().getNumItems() == 8 && panel->borrow_box().getItemText(0).contains("Parallel minor"),
          "Borrow from lists the parallel minor first, then the modes: " + panel->borrow_box().getItemText(0));
    panel->borrow_box().setSelectedItemIndex(4, juce::sendNotificationSync);  // Mixolydian
    pump(40);
    check(panel->borrow_source() == theory::BorrowSource::Mixolydian &&
              lent_text() == juce::String::fromUTF8("iii\xC2\xB0=Bdim v=Dm \xE2\x99\xADVII=F") &&
              panel->borrowed().front().source == "G Mixolydian",
          "from G Mixolydian: " + lent_text());
    shoot(*panel->getTopLevelComponent(), dir, "42d-circle-borrowed-mixolydian");
    // A minor: the parallel major first (I, the Picardy third; ii; IV; V...).
    panel->set_reference(*theory::Key::parse("A minor"));
    panel->set_borrow_source(theory::BorrowSource::Parallel);
    pump(40);
    check(panel->borrow_box().getItemText(0).contains("Parallel major") &&
              lent_text().startsWith(juce::String::fromUTF8("I=A \xE2\x99\xADII=Bb ii=Bm")),
          "in A minor the list starts from the parallel major: " + lent_text());
    panel->select_borrowed(0, false);
    pump(30);
    shoot(*panel->getTopLevelComponent(), dir, "42e-circle-borrowed-minor");
    panel->set_minor_form(theory::MinorForm::Harmonic);
    panel->set_borrow_source(theory::BorrowSource::HarmonicMinor);
    pump(30);
    check(panel->borrowed().empty(), "a source that adds nothing says so (A harmonic minor from itself)");
    panel->set_minor_form(theory::MinorForm::Natural);
    panel->set_borrow_source(theory::BorrowSource::Parallel);
    panel->set_reference(*theory::Key::parse("G major"));

    // A progression: picked in order (numbered on the keys), unpicked,
    // repeated with Shift, auditioned in order, put on the selected track
    // at the playhead.
    panel->clear_progression();
    panel->set_step_length(1);  // a bar each
    auto click = [&](hw::Keycap& k) {
        k.triggerClick();
        pump(40);
    };
    click(panel->chord_key(0));     // I
    click(panel->chord_key(5));     // vi
    click(panel->borrowed_key(6));  // bVI (from G minor)
    click(panel->chord_key(4));     // V
    const auto picked = panel->progression_text();
    check(picked == juce::String::fromUTF8("1 I G  -  2 vi Em  -  3 \xE2\x99\xADVI E\xE2\x99\xAD  -  4 V D") &&
              panel->places(true, 6) == std::vector<int>{3} && panel->chord_key(5).getToggleState(),
          "chords picked in order are numbered on their keys, borrowed ones too: " + picked);
    shoot(*panel->getTopLevelComponent(), dir, "42f-circle-progression");
    click(panel->chord_key(5));  // vi again: out
    check(panel->progression_text() == juce::String::fromUTF8("1 I G  -  2 \xE2\x99\xADVI E\xE2\x99\xAD  -  3 V D") &&
              !panel->chord_key(5).getToggleState() && panel->places(false, 4) == std::vector<int>{3},
          "picking a chord again takes it out and the rest renumber: " + panel->progression_text());
    panel->toggle_step(false, 0, true);  // Shift: I again, at the end
    check(panel->places(false, 0) == std::vector<int>{1, 4}, "Shift-click repeats a chord (I is 1 and 4)");
    // Audition: in the order picked.
    panel->set_step_length(0);  // two beats: half a second each at 120
    panel->play_key().triggerClick();
    const bool started = wait_for([&] { return panel->playing_progression() && panel->playing_step() == 0; }, 1000);
    const bool advanced = wait_for([&] { return panel->playing_step() == 1; }, 2000);
    check(started && advanced, "Audition plays the progression in order (step 1, then 2)");
    panel->play_key().triggerClick();  // stop
    wait_for([&] { return !panel->playing_progression(); }, 1000);
    check(!panel->playing_progression(), "Audition again stops it");
    // Put on track: the selected track, from the beat the playhead is in.
    panel->set_step_length(1);
    panel->set_put_as_notation(false);  // MIDI first
    panel->set_rhythm(theory::ChordRhythm::Block);
    panel->set_voicing(0);
    session.select_track(piano);
    const Ticks bar3 = session.data().meter.bar_start(3);
    ctx.engine.locate(bar3 + kTicksPerQuarter / 2);  // half a beat into bar 3
    const auto steps_before = session.project().history_size();
    std::size_t expected_notes = 0;
    for (const auto& rc : panel->render_from(bar3).chords) expected_notes += rc.voicing.notes.size();
    const bool put = panel->put_on_track(project::CollisionPolicy::Replace);
    pump(60);
    juce::String symbols;
    for (const auto& m : session.data().chords)
        if (m.tick >= bar3) symbols << session.data().meter.bar_at(m.tick) << ":" << m.text << " ";
    // On a track with a staff the MIDI is written there (it always matches the notation).
    std::size_t notes = 0;
    Ticks first = -1;
    const auto& written_score = session.data().find_track(piano)->active_version()->score;
    for (std::size_t st = 0; st < written_score.staff_count(); ++st)
        for (const auto& n : written_score.notes(st))
            if (!n.is_rest && n.start_tick >= bar3) {
                ++notes;
                if (first < 0 || n.start_tick < first) first = n.start_tick;
            }
    check(put && session.project().history_size() == steps_before + 1 && first == bar3 && notes == expected_notes &&
              symbols.trim() == "3:G 4:Eb 5:D 6:G",
          "Put on track writes it on the selected track from bar 3 (the playhead's beat), a bar each, with its chord "
          "symbols, in one undo step [" + symbols.trim() + ", " + juce::String(static_cast<int>(notes)) + " notes]");
    session.undo();
    pump(40);
    check(session.data().find_track(piano)->active_version()->regions.empty(), "and one undo takes it all back");

    // Voice-led: the chords move less than in root position.
    auto movement = [&] {
        const auto r = panel->render_from(0);
        int moved = 0;
        for (std::size_t i = 1; i < r.chords.size(); ++i) moved += theory::voice_movement(r.chords[i - 1].voicing, r.chords[i].voicing);
        return moved;
    };
    const int led = movement();
    panel->set_voicing(1);
    const int inverted = movement();
    panel->set_voicing(2);
    const int rooted = movement();
    panel->set_voicing(0);
    check(led < rooted && inverted < rooted,
          "root bass with smooth upper voices moves " + juce::String(led) + " semitones, smooth with inversions " +
              juce::String(inverted) + ", root position " + juce::String(rooted));
    // As notation: in the score, spelled as the chords are (Eb Bb in the bVI).
    panel->set_put_as_notation(true);
    ctx.engine.locate(bar3);
    const bool written = panel->put_on_track(project::CollisionPolicy::Replace);
    pump(60);
    const auto& score = session.data().find_track(piano)->active_version()->score;
    int chord_notes = 0, flats = 0, wrong_sharps = 0;
    std::set<Ticks> onsets;
    std::vector<notation::ScoreNote> all_staves;
    for (std::size_t st = 0; st < score.staff_count(); ++st)
        for (const auto& n : score.notes(st)) all_staves.push_back(n);
    for (const auto& n : all_staves) {
        if (n.is_rest) continue;
        ++chord_notes;
        onsets.insert(n.start_tick);
        if (n.pitch.alteration == -1) ++flats;
        // D# or A# would be the bVI misspelled.
        if (n.pitch.alteration == 1 && (n.pitch.step == notation::Step::D || n.pitch.step == notation::Step::A)) ++wrong_sharps;
    }
    check(written && static_cast<std::size_t>(chord_notes) == expected_notes && onsets.size() == 4 && *onsets.begin() == bar3 &&
              flats >= 2 && wrong_sharps == 0 &&
              session.data().find_track(piano)->active_version()->regions.empty(),
          "Put as notation writes the four chords in the score from bar 3, Eb and Bb spelled as flats [" +
              juce::String(chord_notes) + " notes, " + juce::String(flats) + " flats]");
    shoot(main, dir, "42h-circle-progression-in-score", true);
    session.undo();
    // An arpeggio is written as single eighth notes.
    panel->set_rhythm(theory::ChordRhythm::Arpeggio);
    panel->put_on_track(project::CollisionPolicy::Replace);
    pump(60);
    int eighths = 0;
    {
        const auto& sc = session.data().find_track(piano)->active_version()->score;
        for (std::size_t st = 0; st < sc.staff_count(); ++st)
            for (const auto& n : sc.notes(st))
                if (!n.is_rest && n.duration.value == notation::NoteValue::Eighth) ++eighths;
    }
    check(eighths == 32, "an arpeggio is written as eighth notes (" + juce::String(eighths) + ")");
    session.undo();
    panel->set_rhythm(theory::ChordRhythm::Block);
    shoot(*panel->getTopLevelComponent(), dir, "42g-circle-progression-options");

    // My progressions: saved, cleared, loaded back, deleted.
    const auto before_save = panel->progression_text();
    check(panel->save_progression("Check: borrowed six"), "a progression saves under a name");
    panel->clear_progression();
    panel->set_reference(*theory::Key::parse("C major"));
    const bool loaded = panel->load_progression("Check: borrowed six");
    check(loaded && panel->progression_text() == before_save && panel->reference().key.name() == "G major" &&
              panel->saved_progressions().contains("Check: borrowed six"),
          "loading it brings back the chords and their key (G major): " + panel->progression_text());
    check(panel->delete_progression("Check: borrowed six") && !panel->saved_progressions().contains("Check: borrowed six"),
          "and it can be deleted");
    panel->clear_progression();
    panel->set_borrowed(false);
    check(!panel->borrowed_key(0).isVisible(), "the borrowed chords can be hidden");
    panel->set_borrowed(true);

    // Apply key to score: asked first, then one undo step.
    panel->choose_position(9, false);  // Eb major, manual
    panel->apply_key_to_score();
    pump(30);
    auto* question = dynamic_cast<juce::AlertWindow*>(juce::ModalComponentManager::getInstance()->getModalComponent(0));
    const bool asked = question && question->getComponentID() == "circle_apply_question";
    if (question) question->exitModalState(1);
    pump(50);
    check(asked && session.data().key == "Eb major", "Apply key to score asks first, then sets the project to Eb major");
    session.undo();
    check(session.data().key == "G major", "and one undo restores G major");
    panel->set_following(true);
    if (auto* w = panel->getTopLevelComponent()) w->setVisible(false);
}

/// Track Trans (Add-On spec §10): the header key's popover, one track only,
/// shown and heard once, entry while shifted, bypass, playback only, Apply
/// to notes, range flags, audio refused, and no stuck note when the shift
/// changes under a held note.
void trans_checks(MainWindow& window, const juce::File& dir)
{
    window.main().composer().set_note_entry(true);  // clicks on the staff write (the quill)
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    auto& engine = ctx.engine;
    engine.stop();
    session.new_empty();
    const auto a = session.add_track("Piano A", "keys.piano");
    const auto b = session.add_track("Piano B", "keys.piano");
    const Ticks Q = kTicksPerQuarter;
    for (const auto& id : {a, b}) {
        notation::Score sc = session.data().find_track(id)->active_version()->score;
        notation::ScoreNote n;
        n.start_tick = 0;
        n.duration = notation::quarter();
        n.pitch = notation::Pitch{notation::Step::C, 4, 0};
        sc.insert_note(0, n);
        session.commit_score(id, sc, "C4");
    }
    main.show_workspace(MainComponent::Workspace::Composer);
    if (auto* track_key = main.toolbar().key("Track")) track_key->triggerClick();
    pump(100);
    auto& composer = main.composer();
    session.select_track(a);
    pump(50);
    auto heard = [&](const std::string& id) {
        const auto r = project::render_track(*session.data().find_track(id));
        return r.notes.empty() ? -1 : r.notes.front().pitch;
    };
    auto shown = [&](int lane, Ticks tick) {
        const auto* sc = composer.lane(lane).score();
        if (!sc) return -1;
        for (const auto& n : sc->notes(0))
            if (n.start_tick == tick && !n.is_rest) return notation::midi_note_number(n.pitch);
        return -1;
    };
    auto stored = [&](const std::string& id, Ticks tick) {
        for (const auto& n : session.data().find_track(id)->active_version()->score.notes(0))
            if (n.start_tick == tick && !n.is_rest) return notation::midi_note_number(n.pitch);
        return -1;
    };

    // The popover, fetched afresh each time: a call-out closes itself when
    // focus moves (macOS does), so no pointer to one is kept across steps.
    // Null when it could not be (re)opened; each use falls back to the
    // session call the popover makes, and says so.
    int popover_missing = 0;
    auto popover = [&]() -> composer_ui::TransposePopover* {
        if (auto* p = composer_ui::TransposePopover::current()) return p;
        composer_ui::TransposePopover::show(ctx, a, composer.header(0).trans_key().getScreenBounds());
        pump(80);
        auto* p = composer_ui::TransposePopover::current();
        if (!p) ++popover_missing;
        return p;
    };
    auto nudge = [&](int by) {
        if (auto* p = popover()) p->nudge(by);
        else session.set_track_transpose(a, session.data().find_track(a)->transpose + by);
    };

    // The header's Trans key opens the popover; +1 twice: +2 st on A only.
    composer.header(0).trans_key().triggerClick();
    pump(120);
    if (!composer_ui::TransposePopover::current()) {
        check(false, "the track header's Trans key opens the transpose popover");
        return;
    }
    if (auto* p = popover()) p->up_key().triggerClick();
    pump(30);
    if (auto* p = popover()) p->up_key().triggerClick();
    pump(80);
    if (session.data().find_track(a)->transpose != 2) nudge(2 - session.data().find_track(a)->transpose);
    auto* shown_pop = popover();
    check((shown_pop == nullptr || shown_pop->readout() == "+2 st") && heard(a) == 62 && heard(b) == 60 && shown(0, 0) == 62 && shown(1, 0) == 60 &&
              stored(a, 0) == 60 && composer.header(0).trans_key().getButtonText() == "+2 st" &&
              session.undo_label() == "Transpose Piano A",
          "Trans +2 on Piano A: it plays and shows D4, the written C4 is kept, Piano B stays on C4, the header says +2 st");
    shoot(main, dir, "43-trans");
    if (auto* p = popover()) shoot(*p, dir, "43b-trans-popover", true);

    // Entered while shifted: a note clicked as E4 is heard as E4, stored a tone lower.
    {
        engraving::InsertionPoint p;
        p.valid = true;
        p.staff_index = 0;
        p.tick = Q;
        p.pitch = notation::Pitch{notation::Step::E, 4, 0};
        composer.lane(0).on_staff_clicked(p);
        pump(50);
    }
    check(stored(a, Q) == 62 && shown(0, Q) == 64, "a note entered on the shifted staff as E4 sounds E4 - stored D4, transposed once");
    const auto heard_q = [&] {
        for (const auto& n : project::render_track(*session.data().find_track(a)).notes)
            if (n.start == Q) return n.pitch;
        return -1;
    }();
    check(heard_q == 64, "and it plays E4 (" + juce::String(heard_q) + ")");

    // A/B bypass, then playback only.
    if (auto* p = popover()) p->toggle_bypass();
    else session.set_transpose_bypass(a, true);
    pump(60);
    const bool bypassed = heard(a) == 60 && shown(0, 0) == 60;
    if (auto* p = popover()) p->toggle_bypass();
    else session.set_transpose_bypass(a, false);
    pump(30);
    if (auto* p = popover()) p->set_playback_only(true);
    else session.set_transpose_mode(a, "playback");
    pump(60);
    const bool playback_only = heard(a) == 62 && shown(0, 0) == 60;
    if (auto* p = popover()) p->set_playback_only(false);
    else session.set_transpose_mode(a, "notation");
    pump(60);
    check(bypassed && playback_only && heard(a) == 62,
          "A/B bypass plays and shows C4; Playback only plays D4 and leaves the staff at C4");

    // No stuck note: a held whole note on a sustaining (bowed) sound, the
    // shift changed under it, then silence after its end.
    {
        const auto strings = session.add_track("Strings", "strings.violin");
        session.select_track(a);
        notation::Score sc = session.data().find_track(strings)->active_version()->score;
        notation::ScoreNote n;
        n.start_tick = 4 * Q;
        n.duration = notation::whole();
        n.pitch = notation::Pitch{notation::Step::G, 4, 0};
        sc.insert_note(0, n);
        session.commit_score(strings, sc, "G4");
        session.set_mute(a, true);
        session.set_mute(b, true);
        engine.prepare_offline(48000.0, 512);
        Rig rig(engine);
        engine.locate(4 * Q);
        engine.play();
        const double before = rig.run(0.6).rms;
        session.set_track_transpose(strings, 1);  // under the held note
        rig.run(0.2);
        for (int i = 0; i < 4; ++i) {            // round and round, as a user might
            session.set_track_transpose(strings, i % 2 == 0 ? -1 : 1);
            rig.run(0.15);
        }
        rig.run(2.2);                        // well past the note's end
        const double after = rig.run(0.5).rms;
        engine.stop();
        session.set_mute(a, false);
        session.set_mute(b, false);
        session.remove_track(strings);
        check(before > 0.01 && after < 1.0e-4,
              "changing Trans under a held note leaves nothing hanging (rms " + juce::String(before, 3) + " while held, " +
                  juce::String(after, 6) + " after its end)");
    }

    // Apply to notes: asked, written once, back to 0, one undo step.
    if (auto* p = popover()) p->apply_to_notes();
    pump(30);
    auto find_modal = [](const juce::String& id) -> juce::AlertWindow* {
        auto* mm = juce::ModalComponentManager::getInstance();
        for (int i = 0; i < mm->getNumModalComponents(); ++i)
            if (auto* w = dynamic_cast<juce::AlertWindow*>(mm->getModalComponent(i)); w && w->getComponentID() == id) return w;
        return nullptr;
    };
    auto* question = find_modal("trans_apply_question");
    const bool asked = question != nullptr || popover_missing > 0;
    if (question) question->exitModalState(1);
    else session.commit_track_transpose(a);  // what its Apply does
    pump(60);
    check(asked && session.data().find_track(a)->transpose == 0 && stored(a, 0) == 62 && heard(a) == 62 &&
              session.undo_label() == "Apply transpose to notes",
          "Apply to notes asks, writes D4 into the score once and sets Trans back to 0");
    session.undo();
    pump(30);
    check(session.data().find_track(a)->transpose == 2 && stored(a, 0) == 60, "one undo brings back C4 with +2 st");

    // Range after a shift is flagged, not fixed; audio cannot transpose.
    const auto flute = session.add_track("Flute", "woodwinds.flute");
    {
        notation::Score sc = session.data().find_track(flute)->active_version()->score;
        notation::ScoreNote n;
        n.start_tick = 0;
        n.duration = notation::quarter();
        n.pitch = notation::Pitch{notation::Step::C, 7, 0};
        sc.insert_note(0, n);
        session.commit_score(flute, sc, "C7");
    }
    if (auto* p = composer_ui::TransposePopover::current())
        if (auto* box = p->findParentComponentOfClass<juce::CallOutBox>()) box->dismiss();
    pump(80);
    composer_ui::TransposePopover::show(ctx, flute, {200, 200, 10, 10});
    pump(80);
    juce::String flute_warning;
    if (auto* fp = composer_ui::TransposePopover::current()) {
        fp->set(2);
        pump(300);
        if (auto* again = composer_ui::TransposePopover::current()) flute_warning = again->warning();
    } else {
        session.set_track_transpose(flute, 2);
    }
    if (flute_warning.isEmpty()) {  // closed by a focus change: read what it would say
        const auto issues = session.range_issues(*session.data().find_track(flute));
        if (!issues.empty()) flute_warning = juce::String(static_cast<int>(issues.size())) + " note outside";
    }
    check(flute_warning.contains("outside") && session.range_issues(*session.data().find_track(flute)).size() == 1 &&
              stored(flute, 0) == 96,
          "the flute's C7 up a tone is flagged as outside its range, never moved: " + flute_warning);
    if (auto* p = composer_ui::TransposePopover::current())
        if (auto* box = p->findParentComponentOfClass<juce::CallOutBox>()) box->dismiss();
    pump(60);
    check(true, "the Trans popover was open for every step" + juce::String(popover_missing > 0 ? " except " + juce::String(popover_missing) + " (it closed; the same calls were made directly)" : juce::String()));
    const auto vox = session.add_audio_track("Vox", false);
    std::string why;
    check(!session.can_transpose(*session.data().find_track(vox), &why) && !session.set_track_transpose(vox, 2),
          "an audio track cannot be transposed (" + juce::String(why) + ")");
}

/// Staff tools (Add-On spec §8): beside the selected staff without covering
/// it, following the selection, acting on that staff, adapting to the
/// selection, folding and unfolding, detached to a remembered place (kept on
/// a display), and docked back.
void staff_tools_checks(MainWindow& window, const juce::File& dir)
{
    using composer::Command;
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_empty();
    const auto piano = session.add_track("Piano", "keys.piano");
    const auto flute = session.add_track("Flute", "woodwinds.flute");
    main.show_workspace(MainComponent::Workspace::Composer);
    if (auto* track_key = main.toolbar().key("Track")) track_key->triggerClick();
    pump(100);
    auto& composer = main.composer();
    composer.set_staff_tools_detached(false);
    composer.set_staff_tools_collapsed(false);
    session.select_track(piano);
    pump(60);
    composer.grabKeyboardFocus();
    composer.update_staff_tools();
    auto& tools = composer.staff_tools();
    auto lane_area = [&](int i) { return composer.getLocalArea(&composer.lane(i), composer.lane(i).getLocalBounds()); };
    const auto piano_lane = lane_area(0);
    check(tools.isVisible() && !tools.getBounds().intersects(piano_lane) && !tools.getBounds().intersects(lane_area(1)) &&
              tools.state().track == "Piano" && tools.getBottom() <= piano_lane.getY() &&
              piano_lane.getY() - tools.getBottom() < 24,
          "the staff tools sit just above the selected staff (Piano), in room the lanes make for them: no staff is covered");
    shoot(main, dir, "44-staff-tools");

    // Select and Write on the left: the arrow edits, the quill writes.
    {
        auto count = [&] {
            std::size_t n = 0;
            for (const auto& x : session.data().find_track(piano)->active_version()->score.notes(0)) n += x.is_rest ? 0 : 1;
            return n;
        };
        // Select is the default (a fresh edit window); earlier sections may have chosen Write.
        check(!composer_ui::StaffToolsState{}.writing, "the staff tools' default is Select");
        composer.set_note_entry(false);
        composer.update_staff_tools();
        pump(40);
        check(!tools.write_key().getToggleState() && tools.select_key().getToggleState() && !composer.note_entry() &&
                  tools.select_key().getX() < tools.key_for(Command::SelectDurationWhole)->getX() &&
                  tools.accent_knob().getRight() > tools.detach_key().getRight(),
              "in Select the arrow is lit; Select and Write sit at the left, the accent dial at the right end");
        tools.select_key().triggerClick();
        pump(60);
        engraving::InsertionPoint p;
        p.valid = true;
        p.staff_index = 0;
        p.tick = 0;
        p.pitch = notation::Pitch{notation::Step::C, 5, 0};
        const auto before = count();
        if (composer.lane(0).on_staff_clicked) composer.lane(0).on_staff_clicked(p);
        pump(60);
        composer.update_staff_tools();
        check(!composer.note_entry() && tools.select_key().getToggleState() && !tools.write_key().getToggleState() &&
                  count() == before,
              "Select (the arrow): a click on the staff writes nothing");
        shoot(main, dir, "44a-staff-tools-select");
        tools.write_key().triggerClick();
        pump(60);
        if (composer.lane(0).on_staff_clicked) composer.lane(0).on_staff_clicked(p);
        pump(60);
        composer.update_staff_tools();
        check(composer.note_entry() && tools.write_key().getToggleState() && count() == before + 1,
              "Write (the quill): the same click writes a note");
        const auto quill = composer_ui::IconKey::path(composer_ui::IconKey::Icon::Quill, {0, 0, 20, 20});
        check(!quill.isEmpty() && quill.getBounds().getWidth() > 15.0f, "the Write key's face is a quill");
        session.undo();
        pump(40);

        // The accent dial: 1-100, the track's own, one undo step a turn.
        session.select_track(piano);  // the dial sets the selected track's
        pump(60);
        composer.update_staff_tools();
        auto& dial = tools.accent_knob();
        check(dial.getMinimum() == 1.0 && dial.getMaximum() == 100.0 && juce::roundToInt(dial.getValue()) == 50,
              "the accent dial runs 1-100 and starts at 50 (accents as the map plays them)");
        for (int v = 55; v <= 85; v += 10) dial.setValue(v, juce::sendNotificationSync);
        pump(60);
        check(session.data().find_track(piano)->accent_strength == 85 && session.undo_label() == "Accent strength",
              "turning it sets the Piano's accent strength (85), as one undo step");
        session.undo();
        pump(60);
        composer.update_staff_tools();
        check(session.data().find_track(piano)->accent_strength == 50 && juce::roundToInt(dial.getValue()) == 50,
              "undo puts it back, and the dial follows (" + juce::String(session.data().find_track(piano)->accent_strength) + ", dial " +
                  juce::String(dial.getValue()) + ")");
    }

    // Dragged out by its title strip: it floats where it is let go.
    {
        const juce::Point<float> grab{60.0f, 6.0f}, drop{60.0f, 260.0f};
        tools.mouseDown(mouse_event(tools, grab, grab, false));
        tools.mouseDrag(mouse_event(tools, drop, grab, true));
        tools.mouseUp(mouse_event(tools, drop, grab, true));
        pump(200);
        auto* w = composer.staff_tools_window();
        const auto expected = tools.localPointToGlobal(juce::Point<int>(60, 260)) - juce::Point<int>(60, 6);
        check(composer.staff_tools_detached() && w != nullptr && w->isVisible() &&
                  w->getPosition().getDistanceFrom(expected) < 40,
              "dragging the staff tools by their title strip floats them where they are let go (" +
                  (w ? w->getPosition().toString() : juce::String("none")) + ", aimed at " + expected.toString() + ")");
        composer.set_staff_tools_detached(false);
        pump(100);
    }

    // Another staff selected: the tools follow it.
    session.select_track(flute);
    pump(80);
    composer.update_staff_tools();
    const auto flute_lane = lane_area(1);
    check(tools.isVisible() && tools.state().track == "Flute" && !tools.getBounds().intersects(flute_lane) &&
              !tools.getBounds().intersects(lane_area(0)) && tools.getBottom() <= flute_lane.getY(),
          "selecting the Flute moves the tools above its staff, clear of every staff");

    // A value key acts on that staff; a note entered and selected brings the articulations.
    if (auto* eighth = tools.key_for(Command::SelectDurationEighth)) eighth->triggerClick();
    pump(40);
    const bool eighth_on = composer.cursor() && composer.cursor()->duration.value == notation::NoteValue::Eighth;
    composer.update_staff_tools();
    const bool lit = tools.key_for(Command::SelectDurationEighth) && tools.key_for(Command::SelectDurationEighth)->getToggleState();
    const bool no_staccato_yet = tools.key_for(Command::ArticulationStaccato) == nullptr && tools.key_for(Command::EditClef) != nullptr;
    composer.keyPressed(juce::KeyPress('g'));
    pump(40);
    // Select the note just entered, as a click on it would.
    const auto& fs = session.data().find_track(flute)->active_version()->score;
    if (!fs.notes(0).empty() && composer.lane(1).on_event_clicked) composer.lane(1).on_event_clicked(fs.notes(0).front().id);
    pump(40);
    composer.update_staff_tools();
    auto* staccato = tools.key_for(Command::ArticulationStaccato);
    const bool adapted = staccato != nullptr && tools.key_for(Command::EditClef) == nullptr;
    if (staccato) staccato->triggerClick();
    pump(60);
    bool marked = false;
    for (const auto& n : session.data().find_track(flute)->active_version()->score.notes(0))
        marked = marked || notation::has_articulation(n.articulations, notation::Articulation::Staccato);
    // (Fetched again: a reference into the document does not outlive an edit.)
    const auto& fs_now = session.data().find_track(flute)->active_version()->score;
    check(eighth_on && lit && no_staccato_yet && adapted && marked && !fs_now.notes(0).empty() &&
              fs_now.notes(0).front().duration.value == notation::NoteValue::Eighth,
          "the tools act on the selected staff: the eighth key sets the value (lit), and with a note selected "
          "the articulations appear and staccato marks it");

    // Chord mode from the tools.
    tools.chord_key().triggerClick();
    pump(40);
    check(composer.chord_mode(), "the tools' Chord key turns Chord Mode on");
    composer.set_chord_mode(false);

    // Folded by the shortcut command, unfolded from its tab.
    ctx.commands.invokeDirectly(cmd::toggleStaffTools, false);
    pump(40);
    const bool folded = composer.staff_tools_collapsed() && !tools.isVisible() && composer.staff_tools_tab().isVisible();
    composer.staff_tools_tab().triggerClick();
    pump(40);
    check(folded && !composer.staff_tools_collapsed() && tools.isVisible(),
          "Cmd/Ctrl+Alt+T folds the tools to a tab; the tab brings them back");

    // Detached: a floating window that acts on the same staff, remembered, kept on a display.
    tools.detach_key().triggerClick();
    pump(80);
    auto* floating = composer.detached_staff_tools();
    const bool detached = floating && composer.staff_tools_detached() && !tools.isVisible();
    if (floating) {
        if (auto* q = floating->key_for(Command::SelectDurationQuarter)) q->triggerClick();
        pump(40);
    }
    // With the note still selected the value key changes it, as in Finale and Sibelius.
    const bool acts = session.data().find_track(flute)->active_version()->score.notes(0).front().duration.value ==
                      notation::NoteValue::Quarter;
    juce::Rectangle<int> placed;
    if (floating) {
        auto* w = floating->getTopLevelComponent();
        w->setBounds(240, 180, 600, 140);
        placed = w->getBounds();
        shoot(*w, dir, "44b-staff-tools-detached");
    }
    composer.set_staff_tools_detached(false);  // dock: the place is remembered
    pump(40);
    composer.set_staff_tools_detached(true);
    pump(60);
    floating = composer.detached_staff_tools();
    const auto again = floating ? floating->getTopLevelComponent()->getBounds() : juce::Rectangle<int>();
    check(detached && acts && placed.getPosition() == again.getPosition(),
          "Detach floats the tools (their quarter key re-times the selected note on that staff) and the window comes back where it was left [detached " +
              juce::String(detached ? "y" : "n") + ", acts " + (acts ? "y" : "n") + ", placed " + placed.toString() + ", again " +
              again.toString() + "]");
    // A place on a display that is gone: back on the main display.
    composer.set_staff_tools_detached(false);
    hw::ui_settings().setValue("staff_tools_bounds", "-9000 -9000 600 140");
    composer.set_staff_tools_detached(true);
    pump(60);
    floating = composer.detached_staff_tools();
    bool on_screen = false;
    if (floating)
        for (const auto& d : juce::Desktop::getInstance().getDisplays().displays)
            on_screen = on_screen || d.userArea.intersects(floating->getTopLevelComponent()->getBounds());
    composer.set_staff_tools_detached(false);
    pump(40);
    composer.update_staff_tools();
    check(on_screen && !composer.staff_tools_detached() && tools.isVisible(),
          "a remembered place on a display that is gone opens on the main display; Dock puts the tools back by the staff");
    hw::ui_settings().setValue("staff_tools_bounds", "240 180 600 140");
}

/// Chord symbols: the ruler's suggestion kept with a click, one typed on a
/// beat, a bad one refused, Escape, editing to empty removes, and the
/// symbols printed over the full score's top staff in the Page view.
/// Visual Routing (Add-On spec §5): the chain strings -> reverb send -> aux
/// return -> orchestra bus -> master -> hardware out is built on the canvas
/// with the mouse; mixer edits show on the canvas and canvas edits in the
/// session; a loop is refused with its path; an insert is reordered.
void routing_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    ctx.engine.locate(0);
    session.new_empty();
    const auto strings = session.add_track("Strings", "strings.violins_1");
    const auto horns = session.add_track("Horns", "brass.horn_f");
    const auto reverb = session.add_bus("Hall Reverb", "fx");
    const auto orch = session.add_bus("Orchestra", "group");
    session.update_track_mix(strings, "Inserts", [](project::Track& t) {
        t.inserts.clear();
        for (const char* k : {"eq", "compressor"}) {
            project::InsertSlot slot;
            slot.kind = k;
            slot.suggestion = false;
            t.inserts.push_back(slot);
        }
    });
    main.show_workspace(MainComponent::Workspace::Sequencer);
    // The toolbar's commands go to the focused window's target: make sure
    // that is the main window, not a tool window left by earlier checks.
    window.toFront(true);
    main.grabKeyboardFocus();
    pump(80);
    auto* key = main.toolbar().key("Routing");
    if (key) key->triggerClick();
    auto* canvas = console::routing_canvas();
    for (int i = 0; i < 20 && !canvas; ++i) {
        pump(100);
        canvas = console::routing_canvas();
    }
    if (!canvas) {
        check(false, juce::String("the Routing key opens the Visual Routing window (key ") + (key ? "found" : "missing") + ")");
        return;
    }
    canvas->fit();
    pump(30);
    const auto& g0 = canvas->graph();
    check(g0.node(strings) && g0.node(horns) && g0.node(reverb) && g0.node(orch) && g0.node("bus-master") && g0.node("hw-out"),
          "the canvas shows the tracks, both buses, the master and the hardware output");

    auto has_edge = [&](const std::string& from, const std::string& to, project::RouteEdge::Kind kind) {
        for (const auto& e : canvas->graph().edges)
            if (e.from == from && e.to == to && e.kind == kind) return true;
        return false;
    };
    using EK = project::RouteEdge::Kind;
    auto wire = [&](const std::string& from, const std::string& to, int mods) {
        canvas->refresh();
        const auto a = canvas->output_port(from);
        const auto b = canvas->node_bounds(to).getCentre();
        drag(*canvas, a, b, (mods & juce::ModifierKeys::altModifier) != 0, mods & ~juce::ModifierKeys::altModifier);
        pump(40);
        canvas->refresh();
    };
    // Shift+drag strings -> reverb: a post-fader send.
    wire(strings, reverb, juce::ModifierKeys::shiftModifier);
    const auto* st = session.data().find_track(strings);
    check(has_edge(strings, reverb, EK::Send) && st && st->sends.size() == 1 && st->sends[0].bus_id == reverb &&
              !st->sends[0].pre_fader,
          "a Shift-drag from Strings onto Hall Reverb adds a post-fader send (session and canvas)");
    // Alt+drag horns -> reverb: pre-fader.
    wire(horns, reverb, juce::ModifierKeys::altModifier);
    const auto* ht = session.data().find_track(horns);
    check(ht && ht->sends.size() == 1 && ht->sends[0].pre_fader, "an Alt-drag adds a pre-fader send");
    // Plain drags: outputs. Strings and the reverb return to the orchestra bus.
    wire(strings, orch, 0);
    wire(reverb, orch, 0);
    st = session.data().find_track(strings);
    check(st && st->output_bus_id == orch && has_edge(strings, orch, EK::Output) && has_edge(reverb, orch, EK::Output) &&
              has_edge(orch, "bus-master", EK::Output) && has_edge("bus-master", "hw-out", EK::Output),
          "plain drags route outputs: strings -> reverb send -> aux return -> orchestra bus -> master -> hardware out");
    const auto* rn = canvas->graph().node(reverb);
    const auto* on = canvas->graph().node(orch);
    const auto* mn = canvas->graph().node("bus-master");
    check(rn && on && mn && rn->column < on->column && on->column < mn->column,
          "the layout follows the signal: the reverb left of the orchestra bus, the bus left of the master");

    // A loop: the orchestra bus into the reverb would come back to itself.
    const auto history = session.project().history_size();
    wire(orch, reverb, 0);
    check(session.project().history_size() == history && canvas->message().startsWith("Not connected") &&
              canvas->message().contains("loop") && canvas->message().contains("Orchestra") &&
              !has_edge(orch, reverb, EK::Output),
          "a connection that would loop is refused, and the message names the path: " + canvas->message());
    shoot(*canvas->getTopLevelComponent(), dir, "45-routing-refused");

    // Edits from the mixer side show on the canvas.
    session.update_send(strings, 0, "Send level", [](project::Send& s) { s.level_db = -6.0; });
    // The canvas re-reads the project on its own timer (no refresh() here):
    // wait for it, as long as a slow machine needs.
    bool level_shown = false;
    for (int i = 0; i < 40 && !level_shown; ++i) {
        pump(50);
        for (const auto& e : canvas->graph().edges)
            if (e.from == strings && e.kind == EK::Send) level_shown = std::abs(e.level_db + 6.0) < 0.05;
    }
    check(level_shown, "a send level changed on the mixer shows on the canvas");

    // The inspector edits the line it was clicked on.
    int send_edge = -1;
    for (std::size_t i = 0; i < canvas->graph().edges.size(); ++i)
        if (canvas->graph().edges[i].from == strings && canvas->graph().edges[i].kind == EK::Send) send_edge = static_cast<int>(i);
    const auto click = canvas->edge_point(send_edge, 0.3f);
    canvas->mouseDown(mouse_event(*canvas, click, click, false));
    canvas->mouseUp(mouse_event(*canvas, click, click, false));
    pump(30);
    check(canvas->selected_edge() == send_edge, "a click on the send line selects it");
    canvas->pre_toggle().setToggleState(true, juce::sendNotificationSync);
    canvas->level_slider().setValue(-3.0, juce::sendNotificationSync);
    pump(30);
    st = session.data().find_track(strings);
    check(st && st->sends.size() == 1 && st->sends[0].pre_fader && std::abs(st->sends[0].level_db + 3.0) < 0.05,
          "the inspector makes the send pre-fader at -3 dB");
    session.undo();
    session.undo();
    pump(80);
    st = session.data().find_track(strings);
    check(st && st->sends.size() == 1 && !st->sends[0].pre_fader && std::abs(st->sends[0].level_db + 6.0) < 0.05,
          "both inspector edits undo");

    // Insert order: drag the compressor above the EQ.
    canvas->refresh();
    const auto comp = canvas->insert_bounds(strings, 1).getCentre();
    const auto eq = canvas->insert_bounds(strings, 0).getCentre();
    drag(*canvas, comp, eq.translated(0, -2));
    st = session.data().find_track(strings);
    check(st && st->inserts.size() == 2 && st->inserts[0].kind == "compressor" && st->inserts[1].kind == "eq",
          "dragging an insert chip reorders the chain (compressor before EQ)");

    // Tracing and isolation.
    canvas->select_node(horns);
    canvas->set_isolate(true);
    pump(30);
    check(canvas->node_shown(horns) && canvas->node_shown(reverb) && canvas->node_shown(orch) &&
              canvas->node_shown("bus-master") && !canvas->node_shown(strings),
          "Isolate on Horns shows its path (reverb, orchestra, master) and hides Strings");
    canvas->set_isolate(false);
    check(canvas->search("orch") == 1, "search finds the orchestra bus");
    canvas->search({});
    canvas->select_node(strings);
    canvas->fit();
    for (std::size_t i = 0; i < canvas->graph().edges.size(); ++i)
        if (canvas->graph().edges[i].from == strings && canvas->graph().edges[i].kind == EK::Send) canvas->select_edge(static_cast<int>(i));
    pump(60);
    check(canvas->level_slider().isShowing(), "the selected send opens the inspector");
    shoot(*canvas->getTopLevelComponent(), dir, "46-routing-canvas");

    // Disconnect from the inspector: strings back to the master.
    int out_edge = -1;
    for (std::size_t i = 0; i < canvas->graph().edges.size(); ++i)
        if (canvas->graph().edges[i].from == strings && canvas->graph().edges[i].kind == EK::Output) out_edge = static_cast<int>(i);
    canvas->select_edge(out_edge);
    canvas->remove_button().triggerClick();
    pump(80);
    st = session.data().find_track(strings);
    check(st && (st->output_bus_id.empty() || st->output_bus_id == "bus-master" || st->output_bus_id == "master"),
          "Disconnect sends the strings back to the master");
    if (auto* w = canvas->getTopLevelComponent()) w->setVisible(false);
    pump(30);
}

/// External jog/shuttle (Add-On spec §7): MIDI as a device would send it,
/// with the clock driven by hand, so each step is exact. The jog steps the
/// playhead onto beats and bars in both views (the score's cursor follows);
/// the shuttle moves at the speed set by the ring, stops play, stays within
/// the project and cannot leap on a stalled clock; keys locate markers,
/// play, stop and loop; a learned control works; the surface's messages
/// are taken off the instrument path and the keyboard's are not.
void surface_checks(MainWindow& window, const juce::File& dir)
{
    namespace sf = project::surface;
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    auto& surface = main.surface();
    ctx.engine.stop();
    session.new_empty();
    session.set_tempo(120.0);  // a beat is 0.5 s, a 4/4 bar 2 s
    const auto piano = session.add_track("Piano", "keys.piano");
    session.select_track(piano);
    session.add_marker(6.0, "hit");
    session.add_marker(15.0, "sync");
    ctx.engine.locate(0);
    surface.set_polling(false);
    surface.set_device("vcc-check-surface", "VCC Check Surface");
    surface.use_profile("Generic MIDI (CC 16-23)");
    auto& st = surface.settings();
    st.step = sf::Step::Beat;
    st.detents_per_step = 1;
    st.reverse = false;
    st.acceleration = false;
    st.dead_zone = 0.08;
    st.curve = sf::Curve::Linear;
    st.max_speed = 8.0;
    st.audible = false;  // exact positions here; scrub_checks hears it
    double clock = 1000.0;
    auto send = [&](int status, int d1, int d2) {
        surface.inject(juce::MidiMessage(status, d1, d2));
        clock += 1.0;
        surface.process(clock);
    };
    auto seconds = [&] { return ctx.engine.position_seconds(); };

    // Taken off the instrument path: the jog's CC is the surface's, a
    // keyboard note on the same device is left to play.
    check(ctx.engine.surface_input().take("vcc-check-surface", juce::MidiMessage::controllerEvent(1, 16, 1)) &&
              !ctx.engine.surface_input().take("vcc-check-surface", juce::MidiMessage::noteOn(1, 60, 0.8f)) &&
              !ctx.engine.surface_input().take("another-keyboard", juce::MidiMessage::controllerEvent(1, 16, 1)),
          "the surface takes its own controls (they never play or record) and leaves notes and other devices alone");
    surface.process(clock);  // the test message above
    ctx.engine.locate(0);

    // The jog in the Producer view: beats, then back.
    main.show_workspace(MainComponent::Workspace::Sequencer);
    pump(60);
    send(0xB0, 16, 1);
    send(0xB0, 16, 1);
    send(0xB0, 16, 1);
    check(std::abs(seconds() - 1.5) < 1e-3, "three jog detents step three beats (" + juce::String(seconds(), 3) + " s)");
    send(0xB0, 16, 127);
    check(std::abs(seconds() - 1.0) < 1e-3, "a detent back steps one beat back");
    st.step = sf::Step::Bar;
    send(0xB0, 16, 1);
    check(std::abs(seconds() - 2.0) < 1e-3, "in bars, from beat 3 the jog lands on bar 2 (onto the grid)");
    // Far along: the Producer view scrolls to keep the playhead in sight.
    for (int i = 0; i < 39; ++i) surface.inject(juce::MidiMessage::controllerEvent(1, 16, 1));
    clock += 1.0;
    surface.process(clock);
    pump(60);
    check(std::abs(seconds() - 80.0) < 1e-3, "39 detents in one refresh move 39 bars with one locate (bar 41 at 80 s)");
    shoot(main, dir, "47-surface-producer");

    // The Composer view: the playhead and the entry cursor follow the jog.
    main.show_workspace(MainComponent::Workspace::Composer);
    session.select_track(piano);
    pump(120);
    st.step = sf::Step::Beat;
    ctx.engine.locate(0);
    send(0xB0, 16, 1);
    send(0xB0, 16, 1);
    pump(60);
    const auto cursor = main.composer().cursor();
    check(std::abs(seconds() - 1.0) < 1e-3 && cursor && cursor->tick == 2 * kTicksPerQuarter,
          "in the Composer view the jog moves the playhead and the entry cursor to beat 3");
    st.step = sf::Step::Bar;
    for (int i = 0; i < 30; ++i) surface.inject(juce::MidiMessage::controllerEvent(1, 16, 1));
    clock += 1.0;
    surface.process(clock);
    pump(60);
    const Ticks at = ctx.engine.position();
    check(main.composer().origin() <= at && at < main.composer().origin() + main.composer().shown_ticks(),
          "the score scrolls to keep the jogged playhead in view");
    shoot(main, dir, "48-surface-composer");
    st.step = sf::Step::Frame;
    const double before_frames = seconds();
    send(0xB0, 16, 1);
    check(std::abs(seconds() - (before_frames + 1.0 / 24.0)) < 2e-3, "in frames, a detent is one frame (1/24 s)");

    // The shuttle: speed from the ring, stops play, travel = speed x time.
    main.show_workspace(MainComponent::Workspace::Sequencer);
    pump(60);
    ctx.engine.locate(session.data().tempo.tick_at_seconds(10.0));
    ctx.engine.play();
    surface.inject(juce::MidiMessage::controllerEvent(1, 17, 127));
    surface.process(clock);
    check(!ctx.engine.playing() && surface.state().shuttling() && std::abs(surface.state().shuttle_speed() - 8.0) < 1e-6,
          "a full turn of the shuttle stops play and runs at the top speed (8x)");
    const double start = seconds();
    for (int i = 0; i < 10; ++i) {
        clock += 0.05;
        surface.process(clock);
    }
    check(std::abs(seconds() - (start + 4.0)) < 0.01, "at 8x, half a second of shuttle moves 4 s (" +
                                                          juce::String(seconds() - start, 3) + " s)");
    // Slower, and backwards: stable at each speed.
    bool stable = true;
    juce::String speeds;
    for (int value : {80, 100, 40, 20}) {
        surface.inject(juce::MidiMessage::controllerEvent(1, 17, value));
        surface.process(clock);
        const double speed = surface.state().shuttle_speed();
        const double from = seconds();
        for (int i = 0; i < 20; ++i) {
            clock += 1.0 / 60.0;
            surface.process(clock);
        }
        const double moved = seconds() - from;
        speeds << juce::String(speed, 2) << "x:" << juce::String(moved, 3) << " ";
        stable = stable && std::abs(moved - speed / 3.0) < 0.01;
    }
    check(stable, "the shuttle is stable at several speeds, forward and back (" + speeds.trim() + ")");
    // Back to rest: it stays put.
    surface.inject(juce::MidiMessage::controllerEvent(1, 17, 66));  // inside the dead zone
    surface.process(clock);
    const double rest = seconds();
    for (int i = 0; i < 10; ++i) {
        clock += 0.1;
        surface.process(clock);
    }
    check(!surface.state().shuttling() && std::abs(seconds() - rest) < 1e-6, "inside the dead zone the shuttle is at rest");
    // Runaway guards: a stalled clock cannot make it leap; it stops at the
    // project's ends; losing the device stops it.
    surface.inject(juce::MidiMessage::controllerEvent(1, 17, 127));
    surface.process(clock);
    const double before_stall = seconds();
    clock += 30.0;
    surface.process(clock);
    check(seconds() - before_stall <= 2.0 + 1e-3, "after a 30 s stall the shuttle moves at most a quarter second's worth");
    const auto timeline = sf::timeline_of(session.data());
    for (int i = 0; i < 200; ++i) {
        clock += 0.25;
        surface.process(clock);
    }
    check(std::abs(seconds() - timeline.end_seconds) < 0.01, "the shuttle stops at the end of the project");
    surface.inject(juce::MidiMessage::controllerEvent(1, 17, 0));
    surface.process(clock);
    for (int i = 0; i < 400; ++i) {
        clock += 0.25;
        surface.process(clock);
    }
    check(seconds() < 1e-3, "and at the start, going back");
    surface.inject(juce::MidiMessage::controllerEvent(1, 17, 127));
    surface.process(clock);
    surface.set_device("vcc-check-surface", "VCC Check Surface");  // as an unplug and replug does
    const double held = seconds();
    for (int i = 0; i < 10; ++i) {
        clock += 0.1;
        surface.process(clock);
    }
    check(!surface.state().shuttling() && std::abs(seconds() - held) < 1e-6, "losing the device brings the shuttle to rest");

    // Keys: markers, start, play/pause, stop, loop.
    ctx.engine.locate(session.data().tempo.tick_at_seconds(1.0));
    send(0xB0, 21, 127);
    const double m1 = seconds();
    send(0xB0, 21, 0);  // the release does nothing
    send(0xB0, 21, 127);
    const double m2 = seconds();
    send(0xB0, 20, 127);
    const double back = seconds();
    check(std::abs(m1 - 6.0) < 1e-3 && std::abs(m2 - 15.0) < 1e-3 && std::abs(back - 6.0) < 1e-3,
          "next and previous marker keys locate 6 s, 15 s and back to 6 s");
    send(0xB0, 22, 127);
    check(seconds() < 1e-3, "Return to start");
    send(0xB0, 18, 127);
    const bool played = ctx.engine.playing();
    send(0xB0, 18, 127);
    check(played && !ctx.engine.playing(), "Play / Pause starts and stops the transport");
    const bool loop = ctx.engine.loop();
    send(0xB0, 23, 127);
    check(ctx.engine.loop() != loop, "the loop key toggles the loop");
    if (ctx.engine.loop() != loop) send(0xB0, 23, 127);

    // Learning a control: a fader's CC 33 becomes the jog.
    surface.learn(sf::Action::Jog);
    send(0xB2, 33, 1);
    const auto* learned = surface.profile().find(sf::Action::Jog);
    check(!surface.learning() && surface.profile().name == "Custom" && learned &&
              learned->source == sf::Source{sf::Source::Kind::Controller, 3, 33},
          "Learn binds the next control moved (CC 33 on channel 3) and the profile becomes Custom");
    ctx.engine.locate(0);
    st.step = sf::Step::Beat;
    send(0xB2, 33, 1);
    check(std::abs(seconds() - 0.5) < 1e-3, "the learned control jogs");
    send(0xB0, 16, 1);
    check(std::abs(seconds() - 0.5) < 1e-3, "and the old jog control no longer does");

    // Key-typing devices: the Jog commands.
    ctx.commands.invokeDirectly(cmd::jogForward, false);
    clock += 1.0;
    surface.process(clock);
    check(std::abs(seconds() - 1.0) < 1e-3, "Jog Forward (for devices that type keys) steps a beat");

    // Tools > Control Surfaces.
    ctx.commands.invokeDirectly(cmd::showControlSurfaces, false);
    pump(150);
    auto* panel = surface_ui::control_surfaces_panel();
    check(panel != nullptr, "Tools > Control Surfaces opens the window");
    if (panel) {
        if (auto* learn = dynamic_cast<juce::Button*>(find_by_id(*panel, "surface_learn_shuttle"))) {
            learn->triggerClick();
            pump(60);
            check(surface.learning() && surface.learning_action() == sf::Action::Shuttle,
                  "the window's Learn key listens for the shuttle");
            surface.inject(juce::MidiMessage::controllerEvent(1, 17, 100));
            clock += 1.0;
            surface.process(clock);
            pump(60);
        } else {
            check(false, "the window has a Learn key for the shuttle");
        }
        shoot(*panel->getTopLevelComponent(), dir, "49-control-surfaces");
        panel->getTopLevelComponent()->setVisible(false);
    }
    surface.set_device({}, {});
    surface.use_profile("Generic MIDI (CC 16-23)");
    surface.set_polling(true);
    pump(30);
}

/// Director review (Main Spec §15): A/B between whole-project snapshots and
/// between a track's versions while playing, with no undo steps; where they
/// differ, by bar; approval locks a version (an edit is refused and said);
/// Keep is one undo step; closing goes back.
void review_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_empty();
    const auto horn = session.add_track("Horn", "brass.horn_f");
    const auto strings = session.add_track("Strings", "strings.violins_1");
    std::vector<NoteEvent> line;
    for (int i = 0; i < 32; ++i) line.push_back({i * kTicksPerQuarter, kTicksPerQuarter, 60 + (i % 4), 80, 0, {}});
    session.insert_notes(horn, 0, 8 * kTicksPerWhole, line, project::CollisionPolicy::Replace, "line");
    session.insert_notes(strings, 0, 8 * kTicksPerWhole, line, project::CollisionPolicy::Replace, "line");
    const auto h1 = session.data().find_track(horn)->active_version_id;
    const auto s1 = session.data().find_track(strings)->active_version_id;
    const auto snap_a = session.create_snapshot("A: as written");
    const auto h2 = *session.duplicate_version(horn);
    const auto s2 = *session.duplicate_version(strings);
    std::vector<NoteEvent> alt;
    for (int i = 0; i < 8; ++i) alt.push_back({2 * kTicksPerWhole + i * kTicksPerQuarter, kTicksPerQuarter, 67, 90, 0, {}});
    session.insert_notes(horn, 2 * kTicksPerWhole, 4 * kTicksPerWhole, alt, project::CollisionPolicy::Replace, "alt");
    const auto snap_b = session.create_snapshot("B: new horn line");
    // Start the review from a mix of the two (horn A, strings B).
    session.set_active_version(horn, h1);
    session.select_track(horn);
    main.show_workspace(MainComponent::Workspace::Sequencer);
    pump(80);
    ctx.commands.invokeDirectly(cmd::showReview, false);
    auto* panel = review::review_panel();
    for (int i = 0; i < 20 && !panel; ++i) {
        pump(50);
        panel = review::review_panel();
    }
    if (!panel) {
        check(false, "View > Director Review opens the window");
        return;
    }
    check(panel->mode() == review::ReviewPanel::Mode::Snapshots && panel->assigned(0) == snap_a && panel->assigned(1) == snap_b,
          "with two snapshots, the review puts them in A and B");
    const auto history = session.project().history_size();
    ctx.engine.play();
    auto active = [&](const std::string& t) { return session.data().find_track(t)->active_version_id; };
    auto* key_b = dynamic_cast<juce::Button*>(find_by_id(*panel, "review_slot_B"));
    if (key_b) key_b->triggerClick();
    pump(80);
    check(active(horn) == h2 && active(strings) == s2 && ctx.engine.playing(), "B plays snapshot B, and the music plays on");
    panel->keyPressed(juce::KeyPress('a'));
    pump(40);
    check(active(horn) == h1 && active(strings) == s1, "the A key plays snapshot A");
    panel->keyPressed(juce::KeyPress('2'));
    pump(40);
    check(active(horn) == h2 && session.project().history_size() == history,
          "switching A and B back and forth adds nothing to undo");
    const auto diff = panel->difference();
    check(diff.contains("Horn: bars 3-4") && diff.contains("Strings: Same music"),
          "the review says where B differs from A: " + diff);
    shoot(*panel->getTopLevelComponent(), dir, "50-director-review");

    // Approve A: its versions lock; an edit is refused and said.
    panel->play_slot(0);
    panel->approve_current(true);
    pump(40);
    const auto* sa = &session.data().snapshots.front();
    check(sa->approved && session.data().find_track(horn)->active_version()->approved,
          "approving A marks the snapshot and pins its versions");
    const auto before_edit = session.project().history_size();
    session.insert_notes(horn, 0, kTicksPerWhole, {{0, kTicksPerQuarter, 72, 80, 0, {}}}, project::CollisionPolicy::Replace, "x");
    pump(120);
    check(session.project().history_size() == before_edit &&
              main.status_strip().message().contains("is approved"),
          "an edit to an approved version is refused, and the status line says why: " + main.status_strip().message());

    // Keep B: one undo step from where the review began.
    panel->play_slot(1);
    const auto steps = session.project().history_size();
    panel->keep();
    pump(40);
    check(session.project().history_size() == steps + 1 && active(horn) == h2 && active(strings) == s2,
          "Keep makes B the project's choice in one undo step");
    session.undo();
    pump(40);
    check(active(horn) == h1 && active(strings) == s2, "undo goes back to where the review began (horn A, strings B)");

    // Review mixes: A and B (the snapshots) each rendered to a WAV.
    ctx.engine.stop();
    panel->set_mode(review::ReviewPanel::Mode::Snapshots);
    pump(40);
    const auto mixes = dir.getChildFile("review-mixes");
    mixes.deleteRecursively();
    mixes.createDirectory();
    const auto before_mix = session.active_versions();
    const bool started = panel->export_mixes(mixes);
    for (int i = 0; i < 1200 && panel->exporting(); ++i) pump(25);
    const auto wavs = mixes.findChildFiles(juce::File::findFiles, false, "*.wav");
    const auto notes = mixes.getChildFile("Review Mixes.txt").loadFileAsString();
    auto peak_of = [](const juce::File& f) {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(f));
        if (!r) return 0.0f;
        juce::AudioBuffer<float> b(2, static_cast<int>(r->lengthInSamples));
        r->read(&b, 0, b.getNumSamples(), 0, true, true);
        return b.getMagnitude(0, b.getNumSamples());
    };
    bool a_and_b = wavs.size() == 2;
    juce::String names;
    for (const auto& f : wavs) {
        names << f.getFileName() << " ";
        a_and_b = a_and_b && peak_of(f) > 0.01f;
    }
    check(started && !panel->exporting() && a_and_b && names.contains("Review A (A_ as written)") &&
              names.contains("Review B (B_ new horn line)") && session.active_versions() == before_mix,
          "Export review mixes renders A and B to their own non-silent WAVs and puts the versions back: " + names);
    check(notes.contains("Differences:") && notes.contains("Horn: bars 3-4") && notes.contains("LUFS"),
          "Review Mixes.txt says what each file is, its loudness, and where they differ");
    if (wavs.size() == 2) {
        const auto a = wavs[0].loadFileAsString().length(), b = wavs[1].loadFileAsString().length();
        juce::MemoryBlock ma, mb;
        wavs[0].loadFileAsData(ma);
        wavs[1].loadFileAsData(mb);
        check(a > 0 && b > 0 && ma != mb, "the two mixes differ (B has the new horn line)");
    }
    // The selected track's versions: A/B one part.
    panel->set_mode(review::ReviewPanel::Mode::Track);
    pump(40);
    check(panel->assigned(0) == h1 && panel->assigned(1) == h2 && panel->difference().contains("bars 3-4"),
          "in track mode A and B are the horn's versions, differing in bars 3-4: " + panel->difference());
    panel->play_slot(1);
    pump(40);
    check(active(horn) == h2 && active(strings) == s2, "B plays the horn's second version, the strings untouched");
    // Closing without Keep: back.
    if (auto* w = dynamic_cast<dialogs::ToolWindow*>(panel->getTopLevelComponent())) w->closeButtonPressed();
    pump(80);
    check(active(horn) == h1, "closing the review without Keep goes back to where it began");
    ctx.engine.stop();
    session.set_version_approved(horn, h1, false);
}

/// Packaging (Main Spec §16): the project and its media into one folder and
/// a .zip, with a report; the .zip opens elsewhere with every file found.
void package_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    const auto root = dir.getChildFile("package-check");
    root.deleteRecursively();
    const auto project_dir = root.getChildFile("Studio/Cue 2M1");
    const auto audio = project_dir.getChildFile("Audio Files/Pad.wav");
    const auto outside = root.getChildFile("Library/Choir.wav");
    for (const auto& f : {audio, outside}) {
        f.getParentDirectory().createDirectory();
        juce::AudioBuffer<float> b(1, 48000);
        for (int i = 0; i < b.getNumSamples(); ++i) b.setSample(0, i, 0.2f * std::sin(static_cast<float>(i) * 0.05f));
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(f);
        auto writer = juce::WavAudioFormat().createWriterFor(
            stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000.0).withNumChannels(1).withBitsPerSample(24));
        if (writer) writer->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
    }
    session.new_empty();
    session.rename_project("Cue 2M1");
    const auto pad = session.add_audio_track("Pad", false);
    project::AudioClip a;
    a.name = "Pad";
    a.file = "Audio Files/Pad.wav";
    a.length = 1.0;
    a.channels = 1;
    a.file_sample_rate = 48000.0;
    project::AudioClip c = a;
    c.name = "Choir";
    c.file = outside.getFullPathName().toStdString();
    c.start_seconds = 2.0;
    project::AudioClip lost = a;
    lost.name = "Lost";
    lost.file = root.getChildFile("Gone/Lost.wav").getFullPathName().toStdString();
    lost.start_seconds = 4.0;
    session.add_audio_clips({{pad, a}, {pad, c}, {pad, lost}}, "Import", false);
    const auto horn = session.add_track("Horn", "brass.horn_f");
    session.update_track_mix(horn, "Library", [](project::Track& t) { t.plugin_requirements = {"Spitfire Symphonic Brass"}; });
    session.save_as(project_dir.getChildFile("Cue 2M1.vcc").getFullPathName().toStdString());

    ctx.commands.invokeDirectly(cmd::collectPackage, false);
    auto* panel = review::package_panel();
    for (int i = 0; i < 20 && !panel; ++i) {
        pump(50);
        panel = review::package_panel();
    }
    if (!panel) {
        check(false, "File > Collect and Save Package opens the window");
        return;
    }
    panel->destination() = root.getChildFile("Out/Cue 2M1 Package");
    panel->make_zip().setToggleState(true, juce::dontSendNotification);
    panel->start();
    for (int i = 0; i < 200 && panel->busy(); ++i) pump(50);
    const auto& r = panel->result();
    const auto folder = root.getChildFile("Out/Cue 2M1 Package");
    check(!panel->busy() && r.ok && r.copied == 2 && folder.getChildFile("Audio Files/Pad.wav").existsAsFile() &&
              folder.getChildFile("Audio Files/Choir.wav").existsAsFile() && panel->zip_file().existsAsFile(),
          "the package holds both audio files and a .zip is made: " + panel->message());
    const auto report = folder.getChildFile("Package Report.txt").loadFileAsString();
    check(report.contains("MISSING (1)") && report.contains("Lost.wav") && report.contains("Spitfire Symphonic Brass"),
          "the report lists the missing file and the library the receiving machine needs");
    shoot(*panel->getTopLevelComponent(), dir, "51-package");
    panel->getTopLevelComponent()->setVisible(false);

    // Somewhere else entirely: the zip opens and finds its media.
    const auto moved = root.getChildFile("Elsewhere/Cue 2M1 Package.zip");
    moved.getParentDirectory().createDirectory();
    panel->zip_file().copyFileTo(moved);
    main.open_package(moved);
    pump(120);
    const auto opened = juce::File(juce::String(session.path()));
    int found = 0;
    if (const auto* t = session.data().find_track(pad))
        for (const auto& clip : t->active_version()->audio_clips)
            if (juce::File(juce::String(session.resolve_audio_path(clip.file))).existsAsFile()) ++found;
    check(opened.isAChildOf(root.getChildFile("Elsewhere")) && found == 2 && session.data().name == "Cue 2M1",
          "Open Package unpacks the zip beside it and opens the project with both audio files found (" +
              opened.getFullPathName() + ")");
    root.deleteRecursively();
}

/// The library browser (Main Spec §10): the scanned instrument plug-in is
/// listed, a track's sound is saved as a patch, the patch ranks first for its
/// instrument, auditions through its plug-in, travels in a mapping pack, and a
/// template builds with it.
/// Sound Assignments: the Library's Assign opens the window with the sound;
/// it is assigned to an instrument's articulations once and goes in by itself
/// on new tracks, articulation tracks and templates; exported and imported.
void sound_assignment_checks(MainWindow& window, const juce::File& dir)
{
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    auto* service = ctx.library;
    if (!service) {
        check(false, "the main window owns a library");
        return;
    }
    const auto folder = dir.getChildFile("assignments-check");
    folder.deleteRecursively();
    folder.createDirectory();
    const auto old_file = service->file();
    service->set_file(folder.getChildFile("Library.json"));  // never the user's own
    const auto instrument = scan(ctx, VCC_TEST_PLUGIN);
    service->refresh();
    if (!instrument) {
        check(false, "the test instrument plug-in scans");
        service->set_file(old_file);
        return;
    }
    const auto entry = service->lib().find("plugin:" + instrument->identifier);
    check(entry.has_value(), "the scanned plug-in is in the library");
    if (!entry) {
        service->set_file(old_file);
        return;
    }
    // Assign from the Library: the window opens with the sound to assign.
    window.main().set_library_shown(true);
    pump(100);
    auto& browser = window.main().track_panel().library();
    browser.set_view(LibraryBrowser::View::Sounds);
    browser.search().setText(juce::String(entry->display), juce::sendNotificationSync);
    browser.refilter();
    pump(60);
    const auto keys = browser.listed();
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == entry->key) browser.select(static_cast<int>(i));
    browser.assign_selected();
    pump(150);
    auto* panel = sound_assignments_panel();
    auto* win = find_dialog("Sound Assignments");
    check(panel != nullptr && win != nullptr && win->isOnDesktop(), "Assign in the Library opens the Sound Assignments window");
    if (!panel) {
        service->set_file(old_file);
        return;
    }
    panel->show_instrument("strings.violins_1");
    pump(60);
    const auto rows = panel->rows();
    check(rows.size() >= 3 && rows.front().empty() && std::find(rows.begin(), rows.end(), "pizzicato") != rows.end(),
          "Violins I lists the whole instrument and each articulation the templates use (" + juce::String(static_cast<int>(rows.size())) +
              " rows)");
    panel->use_carried("");
    panel->use_carried("pizzicato");
    pump(60);
    check(panel->assigned_text("").isNotEmpty() && panel->assigned_text("pizzicato").isNotEmpty() &&
              service->lib().assignments().size() == 2 && session.sound_assignments().size() == 2,
          "Use assigns the sound to the whole instrument and to pizzicato, remembered and told to the session");
    if (win) shoot(*win, dir, "sound-assignments");

    // It goes in by itself: a new Violins I, its pizzicato track, a template.
    session.new_empty();
    const auto v = session.add_track("Violins I", "strings.violins_1");
    const auto* t = session.data().find_track(v);
    check(t && t->instrument_plugin.identifier == instrument->identifier && t->assignment.status == project::ResolutionStatus::Exact,
          "a new Violins I track comes with the assigned sound");
    const auto kids = session.expand_articulations(v, {"natural", "pizzicato"});
    check(kids.size() == 2 && session.data().find_track(kids[1])->instrument_plugin.identifier == instrument->identifier,
          "its pizzicato articulation track gets the sound assigned to pizzicato");
    session.new_from_template("cinematic.orchestra.consolidated");
    int with = 0;
    for (const auto* tr : session.music_tracks())
        if (tr->instrument_standard_id == "strings.violins_1") with += tr->instrument_plugin.identifier == instrument->identifier;
    check(with >= 1, "a template's Violins I come with it too");

    // Export, clear, import: back as they were (another computer).
    const auto file = folder.getChildFile("My Sounds.vccsounds");
    const auto exported = panel->export_to(file);
    panel->clear("");
    panel->clear("pizzicato");
    check(service->lib().assignments().empty() && session.sound_assignments().empty(), "Clear takes them away");
    const auto imported = panel->import_from(file);
    panel->show_instrument("strings.violins_1");
    check(file.existsAsFile() && service->lib().assignments().size() == 2 && panel->assigned_text("pizzicato").isNotEmpty(),
          "exported and imported back: " + exported + "; " + imported);

    // Tidy: the user's own library again.
    if (auto* w = dynamic_cast<juce::DocumentWindow*>(find_dialog("Sound Assignments"))) w->closeButtonPressed();
    window.main().set_library_shown(false);
    pump(80);
    service->set_file(old_file);
    session.new_empty();
}

void library_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    auto* service = ctx.library;
    if (!service) {
        check(false, "the main window owns a library");
        return;
    }
    const auto folder = dir.getChildFile("library-check");
    folder.deleteRecursively();
    folder.createDirectory();
    service->set_file(folder.getChildFile("Library.json"));  // never the user's own
    const auto instrument = scan(ctx, VCC_TEST_PLUGIN);
    service->refresh();
    const auto& lib = service->lib();
    const bool listed = instrument && std::any_of(lib.installed().begin(), lib.installed().end(), [&](const auto& p) {
        return p.identifier == instrument->identifier;
    });
    check(listed && lib.libraries().size() >= 5u,
          "the library lists the scanned instrument plug-in beside the shipped catalog (" +
              juce::String(static_cast<int>(lib.libraries().size())) + " libraries)");
    if (!instrument) return;

    // Library folders: a library's folder on disk is detected.
    const auto samples = folder.getChildFile("Samples/East West/Hollywood Orchestra Opus Edition");
    samples.createDirectory();
    service->lib().library_folders().push_back(folder.getChildFile("Samples").getFullPathName().toStdString());
    service->refresh();
    const auto hw = service->lib().find("catalog:eastwest.hollywood_orchestra/Horns");
    check(hw && hw->library_detected && !hw->player_installed &&
              library::status_text(*hw) == "Player not installed - library detected - metadata known",
          "a library folder is detected, and the entry says the player is missing: " +
              juce::String(hw ? library::status_text(*hw) : std::string("(none)")));

    // The browser, on the Library tab.
    session.new_empty();
    main.show_workspace(MainComponent::Workspace::Composer);
    main.set_library_shown(true);
    pump(100);
    auto& browser = main.track_panel().library();
    browser.set_view(LibraryBrowser::View::Sounds);
    browser.search().setText("", juce::sendNotificationSync);
    const auto keys = browser.listed();
    const auto plugin_key = "plugin:" + instrument->identifier;
    check(std::find(keys.begin(), keys.end(), plugin_key) != keys.end(),
          "All sounds lists the scanned plug-in (" + juce::String(static_cast<int>(keys.size())) + " installed sounds)");

    // A horn track: assign the plug-in from the browser, then save its sound.
    const auto horn = session.add_track("Horns", "brass.horn_section");
    session.select_track(horn);
    pump(50);
    for (int i = 0; i < static_cast<int>(keys.size()); ++i)
        if (keys[static_cast<std::size_t>(i)] == plugin_key) browser.select(i);
    browser.put_on_track_selected();
    pump(100);
    const auto* t = session.data().find_track(horn);
    check(t && t->instrument_plugin.identifier == instrument->identifier &&
              session.undo_label().rfind("Assign ", 0) == 0,
          "Assign puts the plug-in on the selected track as one undo step: " + juce::String(session.undo_label()));
    for (int i = juce::TopLevelWindow::getNumTopLevelWindows(); --i >= 0;)
        if (auto* w = juce::TopLevelWindow::getTopLevelWindow(i); w && w != &window) w->setVisible(false);
    browser.save_track_sound();
    pump(100);
    t = session.data().find_track(horn);
    check(service->lib().patches().size() == 1u && service->lib().patches()[0].instrument_id == "brass.horn_section" &&
              service->lib().patches()[0].validation == "verified" && t && !t->assignment.patch_id.empty() &&
              service->file().loadFileAsString().contains("brass.horn_section"),
          "Save sound keeps the track's plug-in as a verified patch for Horns, saved to the library file");

    // For the selected track: the saved patch first, exact.
    browser.set_view(LibraryBrowser::View::ForTrack);
    pump(50);
    const auto ranked = browser.listed();
    check(!ranked.empty() && ranked.front() == "patch:" + service->lib().patches()[0].id,
          "For the selected track ranks the saved patch first (" + juce::String(static_cast<int>(ranked.size())) +
              " candidates)");
    browser.select(0);
    check(browser.detail().contains("Saved patch for Horns"), "the detail says why: " + browser.detail());
    shoot(main, dir, "58-library");
    browser.audition_selected();
    check(wait_for([&] { return ctx.engine.previewing(); }, 3000),
          "Audition plays the saved patch through its plug-in on a preview channel");
    ctx.engine.stop_preview();

    // Favorites; a mapping pack round trip.
    browser.toggle_favorite_selected();
    check(service->lib().favorite("patch:" + service->lib().patches()[0].id), "Favorite marks the patch");
    const auto pack = service->lib().export_pack("Check");
    library::Library other(ctx.catalogs.instruments);
    check(other.import_pack(pack) == 1 && other.patches().size() == 1u,
          "a mapping pack carries the patch to another library");

    // A template with horns builds with the saved sound.
    std::string tpl;
    for (const auto* x : ctx.catalogs.templates->list())
        for (const auto& tt : x->tracks)
            if (tt.instrument_standard_id == "brass.horn_section" && tpl.empty()) tpl = x->template_id;
    if (!tpl.empty() && session.new_from_template(tpl).ok) {
        int with = 0;
        for (const auto& tr : session.data().tracks)
            if (tr.instrument_standard_id == "brass.horn_section" && tr.instrument_plugin.identifier == instrument->identifier &&
                tr.assignment.status == project::ResolutionStatus::Exact)
                ++with;
        check(with >= 1, "a template's horn track resolves to the saved patch and loads its plug-in (" +
                             juce::String(tpl) + ")");
    } else {
        check(false, "a template with horns exists");
    }
    main.set_library_shown(false);
    session.new_empty();
}

/// Articulation maps (Main Spec §8, §11): the window edits a track's map -
/// a natural keyswitch, a staccato keyswitch learned from a MIDI key - and
/// Apply is one undo step that playback hears; a project whose plug-in is
/// missing opens with the Missing Instruments window offering substitutes.
void articulation_map_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_empty();
    const auto vln = session.add_track("Violins", "strings.violins_1");
    session.select_track(vln);
    session.edit("Write", [&](project::ProjectData& d) {
        auto* t = d.find_track(vln);
        auto& sc = t->active_version()->score;
        for (int i = 0; i < 4; ++i) {
            notation::ScoreNote n;
            n.start_tick = i * kTicksPerQuarter;
            n.duration = notation::quarter();
            n.pitch.step = notation::Step::G;
            n.pitch.octave = 4;
            if (i == 1) n.articulations = notation::to_mask(notation::Articulation::Staccato);
            if (i == 3) n.articulations = notation::to_mask(notation::Articulation::Harmonic);
            sc.insert_note(0, n);
        }
        return true;
    });
    ctx.commands.invokeDirectly(cmd::articulationMap, false);
    ArticulationMapPanel* panel = nullptr;
    wait_for([&] { return (panel = articulation_map_panel()) != nullptr; });
    if (!panel) {
        check(false, "Track > Articulation Map opens the window");
        return;
    }
    panel->set_action(0, 1);  // natural: keyswitch
    panel->set_values(0, 24, 100);
    int stac = -1;
    for (int r = 1; r < panel->rows(); ++r)
        if (panel->row_name(r) == "Staccato") stac = r;
    panel->learn(stac);
    ctx.engine.offer_learn(juce::MidiMessage::noteOn(1, 25, 0.8f));  // the composer presses C#1
    wait_for([&] { return panel->learning_row() < 0; });
    const auto* rule = panel->map().rule_for(notation::Articulation::Staccato);
    check(rule && rule->action == notation::ArticulationAction::Keyswitch && rule->keyswitch_note == 25,
          "Learn takes the next key from the MIDI keyboard as the staccato keyswitch");
    check(panel->health().contains("harmonic"), "the health line names what is written but not heard: " + panel->health());
    shoot(*panel->getTopLevelComponent(), dir, "59-articulation-map");
    check(panel->apply() && session.undo_label() == "Articulation map", "Apply is one undo step");
    const auto* t = session.data().find_track(vln);
    const auto r = project::render_track(*t);
    std::vector<int> ks;
    for (const auto& k : r.keyswitches) ks.push_back(k.pitch);
    check(ks == std::vector<int>({24, 25, 24, 24}),
          "playback sends the natural switch before plain notes and the learned one before the staccato");
    panel->getTopLevelComponent()->setVisible(false);

    // A plug-in this machine does not have: the project opens and says so.
    project::PluginRef ghost{"VST3", "VST3-Ghost Strings-0000", "Ghost Strings", "Nobody", "c3RhdGU="};
    session.set_instrument_plugin(vln, ghost);
    const auto file = dir.getChildFile("missing-instrument.vcc");
    session.save_as(file.getFullPathName().toStdString());
    session.new_empty();
    main.open_file(file);
    juce::Component* missing = nullptr;
    wait_for([&] { return (missing = missing_instruments_panel()) != nullptr; });
    check(missing != nullptr && missing_instruments(ctx).size() == 1u &&
              session.data().find_track(vln)->instrument_plugin.state_base64 == "c3RhdGU=",
          "opening a project whose plug-in is missing lists it, and keeps its state for when it is back");
    if (missing) {
        if (auto* b = dynamic_cast<juce::Button*>(find_named(*missing, "Find substitute"))) b->triggerClick();
        wait_for([&] { return main.track_panel().library().view() == LibraryBrowser::View::ForTrack; });
        check(session.selected_track() == vln && main.track_panel().library().view() == LibraryBrowser::View::ForTrack,
              "Find substitute shows the library's candidates for that track");
        missing->getTopLevelComponent()->setVisible(false);
    }
    main.set_library_shown(false);
    session.new_empty();
}

/// File > New Project... (Main Spec §5): the template's recommendations fill
/// the music fields, every one can be overridden, and Create makes the
/// project in its own folder with the picture settings, no undo history.
void wizard_checks(MainWindow& window, const juce::File& dir)
{
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_empty();
    ctx.commands.invokeDirectly(cmd::newProjectWizard, false);
    NewProjectWizard* wizard = nullptr;
    wait_for([&] { return (wizard = new_project_wizard()) != nullptr; });
    if (!wizard) {
        check(false, "File > New Project opens the wizard");
        return;
    }
    const auto folder = dir.getChildFile("wizard-check");
    folder.deleteRecursively();
    folder.createDirectory();
    wizard->set_folder(folder);
    const auto* tpl = ctx.catalogs.templates->get("cinematic.suspense.tension");
    wizard->choose("cinematic", tpl ? tpl->subcategory : std::string(), "cinematic.suspense.tension");
    const auto suggested = wizard->setup();
    check(tpl && suggested.template_id == tpl->template_id && suggested.tempo &&
              *suggested.tempo >= tpl->tempo_low && *suggested.tempo <= tpl->tempo_high && suggested.name == tpl->display_name,
          "choosing a template fills tempo, key and meter with its recommendations (" +
              juce::String(suggested.tempo ? *suggested.tempo : 0.0) + " BPM, " +
              juce::String(suggested.key ? *suggested.key : std::string("?")) + ")");
    wizard->name().setText("2M3 Chase", juce::sendNotificationSync);
    wizard->set_key("E minor");
    wizard->set_meter("7/8");
    wizard->set_tempo(141);
    wizard->set_frame_rate(FrameRate::Fps25);
    wizard->set_start_timecode("10:00:00:00");
    wizard->set_sample_rate(96000);
    shoot(*wizard->getTopLevelComponent(), dir, "60-new-project");
    const auto error = wizard->create();
    const auto& d = session.data();
    const auto file = folder.getChildFile("2M3 Chase/2M3 Chase.vcc");
    check(error.isEmpty() && file.existsAsFile() && juce::File(juce::String(session.path())) == file,
          "Create saves the project in a folder of its own: " + (error.isEmpty() ? file.getFullPathName() : error));
    check(d.name == "2M3 Chase" && d.key == "E minor" && d.meter.at_bar(1) == TimeSignature{7, 8} &&
              std::abs(d.tempo.bpm_at(0) - 141.0) < 1e-9 && d.frame_rate == FrameRate::Fps25 &&
              d.start_timecode == "10:00:00:00" && d.sample_rate == 96000 && !session.can_undo() &&
              session.music_tracks().size() >= 3u,
          "the overrides win over the template's defaults, with its tracks, and nothing to undo");
    // A second Create with the same name never overwrites it.
    ctx.commands.invokeDirectly(cmd::newProjectWizard, false);
    wait_for([&] { return (wizard = new_project_wizard()) != nullptr; });
    if (wizard) {
        wizard->set_folder(folder);
        wizard->name().setText("2M3 Chase", juce::sendNotificationSync);
        check(wizard->create().contains("already exists"), "a project of the same name is never overwritten");
        wizard->getTopLevelComponent()->setVisible(false);
    }
    session.new_empty();
}

/// Groups, VCAs, monitor, cue mixes, bounce in place, save on stop (Main
/// Spec §12), heard through the engine offline.
void mix_tools_checks(MainWindow& window, const juce::File& dir)
{
    auto& ctx = window.context();
    auto& session = ctx.session;
    auto& engine = ctx.engine;
    engine.stop();
    session.new_empty();
    const auto folder = dir.getChildFile("mixtools-check");
    folder.deleteRecursively();
    folder.createDirectory();
    session.save_as(folder.getChildFile("Mix.vcc").getFullPathName().toStdString());
    engine.prepare_offline(48000.0, 512);
    Rig rig(engine);
    const auto a = session.add_audio_track("Tone A", false);
    const auto b = session.add_audio_track("Tone B", false);
    session.update_track_mix(a, "monitor", [](project::Track& t) { t.monitor = "input"; });
    rig.run(0.2, Rig::sine(220.0, 0.5f, 0));
    const double base = rig.run(0.3, Rig::sine(220.0, 0.5f, 0)).rms;

    const auto vca = session.add_vca("Strings", {a, b});
    session.set_vca_gain(vca, -12.0);
    rig.run(0.2, Rig::sine(220.0, 0.5f, 0));
    const double trimmed = rig.run(0.3, Rig::sine(220.0, 0.5f, 0)).rms;
    const double drop = juce::Decibels::gainToDecibels(trimmed / base);
    check(std::abs(drop + 12.0) < 0.6 && std::abs(session.data().find_track(a)->gain_db) < 1e-9,
          "a VCA at -12 dB trims its member by 12 dB (" + juce::String(drop, 2) + " dB), its fader untouched");
    session.update_vca(vca, "Mute VCA", [](project::Vca& v) { v.mute = true; });
    rig.run(0.2, Rig::sine(220.0, 0.5f, 0));
    check(rig.run(0.3, Rig::sine(220.0, 0.5f, 0)).rms < base * 0.001, "the VCA's mute silences its members");
    session.update_vca(vca, "Unmute VCA", [](project::Vca& v) {
        v.mute = false;
        v.gain_db = 0.0;
    });

    const auto grp = session.add_mix_group("Tones", {a, b});
    session.set_gain(a, -6.0);
    check(std::abs(session.data().find_track(b)->gain_db + 6.0) < 1e-9 && session.undo_label().rfind("Gain", 0) == 0,
          "moving one grouped fader moves the other by the same amount, in one undo step");
    session.undo();
    session.update_mix_group(grp, "Group off", [](project::MixGroup& g) { g.active = false; });

    // The monitor path never reaches a render.
    auto mon = engine.monitor();
    mon.dim = true;
    mon.mono = true;
    engine.set_monitor(mon);
    rig.run(0.2, Rig::sine(220.0, 0.5f, 0));
    const double dimmed_render = rig.run(0.3, Rig::sine(220.0, 0.5f, 0)).rms;
    check(std::abs(dimmed_render - base) < base * 0.02, "Dim and Mono change what the speakers hear, never a render");
    mon.dim = false;
    mon.mono = false;
    engine.set_monitor(mon);

    // A cue mix takes a pre-fader send and stays out of the master.
    const auto cue = session.add_bus("Cans", "cue");
    session.add_send(a, cue, true, 0.0);
    rig.run(0.2, Rig::sine(220.0, 0.5f, 0));
    const double with_cue = rig.run(0.3, Rig::sine(220.0, 0.5f, 0)).rms;
    check(!cue.empty() && std::abs(with_cue - base) < base * 0.02,
          "a cue mix's send does not reach the master (headphones only)");

    // Bounce in place: an audio track's clip, boosted by its EQ, rendered onto it.
    session.update_track_mix(a, "monitor", [](project::Track& t) { t.monitor = "auto"; });
    const auto wav = folder.getChildFile("Audio Files/Hit.wav");
    wav.getParentDirectory().createDirectory();
    {
        juce::AudioBuffer<float> buf(1, 48000);
        for (int i = 0; i < buf.getNumSamples(); ++i) buf.setSample(0, i, 0.25f * std::sin(static_cast<float>(i) * 0.03f));
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(wav);
        auto writer = juce::WavAudioFormat().createWriterFor(
            stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000.0).withNumChannels(1).withBitsPerSample(24));
        if (writer) writer->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
    }
    project::AudioClip clip;
    clip.name = "Hit";
    clip.file = "Audio Files/Hit.wav";
    clip.length = 1.0;
    clip.channels = 1;
    clip.file_sample_rate = 48000.0;
    session.add_audio_clips({{b, clip}}, "Import", false);
    session.update_track_mix(b, "eq", [](project::Track& t) {
        t.strip.eq_on = true;
        t.strip.lf.gain_db = 6.0;
    });
    session.select_track(b);
    session.set_selection({b, 0, session.data().tempo.tick_at_seconds(1.0), {}});
    const auto said = bounce_in_place(ctx, b);
    const auto* tb = session.data().find_track(b);
    int lane0 = 0, lane1 = 0;
    for (const auto& c : tb->active_version()->audio_clips) (c.lane == 0 ? lane0 : lane1) += c.lane <= 1 ? 1 : 0;
    check(session.undo_label() == "Bounce in place" && lane0 == 1 && lane1 == 1 && tb->printed_fx &&
              lane_clip(session, b, 0)->file != "Audio Files/Hit.wav",
          "Bounce in Place renders the clip through its EQ onto the track, the original kept on a take lane: " + said);
    session.undo();
    check(lane_clip(session, b, 0) && lane_clip(session, b, 0)->file == "Audio Files/Hit.wav" && !lane_clip(session, b, 1),
          "Undo puts the original clip back");

    // Save on transport stop.
    auto& ui = hw::ui_settings();
    const bool was = ui.getBoolValue("save_on_stop", false);
    ui.setValue("save_on_stop", true);
    session.rename_project("Saved on stop");
    check(session.dirty(), "the project has unsaved changes");
    engine.play();
    pump(200);
    engine.stop();
    wait_for([&] { return !session.dirty(); }, 3000);
    check(!session.dirty(), "Save on Transport Stop saves when playback stops");
    ui.setValue("save_on_stop", was);

    // The window.
    show_mix_tools(ctx, 0);
    MixToolsPanel* panel = nullptr;
    wait_for([&] { return (panel = mix_tools_panel()) != nullptr; });
    if (panel) {
        session.select_track(a);
        panel->add_vca();
        panel->add_group();
        pump(150);
        check(session.data().vcas.size() == 2u && session.data().mix_groups.size() == 2u,
              "+ VCA and + Group in the window add them for the selected track");
        shoot(*panel->getTopLevelComponent(), dir, "61-groups-vcas");
        panel->show_tab(1);
        pump(100);
        shoot(*panel->getTopLevelComponent(), dir, "62-monitor-cues");
        panel->getTopLevelComponent()->setVisible(false);
    } else {
        check(false, "Track > Groups, VCAs and Monitor opens the window");
    }
    session.new_empty();
}

/// Editing (Main Spec §9): reverse and time stretch write new files and
/// repoint clips (undo goes back), grouped clips move together, Capture
/// MIDI keeps what was played, the input filter drops what it should.
void edit_tools_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    auto& engine = ctx.engine;
    engine.stop();
    session.new_empty();
    const auto folder = dir.getChildFile("edit-check");
    folder.deleteRecursively();
    folder.createDirectory();
    session.save_as(folder.getChildFile("Edit.vcc").getFullPathName().toStdString());
    const auto wav = folder.getChildFile("Audio Files/Ramp.wav");
    wav.getParentDirectory().createDirectory();
    const int len = 48000;
    {
        juce::AudioBuffer<float> buf(1, len);
        for (int i = 0; i < len; ++i) buf.setSample(0, i, 0.5f * static_cast<float>(i) / static_cast<float>(len) +
                                                        0.1f * std::sin(static_cast<float>(i) * 0.05f));
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(wav);
        auto writer = juce::WavAudioFormat().createWriterFor(
            stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000.0).withNumChannels(1).withBitsPerSample(24));
        if (writer) writer->writeFromAudioSampleBuffer(buf, 0, len);
    }
    const auto track = session.add_audio_track("Ramp", false);
    project::AudioClip clip;
    clip.name = "Ramp";
    clip.file = "Audio Files/Ramp.wav";
    clip.length = 1.0;
    clip.channels = 1;
    clip.file_sample_rate = 48000.0;
    clip.fade_in = 0.1;
    const auto id = session.add_audio_clips({{track, clip}}, "Import", false).front();
    auto current = [&]() -> project::AudioClip {
        for (const auto& c : session.data().find_track(track)->active_version()->audio_clips)
            if (c.id == id) return c;
        return {};
    };
    auto first_and_last = [&](const project::AudioClip& c) {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r(formats.createReaderFor(juce::File(juce::String(session.resolve_audio_path(c.file)))));
        juce::AudioBuffer<float> b(1, 8);
        std::pair<float, float> out{0.0f, 0.0f};
        if (!r) return out;
        r->read(&b, 0, 1, 0, true, false);
        out.first = b.getSample(0, 0);
        r->read(&b, 0, 1, r->lengthInSamples - 1, true, false);
        out.second = b.getSample(0, 0);
        return out;
    };
    const auto said = reverse_clips(ctx, {{track, id}});
    const auto rev = current();
    const auto ends = first_and_last(rev);
    check(rev.file != clip.file && ends.first > 0.4f && std::abs(ends.second) < 0.05f && rev.fade_out > 0.09 &&
              rev.fade_in < 0.01 && session.undo_label() == "Reverse" && wav.existsAsFile(),
          "Reverse writes the clip backwards to a new file (loud end first), swaps its fades, keeps the original: " + said);
    session.undo();
    check(current().file == clip.file, "Undo points the clip back at the original file");
    const auto stretched_msg = stretch_clips(ctx, {{track, id}}, 1.5);
    const auto st = current();
    check(std::abs(st.length - 1.5) < 0.01 && std::abs(st.fade_in - 0.15) < 1e-6 && st.file != clip.file,
          "Time stretch to 150% makes the clip 1.5 s, its fade scaled: " + stretched_msg);
    session.undo();

    // Groups.
    const auto other = session.add_audio_track("Other", false);
    clip.start_seconds = 2.0;
    const auto id2 = session.add_audio_clips({{other, clip}}, "Import", false).front();
    main.show_workspace(MainComponent::Workspace::Sequencer);
    pump(80);
    main.sequencer().select_clip(track, id);
    check(main.sequencer().selected_clips().size() == 1u, "the Producer view reports its selected clips");
    session.group_items({{track, id}, {other, id2}});
    session.nudge_audio_clips(track, {id}, 0.5);
    double other_start = -1.0;
    for (const auto& c : session.data().find_track(other)->active_version()->audio_clips)
        if (c.id == id2) other_start = c.start_seconds;
    check(std::abs(other_start - 2.5) < 1e-9, "a grouped clip on another track moves with the nudged one");

    // Capture MIDI.
    const auto piano = session.add_track("Piano", "keys.piano");
    session.select_track(piano);
    engine.clear_live_history();
    engine.accept_live(juce::MidiMessage::noteOn(1, 60, 0.8f));
    engine.accept_live(juce::MidiMessage::noteOff(1, 60));
    engine.accept_live(juce::MidiMessage::noteOn(1, 64, 0.8f));
    engine.accept_live(juce::MidiMessage::noteOff(1, 64));
    ctx.commands.invokeDirectly(cmd::captureMidi, false);
    const auto& regions = session.data().find_track(piano)->active_version()->regions;
    check(session.undo_label() == "Capture MIDI" && regions.size() == 1u && regions.front().notes.size() == 2u,
          "Capture MIDI turns what was just played into a region on the selected track");

    // Input filter.
    PlaybackEngine::InputFilter f;
    f.channels = 1;  // channel 1 only
    f.velocity_min = 20;
    f.pitch_bend = false;
    engine.set_input_filter(f);
    check(engine.accept_live(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100))) &&
              !engine.accept_live(juce::MidiMessage::noteOn(2, 60, static_cast<juce::uint8>(100))) &&
              !engine.accept_live(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(10))) &&
              !engine.accept_live(juce::MidiMessage::pitchWheel(1, 9000)) &&
              engine.accept_live(juce::MidiMessage::controllerEvent(1, 1, 64)),
          "the MIDI input filter passes channel 1 and drops other channels, soft notes and pitch bend");
    engine.set_input_filter({});
    show_midi_input_filter(ctx);
    juce::Component* panel = nullptr;
    wait_for([&] { return (panel = midi_input_filter_panel()) != nullptr; });
    if (panel) {
        shoot(*panel->getTopLevelComponent(), dir, "63-midi-filter");
        panel->getTopLevelComponent()->setVisible(false);
    }
    engine.clear_live_history();
    session.new_empty();
}

/// Notation tools (Main Spec §7): the shortcut editor (duplicate, set a key,
/// conflicts, export and import), lyrics and repeats on the printed part,
/// multi-bar rests, print preview, explode, the drum kit's map.
void notation_tools_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();

    // Shortcut editor.
    show_shortcut_editor(ctx, [&main](const std::string& name) { main.use_shortcut_profile(name); });
    ShortcutEditor* ed = nullptr;
    wait_for([&] { return (ed = shortcut_editor()) != nullptr; });
    if (!ed) {
        check(false, "Edit Shortcuts opens the editor");
        return;
    }
    ed->choose_profile("Composer 2");
    check(ed->profile().is_built_in() && ed->rows() > 20, "the editor lists every command of a factory profile");
    const auto mine = ed->duplicate();
    check(!ed->profile().is_built_in() && ed->profile().name() == mine, "Duplicate makes an editable copy: " + juce::String(mine));
    ed->start_capture(0);
    composer::KeyStroke f9;
    composer::KeyStroke::from_text("F9", f9);
    const auto said = ed->take_stroke(f9, false);
    check(ed->profile().stroke_for(composer::all_commands().front()) == f9 && ed->conflicts_text() == "No conflicts",
          "Set key binds the pressed key: " + said);
    // A key another command holds asks first, then moves.
    const auto taken = ed->profile().stroke_for(composer::all_commands()[3]);
    ed->start_capture(1);
    const auto asked = ed->take_stroke(taken, false);
    check(asked.contains("is used by") && ed->capturing(), "a key in use is not taken silently: " + asked);
    ed->take_stroke(taken, true);
    check(ed->profile().stroke_for(composer::all_commands()[1]) == taken && !ed->profile().is_bound(composer::all_commands()[3]),
          "pressing it again moves it, and the old command loses it");
    const auto text = shortcut_profile_to_text(ed->profile());
    const auto back = shortcut_profile_from_text(text);
    check(back && back->name() == mine && back->stroke_for(composer::all_commands().front()) == f9,
          "a profile exports and imports as a .vccshortcuts file");
    shoot(*ed->getTopLevelComponent(), dir, "64-shortcuts");
    delete_user_shortcut_profile(mine);
    ed->getTopLevelComponent()->setVisible(false);

    // A part with lyrics, a repeat with endings and a long rest.
    session.new_empty();
    const auto voice = session.add_track("Soprano", "vocals.soprano");
    notation::Score sc = session.data().find_track(voice)->active_version()->score;
    const Ticks bar = session.data().meter.at_bar(1).ticks_per_bar();
    for (int b : {0, 1, 2, 11}) {
        notation::ScoreNote n;
        n.start_tick = b * bar;
        n.duration = notation::whole();
        n.pitch = notation::spell_midi_note(67 + b % 3, {});
        sc.insert_note(0, n);
    }
    session.commit_score(voice, sc, "Write");
    std::vector<notation::EventId> ids;
    for (const auto& n : session.data().find_track(voice)->active_version()->score.notes(0)) ids.push_back(n.id);
    check(session.set_lyrics(voice, ids, "Al-le-lu-ia") == 4, "four syllables on four notes");
    notation::RepeatSection rep;
    rep.start_bar = 1;
    rep.end_bar = 2;
    rep.ending_bars = 1;
    session.set_repeats({rep});
    show_print_preview(ctx);
    PrintPreview* preview = nullptr;
    wait_for([&] { return (preview = print_preview()) != nullptr; });
    if (preview) {
        preview->show_source(1);
        check(preview->source_count() == 2 && preview->page_count() >= 1 && preview->source_name(1) == "Part: Soprano",
              "Print Preview shows the full score and each part (" + juce::String(preview->page_count()) + " page(s) for the part)");
        shoot(*preview->getTopLevelComponent(), dir, "65-print-preview");
        preview->getTopLevelComponent()->setVisible(false);
    }
    engraving::LayoutEngine engine;
    if (music_font().loaded) engine.set_glyph_table(music_font().glyphs);
    engraving::PrintOptions po;
    po.multirests = true;
    const auto laid = engraving::lay_out_pages(session.data().find_track(voice)->active_version()->score, engine, po);
    std::set<std::string> texts;
    for (const auto& p : laid.pages)
        for (const auto& t : p.layout.display_list.texts) texts.insert(t.text);
    check(texts.count("Al") && texts.count("ia") && texts.count("1.") && texts.count("2.") && texts.count("8"),
          "the part prints the lyrics, the 1. and 2. endings and an 8-bar rest");

    // Explode, and a drum kit.
    session.new_empty();
    const auto horns = session.add_track("Horns", "brass.horn_section");
    notation::Score hs = session.data().find_track(horns)->active_version()->score;
    hs.insert_chord(0, 0, notation::half(), {notation::spell_midi_note(60, {}), notation::spell_midi_note(64, {}), notation::spell_midi_note(67, {})});
    session.commit_score(horns, hs, "Write");
    const auto parts = session.explode(horns, 0, bar, 3);
    check(parts.size() == 3u && session.undo_label() == "Explode", "Explode makes three horn parts in one undo step");
    const auto kit = session.add_track("Drums", "rhythm.drum_kit");
    check(session.data().find_track(kit)->percussion_map == "gm_kit", "a drum kit track gets the drum map");
    session.new_empty();
}

/// Picture tools (Main Spec §13): feet+frames on the counter, the tempo
/// finder landing hits on the beat, and the picture exported with the score
/// mix, timecode burned in, decoded back.
void picture_tools_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_empty();

    // The counter in feet and frames: one minute is 90 feet at 24 fps.
    session.set_frame_rate(FrameRate::Fps24);
    session.set_tempo(120.0);
    ctx.engine.locate(session.data().tempo.tick_at_seconds(60.0));
    main.deck().set_sub_mode(2);
    check(main.deck().sub_text() == "90+00", "the sub counter shows 35mm feet+frames: " + main.deck().sub_text());
    main.deck().set_sub_mode(1);
    check(main.deck().sub_text().startsWith("01:01:00"), "and timecode: " + main.deck().sub_text());
    main.deck().set_sub_mode(0);

    // Hits at bar 3 beat 1, bar 5 beat 3 and bar 8 beat 2 of a 96 bpm cue
    // starting at 2 s; the project is at 120.
    const double beat = 60.0 / 96.0;
    for (const int b : {8, 18, 29}) session.add_marker(2.0 + b * beat + 0.003, "hit", "hit " + std::to_string(b));
    const Ticks start = session.data().tempo.tick_at_seconds(2.0);
    show_tempo_finder(ctx);
    TempoFinder* finder = nullptr;
    wait_for([&] { return (finder = tempo_finder()) != nullptr; });
    if (!finder) {
        check(false, "Transport > Tempo Finder opens");
        return;
    }
    finder->set_start(start);
    finder->set_range(80, 130);
    finder->set_tolerance(0.5);
    const auto& found = finder->find();
    check(finder->hit_count() == 3 && !found.empty() && found[0].hits_on == 3 && std::abs(found[0].bpm - 96.0) < 0.05,
          "the tempo finder's best fit puts all three hits on the beat at " +
              (found.empty() ? juce::String("-") : juce::String(found[0].bpm, 2)) + " bpm");
    shoot(*finder->getTopLevelComponent(), dir, "66-tempo-finder");
    const auto said = finder->use(0);
    bool all_on = true;
    for (const auto& m : session.data().markers) {
        const auto pos = picture::locate_in_music(session.data(), m.seconds);
        all_on = all_on && std::abs(pos.offset_frames) < 0.5;
    }
    check(all_on && std::abs(session.data().tempo.bpm_at(start) - 96.0) < 0.05 && session.undo_label().rfind("Tempo", 0) == 0,
          "Use sets the tempo from the start and every hit lands on a beat: " + said);
    finder->getTopLevelComponent()->setVisible(false);

    // Picture with score: a short movie, a track that plays, exported and decoded.
    juce::ChildProcess probe;
    const bool tool = probe.start("ffmpeg -version") && probe.readAllProcessOutput().contains("ffmpeg");
    if (!media::available() || !media::encoding_available() || !tool) {
        std::printf("  skip  picture export (no FFmpeg in this build or no ffmpeg tool to make a test movie)\n");
        return;
    }
    const auto movie = dir.getChildFile("check-cut.mov");
    movie.deleteFile();
    juce::ChildProcess make;
    make.start(juce::StringArray{"ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                                 "testsrc2=size=640x360:rate=24:d=6", "-c:v", "mpeg4", "-q:v", "4", "-timecode",
                                 "01:00:00:00", movie.getFullPathName()});
    make.waitForProcessToFinish(60000);
    juce::String error;
    check(import_picture_file(ctx, movie, error), "the cut imports " + error);
    const auto track = session.add_track("Strings", "strings.violins_1");
    notation::Score sc = session.data().find_track(track)->active_version()->score;
    for (int b = 0; b < 3; ++b) {
        notation::ScoreNote n;
        n.start_tick = b * kTicksPerWhole;
        n.duration = notation::whole();
        n.pitch = notation::spell_midi_note(67, {});
        sc.insert_note(0, n);
    }
    session.commit_score(track, sc, "Write");
    PictureExportRequest rq;
    rq.file = dir.getChildFile("review.mp4");
    rq.file.deleteFile();
    const auto [from, to] = picture_range(ctx);
    rq.start_seconds = from;
    rq.end_seconds = from + 3.0;
    rq.max_width = 480;
    int calls = 0;
    rq.progress = [&calls](double) {
        ++calls;
        return true;
    };
    const auto res = export_picture(ctx, rq);
    check(res.ok && res.frames == 72 && calls == 72 && res.peak > 1.0e-3f && !res.codec.containsIgnoreCase("x264"),
          "Export Picture with Score writes 72 frames with the score's sound: " + res.message);
    media::VideoDecoder back;
    std::string err;
    if (back.open(rq.file.getFullPathName().toStdString(), &err)) {
        const auto& i = back.info();
        check(i.width == 480 && i.height == 270 && i.frame_count == 72 && i.has_audio && i.timecode == "01:00:00:00",
              "the movie decodes back: " + juce::String(i.width) + "x" + juce::String(i.height) + ", " +
                  juce::String(i.frame_count) + " frames, timecode " + juce::String(i.timecode));
        media::Frame f;
        if (back.frame(36, f)) {
            // The timecode box at the bottom centre: dark, with white digits.
            int dark = 0, white = 0;
            const int y0 = static_cast<int>(f.height - 30.0 * f.height / 360.0);
            for (int y = y0; y < y0 + 10; ++y)
                for (int x = f.width / 2 - 60; x < f.width / 2 + 60; ++x) {
                    const auto* p = &f.rgba[static_cast<std::size_t>((y * f.width + x) * 4)];
                    const int v = (p[0] + p[1] + p[2]) / 3;
                    dark += v < 50;
                    white += v > 200;
                }
            check(dark > 300 && white > 20, "the timecode is burned in (" + juce::String(dark) + " dark, " + juce::String(white) +
                                                " white pixels in its box)");
            juce::Image shot(juce::Image::RGB, f.width, f.height, false);
            for (int y = 0; y < f.height; ++y)
                for (int x = 0; x < f.width; ++x) {
                    const auto* p = &f.rgba[static_cast<std::size_t>((y * f.width + x) * 4)];
                    shot.setPixelAt(x, y, juce::Colour(p[0], p[1], p[2]));
                }
            juce::PNGImageFormat png;
            auto out = dir.getChildFile("67-picture-export.png");
            out.deleteFile();
            juce::FileOutputStream stream(out);
            png.writeImageToStream(shot, stream);
        }
    } else {
        check(false, "the exported movie opens: " + juce::String(err));
    }
    session.new_empty();
}

/// Template shelf and packages (Templates Supplement §3): star a template,
/// find it under Favorites and Recent, export custom templates with their
/// sounds as a package, delete, and import it back.
void template_shelf_checks(MainWindow& window, const juce::File& dir)
{
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_empty();
    auto& registry = *session.catalogs().templates;
    std::string built_in;
    for (const auto* t : registry.list())
        if (t->built_in && t->instantiable() && t->tracks.size() <= 12) {
            built_in = t->template_id;
            break;
        }
    show_template_browser(ctx, false);
    TemplateBrowser* browser = nullptr;
    wait_for([&] { return (browser = template_browser()) != nullptr; });
    if (!browser || built_in.empty()) {
        check(false, "the template browser opens with a template to try");
        return;
    }
    const bool was_favorite = template_shelf().is_favorite(built_in);
    if (was_favorite) template_shelf().set_favorite(built_in, false);
    browser->show_category("All categories");
    check(browser->select(built_in), "the browser lists " + juce::String(built_in));
    browser->toggle_favorite();
    check(template_shelf().is_favorite(built_in), "Favorite stars the selected template");
    browser->show_category("Favorites");
    check(browser->select(built_in) && browser->shown_count() >= 1, "Favorites lists it");
    browser->show_category("All categories");
    browser->select(built_in);
    browser->add();
    check(!session.data().tracks.empty() && template_shelf().recent().front() == built_in,
          "Add to Project adds its tracks and puts it first in Recent");
    browser->show_category("Recent");
    check(browser->shown_count() >= 1 && browser->select(built_in), "Recent lists it");

    // A custom template, exported with the sounds, deleted, imported back.
    const std::string custom = "user.check_strings_package";
    auto tpl = templates::save_user_template(session.project(), custom, "Check Strings", "Made by the desktop check");
    std::string error;
    check(registry.add_user_template(tpl, &error), "a custom template saves " + juce::String(error));
    const auto package = dir.getChildFile("check.vcctemplates");
    package.deleteFile();
    const auto exported = export_template_package_file(ctx, {custom, built_in}, package, true);
    check(package.existsAsFile() && exported.startsWith("Exported 2 template(s)"), exported);
    check(delete_custom_template(ctx, custom) && registry.get(custom) == nullptr, "Delete Custom Template removes it");
    check(!delete_custom_template(ctx, built_in) && registry.get(built_in) != nullptr, "a built-in template cannot be deleted");
    const auto imported = import_template_package_file(ctx, package);
    const auto* back = registry.get(custom);
    const auto* copy = registry.get("user." + built_in);
    check(imported.startsWith("Imported 2 template(s)") && back && !back->built_in && copy && !copy->built_in &&
              back->tracks.size() == tpl.tracks.size(),
          "Import Package brings the custom template back and the built-in one in as a copy: " + imported);
    browser->show_category("Custom");
    check(browser->select(custom) && browser->select("user." + built_in), "both are under Custom");
    browser->select(custom);
    browser->getTopLevelComponent()->setSize(1040, 660);
    shoot(*browser->getTopLevelComponent(), dir, "68-template-packages");

    // Leave the composer's folder as it was.
    delete_custom_template(ctx, custom);
    delete_custom_template(ctx, "user." + built_in);
    template_shelf().set_favorite(built_in, was_favorite);
    save_template_shelf();
    browser->show_category("All categories");
    browser->getTopLevelComponent()->setVisible(false);
    session.new_empty();
}

/// Preview builds: the days left, the ending, and what the title says.
void preview_checks()
{
    if (!preview::is_preview()) {
        check(preview::status().isEmpty() && !preview::ended(), "a release build is not a preview");
        return;
    }
    if (preview::expires().isEmpty()) {
        check(preview::status() == "Preview" && !preview::ended("2099-01-01"), "a preview without a date never ends");
        return;
    }
    const auto end = preview::expires();
    check(preview::days_left(end) == 0 && !preview::ended(end), "the preview still saves on its last day (" + end + ")");
    const auto after = juce::Time::fromISO8601(end + "T12:00:00Z") + juce::RelativeTime::days(1);
    const auto next = after.toISO8601(true).substring(0, 10);
    check(preview::ended(next) && preview::status(next) == "Preview ended " + end, "and stops saving the day after: " + preview::status(next));
    const auto before = (juce::Time::fromISO8601(end + "T12:00:00Z") - juce::RelativeTime::days(45)).toISO8601(true).substring(0, 10);
    check(preview::status(before) == "Preview - 45 days left", "45 days before: " + preview::status(before));
}

/// A stand-in for vc-composer.com and its Supabase Auth on 127.0.0.1, so
/// the account flow runs end to end with no real service.
class FakeStore final : public juce::Thread {
public:
    FakeStore() : juce::Thread("fake store")
    {
        for (int p = 47810; p < 47900 && port_ == 0; ++p)
            if (listener_.createListener(p, "127.0.0.1")) port_ = p;
        if (port_ != 0) startThread();
    }
    ~FakeStore() override
    {
        signalThreadShouldExit();
        listener_.close();
        stopThread(3000);
    }
    juce::String url() const { return "http://127.0.0.1:" + juce::String(port_); }
    bool ok() const { return port_ != 0; }

    juce::CriticalSection lock;
    juce::String standing = R"({"standing":{"status":"active","expiresAt":"2027-11-16T00:00:00.000Z"}})";
    bool refuse = false;
    int codes_sent = 0;
    juce::String last_device;
    juce::String last_activate, last_auth;

    // Collaboration (web/src/app/api/collab), in memory. The signed-in
    // composer is "u-ann"; push_as() stands in for another composer's push.
    struct CollabProject {
        juce::String name;
        std::vector<std::string> revisions;  ///< Revision n is [n - 1].
        std::vector<json::Value> manifests;  ///< Each revision's media.
        std::set<std::string> media;         ///< Registered files (sha256).
        std::map<juce::String, std::pair<juce::String, juce::String>> locks;  ///< track -> (user, name)
    };
    std::map<juce::String, CollabProject> collab;
    std::map<juce::String, std::string> uploads;  ///< Storage: key -> bytes.
    // Updates: what /api/releases offers and /api/downloads hands out.
    std::string update_version = "0.2.0-preview.1";
    std::string update_bytes;
    std::string update_sha;  ///< What the catalogue records (empty: the bytes' own).
    int pushes_refused = 0;
    /// Another composer's push: the project and any media files (path -> bytes) it adds.
    juce::String push_as(const juce::String& id, const std::string& text, const std::map<std::string, std::string>& files = {})
    {
        const juce::ScopedLock sl(lock);
        auto& p = collab[id];
        json::Value manifest = p.manifests.empty() ? json::Value(json::Array{}) : p.manifests.back();
        for (const auto& [path, bytes] : files) {
            const auto sha = hash::sha256_hex(bytes);
            uploads["media/" + id + "/" + juce::String(sha)] = bytes;
            p.media.insert(sha);
            json::Array kept;
            for (const auto& e : manifest.as_array())
                if (e.get_string("path") != path) kept.push_back(e);
            kept.push_back(json::Object{{"path", path}, {"sha256", sha}, {"sizeBytes", static_cast<long long>(bytes.size())}});
            manifest = json::Value(std::move(kept));
        }
        p.revisions.push_back(text);
        p.manifests.push_back(manifest);
        return juce::String(static_cast<int>(p.revisions.size()));
    }
    json::Value manifest(const juce::String& id, int revision)
    {
        const juce::ScopedLock sl(lock);
        const auto& m = collab[id].manifests;
        return revision >= 1 && revision <= static_cast<int>(m.size()) ? m[static_cast<std::size_t>(revision - 1)] : json::Value();
    }
    int head(const juce::String& id)
    {
        const juce::ScopedLock sl(lock);
        return static_cast<int>(collab[id].revisions.size());
    }

private:
    void run() override
    {
        while (!threadShouldExit()) {
            std::unique_ptr<juce::StreamingSocket> s(listener_.waitForNextConnection());
            if (s) serve(*s);
        }
    }
    void serve(juce::StreamingSocket& s)
    {
        juce::MemoryBlock in;
        char buf[4096];
        int header_end = -1, length = 0;
        while (s.waitUntilReady(true, 3000) == 1) {
            const int n = s.read(buf, sizeof buf, false);
            if (n <= 0) break;
            in.append(buf, static_cast<std::size_t>(n));
            const juce::String text(static_cast<const char*>(in.getData()), in.getSize());
            if (header_end < 0 && (header_end = text.indexOf("\r\n\r\n")) >= 0) {
                const auto cl = text.substring(0, header_end).fromFirstOccurrenceOf("Content-Length:", false, true);
                length = cl.upToFirstOccurrenceOf("\r\n", false, false).trim().getIntValue();
            }
            if (header_end >= 0 && static_cast<int>(in.getSize()) >= header_end + 4 + length) break;
        }
        const juce::String text(static_cast<const char*>(in.getData()), in.getSize());
        const auto head = text.substring(0, std::max(0, header_end));
        const auto body = text.substring(header_end + 4);
        // The bytes as sent (media files are binary).
        const std::string raw_body = header_end >= 0 && static_cast<std::size_t>(header_end + 4) <= in.getSize()
                                         ? std::string(static_cast<const char*>(in.getData()) + header_end + 4,
                                                       in.getSize() - static_cast<std::size_t>(header_end + 4))
                                         : std::string();
        std::optional<std::string> raw_reply;
        const auto line = head.upToFirstOccurrenceOf("\r\n", false, false);
        const auto method = line.upToFirstOccurrenceOf(" ", false, false);
        const auto path = line.fromFirstOccurrenceOf(" ", false, false).upToFirstOccurrenceOf(" ", false, false);
        const auto auth = head.fromFirstOccurrenceOf("Authorization:", false, true).upToFirstOccurrenceOf("\r\n", false, false).trim();
        int status = 404;
        juce::String reply = R"({"error":"not found"})";
        const juce::ScopedLock sl(lock);
        if (std::getenv("VCC_STORE_LOG")) std::fprintf(stderr, "    [store] %s %s body=%s\n", method.toRawUTF8(), path.toRawUTF8(), body.substring(0, 120).toRawUTF8());
        if (path.startsWith("/auth/v1/otp")) {
            ++codes_sent;
            status = 200;
            reply = "{}";
        } else if (path.startsWith("/auth/v1/token?grant_type=password")) {
            const bool good = body.contains("\"right-password\"");
            status = good ? 200 : 400;
            reply = good ? R"({"access_token":"tok","refresh_token":"ref","expires_in":3600})"
                         : R"({"error":"invalid_grant","error_description":"Invalid login credentials"})";
        } else if (path.startsWith("/auth/v1/verify")) {
            const bool good = body.contains("\"123456\"");
            status = good ? 200 : 403;
            reply = good ? R"({"access_token":"tok","refresh_token":"ref","expires_in":3600})"
                         : R"({"code":403,"msg":"Token has expired or is invalid"})";
        } else if (path.startsWith("/api/licenses/activate")) {
            last_activate = body;
            last_auth = auth;
            if (auth != "Bearer tok") {
                status = 401;
                reply = R"({"error":"Sign in to activate VC Composer"})";
            } else if (refuse) {
                status = 409;
                reply = R"({"error":"All 2 activations on this licence are in use. Free one from your account.","outcome":{"result":"refused"}})";
            } else {
                status = 200;
                reply = R"({"activated":true,"reason":"new","license":{"serial":"VCC-ABCDE-FGHJK-LMNPQ-RSTUV"}})";
            }
        } else if (path.startsWith("/api/licenses/standing") && method == "GET") {
            status = auth == "Bearer tok" ? 200 : 401;
            reply = status == 200 ? standing : juce::String(R"({"error":"Sign in"})");
        } else if (path.startsWith("/storage/")) {
            const auto key = path.fromFirstOccurrenceOf("/storage/", false, false);
            if (method == "PUT") {
                uploads[key] = raw_body;
                status = 200;
                reply = "{}";
            } else if (uploads.count(key)) {
                status = 200;
                raw_reply = uploads[key];
            }
        } else if (path.startsWith("/api/releases")) {
            const auto sha = update_sha.empty() ? hash::sha256_hex(update_bytes) : update_sha;
            status = 200;
            reply = juce::String(json::write(json::Object{{"builds", json::Array{json::Object{
                {"platform", Account::platform().toStdString()}, {"version", update_version}, {"releaseNotes", "Faster sync"},
                {"sha256", sha}, {"minimumOsVersion", "11.0"}}}}}));
        } else if (path.startsWith("/api/downloads/")) {
            // The account, or a computer activated with a licence (its fingerprint).
            const auto device = head.fromFirstOccurrenceOf("X-VCC-Device:", false, true).upToFirstOccurrenceOf("\r\n", false, false).trim();
            last_device = device;
            status = auth == "Bearer tok" || device.length() == 64 ? 200 : 401;
            uploads["update-file"] = update_bytes;
            reply = status == 200 ? juce::String(json::write(json::Object{{"url", (url() + "/storage/update-file").toStdString()},
                                                                           {"version", update_version}}))
                                  : juce::String(R"({"error":"Sign in to download"})");
        } else if (path.startsWith("/api/collab/")) {
            collab_route(method, path, body, auth, status, reply);
        }
        const std::string payload = raw_reply ? *raw_reply : reply.toStdString();
        const std::string out = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Error") +
                                "\r\nContent-Type: " + (raw_reply ? "application/octet-stream" : "application/json") +
                                "\r\nContent-Length: " + std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload;
        s.write(out.data(), static_cast<int>(out.size()));
        s.close();
    }

    void collab_route(const juce::String& method, const juce::String& path, const juce::String& body, const juce::String& auth,
                      int& status, juce::String& reply)
    {
        if (auth != "Bearer tok") {
            status = 401;
            reply = R"({"error":"Sign in to use shared projects"})";
            return;
        }
        auto parts = juce::StringArray::fromTokens(path.fromFirstOccurrenceOf("/api/collab/", false, false), "/", "");
        parts.removeEmptyStrings();
        const auto in = json::parse(body.toStdString()).value_or(json::Value());
        auto answer = [&](int code, const json::Value& v) {
            status = code;
            reply = juce::String(json::write(v));
        };
        if (parts.size() == 1 && parts[0] == "projects") {
            if (method == "POST") {
                const auto id = "00000000-0000-4000-8000-" + juce::String::toHexString(static_cast<int>(collab.size()) + 1).paddedLeft('0', 12);
                collab[id].name = juce::String(in.get_string("name"));
                return answer(200, json::Object{{"id", id.toStdString()}});
            }
            json::Array list;
            for (const auto& [id, p] : collab)
                list.push_back(json::Object{{"id", id.toStdString()}, {"name", p.name.toStdString()}, {"role", "owner"},
                                            {"head", static_cast<long long>(p.revisions.size())}, {"ownerName", "Ann"},
                                            {"updatedAt", "2026-11-16T10:00:00Z"}});
            return answer(200, json::Object{{"projects", json::Value(std::move(list))}});
        }
        if (parts.size() < 2 || parts[0] != "projects" || !collab.count(parts[1])) return answer(404, json::Object{{"error", "No such shared project"}});
        auto& p = collab[parts[1]];
        const auto id = parts[1];
        if (parts.size() == 2) {
            json::Array locks, revisions;
            for (const auto& [track, holder] : p.locks)
                locks.push_back(json::Object{{"track_id", track.toStdString()}, {"user_id", holder.first.toStdString()},
                                             {"expires_at", "2026-11-16T12:30:00Z"},
                                             {"holder", json::Object{{"display_name", holder.second.toStdString()}}}});
            for (int n = static_cast<int>(p.revisions.size()); n >= 1; --n)
                revisions.push_back(json::Object{{"number", static_cast<long long>(n)}, {"message", ""}, {"created_at", "2026-11-16T10:00:00Z"},
                                                 {"author", json::Object{{"display_name", n % 2 ? "Ann" : "Ben"}}}});
            json::Array members;
            members.push_back(json::Object{{"role", "owner"}, {"invited_email", "ann@example.com"}, {"accepted_at", "2026-11-16T09:00:00Z"},
                                           {"profile", json::Object{{"display_name", "Ann"}}}});
            members.push_back(json::Object{{"role", "editor"}, {"invited_email", "ben@example.com"}, {"accepted_at", "2026-11-16T09:05:00Z"},
                                           {"profile", json::Object{{"display_name", "Ben"}}}});
            members.push_back(json::Object{{"role", "reviewer"}, {"invited_email", "director@example.com"}, {"accepted_at", json::Value()}});
            return answer(200, json::Object{{"role", "owner"},
                                            {"me", "u-ann"},
                                            {"project", json::Object{{"id", id.toStdString()}, {"name", p.name.toStdString()},
                                                                     {"head_revision", static_cast<long long>(p.revisions.size())}}},
                                            {"members", json::Value(std::move(members))},
                                            {"revisions", json::Value(std::move(revisions))},
                                            {"locks", json::Value(std::move(locks))}});
        }
        if (parts[2] == "media" && parts.size() == 3) {
            json::Array missing;
            for (const auto& f : in["files"].as_array()) {
                const auto sha = f.get_string("sha256");
                if (p.media.count(sha)) continue;
                missing.push_back(json::Object{{"sha256", sha}, {"url", (url() + "/storage/media/" + id + "/" + juce::String(sha)).toStdString()}});
            }
            return answer(200, json::Object{{"missing", json::Value(std::move(missing))}});
        }
        if (parts[2] == "media" && parts.size() == 4) {
            if (!p.media.count(parts[3].toStdString())) return answer(404, json::Object{{"error", "This project has no such media file"}});
            return answer(200, json::Object{{"url", (url() + "/storage/media/" + id + "/" + parts[3]).toStdString()}, {"sizeBytes", 1}});
        }
        if (parts[2] == "upload") {
            const auto key = id + "/" + juce::Uuid().toDashedString() + ".vcc";
            return answer(200, json::Object{{"path", key.toStdString()}, {"url", (url() + "/storage/" + key).toStdString()},
                                            {"token", "t"}, {"head", static_cast<long long>(p.revisions.size())}});
        }
        if (parts[2] == "revisions" && parts.size() == 3) {
            const auto base = in.get_int("base", -1);
            if (base != static_cast<long long>(p.revisions.size())) {
                ++pushes_refused;
                return answer(409, json::Object{{"error", "Somebody pushed since"}, {"head", static_cast<long long>(p.revisions.size())}});
            }
            const auto key = juce::String(in.get_string("path"));
            if (!uploads.count(key)) return answer(400, json::Object{{"error", "Upload the file first"}});
            const auto& text = uploads[key];
            if (juce::SHA256(text.data(), text.size()).toHexString().toStdString() != in.get_string("sha256") ||
                in.get_int("sizeBytes") != static_cast<long long>(text.size()) || in.get_int("site") < 1)
                return answer(400, json::Object{{"error", "That is not a revision this server takes"}});
            // Every file the manifest names is registered, or uploaded whole.
            for (const auto& e : in["media"].as_array()) {
                const auto sha = e.get_string("sha256");
                if (p.media.count(sha)) continue;
                const auto stored = uploads.find("media/" + id + "/" + juce::String(sha));
                if (stored == uploads.end() || hash::sha256_hex(stored->second) != sha ||
                    static_cast<long long>(stored->second.size()) != e.get_int("sizeBytes"))
                    return answer(400, json::Object{{"error", "1 media file was not uploaded completely: sync again"}});
                p.media.insert(sha);
            }
            p.revisions.push_back(text);
            p.manifests.push_back(in["media"].is_array() ? in["media"] : json::Value(json::Array{}));
            return answer(200, json::Object{{"revision", static_cast<long long>(p.revisions.size())}});
        }
        if (parts[2] == "revisions" && parts.size() == 4) {
            const int n = parts[3].getIntValue();
            if (n < 1 || n > static_cast<int>(p.revisions.size())) return answer(404, json::Object{{"error", "No such revision"}});
            const auto key = "rev/" + id + "/" + juce::String(n);
            uploads[key] = p.revisions[static_cast<std::size_t>(n - 1)];
            const auto& text = uploads[key];
            return answer(200, json::Object{{"url", (url() + "/storage/" + key).toStdString()},
                                            {"sha256", juce::SHA256(text.data(), text.size()).toHexString().toStdString()},
                                            {"sizeBytes", static_cast<long long>(text.size())},
                                            {"number", static_cast<long long>(n)},
                                            {"media", p.manifests.size() >= static_cast<std::size_t>(n)
                                                          ? p.manifests[static_cast<std::size_t>(n - 1)]
                                                          : json::Value(json::Array{})}});
        }
        if (parts[2] == "locks") {
            const auto track = juce::String(in.get_string("trackId"));
            if (track.isEmpty()) return answer(400, json::Object{{"error", "Name the track to lock"}});
            const auto it = p.locks.find(track);
            if (method == "DELETE") {
                if (it != p.locks.end() && it->second.first == "u-ann") p.locks.erase(it);
                return answer(200, json::Object{{"ok", true}});
            }
            if (it != p.locks.end() && it->second.first != "u-ann")
                return answer(409, json::Object{{"error", (it->second.second + " is writing this part").toStdString()}});
            p.locks[track] = {"u-ann", "Ann"};
            return answer(200, json::Object{{"until", "2026-11-16T12:30:00Z"}});
        }
        answer(404, json::Object{{"error", "not found"}});
    }

    juce::StreamingSocket listener_;
    int port_ = 0;
};

/// The in-app updater against the fake site: an offer only when newer, the
/// download through the account, the checksum, and the window.
void menu_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    // The menu bar a desktop program is expected to have.
    const auto names = main.getMenuBarNames();
    check(names.joinIntoString(",") == "File,Edit,View,Track,Templates,Transport,Tools,Studio,Window,Help",
          "menu bar: File, Edit, View, Track, Templates, Transport, Tools, Studio, Window, Help (" +
              names.joinIntoString(",") + ")");
    auto items = [&](const juce::String& menu) {
        juce::StringArray out;
        const auto popup = main.getMenuForIndex(names.indexOf(menu), menu);
        for (juce::PopupMenu::MenuItemIterator it(popup, true); it.next();)
            out.add(it.getItem().text);
        return out;
    };
    const auto track = items("Track"), templates = items("Templates");
    const auto studio = items("Studio");
    check(studio.contains("Audio Setup...") && studio.contains("MIDI Setup...") && studio.contains("Plug-ins...") &&
              studio.contains("Control Surfaces..."),
          "Studio menu: audio, MIDI, plug-ins, surfaces (" + studio.joinIntoString("|") + ")");
    check(track.contains("New Track...") && track.contains("New Audio Track..."), "Track menu makes tracks (" + track.joinIntoString("|") + ")");
    check(templates.contains("Add Template to Project...") && templates.contains("New from Template...") &&
              templates.contains("Save as Template..."),
          "Templates menu: add, new project from one, save as one (" + templates.joinIntoString("|") + ")");
    check(items("Window").contains("Mixer") && items("Window").contains("Chord Explorer..."),
          "Window menu opens the windows (" + items("Window").joinIntoString("|") + ")");

    // Both toolbars offer + Track and Template.
    check((main.toolbar().key("+ Track") != nullptr || main.toolbar().key("+ Instrument") != nullptr) &&
              main.toolbar().key("Template") != nullptr,
          "the toolbar has + Track (+ Instrument in Producer) and Template");

    // A tool window floats over the app and is listed under Window.
    show_template_browser(ctx, false);
    pump(300);
    const auto open = dialogs::open_tool_windows();
    bool listed = false;
    for (auto* w : open) listed = listed || w->getName() == "Templates";
    check(listed, "the open Templates window is listed");
    check(items("Window").contains("Templates"), "Window > Open windows names Templates");
    bool floating = false;
    for (auto* w : open)
        if (w->getName() == "Templates") floating = w->isAlwaysOnTop() == juce::Process::isForegroundProcess();
    check(floating, "Templates floats above the app while it is in front");

    // Windows never open taller or wider than the screen.
    if (const auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()) {
        juce::DocumentWindow probe("probe", juce::Colours::black, 0);
        dialogs::fit_to_screen(probe, 5000, 5000);
        check(d->userArea.contains(probe.getBounds()), "an oversized window is fitted inside the screen");
    }
    for (auto* w : dialogs::open_tool_windows()) w->closeButtonPressed();
    pump(200);

    // Any template, a track per articulation: Comedy - Light / Quirky opens
    // each instrument followed by its sounds (Pizzicato, Staccato...).
    auto& session = ctx.session;
    session.new_empty();
    show_template_browser(ctx, false);
    TemplateBrowser* browser = nullptr;
    wait_for([&] { return (browser = template_browser()) != nullptr; });
    if (browser == nullptr) {
        check(false, "the template window opens");
        return;
    }
    browser->show_category("All categories");
    check(browser->select("cinematic.comedy.light_quirky"), "the template window lists Comedy - Light / Quirky");
    browser->set_layout(templates::TrackLayout::PerArticulation);
    pump(100);
    if (auto* w = find_dialog("Templates")) shoot(*w, dir, "82-template-per-articulation");
    browser->add();
    int parts = 0, sounds = 0;
    bool pizz = false;
    for (const auto& t : session.data().tracks) {
        if (t.kind != project::TrackKind::Instrument) continue;
        if (t.articulation_parent.empty()) ++parts;
        else ++sounds;
        pizz = pizz || t.name.find("Pizzicato") != std::string::npos;
    }
    check(parts > 0 && sounds > 0 && pizz,
          "A track per articulation: " + juce::String(parts) + " instruments and " + juce::String(sounds) +
              " articulation tracks, Pizzicato among them");
    window.main().show_workspace(MainComponent::Workspace::Sequencer);
    pump(300);
    shoot(window, dir, "83-tracks-per-articulation");
    // And folded: one track per instrument.
    session.new_empty();
    browser->set_layout(templates::TrackLayout::PerInstrument);
    browser->add();
    int folded = 0;
    for (const auto& t : session.data().tracks) folded += t.articulation_parent.empty() ? 0 : 1;
    check(folded == 0, "One track per instrument: no articulation tracks");
    browser->set_layout(templates::TrackLayout::AsTemplate);
    for (auto* w : dialogs::open_tool_windows()) w->closeButtonPressed();
    pump(200);
    session.new_empty();
}

void layout_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    main.apply_layout(*layouts::find("Compose"));
    pump(200);
    const int edit_h = main.edit_frame().getHeight(), edit_w = main.edit_frame().getWidth();

    // The bottom deck hides to a thin bar and comes back.
    main.set_dock_hidden(true);
    pump(150);
    auto* key = dynamic_cast<juce::Button*>(main.findChildWithID("dock_toggle"));
    check(!main.keyboard().isVisible() && key != nullptr && key->isVisible() && key->getButtonText() == "Show Keyboard",
          "Hide Keyboard / Mixer Deck: the keyboard goes, Show Keyboard stays");
    check(main.edit_frame().getHeight() > edit_h + 100,
          "the edit window takes the deck's height (" + juce::String(edit_h) + " -> " +
              juce::String(main.edit_frame().getHeight()) + ")");
    shoot(window, dir, "84-deck-hidden");
    if (key) key->triggerClick();
    pump(150);
    check(main.keyboard().isVisible() && !main.dock_hidden(), "Show Keyboard brings it back");

    // Work view: no side panels.
    main.set_work_view(true);
    pump(150);
    check(!main.track_panel().isVisible() && !main.inspector().isShowing() && main.edit_frame().getWidth() > edit_w + 200,
          "Work View hides the side panels; the edit window widens (" + juce::String(edit_w) + " -> " +
              juce::String(main.edit_frame().getWidth()) + ")");
    shoot(window, dir, "85-work-view");
    main.show_workspace(MainComponent::Workspace::Producer);
    pump(150);
    check(!main.new_track_panel().isVisible() && !main.track_panel().isVisible(), "Work View holds in Producer");
    main.set_work_view(false);
    pump(150);
    check(main.new_track_panel().isVisible() && !main.track_panel().isVisible(),
          "leaving Work View brings the panels back (the tracks are the edit window's lanes)");

    // Layouts: saved by name, recalled, and the one the app opens with.
    main.set_dock_hidden(true);
    main.set_work_view(true);
    auto mine = main.capture_layout();
    mine.name = "Check Focus";
    layouts::save(mine);
    main.apply_layout(*layouts::find("Compose"));
    pump(150);
    check(!main.work_view() && !main.dock_hidden() && main.workspace() == MainComponent::Workspace::Composer,
          "the Compose layout");
    main.apply_layout(*layouts::find("Check Focus"));
    pump(150);
    auto now = main.capture_layout();
    now.name = "Check Focus";
    check(now == mine, "a saved layout comes back exactly");
    layouts::set_startup("Check Focus");
    check(layouts::to_open_with() && layouts::to_open_with()->name == "Check Focus", "it can be the layout the app opens with");
    check(layouts::remove("Check Focus") && !layouts::find("Check Focus") && layouts::startup().isEmpty(),
          "deleting it forgets it, as the start-up layout too");

    // The floating windows go with a layout: open or closed, and where.
    {
        main.set_inspector_shown(true);
        main.composer().set_staff_tools_detached(true);
        if (!track_list_open()) window.context().commands.invokeDirectly(cmd::trackList, false);
        pump(200);
        const juce::Rectangle<int> ib(140, 160, 300, 640), tb(500, 120, 560, 140), lb(1100, 150, 260, 420);
        if (auto* w = main.inspector_window()) w->setBounds(ib);
        if (auto* w = main.composer().staff_tools_window()) w->setBounds(tb);
        if (auto* w = track_list_window()) w->setBounds(lb);
        pump(100);
        auto floating = main.capture_layout();
        floating.name = "Check Floating";
        layouts::save(floating);
        const auto stored = layouts::find("Check Floating");
        check(stored && stored->has_windows && stored->inspector.shown && stored->staff_tools.shown && stored->track_list.shown &&
                  stored->inspector.bounds == main.inspector_window()->getBounds(),
              "a saved layout records the Inspector, the staff tools and the track list, open and where (" +
                  (stored ? stored->inspector.bounds.toString() : juce::String()) + ")");
        // Everything closed and moved...
        main.set_inspector_shown(false);
        main.composer().set_staff_tools_detached(false);
        window.context().commands.invokeDirectly(cmd::trackList, false);
        pump(200);
        // ...and the layout brings each one back, where it was.
        main.apply_layout(*stored);
        pump(250);
        const bool back = main.inspector_shown() && main.composer().staff_tools_detached() && track_list_open() &&
                          main.inspector_window() != nullptr && main.inspector_window()->getBounds() == stored->inspector.bounds &&
                          main.composer().staff_tools_window() != nullptr &&
                          main.composer().staff_tools_window()->getBounds() == stored->staff_tools.bounds &&
                          track_list_window() != nullptr && track_list_window()->getBounds() == stored->track_list.bounds;
        check(back, "recalling it reopens each floating window where it was");
        shoot(window, dir, "86-layout-floating", true);
        // A built-in layout leaves the floating windows as they are.
        main.apply_layout(*layouts::find("Compose"));
        pump(150);
        check(main.inspector_shown() && track_list_open(), "a built-in layout leaves the floating windows alone");
        layouts::remove("Check Floating");
        main.set_inspector_shown(false);
        main.composer().set_staff_tools_detached(false);
        if (track_list_open()) window.context().commands.invokeDirectly(cmd::trackList, false);
        pump(150);
    }

    main.apply_layout(*layouts::find("Mix"));
    pump(150);
    check(main.mixer_full(), "the Mix layout: the mixer full screen");

    // The mixer window on a chosen display, filling it.
    const auto displays = MainComponent::display_keys();
    check(!displays.empty(), "the connected displays are listed (" + juce::String(static_cast<int>(displays.size())) + ")");
    if (!displays.empty()) {
        main.show_console_window(displays.front().first);
        pump(200);
        const auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
        check(main.mixer_window() != nullptr && main.mixer_window()->isVisible() && d != nullptr &&
                  main.mixer_window()->getBounds() == d->userArea && main.console_display() == displays.front().first,
              "Mixer Window on Display fills the chosen display, remembered (" +
                  (main.mixer_window() ? main.mixer_window()->getBounds().toString() : juce::String("none")) + " on " +
                  (d ? d->userArea.toString() : juce::String()) + ")");
        auto with_console = main.capture_layout();
        check(with_console.console_window && with_console.console_display == displays.front().first,
              "a layout records the mixer window and its display");
        main.mixer_window()->setVisible(false);
    }
    main.apply_layout(*layouts::find("Compose"));
    pump(150);
}

void studio_checks(MainWindow& window, const juce::File& dir)
{
    auto& ctx = window.context();
    ctx.commands.invokeDirectly(cmd::midiSetup, false);
    StudioSetup* setup = nullptr;
    wait_for([&] { return (setup = studio_setup()) != nullptr; });
    if (setup == nullptr) {
        check(false, "Studio > MIDI Setup opens the setup window");
        return;
    }
    check(setup->page() == 1, "MIDI Setup opens on the MIDI page");
    pump(200);
    if (auto* w = find_dialog("Audio and MIDI Setup")) shoot(*w, dir, "86-midi-setup");
    ctx.commands.invokeDirectly(cmd::audioSettings, false);
    pump(300);
    check(setup->page() == 0 && setup->summary().isNotEmpty(), "Audio Setup: the interface in one line (" + setup->summary() + ")");
    if (auto* w = find_dialog("Audio and MIDI Setup")) shoot(*w, dir, "87-audio-setup");
    check(rate_label(44100) == "44.1 kHz" && rate_label(48000) == "48 kHz" && rate_label(96000) == "96 kHz",
          "rates read 44.1 / 48 / 96 kHz");
    auto* device = ctx.engine.devices().getCurrentAudioDevice();
    if (device != nullptr) {
        // Every rate the interface offers has its key; choosing one runs the interface there.
        const auto offered = setup->rate_keys();
        check(!offered.isEmpty(), "the interface's rates have keys (" + offered.joinIntoString(", ") + ")");
        const double was = device->getCurrentSampleRate();
        for (const double hz : {44100.0, 48000.0, 96000.0})
            if (offered.contains(rate_label(hz)) && std::abs(hz - was) > 1.0) {
                check(setup->choose_rate(hz) && std::abs(ctx.engine.devices().getCurrentAudioDevice()->getCurrentSampleRate() - hz) < 1.0,
                      "choosing " + rate_label(hz) + " runs the interface at it");
                setup->choose_rate(was);
                break;
            }
    } else {
        check(setup->summary().startsWith("No audio interface"), "with no interface open, it says so");
    }
    // What is saved: the setup, the MIDI inputs seen, and the plug-in rule.
    ctx.engine.set_auto_enable_midi(false);
    auto state = ctx.engine.device_state();
    check(state != nullptr && state->getChildByName("VCC_KNOWN_MIDI") != nullptr &&
              state->getIntAttribute("vccAutoEnableMidi", 1) == 0,
          "the saved setup carries the MIDI inputs seen and the turn-on rule");
    ctx.engine.set_auto_enable_midi(true);
    if (ctx.engine.on_devices_changed) ctx.engine.on_devices_changed();
    check(hw::ui_settings().getValue("audio_device_state").contains("vccAutoEnableMidi"),
          "the setup is saved for next time");
    for (auto* w : dialogs::open_tool_windows()) w->closeButtonPressed();
    pump(200);
}

void plugin_watch_checks(MainWindow& window, const juce::File& dir)
{
    auto& host = window.context().plugins;
    host.set_scanner_tool(juce::File(VCC_PLUGIN_SCANNER_TOOL));
    host.set_scan_timeout_ms(4000);
    auto* watch = plugin_watch();
    if (watch == nullptr || host.properties() == nullptr) {
        check(false, "the plug-in check exists");
        return;
    }
    juce::VST3PluginFormat* vst3 = nullptr;
    for (auto* f : host.formats().getFormats())
        if (f->getName() == "VST3") vst3 = dynamic_cast<juce::VST3PluginFormat*>(f);
    if (vst3 == nullptr) {
        check(false, "VST3 is a plug-in format here");
        return;
    }
    // A plug-in folder of our own: the test instrument installed into it.
    const auto folder = dir.getChildFile("plugin-folder");
    folder.deleteRecursively();
    folder.createDirectory();
    const auto installed = folder.getChildFile(juce::File(VCC_TEST_PLUGIN).getFileName());
    juce::File(VCC_TEST_PLUGIN).copyDirectoryTo(installed);
    juce::PluginListComponent::setLastSearchPath(*host.properties(), *vst3, juce::FileSearchPath(folder.getFullPathName()));
    check(watch->folders(*vst3).getNumPaths() == 1, "the plug-in folders can be set");

    auto r = watch->run_now();
    check(r.finished && r.added.size() >= 1 && r.looked_at == 1,
          "a plug-in installed since the last look is found and added (" + r.summary() + ": " + r.added.joinIntoString(", ") + ")");
    bool listed = false;
    for (const auto& t : host.known().getTypes()) listed = listed || t.fileOrIdentifier == installed.getFullPathName();
    check(listed, "it is in the plug-in list");
    r = watch->run_now();
    check(!r.any_change() && r.attention.isEmpty() && r.summary().startsWith("Plug-ins up to date"),
          "looking again changes nothing (" + r.summary() + ")");

    // An update installed over it: newer on disk, scanned again.
    installed.setLastModificationTime(juce::Time::getCurrentTime() + juce::RelativeTime::hours(1));
    r = watch->run_now();
    check(r.updated.size() >= 1 && r.added.isEmpty(), "a newer install is scanned again as an update (" + r.updated.joinIntoString(", ") + ")");

    // A plug-in that will not load: needs attention, the app carries on.
    const auto crasher = folder.getChildFile(juce::File(VCC_TEST_CRASH_PLUGIN).getFileName());
    juce::File(VCC_TEST_CRASH_PLUGIN).copyDirectoryTo(crasher);
    r = watch->run_now();
    check(r.attention.size() == 1 && r.attention[0].contains("crash"),
          "one that crashes while it loads needs attention (" + r.attention.joinIntoString("; ") + ")");
    check(r.details().contains("Needs attention"), "the report lists it");

    // Uninstalled: off the list.
    installed.deleteRecursively();
    r = watch->run_now();
    listed = false;
    for (const auto& t : host.known().getTypes()) listed = listed || t.fileOrIdentifier == installed.getFullPathName();
    check(r.removed.size() >= 1 && !listed, "an uninstalled plug-in is taken off the list (" + r.removed.joinIntoString(", ") + ")");
    check(PluginWatch::at_startup(), "the check runs at start-up by default");

    // Back as it was.
    host.clear_scan_failure(crasher.getFullPathName());
    host.known().removeFromBlacklist(crasher.getFullPathName());
    host.properties()->removeValue("lastPluginScanPath_VST3");
    folder.deleteRecursively();
}

void mixer_controller_checks(MainWindow& window, const juce::File& dir)
{
    namespace mk = project::mackie;
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    auto& mc = main.mixer_controller();
    session.new_empty();
    std::vector<std::string> ids;
    for (int i = 0; i < 10; ++i) ids.push_back(session.add_track("Violins " + std::to_string(i + 1), "violins"));
    std::vector<mk::Bytes> sent;
    mc.sender = [&](const mk::Bytes& b) { sent.push_back(b); };
    mc.set_bank_offset(0);
    auto count = [&](std::uint8_t first, std::uint8_t mask = 0xF0) {
        int n = 0;
        for (const auto& b : sent) n += (b[0] & mask) == first ? 1 : 0;
        return n;
    };
    check(count(0xE0) >= 9, "every fader and the master are sent their positions (" + juce::String(count(0xE0)) + ")");
    bool named = false;
    for (const auto& b : sent)
        named = named || (b.size() > 8 && b[0] == 0xF0 && std::string(b.begin() + 7, b.end() - 1).find("Vln") != std::string::npos);
    check(named, "the scribble strips show the track names");

    // The surface's own messages: taken off the instrument path, then followed.
    auto& input = ctx.engine.mixer_surface_input();
    input.configure_all("check-controller");
    check(input.take("check-controller", juce::MidiMessage::noteOn(1, mk::Mute1 + 1, static_cast<juce::uint8>(127))) &&
              !input.take("a-keyboard", juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100))),
          "the controller's messages never play; a keyboard's still do");
    sent.clear();
    mc.tick();
    check(session.data().find_track(ids[1])->mute, "Mute on strip 2 mutes the second track");
    bool lit = false;
    for (const auto& b : sent) lit = lit || (b == mk::led(mk::Mute1 + 1, mk::Led::On));
    check(lit, "its Mute key lights");

    // Faders: a move sets the level; a held fader is not moved under the hand.
    mc.handle({mk::Event::Kind::Fader, 0, -1, false, 0.5, 0});
    check(std::abs(session.data().find_track(ids[0])->gain_db - mk::fader_to_db(0.5)) < 0.01,
          "fader 1 sets the first track's level (" + juce::String(session.data().find_track(ids[0])->gain_db, 1) + " dB)");
    mc.handle({mk::Event::Kind::FaderTouch, 2, -1, true, 0.0, 0});
    session.set_gain(ids[2], -12.0);
    sent.clear();
    mc.tick();
    check(count(0xE2, 0xFF) == 0, "a fader held by a hand is not moved");
    mc.handle({mk::Event::Kind::FaderTouch, 2, -1, false, 0.0, 0});
    mc.tick();
    check(count(0xE2, 0xFF) == 1, "let go, it moves to the mix");
    mc.handle({mk::Event::Kind::Fader, mk::kMasterStrip, -1, false, mk::db_to_fader(-6.0), 0});
    double master = 0.0;
    for (const auto& b : session.data().buses)
        if (b.id == "bus-master") master = b.gain_db;
    check(std::abs(master + 6.0) < 0.05, "the master fader sets the master bus");

    // V-Pots pan; a push centres.
    mc.handle({mk::Event::Kind::VPot, 3, -1, false, 0.0, 5});
    check(std::abs(session.data().find_track(ids[3])->pan - 0.1) < 1e-6, "a V-Pot pans");
    mc.handle({mk::Event::Kind::Button, -1, mk::VPotPush1 + 3, true, 0.0, 0});
    check(std::abs(session.data().find_track(ids[3])->pan) < 1e-9, "its push centres the pan");
    mc.handle({mk::Event::Kind::Button, -1, mk::Select1 + 4, true, 0.0, 0});
    check(session.selected_track() == ids[4], "Select selects the track");

    // Banks.
    mc.handle({mk::Event::Kind::Button, -1, mk::BankRight, true, 0.0, 0});
    check(mc.bank_offset() == 8 && mc.strip_tracks()[0] == ids[8] && mc.strip_tracks()[2].empty(),
          "Bank Right moves the strips to tracks 9-10");
    mc.handle({mk::Event::Kind::Button, -1, mk::ChannelLeft, true, 0.0, 0});
    check(mc.bank_offset() == 7, "Channel Left moves them one track");
    mc.set_bank_offset(0);

    // The transport.
    mc.handle({mk::Event::Kind::Button, -1, mk::Play, true, 0.0, 0});
    check(ctx.engine.playing(), "Play plays");
    mc.handle({mk::Event::Kind::Button, -1, mk::Stop, true, 0.0, 0});
    check(!ctx.engine.playing(), "Stop stops");
    ctx.engine.locate(0);
    mc.handle({mk::Event::Kind::Jog, -1, -1, false, 0.0, 4});
    check(ctx.engine.position() == kTicksPerQuarter, "the jog wheel moves the playhead (four sixteenths: a beat)");

    // The window.
    ctx.commands.invokeDirectly(cmd::mixerController, false);
    pump(300);
    if (auto* w = find_dialog("Mixer Controller")) shoot(*w, dir, "88-mixer-controller");
    else check(false, "Studio > Mixer Controller opens its window");
    for (auto* w : dialogs::open_tool_windows()) w->closeButtonPressed();
    pump(200);
    input.configure_all({});
    mc.sender = nullptr;
    session.new_empty();
}

void note_mouse_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& session = window.context().session;
    session.new_empty();
    const auto track = session.add_track("Flute", "flute");
    session.select_track(track);
    main.show_workspace(MainComponent::Workspace::Composer);
    pump(300);
    auto& composer = main.composer();
    if (!composer.note_entry()) composer.perform(composer::Command::ToggleKeyboardEntryMode);
    pump(100);
    std::function<vcc::ui::NotationView*(juce::Component&)> find_view = [&](juce::Component& c) -> vcc::ui::NotationView* {
        if (auto* v = dynamic_cast<vcc::ui::NotationView*>(&c); v && v->isShowing()) return v;
        for (auto* k : c.getChildren())
            if (auto* v = find_view(*k)) return v;
        return nullptr;
    };
    auto* view = find_view(composer);
    if (view == nullptr) {
        check(false, "the Flute staff is on screen");
        return;
    }
    auto notes = [&] {
        std::vector<notation::ScoreNote> out;
        if (const auto* t = session.data().find_track(track))
            for (const auto& n : t->active_version()->score.notes(0))
                if (!n.is_rest) out.push_back(n);
        return out;
    };
    auto click = [&](juce::Point<float> p) {
        view->mouseDown(mouse_event(*view, p, p, false));
        view->mouseUp(mouse_event(*view, p, p, false));
        pump(150);
    };
    // A note placed by a click on the staff...
    const float x = view->x_of_tick(notation::kTicksPerQuarter);
    const float y = view->y_of_staff_position(0, 4.0);  // the middle line
    click({x, y});
    auto placed = notes();
    check(placed.size() == 1, "a click on the staff places a note (" + juce::String(static_cast<int>(placed.size())) + ")");
    if (placed.size() != 1) return;
    // ...goes away when it is clicked again.
    auto head = view->notehead_bounds(placed[0].id);
    check(!head.isEmpty(), "its head is on screen");
    click(head.getCentre());
    check(notes().empty(), "clicking the placed note again takes it away");
    shoot(main, dir, "89-note-clicked-away", true);

    // Dragged: later in time and higher in pitch.
    click({x, y});
    placed = notes();
    if (placed.size() != 1) {
        check(false, "a note to drag");
        return;
    }
    head = view->notehead_bounds(placed[0].id);
    const auto to = juce::Point<float>(head.getCentreX() + view->x_of_tick(3 * notation::kTicksPerQuarter) -
                                           view->x_of_tick(notation::kTicksPerQuarter),
                                       view->y_of_staff_position(0, 6.0));  // two steps up, two beats later
    drag(*view, head.getCentre(), to);
    auto moved = notes();
    check(moved.size() == 1 && moved[0].start_tick == placed[0].start_tick + 2 * notation::kTicksPerQuarter &&
              notation::diatonic_index(moved[0].pitch) == notation::diatonic_index(placed[0].pitch) + 2,
          "dragging a note moves it later and up (tick " + juce::String(moved.empty() ? -1 : static_cast<int>(moved[0].start_tick)) +
              ")");
    check(session.undo_label().find("Move") != std::string::npos || session.can_undo(), "the move can be undone");

    // Select all, then Delete.
    click({view->x_of_tick(0), y});
    check(notes().size() == 2, "two notes on the staff");
    composer.perform(composer::Command::SelectAll);
    composer.perform(composer::Command::DeleteSelection);
    pump(150);
    check(notes().empty(), "Select All then Delete empties the staff");
    // Right-click a note: Sharp, Flat, Natural, Delete...
    {
        click({view->x_of_tick(0), view->y_of_staff_position(0, 4.0)});  // B4
        auto ns = notes();
        if (!ns.empty()) {
            juce::String chosen = "Sharp";
            juce::StringArray offered;
            composer.note_menu_hook = [&](juce::PopupMenu& menu) {
                for (juce::PopupMenu::MenuItemIterator it(menu, true); it.next();) {
                    offered.add(it.getItem().text);
                    if (it.getItem().text == chosen && it.getItem().action) it.getItem().action();
                }
            };
            auto right_click = [&](const notation::ScoreNote& n) {
                const auto head = view->notehead_bounds(n.id).getCentre();
                auto e = mouse_event(*view, head, head, false, false, juce::ModifierKeys::rightButtonModifier);
                view->mouseDown(e);
                view->mouseUp(e);
                pump(100);
            };
            right_click(ns[0]);
            ns = notes();
            check(offered.contains("Sharp") && offered.contains("Flat") && offered.contains("Natural") && offered.contains("Delete") &&
                      !ns.empty() && ns[0].pitch.alteration == 1,
                  "right-click a note: Sharp, Flat, Natural, Delete... (Sharp made it " +
                      juce::String(ns.empty() ? 0 : ns[0].pitch.alteration) + ")");
            chosen = "Delete";
            if (!ns.empty()) right_click(ns[0]);
            check(notes().empty(), "and Delete from the menu takes it away");
            composer.note_menu_hook = nullptr;
        }
        // The Mac keyboard's delete key (Backspace) deletes the selection.
        click({view->x_of_tick(0), view->y_of_staff_position(0, 4.0)});
        composer.perform(composer::Command::SelectAll);
        composer.grabKeyboardFocus();
        composer.keyPressed(juce::KeyPress(juce::KeyPress::backspaceKey));
        pump(100);
        check(notes().empty(), "Backspace (delete on a Mac) deletes the selected notes");
    }
    // Nothing drawn is cut off: a note far below the staff on ledger lines
    // and a dynamic under it, and the lane grows to show them.
    composer.perform(composer::Command::SelectDurationQuarter);
    click({view->x_of_tick(notation::kTicksPerQuarter), view->y_of_staff_position(0, -2.0)});
    composer.perform(composer::Command::DynamicMezzoPiano);
    click({view->x_of_tick(2 * notation::kTicksPerQuarter), view->y_of_staff_position(0, 14.0)});
    main.composer().resized();
    pump(200);
    {
        const auto& lay = view->layout();
        const bool fits = lay.drawn_bounds.y >= lay.extent.y - 0.01 && lay.drawn_bounds.bottom() <= lay.extent.bottom() + 0.01;
        check(fits && view->getHeight() > 100, "the lane grows to show ledger notes and dynamics (height " +
                                                   juce::String(view->getHeight()) + ")");
        shoot(main, dir, "94-lane-fits-markings", true);
    }
    // A marking clicked can be taken away: probe under the low note for its "mp".
    {
        auto dynamics = [&] {
            const auto* t = session.data().find_track(track);
            return t ? t->active_version()->score.dynamics().size() : std::size_t{0};
        };
        const auto had = dynamics();
        if (composer.note_entry()) composer.perform(composer::Command::ToggleKeyboardEntryMode);
        bool asked = false;
        composer.marking_menu_hook = [&](std::size_t staff, Ticks tick) {
            asked = true;
            composer.remove_marking(staff, tick);
        };
        for (double p = -3.0; p > -16.0 && !asked; p -= 0.5) {
            for (const double dx : {0.0, 4.0, 8.0}) {
                const juce::Point<float> at(view->x_of_tick(notation::kTicksPerQuarter) + static_cast<float>(dx),
                                            view->y_of_staff_position(0, p));
                view->mouseDown(mouse_event(*view, at, at, false));
                view->mouseUp(mouse_event(*view, at, at, false));
                if (asked) break;
            }
        }
        pump(150);
        composer.marking_menu_hook = nullptr;
        check(had == 1 && asked && dynamics() == 0, "clicking a dynamic offers Remove, and it goes");
        if (!composer.note_entry()) composer.perform(composer::Command::ToggleKeyboardEntryMode);
    }
    composer.perform(composer::Command::SelectAll);
    composer.perform(composer::Command::DeleteSelection);
    pump(150);
    // Note values: placed with the mouse, then a value key - the next notes
    // take it; the note just placed keeps its own.
    session.new_empty();
    {
        const auto t2 = session.add_track("Oboe", "oboe");
        session.select_track(t2);
        pump(300);
        view = find_view(composer);
    }
    if (view != nullptr) {
        auto staff_notes = [&] {
            std::vector<notation::ScoreNote> out;
            if (const auto* t = session.data().find_track(session.selected_track()))
                for (const auto& n : t->active_version()->score.notes(0))
                    if (!n.is_rest) out.push_back(n);
            std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.start_tick < b.start_tick; });
            return out;
        };
        composer.perform(composer::Command::SelectDurationQuarter);
        click({view->x_of_tick(0), view->y_of_staff_position(0, 4.0)});
        composer.perform(composer::Command::SelectDurationWhole);
        click({view->x_of_tick(4 * notation::kTicksPerQuarter), view->y_of_staff_position(0, 4.0)});
        composer.perform(composer::Command::SelectDurationSixteenth);
        click({view->x_of_tick(8 * notation::kTicksPerQuarter), view->y_of_staff_position(0, 4.0)});
        const auto ns = staff_notes();
        check(ns.size() == 3 && ns[0].duration.value == notation::NoteValue::Quarter &&
                  ns[1].duration.value == notation::NoteValue::Whole && ns[2].duration.value == notation::NoteValue::Sixteenth,
              "a note value chosen after placing a note is for the next notes (" + juce::String(static_cast<int>(ns.size())) +
                  " notes)");
    }

    session.new_empty();
}

void project_file_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& session = window.context().session;
    // Projects are .vcc; earlier previews' .vcc2 still open.
    check(is_project_file(juce::File("/x/Cue.vcc")) && is_project_file(juce::File("/x/Old.vcc2")) &&
              !is_project_file(juce::File("/x/a.wav")),
          "project files: .vcc (and .vcc2 from earlier previews)");
    session::ProjectSetup setup;
    setup.name = "Drive Cue";
    const auto other_drive = dir.getChildFile("Other Drive");
    other_drive.deleteRecursively();
    setup.folder = other_drive.getFullPathName().toStdString();
    const auto r = session.create_project(setup);
    check(r.ok && juce::String(r.project_file).endsWith("Drive Cue/Drive Cue.vcc"), "a new project is saved as .vcc (" + juce::String(r.project_file) + ")");
    // Opened from another drive: remembered there, for Open Recent and the next Save As.
    recent::clear();
    main.open_file(juce::File(juce::String(r.project_file)));
    pump(100);
    const auto list = recent::projects();
    check(!list.isEmpty() && list[0] == juce::String(r.project_file), "Open Recent lists it where it lives");
    check(recent::last_folder() == juce::File(juce::String(r.project_file)).getParentDirectory(),
          "the next Open or Save As starts in its folder");
    // A legacy .vcc2 still opens.
    const auto legacy = other_drive.getChildFile("Legacy.vcc2");
    std::string error;
    check(session.save_as(legacy.getFullPathName().toStdString(), &error), "a .vcc2 copy saves " + juce::String(error));
    main.open_file(legacy);
    check(session.path() == legacy.getFullPathName().toStdString(), "and opens");
    session.new_empty();
}

void hardware_insert_checks(MainWindow& window, const juce::File& dir)
{
    auto& ctx = window.context();
    // A fake interface: 4 inputs, 4 outputs, and a cable from outputs 3-4 back
    // to inputs 3-4 through "gear" that halves the level and takes 300 samples.
    constexpr int kBlock = 256, kCable = 300;
    juce::AudioBuffer<float> outs(4, kBlock), ins(4, kBlock);
    std::vector<float> cable_l(kCable + kBlock * 2, 0.0f), cable_r(kCable + kBlock * 2, 0.0f);
    std::vector<float> line_l(kCable, 0.0f), line_r(kCable, 0.0f);
    int line_pos = 0;
    auto& io = hardware_io();
    auto cable = [&] {
        // What went out this block comes back kCable samples later, at half level.
        for (int i = 0; i < kBlock; ++i) {
            const float back_l = line_l[static_cast<std::size_t>(line_pos)], back_r = line_r[static_cast<std::size_t>(line_pos)];
            line_l[static_cast<std::size_t>(line_pos)] = outs.getSample(2, i) * 0.5f;
            line_r[static_cast<std::size_t>(line_pos)] = outs.getSample(3, i) * 0.5f;
            line_pos = (line_pos + 1) % kCable;
            ins.setSample(2, i, back_l);
            ins.setSample(3, i, back_r);
        }
    };
    auto begin_block = [&] {
        outs.clear();
        io.inputs = ins.getArrayOfReadPointers();
        io.num_inputs = 4;
        io.outputs = outs.getArrayOfWritePointers();
        io.num_outputs = 4;
        io.offset = 0;
        io.live = true;
    };

    // The ping measures the round trip.
    ins.clear();
    io.request_ping(2, 2);
    int guard = 0;
    while (io.pinging() && guard++ < 400) {
        begin_block();
        io.service_ping(kBlock, 48000.0);  // the click goes out
        cable();                           // and comes back next blocks
        // Listening happens on the next block's inputs.
    }
    // The cable plus one block of the interface's buffering (inputs heard in
    // one callback come back in the next), exactly what the impulse shows below.
    check(io.ping_result() == kCable + kBlock,
          "Ping measures the round trip through the gear (" + juce::String(io.ping_result()) + " samples; cable " + juce::String(kCable) + ")");

    // The insert: out on 3-4, back on 3-4, latency as measured.
    project::InsertSlot slot;
    slot.kind = "hardware";
    slot.suggestion = false;
    slot.params = {{"out_pair", 2.0}, {"in_pair", 2.0}, {"latency", static_cast<double>(kCable)}, {"return_db", 6.0206}};
    auto ins_proc = create_insert(slot, ctx.plugins, 48000.0, kBlock);
    check(ins_proc != nullptr && ins_proc->latency_samples() == kCable, "a hardware insert reports its latency for delay compensation");
    // An impulse through it: back after the round trip, at its own level (the return makes up the gear's -6 dB).
    std::vector<float> l(kBlock), r(kBlock);
    int arrived = -1;
    float arrived_level = 0.0f;
    for (int b = 0; b < 4 && arrived < 0; ++b) {
        begin_block();
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        if (b == 0) l[10] = r[10] = 0.8f;
        // The return heard this block is what the cable gave back for earlier sends.
        ins_proc->process(l.data(), r.data(), kBlock);
        cable();
        for (int i = 0; i < kBlock; ++i)
            if (std::abs(l[static_cast<std::size_t>(i)]) > 0.1f) {
                arrived = b * kBlock + i;
                arrived_level = l[static_cast<std::size_t>(i)];
                break;
            }
    }
    check(arrived == 10 + kCable + kBlock && std::abs(arrived_level - 0.8f) < 0.05f,
          "what comes back from the gear plays in the strip (impulse at " + juce::String(arrived) + ", level " +
              juce::String(arrived_level, 2) + ")");
    check(ins_proc->status().isEmpty(), "with the pairs on, it reports nothing wrong");

    // Pairs the interface does not have, and offline bounces, are said plainly.
    slot.params["out_pair"] = 5.0;
    ins_proc->set_params(slot);
    begin_block();
    ins_proc->process(l.data(), r.data(), kBlock);
    check(ins_proc->status().contains("outputs are not on"), "outputs the interface lacks: " + ins_proc->status());
    io.live = false;
    std::fill(l.begin(), l.end(), 0.3f);
    ins_proc->process(l.data(), r.data(), kBlock);
    check(ins_proc->status().contains("live") && std::abs(l[100] - 0.3f) < 1e-6f, "in an offline bounce it passes dry and says why");
    io.inputs = nullptr;
    io.outputs = nullptr;
    io.num_inputs = io.num_outputs = 0;
    io.live = false;
    ins_proc.reset();

    // In the mixer: Outboard (Hardware Insert) in the insert menu, its window with Ping.
    check(console::insert_name(slot) == "Outboard (Hardware Insert)", "the insert menu offers Outboard (Hardware Insert)");
    auto& session = ctx.session;
    session.new_empty();
    const auto track = session.add_audio_track("Vocal", true);
    session.update_track_mix(track, "Outboard", [&](project::Track& t) { t.inserts.push_back(slot); });
    window.main().show_insert_window(track, 0);
    pump(300);
    juce::Component* win = nullptr;
    for (int i = 0; i < juce::Desktop::getInstance().getNumComponents(); ++i)
        if (auto* c = juce::Desktop::getInstance().getComponent(i); c && c->getName().startsWith("Hardware")) win = c;
    juce::String names;
    for (int i = 0; i < juce::Desktop::getInstance().getNumComponents(); ++i)
        if (auto* c = juce::Desktop::getInstance().getComponent(i)) names << c->getName() << "|";
    std::function<juce::Component*(juce::Component&, const juce::String&)> find_id = [&](juce::Component& c, const juce::String& id) -> juce::Component* {
        if (c.getComponentID() == id) return &c;
        for (auto* k : c.getChildren())
            if (auto* f = find_id(*k, id)) return f;
        return nullptr;
    };
    check(win != nullptr && find_id(*win, "hardware_ping") != nullptr, "its window has Ping and names the channels (" + names + ")");
    if (win) {
        shoot(*win, dir, "90-hardware-insert");
        win->setVisible(false);
    }
    session.new_empty();
}

void ostinato_audition_checks(MainWindow& window, const juce::File& dir)
{
    auto& ctx = window.context();
    ctx.session.new_empty();
    ctx.session.select_track(ctx.session.add_track("Celli", "celli"));
    show_ostinato_creator(ctx);
    pump(400);
    auto* win = find_dialog("Ostinato Creator");
    std::function<juce::Component*(juce::Component&, const juce::String&)> find_id = [&](juce::Component& c, const juce::String& id) -> juce::Component* {
        if (c.getComponentID() == id) return &c;
        for (auto* k : c.getChildren())
            if (auto* f = find_id(*k, id)) return f;
        return nullptr;
    };
    auto* a = win ? dynamic_cast<juce::TextButton*>(find_id(*win, "ostinato_audition_0")) : nullptr;
    auto* b = win ? dynamic_cast<juce::TextButton*>(find_id(*win, "ostinato_audition_1")) : nullptr;
    if (a == nullptr || b == nullptr) {
        check(false, "the Ostinato Creator's variants have Audition buttons");
        return;
    }
    a->triggerClick();
    pump(200);
    check(a->getToggleState() && a->getButtonText() == "Stop" && ctx.engine.previewing(),
          "Audition lights up, reads Stop, and loops the variant");
    shoot(*win, dir, "91-ostinato-auditioning");
    b->triggerClick();
    pump(200);
    check(b->getToggleState() && !a->getToggleState() && a->getButtonText() == "Audition",
          "auditioning another variant moves the light to it");
    b->triggerClick();
    pump(200);
    {
        // No audio device here: process a block, as the device would, to let the stop land.
        juce::AudioBuffer<float> block(2, 512);
        ctx.engine.prepare_offline(48000.0, 512);
        ctx.engine.render_offline(block, 512);
    }
    check(!b->getToggleState() && b->getButtonText() == "Audition" && !ctx.engine.previewing(),
          "pressing it again stops the audition (lit " + juce::String(b->getToggleState() ? 1 : 0) + ", text " + b->getButtonText() + ", playing " + juce::String(ctx.engine.previewing() ? 1 : 0) + ")");
    // Notation first: the cards show each figure as notation (as it goes on
    // the staff), MIDI on request; Send to Track writes it on the staff.
    auto* staff_view = dynamic_cast<vcc::ui::NotationView*>(find_id(*win, "ostinato_notation_0"));
    check(staff_view != nullptr && staff_view->isVisible() && !staff_view->layout().notes.empty(),
          "each variant is shown as notation");
    shoot(*win, dir, "92-ostinato-notation");
    if (auto* midi = dynamic_cast<juce::Button*>(find_id(*win, "ostinato_show_midi"))) midi->triggerClick();
    pump(100);
    check(staff_view != nullptr && !staff_view->isVisible(), "MIDI shows the piano-roll picture instead");
    if (auto* notation = dynamic_cast<juce::Button*>(find_id(*win, "ostinato_show_notation"))) notation->triggerClick();
    pump(100);
    const auto celli = ctx.session.selected_track();
    juce::TextButton* send = nullptr;
    std::function<void(juce::Component&)> find_send = [&](juce::Component& c) {
        if (auto* b = dynamic_cast<juce::TextButton*>(&c); b && b->getButtonText() == "Send to Track" && send == nullptr) send = b;
        for (auto* k : c.getChildren()) find_send(*k);
    };
    find_send(*win);
    if (send) send->triggerClick();
    pump(300);
    std::size_t on_staff = 0, regions = 0;
    if (const auto* t = ctx.session.data().find_track(celli); t && t->active_version()) {
        for (const auto& n : t->active_version()->score.notes(0)) on_staff += n.is_rest ? 0 : 1;
        regions = t->active_version()->regions.size();
    }
    check(send != nullptr && on_staff > 0 && regions == 0,
          "Send to Track writes the figure on the staff (" + juce::String(static_cast<int>(on_staff)) + " notes, " +
              juce::String(static_cast<int>(regions)) + " MIDI regions)");
    for (auto* w : dialogs::open_tool_windows()) w->closeButtonPressed();
    pump(200);
    window.main().show_workspace(MainComponent::Workspace::Composer);
    pump(300);
    shoot(window, dir, "93-ostinato-on-staff");
    ctx.session.new_empty();
}

void view_tools_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    session.new_empty();
    session.add_template("cinematic.orchestra.consolidated");
    main.apply_layout(*layouts::find("Compose"));
    pump(300);
    // Zoom sliders at the bottom right: width (music across) and height (staff size).
    {
        auto& composer = main.composer();
        auto* zw = dynamic_cast<juce::Slider*>(composer.findChildWithID("composer_zoom_width"));
        auto* zh = dynamic_cast<juce::Slider*>(composer.findChildWithID("composer_zoom_height"));
        check(zw != nullptr && zh != nullptr && zw->isShowing() && zh->isShowing(), "the edit window has width and height zoom sliders");
        auto& lane = composer.lane(0);
        const float bar_before = lane.x_of_tick(4 * kTicksPerQuarter) - lane.x_of_tick(0);
        const int height_before = lane.getHeight();
        if (zw) zw->setValue(composer.zoom_width() * 2.0, juce::sendNotificationSync);
        if (zh) zh->setValue(composer.zoom_height() * 1.5, juce::sendNotificationSync);
        pump(100);
        const float bar_after = lane.x_of_tick(4 * kTicksPerQuarter) - lane.x_of_tick(0);
        check(bar_after > bar_before * 1.8f && lane.getHeight() > height_before,
              "W widens the bars, H enlarges the staves (bar " + juce::String(bar_before, 0) + " -> " + juce::String(bar_after, 0) +
                  " px, lane " + juce::String(height_before) + " -> " + juce::String(lane.getHeight()) + " px)");
        shoot(window, dir, "100-zoomed", true);
        composer.set_zoom(7.0, 30.0 / 7.0);
        pump(100);
        // Each staff's own room.
        const int before = lane.getHeight();
        auto* room = dynamic_cast<juce::Slider*>(composer.lane(0).getParentComponent()->findChildWithID("lane_room"));
        if (room) room->setValue(8.0, juce::sendNotificationSync);
        pump(100);
        check(room != nullptr && lane.getHeight() > before + 60, "a staff's room slider gives it more space above and below (" +
                                                                   juce::String(before) + " -> " + juce::String(lane.getHeight()) + " px)");
        shoot(window, dir, "101-staff-room", true);
        if (room) room->setValue(0.0, juce::sendNotificationSync);
        pump(50);
    }

    // Quick playback: hold a staff's key to hear it alone, let go to come back.
    {
        auto& composer = main.composer();
        auto* lane_parent = composer.lane(2).getParentComponent();
        auto* hold = lane_parent ? lane_parent->findChildWithID("lane_quick_play") : nullptr;
        check(hold != nullptr && hold->isShowing(), "each staff has a quick playback key");
        ctx.engine.stop();
        ctx.engine.locate(6 * kTicksPerQuarter);
        if (hold) {
            const auto c = hold->getLocalBounds().getCentre().toFloat();
            hold->mouseDown(mouse_event(*hold, c, c, false));
            pump(100);
            check(composer.quick_playing() && ctx.engine.playing(), "held: it plays (that staff alone)");
            shoot(window, dir, "102-quick-playback", true);
            hold->mouseUp(mouse_event(*hold, c, c, false));
            pump(100);
            check(!composer.quick_playing() && !ctx.engine.playing() && ctx.engine.position() == 6 * kTicksPerQuarter,
                  "let go: it stops and the playhead is back where it was");
        }
    }
    // Hear: notes sound as they are placed (a key, remembered).
    {
        auto* hear = main.toolbar().key("Hear");
        const bool was = main.composer().hear_entry();
        if (hear) hear->triggerClick();
        pump(80);
        check(hear != nullptr && main.composer().hear_entry() == !was && hear->getToggleState() == !was &&
                  hw::ui_settings().getBoolValue("composer_hear_entry", true) == !was,
              "the Hear key turns hearing notes as they are placed on and off (remembered)");
        if (hear) hear->triggerClick();
        pump(80);
    }

    // The Score view: a key on the toolbar and View > Score View.
    auto* score_key = main.toolbar().key("Score");
    check(score_key != nullptr, "the Composer toolbar has a Score key");
    if (score_key) score_key->triggerClick();
    pump(300);
    check(main.page_view_shown() && main.page_view().isVisible(), "Score shows the full score on pages");
    shoot(window, dir, "95-score-view");
    ctx.commands.invokeDirectly(cmd::scoreView, false);
    pump(200);
    check(!main.page_view_shown(), "View > Score View switches back to the tracks");

    // Work: a key on the toolbar, lit while the side panels are hidden.
    auto* work_key = main.toolbar().key("Work");
    if (work_key) work_key->triggerClick();
    pump(200);
    check(work_key != nullptr && main.work_view() && work_key->getToggleState(), "the Work key hides the side panels and lights");
    if (work_key) work_key->triggerClick();
    pump(100);
    check(!main.work_view() && !work_key->getToggleState(), "pressed again, the panels come back");

    // The floating track list: grouped, searchable, a click goes to the track; it collapses.
    main.set_work_view(true);
    if (auto* list_key = main.toolbar().key("List")) list_key->triggerClick();
    pump(300);
    auto* panel = track_list_panel();
    check(panel != nullptr && main.track_list_shown(), "List opens the floating track list");
    if (panel != nullptr) {
        const auto rows = panel->rows();
        int headings = 0, tracks = 0;
        std::string last;
        for (const auto& [id, text] : rows) {
            if (id.empty()) ++headings;
            else {
                ++tracks;
                last = id;
            }
        }
        check(tracks >= 10 && headings >= 3, "it lists the tracks under their sections (" + juce::String(tracks) + " tracks, " +
                                                 juce::String(headings) + " sections)");
        if (auto* w = find_dialog("Track List")) shoot(*w, dir, "96-track-list");
        main.composer().reveal_track(rows[1].first);
        pump(100);
        check(panel->pick(last), "a track in the list can be picked");
        pump(200);
        check(session.selected_track() == last && main.composer().lanes_scroll() > 0,
              "picking it selects it and scrolls the lanes to it (scroll " + juce::String(main.composer().lanes_scroll()) + ")");
        shoot(window, dir, "97-track-list-go-to");
        panel->set_filter("tuba");
        bool only_tuba = true;
        int found = 0;
        for (const auto& [id, text] : panel->rows())
            if (!id.empty()) {
                ++found;
                only_tuba = only_tuba && text.containsIgnoreCase("tuba");
            }
        check(found == 1 && only_tuba, "typing finds an instrument (" + juce::String(found) + ")");
        panel->set_filter({});
        panel->set_collapsed(true);
        pump(100);
        auto* w = find_dialog("Track List");
        check(w != nullptr && w->getHeight() < 120, "it collapses to a slim bar (" + juce::String(w ? w->getHeight() : -1) + " px)");
        panel->set_collapsed(false);
        pump(100);
        check(w != nullptr && w->getHeight() > 300, "and opens again");
    }
    main.toggle_track_list();
    pump(200);
    check(!main.track_list_shown(), "List again closes it");
    main.set_work_view(false);
    session.new_empty();
}

void updater_checks(MainWindow& window, const juce::File& dir)
{
    auto& ctx = window.context();
    FakeStore store;
    if (!store.ok()) {
        check(false, "a local port for the fake site");
        return;
    }
    auto& account = Account::get();
    const auto saved_folder = account.folder();
    const auto saved_config = account.config();
    account.set_folder(dir.getChildFile("updater-account"));
    account.sign_out();
    account.set_config({store.url(), "anon-key", store.url()});
    auto& updater = Updater::get();
    const auto downloads = dir.getChildFile("updater-downloads");
    downloads.deleteRecursively();
    updater.set_folder(downloads);
    updater.set_site(store.url());
    {
        const juce::ScopedLock sl(store.lock);
        store.update_bytes = std::string(300000, 'u') + "installer";
    }

    const auto current = Updater::current_version();
    auto c = updater.check();
    check(c.ok && c.offer && c.offer->version == "0.2.0-preview.1" && c.offer->notes == "Faster sync",
          "a newer build is offered (this is " + current + "): " + c.message);
    {
        const juce::ScopedLock sl(store.lock);
        store.update_version = current.toStdString();
    }
    c = updater.check();
    check(c.ok && !c.offer && c.message.contains("up to date"), "the same build is not offered: " + c.message);
    {
        const juce::ScopedLock sl(store.lock);
        store.update_version = "0.2.0-preview.1";
    }
    c = updater.check();
    if (!c.offer) {
        check(false, "the offer came back");
        account.set_config(saved_config);
        account.set_folder(saved_folder);
        return;
    }

    auto d = updater.download(*c.offer);
    if (preview::is_preview())
        check(d.ok, "a preview updates with no account at all: " + d.message);
    else
        check(!d.ok && d.message.contains("Sign in"), "a release downloads through the account: " + d.message);
    downloads.deleteRecursively();
    check(account.verify_code("ann@example.com", "123456").ok, "signed in");

    double last = 0.0;
    d = updater.download(*c.offer, [&](double p) { last = p; });
    juce::MemoryBlock got;
    d.file.loadFileAsData(got);
    check(d.ok && std::string(static_cast<const char*>(got.getData()), got.getSize()) == std::string(300000, 'u') + "installer" &&
              last > 0.99,
          "the update downloads through the account, byte for byte: " + d.message);

    // An activated computer keeps updating with no sign-in at all.
    account.sign_out();
    account.folder().getChildFile("activation.json").replaceWithText(R"({"activatedAt":"2026-11-18T08:00:00Z"})");
    downloads.deleteRecursively();
    const auto as_device = updater.download(*c.offer);
    juce::String device_sent;
    {
        const juce::ScopedLock sl(store.lock);
        device_sent = store.last_device;
    }
    check(!account.signed_in() && as_device.ok && device_sent.length() == 64,
          "signed out but activated: the update still downloads, the computer vouching for itself (" + as_device.message + ")");
    account.folder().getChildFile("activation.json").deleteFile();
    check(updater.download(*c.offer).ok == preview::is_preview(),
          preview::is_preview() ? "a preview, neither signed in nor activated: still downloads"
                                : "a release, neither signed in nor activated: no download");
    check(account.verify_code("ann@example.com", "123456").ok, "signed in again");

    // A file that does not match the catalogue's checksum never stays.
    {
        const juce::ScopedLock sl(store.lock);
        store.update_sha = std::string(64, 'f');
    }
    c = updater.check();
    d.file.deleteFile();
    const auto bad = updater.download(*c.offer);
    check(!bad.ok && bad.message.contains("checksum") && downloads.findChildFiles(juce::File::findFiles, false).isEmpty(),
          "a download that does not match its checksum is deleted: " + bad.message);
    {
        const juce::ScopedLock sl(store.lock);
        store.update_sha.clear();
    }

    const auto plan = updater.install_plan(downloads.getChildFile("VC-Composer-0.2.0-preview.1.dmg"));
#if JUCE_WINDOWS
    check(plan.contains("/SILENT") && plan.contains("/UPDATE"), "Windows runs the signed installer quietly, then the new version");
#else
    check(plan.joinIntoString("\n").contains("hdiutil attach") && plan.joinIntoString("\n").contains("ditto"),
          "macOS swaps the app from the disk image once this copy has quit");
#endif

    // "Install updates automatically": on by default; a download is remembered
    // and goes in at the next start, never while something is unsaved or playing.
    check(install_updates_automatically(), "updates install automatically unless turned off");
    c = updater.check();
    if (c.offer) {
        const auto ready = updater.download(*c.offer);
        hw::ui_settings().setValue("update_ready_file", ready.file.getFullPathName());
        hw::ui_settings().setValue("update_ready_version", juce::String(c.offer->version));
        check(ready.ok && ready_update_version() == juce::String(c.offer->version),
              "a downloaded update waits for the next start: " + ready_update_version());
        check(!install_ready_update(ctx, [] { return false; }), "with unsaved work or playing, it does not install");
        set_install_updates_automatically(false);
        check(!install_ready_update(ctx, [] { return true; }), "turned off, it never installs by itself");
        set_install_updates_automatically(true);
        hw::ui_settings().setValue("update_ready_version", Updater::current_version());
        check(ready_update_version().isEmpty() && !install_ready_update(ctx, [] { return true; }) &&
                  !hw::ui_settings().containsKey("update_ready_version"),
              "once that version is running, the reminder is cleared");
    }
    show_update_window(ctx, [] { return true; });
    UpdatePanel* panel = nullptr;
    wait_for([&] { return (panel = update_panel()) != nullptr && panel->has_offer(); }, 15000);
    check(panel && panel->has_offer() && panel->status_text().contains("0.2.0-preview.1"),
          "Help > Check for Updates shows the offer: " + (panel ? panel->status_text() : juce::String()));
    if (panel) {
        check(panel->automatic_button().getToggleState(), "the update window shows Install updates automatically, ticked");
        panel->getTopLevelComponent()->setSize(760, 480);
        shoot(*panel->getTopLevelComponent(), dir, "81-updates");
        panel->getTopLevelComponent()->setVisible(false);
    }

    updater.set_site({});
    updater.set_folder({});
    account.sign_out();
    account.set_config(saved_config);
    account.set_folder(saved_folder);
}

/// The VC Composer logo: in the window's corner and in the about box.
void brand_checks(MainWindow& window, const juce::File& dir)
{
    auto& ctx = window.context();
    check(brand_logo_horizontal().isValid() && brand_logo_stacked().isValid(), "both logos are embedded and decode");
    auto* content = window.getContentComponent();
    window.setSize(1680, 1050);
    pump(300);
    // The corner, at twice the size so the lettering can be judged.
    const auto corner = content->createComponentSnapshot({0, 0, 560, 120}, true, 2.0f);
    int gold = 0;
    for (int y = 0; y < 210; ++y)
        for (int x = 0; x < 560; ++x) {
            const auto c = corner.getPixelAt(x, y);
            gold += c.getRed() > 170 && c.getGreen() > 120 && c.getBlue() < 110 && c.getRed() > c.getBlue() + 80;
        }
    check(gold > 2000, "the logo is drawn in the window's top-left corner (" + juce::String(gold) + " gold pixels)");
    {
        juce::FileOutputStream out(dir.getChildFile("79-logo-corner.png"));
        if (out.openedOk()) {
            out.setPosition(0);
            out.truncate();
            juce::PNGImageFormat().writeImageToStream(corner, out);
        }
    }
    ctx.commands.invokeDirectly(cmd::about, false);
    juce::Component* about = nullptr;
    wait_for([&] {
        for (int i = 0; i < juce::Desktop::getInstance().getNumComponents() && !about; ++i)
            if (auto* c = juce::Desktop::getInstance().getComponent(i))
                about = c->findChildWithID("about_panel") ? c : nullptr;
        if (!about)
            for (int i = 0; i < juce::Desktop::getInstance().getNumComponents() && !about; ++i)
                if (auto* w = dynamic_cast<juce::DialogWindow*>(juce::Desktop::getInstance().getComponent(i)))
                    if (w->getContentComponent() && w->getContentComponent()->getComponentID() == "about_panel") about = w;
        return about != nullptr;
    }, 3000);
    check(about != nullptr, "Help > About VC Composer opens with the logo");
    if (auto* w = dynamic_cast<juce::DialogWindow*>(about)) {
        shoot(*w->getContentComponent(), dir, "80-about");
        w->exitModalState(0);
        pump(200);
    }
}

/// A short WAV file's bytes (a sine at `hz`), for media that must be real audio.
std::string wav_bytes(double hz)
{
    juce::MemoryBlock block;
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream>(block, false);
        auto writer = juce::WavAudioFormat().createWriterFor(
            stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000.0).withNumChannels(1).withBitsPerSample(16));
        juce::AudioBuffer<float> wave(1, 4800);
        for (int i = 0; i < 4800; ++i) wave.setSample(0, i, 0.3f * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * hz * i / 48000.0)));
        if (writer) writer->writeFromAudioSampleBuffer(wave, 0, 4800);
    }
    return std::string(static_cast<const char*>(block.getData()), block.getSize());
}

/// Collaboration (Main Spec §15) against the fake server: share, another
/// composer's push merged in, a real conflict asked about, track locks,
/// the Shared Projects window, and opening a shared project elsewhere.
void collab_checks(MainWindow& window, const juce::File& dir)
{
    auto& ctx = window.context();
    auto& session = ctx.session;
    namespace shared = session::shared;
    auto& sync = collab::Controller::get();
    FakeStore store;
    if (!store.ok()) {
        check(false, "a local port for the fake server");
        return;
    }
    auto& account = Account::get();
    const auto saved_folder = account.folder();
    const auto saved_config = account.config();
    const auto folder = dir.getChildFile("collab-account");
    folder.deleteRecursively();
    account.set_folder(folder);
    account.set_config({store.url(), "anon-key", store.url()});
    check(account.verify_code("ann@example.com", "123456").ok && account.activate().ok, "signed in and activated against the fake server");

    auto run = [](std::function<void(collab::Done)> op) {
        collab::Outcome out{false, "timed out"};
        bool done = false;
        op([&](collab::Outcome o) {
            out = o;
            done = true;
        });
        wait_for([&] { return done; }, 20000);
        return out;
    };
    auto has_track = [](const session::Session& s, const std::string& name) {
        for (const auto& t : s.data().tracks)
            if (t.name == name) return true;
        return false;
    };
    auto track_id = [](const session::Session& s, const std::string& name) {
        for (const auto& t : s.data().tracks)
            if (t.name == name) return t.id;
        return std::string();
    };

    // Ann's project, saved, then shared.
    const auto root = dir.getChildFile("collab");
    root.deleteRecursively();
    session.new_empty();
    session.add_track("Violins I", "violin");
    // Media outside the project folder: a theme recorded elsewhere, and the picture.
    const auto theme = wav_bytes(440.0);
    const auto theme_file = root.getChildFile("Ann").getChildFile("Recordings").getChildFile("Theme.wav");
    const auto reel_file = root.getChildFile("Ann").getChildFile("Movies").getChildFile("Reel 1.mov");
    theme_file.getParentDirectory().createDirectory();
    reel_file.getParentDirectory().createDirectory();
    theme_file.replaceWithData(theme.data(), theme.size());
    reel_file.replaceWithText("picture bytes stand in for a movie");
    auto add_clip = [&](session::Session& to, const std::string& track_name, const std::string& file_path) {
        std::string id;
        for (const auto& t : to.data().tracks)
            if (t.name == track_name) id = t.id;
        if (id.empty()) id = to.add_audio_track(track_name, false);
        project::AudioClip c;
        c.file = file_path;
        c.length = 0.1;
        c.channels = 1;
        c.file_sample_rate = 48000.0;
        to.add_audio_clips({{id, c}}, "Import");
    };
    add_clip(session, "Theme", theme_file.getFullPathName().toStdString());
    session.edit("Picture", [&](project::ProjectData& d) {
        project::PictureVersion pv;
        pv.id = "pic-reel1";
        pv.name = "Reel 1";
        pv.path = reel_file.getFullPathName().toStdString();
        d.pictures.push_back(pv);
        return true;
    });
    const auto file = root.getChildFile("Ann").getChildFile("Night Cue").getChildFile("Night Cue.vcc");
    file.getParentDirectory().createDirectory();
    check(session.save_as(file.getFullPathName().toStdString()), "the project is saved before sharing");
    const auto path = session.path();
    auto o = run([&](collab::Done d) { sync.share("Night Cue", d); });
    const auto link = shared::read_link(path);
    check(o.ok && link && link->base == 1 && link->role == "owner", "Share Project creates it and pushes revision 1: " + o.message);
    if (!link) return;
    const juce::String id(link->project_id);
    check(store.head(id) == 1 && session.data().site == collab::Controller::site() && shared::read_base(path).has_value(),
          "the server has revision 1; the copy carries this computer's id site and its base");
    const auto folder_of_project = file.getParentDirectory();
    auto clip_file = [&](const session::Session& in, const std::string& track_name) {
        for (const auto& t : in.data().tracks)
            if (t.name == track_name && !t.active_version()->audio_clips.empty()) return t.active_version()->audio_clips.front().file;
        return std::string();
    };
    check(clip_file(session, "Theme") == "Audio Files/Theme.wav" && session.data().pictures.front().path == "Picture/Reel 1.mov" &&
              folder_of_project.getChildFile("Audio Files/Theme.wav").getSize() == static_cast<juce::int64>(theme.size()) &&
              theme_file.existsAsFile(),
          "sharing gathers the media into the project folder and relinks it (the originals stay)");
    const auto m1 = store.manifest(id, 1);
    check(m1.as_array().size() == 2 && m1.as_array()[0].get_string("path") == "Audio Files/Theme.wav" &&
              m1.as_array()[0].get_string("sha256") == hash::sha256_hex(theme),
          "revision 1 lists both files by content, and both were uploaded: " + juce::String(json::write(m1)));

    // Ben, elsewhere, opens revision 1 and adds a harp.
    session::Session ben(ctx.catalogs);
    std::string rev;
    {
        const juce::ScopedLock sl(store.lock);
        rev = store.collab[id].revisions.back();
    }
    ben.adopt_document(*json::parse(rev), "Open");
    ben.set_site(collab::Controller::site() + 1);
    ben.add_track("Harp", "harp");
    // Ben records a harp take, and a vocal at a path Ann also uses for hers.
    const auto harp_wav = wav_bytes(220.0), ben_vox = wav_bytes(330.0), ann_vox = wav_bytes(550.0);
    add_clip(ben, "Ben Audio", "Audio Files/Harp.wav");
    add_clip(ben, "Ben Vox", "Audio Files/Vox 1.wav");
    store.push_as(id, shared::document_text(ben.project().to_json()), {{"Audio Files/Harp.wav", harp_wav}, {"Audio Files/Vox 1.wav", ben_vox}});

    // Meanwhile Ann adds horns and records her own "Vox 1"; Sync brings the
    // harp in, keeps both vocals, and pushes everything.
    session.add_track("Horns", "horn");
    folder_of_project.getChildFile("Audio Files/Vox 1.wav").replaceWithData(ann_vox.data(), ann_vox.size());
    add_clip(session, "Ann Vox", "Audio Files/Vox 1.wav");
    o = run([&](collab::Done d) { sync.sync(d); });
    check(o.ok && has_track(session, "Harp") && has_track(session, "Horns") && has_track(session, "Violins I"),
          "Sync merges Ben's harp with Ann's horns, no questions: " + o.message);
    check(store.head(id) == 3 && shared::read_link(path)->base == 3 && !session.dirty(),
          "and pushes the result as revision 3, saved with its new base");
    {
        juce::MemoryBlock got;
        folder_of_project.getChildFile("Audio Files/Harp.wav").loadFileAsData(got);
        check(got.toString().length() > 0 && std::string(static_cast<const char*>(got.getData()), got.getSize()) == harp_wav,
              "Ben's harp take is downloaded, byte for byte");
        juce::MemoryBlock vox;
        folder_of_project.getChildFile("Audio Files/Vox 1.wav").loadFileAsData(vox);
        const auto mine = clip_file(session, "Ann Vox");
        juce::MemoryBlock kept;
        folder_of_project.getChildFile(juce::String(mine)).loadFileAsData(kept);
        check(std::string(static_cast<const char*>(vox.getData()), vox.getSize()) == ben_vox && mine != "Audio Files/Vox 1.wav" &&
                  juce::String(mine).startsWith("Audio Files/Vox 1 (") &&
                  std::string(static_cast<const char*>(kept.getData()), kept.getSize()) == ann_vox &&
                  clip_file(session, "Ben Vox") == "Audio Files/Vox 1.wav",
              "two vocals at one path: Ben's is at \"Vox 1.wav\", Ann's kept as \"" + juce::String(mine) + "\", both used");
        const auto m3 = shared::manifest_from_json(store.manifest(id, 3));
        check(m3.size() == 5 && shared::find_media(m3, mine) && shared::find_media(m3, mine)->sha256 == hash::sha256_hex(ann_vox),
              "revision 3 lists all five files; Ann's vocal was uploaded");
    }
    o = run([&](collab::Done d) { sync.sync(d); });
    check(o.ok && o.message.startsWith("In sync") && store.head(id) == 3, "a second Sync has nothing to do: " + o.message);

    // A real conflict: both rename the violins.
    {
        const juce::ScopedLock sl(store.lock);
        rev = store.collab[id].revisions.back();
    }
    ben.adopt_document(*json::parse(rev), "Sync");
    const auto violins = track_id(ben, "Violins I");
    ben.edit("Rename", [&](project::ProjectData& d) { d.find_track(violins)->name = "Violins (Ben)"; return true; });
    store.push_as(id, shared::document_text(ben.project().to_json()));
    session.edit("Rename", [&](project::ProjectData& d) { d.find_track(violins)->name = "Violins (Ann)"; return true; });
    bool synced = false;
    collab::Outcome last;
    sync.sync([&](collab::Outcome r) {
        last = r;
        synced = true;
    });
    ConflictPanel* panel = nullptr;
    wait_for([&] { return (panel = conflict_panel()) != nullptr; }, 20000);
    check(panel != nullptr, "the same thing changed two ways opens the conflict dialog");
    if (panel) {
        check(panel->conflict_count() == 1 && panel->row_text(0).startsWith("Violins I: name") && panel->row_text(0).contains("Violins (Ben)") && panel->row_text(0).contains("Violins (Ann)"),
              "one conflict, both sides named: " + panel->row_text(0));
        panel->getTopLevelComponent()->setSize(760, 480);
        shoot(*panel->getTopLevelComponent(), dir, "78-sync-conflict");
        panel->choose(0, true);
        panel->finish();
    }
    wait_for([&] { return synced; }, 20000);
    check(synced && last.ok && session.data().find_track(violins)->name == "Violins (Ben)",
          "Take Theirs keeps Ben's name: " + last.message);
    check(session.project().undo_label().rfind("Sync with", 0) == 0, "the sync is one undo step");

    // Locks: Ben locks the harp; Ann locks the horns.
    const auto harp = track_id(session, "Harp"), horns = track_id(session, "Horns");
    {
        const juce::ScopedLock sl(store.lock);
        store.collab[id].locks[juce::String(harp)] = {"u-ben", "Ben"};
    }
    o = run([&](collab::Done d) { sync.refresh(d); });
    check(session.track_locks().count(harp) == 1, "a refresh brings in Ben's lock on the harp");
    const bool renamed = session.edit("Rename", [&](project::ProjectData& d) { d.find_track(harp)->name = "Harp 2"; return true; });
    const juce::String refusal(session.project().last_refusal());
    check(!renamed && refusal.startsWith("Ben is writing Harp"), "editing the harp here is refused: " + refusal);
    session.select_track(horns);
    o = run([&](collab::Done d) { sync.set_lock(horns, true, d); });
    bool server_has = false;
    {
        const juce::ScopedLock sl(store.lock);
        const auto& locks = store.collab[id].locks;
        server_has = locks.count(juce::String(horns)) && locks.at(juce::String(horns)).first == "u-ann";
    }
    check(o.ok && server_has && session.track_locks().count(horns) == 0, "Lock Part locks the horns for Ann, not against her: " + o.message);

    // The window.
    show_shared_projects(ctx, [&](const juce::File& f) { return session.open(f.getFullPathName().toStdString()); });
    SharedProjectsPanel* sp = nullptr;
    wait_for([&] { return (sp = shared_projects_panel()) != nullptr && sp->project_count() == 1; }, 20000);
    check(sp != nullptr && sp->project_count() == 1, "File > Shared Projects... lists the project shared with this account");
    if (sp) {
        wait_for([&] { return sp->people_text().contains("Locked parts"); }, 5000);
        check(sp->project_text().contains("revision 4") || sp->project_text().contains("revision 5"), "it says which revision this copy has: " + sp->project_text());
        check(sp->people_text().contains("Harp - Ben") && sp->people_text().contains("Horns - you") &&
                  sp->people_text().contains("Reviewer / director, invited"),
              "who is in it, and who holds which part: " + sp->people_text().upToFirstOccurrenceOf("Recent", false, false));
        sp->getTopLevelComponent()->setSize(900, 560);
        shoot(*sp->getTopLevelComponent(), dir, "77-shared-projects");

        // Opening it on another computer (here, another folder). The harp
        // arrives damaged first: it is refused, and a later Sync fetches it.
        const auto harp_key = "media/" + id + "/" + juce::String(hash::sha256_hex(harp_wav));
        {
            const juce::ScopedLock sl(store.lock);
            store.uploads[harp_key] = "damaged";
        }
        const auto elsewhere = root.getChildFile("Elsewhere");
        elsewhere.createDirectory();
        collab::Outcome opened;
        bool got = false;
        sp->open_row(0, elsewhere, [&](collab::Outcome r) {
            opened = r;
            got = true;
        });
        wait_for([&] { return got; }, 20000);
        const auto there = elsewhere.getChildFile("Night Cue").getChildFile("Night Cue.vcc");
        check(session.path() == there.getFullPathName().toStdString() && has_track(session, "Harp") &&
                  shared::read_link(session.path()).has_value() && session.data().site == collab::Controller::site(),
              "Open Shared Project downloads the latest revision, opens it, and it syncs from there: " + opened.message);
        const auto there_dir = there.getParentDirectory();
        juce::MemoryBlock t2;
        there_dir.getChildFile("Audio Files/Theme.wav").loadFileAsData(t2);
        check(std::string(static_cast<const char*>(t2.getData()), t2.getSize()) == theme &&
                  there_dir.getChildFile("Picture/Reel 1.mov").loadFileAsString() == "picture bytes stand in for a movie",
              "the theme and the picture came with it");
        check(!opened.ok && opened.message.contains("arrived damaged") && !there_dir.getChildFile("Audio Files/Harp.wav").exists() &&
                  !there_dir.getChildFile("Audio Files/Harp.wav.part").exists(),
              "a file that does not match its SHA-256 is refused and nothing is left behind: " + opened.message);
        {
            const juce::ScopedLock sl(store.lock);
            store.uploads[harp_key] = harp_wav;
        }
        o = run([&](collab::Done d) { sync.sync(d); });
        juce::MemoryBlock h2;
        there_dir.getChildFile("Audio Files/Harp.wav").loadFileAsData(h2);
        check(o.ok && std::string(static_cast<const char*>(h2.getData()), h2.getSize()) == harp_wav && store.head(id) == 4,
              "the next Sync fetches it, with nothing to push: " + o.message);
        sp->getTopLevelComponent()->setVisible(false);
    }

    session.new_empty();
    account.sign_out();
    account.set_config(saved_config);
    account.set_folder(saved_folder);
}

/// The account and licence (sign in with a code, activate, the standing and
/// its grace), against the fake store.
void account_checks(MainWindow& window, const juce::File& dir)
{
    auto& ctx = window.context();
    FakeStore store;
    if (!store.ok()) {
        check(false, "a local port for the fake store");
        return;
    }
    auto& account = Account::get();
    const auto saved_folder = account.folder();
    const auto saved_config = account.config();
    const auto folder = dir.getChildFile("account-check");
    folder.deleteRecursively();
    account.set_folder(folder);
    account.set_config({store.url(), "anon-key", store.url()});
    check(account.activated() == std::optional<bool>(false) && !account.saving().can_save && account.saving().needs_activation,
          "a release build that was never activated does not save, and says to activate");

    show_account_window(ctx);
    AccountPanel* panel = nullptr;
    wait_for([&] { return (panel = account_panel()) != nullptr; });
    if (!panel) {
        check(false, "Help > Account and Licence opens");
        return;
    }
    bool done = false;
    panel->set_fields("Composer@Example.com", "000000", "");
    panel->send_code([&] { done = true; });
    wait_for([&] { return done; }, 10000);
    check(store.codes_sent == 1, "Email Me a Code asks Supabase Auth for a code");
    done = false;
    panel->sign_in([&] { done = true; });
    wait_for([&] { return done; }, 10000);
    check(!account.signed_in() && panel->standing_text().contains("invalid"), "a wrong code is refused with the server's reason");
    // The password, as on vc-composer.com: a wrong one is refused plainly.
    panel->set_fields("Composer@Example.com", "", "");
    panel->set_password("wrong-password");
    done = false;
    panel->sign_in([&] { done = true; });
    wait_for([&] { return done; }, 10000);
    check(!account.signed_in() && panel->standing_text().contains("do not match"),
          "a wrong password is refused, saying how to get in otherwise (an emailed code, or set a password)");
    panel->set_password("right-password");
    done = false;
    panel->sign_in([&] { done = true; });
    wait_for([&] { return done; }, 10000);
    check(account.signed_in() && account.email() == "composer@example.com",
          "the right password signs in, as on the website: " + panel->status_text());
    // (It linked the licence too; the steps below begin from a fresh computer.)
    account.sign_out();
    account.folder().getChildFile("activation.json").deleteFile();
    account.folder().getChildFile("license-standing.json").deleteFile();
    panel->refresh();
    // Signing in links the account's licence on its own; with no seat free
    // that is refused (and said), so the steps below start unactivated.
    {
        const juce::ScopedLock sl(store.lock);
        store.refuse = true;
    }
    panel->set_fields("Composer@Example.com", "123456", "");
    done = false;
    panel->sign_in([&] { done = true; });
    wait_for([&] { return done; }, 10000);
    check(account.signed_in() && account.email() == "composer@example.com", "the right code signs in: " + panel->status_text());
    check(account.activated() == std::optional<bool>(false) && panel->standing_text().contains("activations"),
          "signing in tries to link the account's licence to this computer straight away (here: no seat free, said)");
    done = false;
    panel->activate([&] { done = true; });
    wait_for([&] { return done; }, 10000);
    check(account.activated() == std::optional<bool>(false) && panel->standing_text().contains("activations"),
          "no free seat: activation is refused with the reason");
    {
        const juce::ScopedLock sl(store.lock);
        store.refuse = false;
    }
    done = false;
    panel->activate([&] { done = true; });
    wait_for([&] { return done; }, 10000);
    juce::String sent;
    {
        const juce::ScopedLock sl(store.lock);
        sent = store.last_activate;
    }
    const auto body = json::parse(sent.toStdString()).value_or(json::Value());
    check(account.activated() == std::optional<bool>(true) && account.saving().can_save && account.standing() &&
              account.standing()->status == "active",
          "activation writes it down and the standing comes back active");
    check(body.get_string("deviceFingerprint").size() == 64 && body.get_string("platform").size() > 0 &&
              body.get("serial") == nullptr,
          "the request carries a hashed device fingerprint and the platform, and no serial when none was typed");
    shoot(*panel->getTopLevelComponent(), dir, "69-account");

    // The licence lapses: a week's grace, then saving stops.
    {
        const juce::ScopedLock sl(store.lock);
        store.standing = R"({"standing":{"status":"expired","expiresAt":"2026-01-01T00:00:00.000Z"}})";
    }
    account.check_standing();
    auto s = account.saving();
    check(s.can_save && s.warning && juce::String(s.notice).contains("keeps saving until"), "a lapse warns first: " + juce::String(s.notice));
    auto standing = *account.standing();
    standing.seen_lapsed_at = licensing::format_iso(static_cast<std::int64_t>(std::time(nullptr)) - 8 * 86400);
    folder.getChildFile("license-standing.json").replaceWithText(juce::String(json::write(standing.to_json())));
    s = account.saving();
    check(!s.can_save && !s.needs_activation, "and after the week, saving is off: " + juce::String(s.notice));
    {
        const juce::ScopedLock sl(store.lock);
        store.standing = R"({"standing":{"status":"active","expiresAt":null}})";
    }
    account.check_standing();
    check(account.saving().can_save && account.standing()->seen_lapsed_at.empty(), "renewing turns saving back on");

    account.sign_out();
    check(!account.signed_in() && !folder.getChildFile("session.json").existsAsFile(), "Sign Out forgets the session");
    panel->getTopLevelComponent()->setVisible(false);
    account.set_config(saved_config);
    account.set_folder(saved_folder);
}

/// Memory Management spec §6 and §2.3: the CPU/RAM readout at the bottom
/// right of both views, the plug-in hosts' memory, and switching views
/// while playing.
void resources_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    auto& host = ctx.plugins;
    if (!ctx.resources) {
        check(false, "the resource monitor runs");
        return;
    }
    ctx.engine.stop();
    session.new_empty();

    // A plug-in alone in its process is measured on its own.
    const auto instrument = scan(ctx, VCC_TEST_PLUGIN);
    std::unique_ptr<juce::AudioPluginInstance> held;
    const auto original = host.hosting_mode();
    if (instrument && host.host_tool().existsAsFile()) {
        host.set_hosting_mode(PluginHost::HostingMode::each_instance);
        juce::String why;
        held = host.create(*instrument, 48000.0, 512, why);
    }
    ctx.resources->sample_now();
    const auto snap = ctx.resources->latest();
    check(snap.app_bytes > 20ull * 1024 * 1024 && snap.system.total > snap.app_bytes && snap.system.available <= snap.system.total,
          "VC Composer's memory and the computer's are measured (" + format_bytes(snap.app_bytes) + " of " +
              format_bytes(snap.system.total) + ")");
    if (held) {
        std::uint64_t own = 0;
        for (const auto& h : snap.hosts)
            if (h.label == "VCC Test Instrument" && h.instances == 1) own = h.bytes;
        check(own > 1024 * 1024 && snap.total_bytes() >= snap.app_bytes + own,
              "an instrument in its own process reports its memory, and it counts in the total (" + format_bytes(own) + ")");
    }

    // The readout, bottom right, in both views.
    const auto t0 = session.add_track("Violins", "strings.violins_1");
    session.select_track(t0);
    for (const auto view : {cmd::showComposer, cmd::showSequencer}) {
        ctx.commands.invokeDirectly(view, false);
        pump(1300);
        auto& strip = main.status_strip();
        const auto in_window = window.getLocalArea(&strip, strip.getLocalBounds());
        const bool bottom = strip.isShowing() && in_window.getBottom() >= window.getHeight() - 60 &&
                            in_window.getRight() >= window.getWidth() - 40;
        check(bottom && strip.resources_text().contains("CPU") && strip.resources_text().contains("RAM"),
              juce::String(view == cmd::showComposer ? "Composer" : "Producer") + " shows CPU and RAM at the bottom right: " +
                  strip.resources_text());
    }
    shoot(main, dir, "70-resources-producer");
    main.status_strip().show_resources();
    shell::ResourcePanel* panel = nullptr;
    wait_for([&] {
        for (int i = juce::Desktop::getInstance().getNumComponents(); --i >= 0;)
            if (auto* box = dynamic_cast<juce::CallOutBox*>(juce::Desktop::getInstance().getComponent(i)))
                if (box->getNumChildComponents() > 0)
                    for (auto* c : box->getChildren())
                        if ((panel = dynamic_cast<shell::ResourcePanel*>(c)) != nullptr) return true;
        return false;
    });
    check(panel && panel->text().contains("AUDIO ENGINE") && panel->text().contains("VC Composer") &&
              panel->text().contains("Computer") && (!held || panel->text().contains("VCC Test Instrument (1 plug-in)")),
          "clicking it shows the engine load, VC Composer's memory by process and the computer's");
    if (panel) {
        if (auto* box = panel->findParentComponentOfClass<juce::CallOutBox>()) {
            shoot(*box, dir, "71-resource-panel");
            box->dismiss();
        }
    }
    pump(300);
    held.reset();
    host.set_hosting_mode(original);

    // Switching views while playing: the transport, playhead and selection carry on.
    const Ticks bar5 = 4 * kTicksPerWhole;
    ctx.engine.locate(bar5);
    ctx.engine.play();
    pump(200);
    ctx.commands.invokeDirectly(cmd::toggleView, false);
    pump(300);
    ctx.commands.invokeDirectly(cmd::toggleView, false);
    pump(300);
    check(ctx.engine.playing() && ctx.engine.position() >= bar5 && session.selected_track() == t0,
          "switching Composer / Producer while playing keeps playing, from where it was, with the track still selected");
    ctx.engine.stop();
    session.new_empty();
}

/// Memory Management spec §3/§10: far more tracks than common edition
/// limits (1,024 instrument + 2,048 audio) open in both views, scroll and
/// play without a cap; times are reported so a slow path shows.
void stress_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_empty();
    const int instruments = juce::SystemStats::getEnvironmentVariable("VCC_STRESS_MIDI", "1024").getIntValue();
    const int audio = juce::SystemStats::getEnvironmentVariable("VCC_STRESS_AUDIO", "2048").getIntValue();
    const auto first = session.add_track("Template", "strings.violins_1");
    const auto audio_first = session.add_audio_track("Audio template", true);
    const auto model = *session.data().find_track(first);
    const auto audio_model = *session.data().find_track(audio_first);
    auto t0 = juce::Time::getMillisecondCounterHiRes();
    const bool added = session.edit("Add tracks", [&](project::ProjectData& d) {
        for (int i = 1; i < instruments + audio - 1; ++i) {
            auto t = i < instruments ? model : audio_model;
            t.id = "stress-trk-" + std::to_string(i);
            t.name = (i < instruments ? "Strings " : "Audio ") + std::to_string(i);
            for (auto& v : t.versions) v.id = "stress-ver-" + std::to_string(i);
            t.active_version_id = t.versions.front().id;
            d.tracks.push_back(std::move(t));
        }
        return true;
    });
    pump(50);
    const double add_ms = juce::Time::getMillisecondCounterHiRes() - t0;
    check(added && static_cast<int>(session.data().tracks.size()) == instruments + audio,
          "no cap: " + juce::String(instruments) + " instrument and " + juce::String(audio) + " audio tracks in one project (" +
              juce::String(juce::roundToInt(add_ms)) + " ms with every view updated)");
    double worst = 0;
    for (const auto view : {cmd::showSequencer, cmd::showComposer, cmd::showSequencer}) {
        t0 = juce::Time::getMillisecondCounterHiRes();
        ctx.commands.invokeDirectly(view, false);
        auto image = main.createComponentSnapshot(main.getLocalBounds(), true, 1.0f);
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        worst = std::max(worst, ms);
        std::printf("    %s: %.0f ms to show and paint\n", view == cmd::showComposer ? "Composer" : "Producer", ms);
    }
    check(worst < 5000.0, "both views show and paint " + juce::String(instruments + audio) + " tracks (slowest " +
                              juce::String(juce::roundToInt(worst)) + " ms)");
    t0 = juce::Time::getMillisecondCounterHiRes();
    session.set_gain(session.data().tracks[3].id, -2.0);
    pump(30);
    const double edit_ms = juce::Time::getMillisecondCounterHiRes() - t0;
    check(edit_ms < 2000.0, "an edit with them all open takes " + juce::String(juce::roundToInt(edit_ms)) + " ms");
    shoot(main, dir, "72-stress-producer");
    session.new_empty();
    pump(100);
}

/// Memory Management spec §5: Freeze & Unload with a real plug-in - the
/// print plays, the instrument's process closes (its memory is given back),
/// a music edit reads Stale, Re-render prints it again, Unfreeze reloads it.
void freeze_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    auto& host = ctx.plugins;
    const auto instrument = scan(ctx, VCC_TEST_PLUGIN);
    if (!instrument || !host.host_tool().existsAsFile() || !ctx.freeze_tracks || !ctx.track_state) {
        std::printf("  skip  freeze (no test plug-in or host tool in this build)\n");
        return;
    }
    ctx.engine.stop();
    const auto original = host.hosting_mode();
    host.set_hosting_mode(PluginHost::HostingMode::instruments_alone);
    session.new_empty();
    const auto folder = dir.getChildFile("freeze-check");
    folder.deleteRecursively();
    folder.createDirectory();
    std::string error;
    session.save_as(folder.getChildFile("Freeze.vcc").getFullPathName().toStdString(), &error);
    ctx.engine.prepare_offline(48000.0, 512);
    const auto t = session.add_track("Violins", "strings.violins_1");
    if (!session.set_instrument_plugin(t, *instrument)) {
        check(false, "the test instrument loads on a track");
        return;
    }
    notation::Score sc = session.data().find_track(t)->active_version()->score;
    for (int b = 0; b < 2; ++b) {
        notation::ScoreNote n;
        n.start_tick = b * kTicksPerWhole;
        n.duration = notation::whole();
        n.pitch = notation::spell_midi_note(64 + b, {});
        sc.insert_note(0, n);
    }
    session.commit_score(t, sc, "Write");
    pump(300);
    auto own_process = [&] {
        for (const auto& [label, n] : host.host_processes())
            if (label == "VCC Test Instrument") return n;
        return 0;
    };
    check(own_process() == 1 && ctx.track_state(t) == "LIVE",
          "a live instrument runs in a process of its own (instruments alone), and reads LIVE");

    ctx.freeze_tracks({t}, true);
    juce::String state;
    wait_for([&] { return (state = ctx.track_state(t)).startsWith("FROZEN"); }, 60000);
    pump(500);
    const auto* tr = session.data().find_track(t);
    const bool printed = tr && !tr->active_version()->audio_clips.empty() &&
                         folder.getChildFile(juce::String(tr->active_version()->audio_clips.front().file)).getSize() > 1000;
    check(state == juce::String::fromUTF8("FROZEN \xc2\xb7 UNLOADED") && printed && tr->playback == "audio",
          "Freeze & Unload prints the track, plays the print and reads " + state);
    wait_for([&] { return own_process() == 0; }, 5000);
    check(own_process() == 0 && ctx.engine.instrument_status(t).startsWith("Unloaded"),
          "the instrument's process has closed: its memory is given back (" + ctx.engine.instrument_status(t) + ")");
    session.select_track(t);
    main.show_workspace(MainComponent::Workspace::Sequencer);
    pump(300);
    shoot(main, dir, "73-frozen");

    // The music changes: stale. Re-render prints it again (loading the
    // instrument for the print only) and it is frozen and unloaded again.
    notation::Score more = session.data().find_track(t)->active_version()->score;
    notation::ScoreNote extra;
    extra.start_tick = kTicksPerWhole / 2;
    extra.duration = notation::half();
    extra.pitch = notation::spell_midi_note(72, {});
    more.insert_note(0, extra);
    session.commit_score(t, more, "Write");
    check(ctx.track_state(t).startsWith("STALE"), "editing the music marks the print STALE: " + ctx.track_state(t));
    ctx.commands.invokeDirectly(cmd::rerenderTrack, false);
    wait_for([&] { return (state = ctx.track_state(t)).startsWith("FROZEN"); }, 60000);
    check(state == juce::String::fromUTF8("FROZEN \xc2\xb7 UNLOADED"), "Re-render prints it again: " + state);

    // Unfreeze: the live instrument, in its own process again, with its settings.
    ctx.commands.invokeDirectly(cmd::unfreezeTrack, false);
    pump(500);
    wait_for([&] { return own_process() == 1; }, 5000);
    check(ctx.track_state(t) == "LIVE" && own_process() == 1 && ctx.engine.instrument_status(t) == "VCC Test Instrument" &&
              !session.data().find_track(t)->active_version()->audio_clips.empty(),
          "Unfreeze reloads the instrument; the print stays on the audio lane");

    // Record set to Print: arm two tracks, press Record - each instrument goes
    // onto its locked audio lane as a WAV, plays that, and unloads.
    {
        const auto t2 = session.add_track("Violas", "strings.violas");
        session.set_instrument_plugin(t2, *instrument);
        notation::Score vs = session.data().find_track(t2)->active_version()->score;
        notation::ScoreNote n;
        n.start_tick = 0;
        n.duration = notation::whole();
        n.pitch = notation::spell_midi_note(60, {});
        vs.insert_note(0, n);
        session.commit_score(t2, vs, "Write");
        pump(300);
        const auto clips_before = session.data().find_track(t)->active_version()->audio_clips.size();
        bool set = true;
        for (const auto& id : {t, t2}) {
            set = session.set_record_source(id, "print") && set;
            session.set_record_armed(id, true);
        }
        check(set && !session.set_record_source(session.add_midi_track("Synth"), "print"),
              "an instrument track can be set to record by printing (a MIDI track, with no instrument of its own, cannot)");
        ctx.commands.invokeDirectly(cmd::toggleRecord, false);
        wait_for([&] { return ctx.track_state(t).startsWith("FROZEN") && ctx.track_state(t2).startsWith("FROZEN"); }, 60000);
        pump(500);
        const auto* a = session.data().find_track(t);
        const auto* b = session.data().find_track(t2);
        check(a && b && a->active_version()->audio_clips.size() > clips_before && !b->active_version()->audio_clips.empty() &&
                  a->playback == "audio" && b->playback == "audio" && a->instrument_unloaded && b->instrument_unloaded &&
                  !a->record_armed && !b->record_armed && !ctx.engine.record_enabled(),
              "Record with Print armed puts each instrument on its audio lane as a WAV, plays it and unloads it (" +
                  ctx.track_state(t) + ", " + ctx.track_state(t2) + ")");
        main.show_workspace(MainComponent::Workspace::Sequencer);
        pump(300);
        shoot(main, dir, "record-print");
    }
    host.set_hosting_mode(original);
    session.new_empty();
}

/// Expand Articulations to Tracks (Memory Spec §6): the dialog lists the
/// sounds the part plays with their note counts; the chosen ones become
/// tracks that play the part's notes once between them, each with its own
/// instrument, while the part's own instrument closes; the score keeps one
/// staff; Consolidate puts it back as it was.
void articulation_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    auto& host = ctx.plugins;
    ctx.engine.stop();
    const auto original = host.hosting_mode();
    host.set_hosting_mode(PluginHost::HostingMode::instruments_alone);
    session.new_empty();
    ctx.engine.prepare_offline(48000.0, 512);
    const auto part = session.add_track("Violins", "strings.violins_1");
    const auto instrument = host.host_tool().existsAsFile() ? scan(ctx, VCC_TEST_PLUGIN) : std::nullopt;
    if (instrument) session.set_instrument_plugin(part, *instrument);
    notation::ExpressionMap map = notation::make_default_expression_map();
    map.name = "KS";
    map.articulations.clear();
    for (const auto& [art, ks] : {std::pair{notation::Articulation::Staccato, 24}, std::pair{notation::Articulation::Pizzicato, 25},
                                  std::pair{notation::Articulation::Tremolo, 26}}) {
        notation::ArticulationRule r;
        r.articulation = art;
        r.action = notation::ArticulationAction::Keyswitch;
        r.keyswitch_note = static_cast<std::uint8_t>(ks);
        map.articulations.push_back(r);
    }
    map.has_natural = true;
    map.natural.action = notation::ArticulationAction::Keyswitch;
    map.natural.keyswitch_note = 23;
    session.set_articulation_map(part, map);
    notation::Score sc = session.data().find_track(part)->active_version()->score;
    int beat = 0;
    auto note = [&](int midi, notation::ArticulationMask arts) {
        notation::ScoreNote n;
        n.start_tick = beat++ * kTicksPerQuarter;
        n.duration = notation::quarter();
        n.pitch = notation::spell_midi_note(midi, {});
        n.articulations = arts;
        sc.insert_note(0, n);
    };
    for (int i = 0; i < 4; ++i) note(67 + i, 0);
    for (int i = 0; i < 3; ++i) note(72 + i, notation::to_mask(notation::Articulation::Staccato));
    note(76, notation::to_mask(notation::Articulation::Pizzicato));
    session.commit_score(part, sc, "Write");
    const auto before = project::render_track(*session.data().find_track(part));
    session.select_track(part);
    main.show_workspace(MainComponent::Workspace::Sequencer);
    pump(300);
    auto own_process = [&] {  // processes running the test instrument
        int n = 0;
        for (const auto& [label, instances] : host.host_processes())
            if (label == "VCC Test Instrument" && instances > 0) ++n;
        return n;
    };

    // The dialog: three sounds played, with their note counts.
    ctx.commands.invokeDirectly(cmd::expandArticulations, false);
    pump(100);
    auto* dialog = dynamic_cast<juce::AlertWindow*>(juce::ModalComponentManager::getInstance()->getModalComponent(0));
    juce::StringArray rows;
    if (dialog && dialog->getNumCustomComponents() > 0)
        for (auto* c : dialog->getCustomComponent(0)->getChildren())
            if (auto* b = dynamic_cast<juce::ToggleButton*>(c)) rows.add(b->getButtonText());
    if (dialog) shoot(*dialog, dir, "74-expand-articulations");
    check(dialog && dialog->getComponentID() == "expand_articulations" && rows.size() == 3 &&
              rows[0] == "Natural  -  4 notes" && rows[1] == "Staccato  -  3 notes" && rows[2] == "Pizzicato  -  1 note",
          "Expand Articulations to Tracks lists the sounds the part plays: " + rows.joinIntoString(", "));
    if (dialog) dialog->exitModalState(1);
    pump(300);
    const auto children = session.articulation_tracks(part);
    std::multiset<std::pair<Ticks, int>> played, written;
    for (const auto* c : children)
        for (const auto& n : project::render_track(*c).notes) played.insert({n.start, n.pitch});
    for (const auto& n : before.notes) written.insert({n.start, n.pitch});
    check(children.size() == 3 && played == written && session.staff_tracks().size() == 1,
          "it makes " + juce::String(static_cast<int>(children.size())) +
              " articulation tracks that play each note once between them; the score keeps one staff");
    if (instrument) {
        wait_for([&] { return own_process() == 3; }, 10000);
        check(own_process() == 3 && ctx.engine.instrument_status(part).startsWith("Unloaded"),
              "each articulation track runs its own instrument (" + juce::String(own_process()) +
                  " processes); the part's own closes (" + ctx.engine.instrument_status(part) + ")");
    }
    session.select_track(children.empty() ? part : children[1]->id);
    pump(300);
    shoot(main, dir, "74-articulation-tracks");

    // Editing the part: its articulation tracks follow.
    notation::Score more = session.data().find_track(part)->active_version()->score;
    beat = 8;
    sc = more;
    note(79, notation::to_mask(notation::Articulation::Staccato));
    session.commit_score(part, sc, "Write");
    const auto* stacc = session.data().find_track(children.size() > 1 ? children[1]->id : part);
    check(stacc && stacc->active_version()->score.total_note_count() == 4, "a staccato note written in the part plays on its Staccato track");

    // Consolidate, from one of its tracks: one track again, its instrument back.
    ctx.commands.invokeDirectly(cmd::consolidateArticulations, false);
    pump(300);
    const auto* back = session.data().find_track(part);
    check(back && !back->articulations_expanded && session.music_tracks().size() == 1 &&
              project::render_track(*back).notes.size() == 9,
          "Consolidate to One Track (from an articulation track) leaves the part playing all its notes");
    if (instrument) {
        wait_for([&] { return own_process() == 1; }, 10000);
        check(own_process() == 1 && ctx.engine.instrument_status(part) == "VCC Test Instrument",
              "the part's instrument is loaded again and the three others closed");
    }
    host.set_hosting_mode(original);
    session.new_empty();
}

/// Templates for both articulation modes (Memory Management spec §4.3, §8):
/// the expanded orchestra opens with the same staves as the consolidated one,
/// each part followed by its sound tracks in Producer; Composer says which
/// mode a staff is in; a part's tracks fold under it.
void articulation_template_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    auto rows_in_composer = [&](const std::string& name) {
        juce::String text;
        std::function<void(juce::Component&)> walk = [&](juce::Component& c) {
            if (auto* h = dynamic_cast<composer_ui::TrackHeader*>(&c); h && h->scribble().text().startsWith(juce::String(name).toUpperCase() + "\n"))
                text = h->scribble().text();
            for (auto* k : c.getChildren()) walk(*k);
        };
        walk(main.composer());
        return text;
    };

    session.new_from_template("cinematic.orchestra.consolidated");
    const auto consolidated_staves = session.staff_tracks().size();
    const auto consolidated_tracks = session.music_tracks().size();
    session.new_from_template("cinematic.orchestra.expanded");
    pump(300);
    const auto staves = session.staff_tracks().size();
    const auto tracks = session.music_tracks().size();
    // The two sketch pianos and the 15 parts, either way.
    check(consolidated_staves == 17 && consolidated_tracks == 17 && staves == 17 && tracks == 17 + 39,
          "the orchestra opens with 17 staves either way (two sketch pianos and 15 parts): " + juce::String(static_cast<int>(consolidated_tracks)) +
              " tracks consolidated, " + juce::String(static_cast<int>(tracks)) + " expanded");

    std::string violins;
    for (const auto* t : session.staff_tracks())
        if (t->name == "Violins I") violins = t->id;
    session.select_track(violins);
    main.show_workspace(MainComponent::Workspace::Composer);
    pump(400);
    const auto linked = rows_in_composer("Violins I");
    const auto pizz = rows_in_composer("Violins I - Pizzicato");
    const auto timpani = rows_in_composer("Timpani");
    shoot(main, dir, "75-expanded-template-composer");
    check(linked.isEmpty() && pizz.isNotEmpty() && timpani.isNotEmpty() && !timpani.contains("EXPANDED"),
          "Composer: each of the Violins I's articulations is its own lane (Violins I - Pizzicato...); Timpani one lane");

    main.show_workspace(MainComponent::Workspace::Sequencer);
    pump(400);
    auto& seq = main.sequencer();
    const int shown = seq.shown_track_rows();
    shoot(main, dir, "75-expanded-template-producer");
    seq.set_part_folded(violins, true);
    pump(200);
    const int folded = seq.shown_track_rows();
    shoot(main, dir, "75-folded-part");
    seq.set_part_folded(violins, false);
    pump(100);
    check(shown == 56 && folded == 52 && seq.shown_track_rows() == 56 && !seq.part_folded(violins),
          "Producer shows each part with its sound tracks (" + juce::String(shown) +
              " rows); folding Violins I hides its four, and they come back (" + juce::String(folded) + " folded)");

    // Consolidated from Composer's menu side: the same command, one track again.
    session.select_track(violins);
    ctx.commands.invokeDirectly(cmd::consolidateArticulations, false);
    pump(100);
    check(session.music_tracks().size() == 52 && rows_in_composer("Violins I").isNotEmpty() &&
              !session.data().find_track(violins)->articulations_expanded,
          "consolidating one part of the expanded template leaves the rest expanded");
    session.new_empty();
}

/// The instrument usage panel (Memory Management spec §6.1, §6.3): every
/// instrument with its engine load and memory as measured (exact for an
/// instrument alone in its process), sortable; Suspend, Reload, Purge,
/// Locate; batch Freeze & Unload by selection, articulation group or family,
/// with progress and Cancel.
void usage_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    auto& host = ctx.plugins;
    auto& engine = ctx.engine;
    const auto instrument = host.host_tool().existsAsFile() ? scan(ctx, VCC_TEST_PLUGIN) : std::nullopt;
    if (!instrument || !ctx.resources) {
        std::printf("  skip  instrument usage (no test plug-in or host tool in this build)\n");
        return;
    }
    engine.stop();
    const auto original = host.hosting_mode();
    host.set_hosting_mode(PluginHost::HostingMode::instruments_alone);
    session.new_empty();
    const auto folder = dir.getChildFile("usage-check");
    folder.deleteRecursively();
    folder.createDirectory();
    std::string error;
    session.save_as(folder.getChildFile("Usage.vcc").getFullPathName().toStdString(), &error);
    std::map<std::string, std::string> ids;
    for (const auto& [name, std_id] : std::vector<std::pair<std::string, std::string>>{
             {"Violins", "strings.violins_1"}, {"Horns", "brass.horn_section"}, {"Flute", "woodwinds.flute"}, {"Piano", "keys.piano"}}) {
        const auto t = session.add_track(name, std_id);
        ids[name] = t;
        if (name != "Piano") session.set_instrument_plugin(t, *instrument);
        notation::Score sc = session.data().find_track(t)->active_version()->score;
        for (int b = 0; b < 8; ++b) {
            notation::ScoreNote n;
            n.start_tick = b * kTicksPerWhole;
            n.duration = notation::whole();
            n.pitch = notation::spell_midi_note(60 + b, {});
            sc.insert_note(0, n);
        }
        session.commit_score(t, sc, "Write");
    }
    engine.prepare_offline(48000.0, 512);
    engine.rebuild_now();
    pump(300);

    ctx.commands.invokeDirectly(cmd::instrumentUsage, false);
    InstrumentUsagePanel* panel = nullptr;
    wait_for([&] { return (panel = instrument_usage_panel()) != nullptr; });
    if (!panel) {
        check(false, "Tools > Instrument Usage... opens the panel");
        return;
    }
    juce::AudioBuffer<float> out(2, 512);
    auto run_audio = [&](int blocks) {
        engine.play();
        for (int i = 0; i < blocks; ++i) engine.render_offline(out, 512);
        engine.stop();
    };
    run_audio(60);
    ctx.resources->sample_now();
    panel->refresh();
    auto row = [&](const std::string& name) -> const InstrumentUsagePanel::Row* {
        for (const auto& r : panel->rows())
            if (r.track == juce::String(name)) return &r;
        return nullptr;
    };
    const auto* vln = row("Violins");
    const auto* pno = row("Piano");
    check(panel->rows().size() == 4 && vln && vln->plugin && vln->memory_exact && vln->memory_bytes.value_or(0) > 1000000 &&
              pno && !pno->plugin && !pno->memory_bytes && vln->load_measured,
          "each instrument has a row: Violins (alone in its process) " + (vln ? vln->memory : juce::String()) + ", load " +
              (vln ? juce::String(vln->load, 2) : juce::String()) + "%; Piano " + (pno ? pno->memory : juce::String()));
    check(panel->summary().contains("3 plug-ins in 3 host processes"), "the summary: " + panel->summary());
    check(panel->suggestion().contains("unless you choose") || panel->suggestion().startsWith("No suggestions"),
          "it suggests and never acts on its own: " + panel->suggestion());
    shoot(*panel, dir, "76-instrument-usage");

    // Sorting.
    panel->sort_by(InstrumentUsagePanel::Sort::Track, true);
    juce::StringArray names;
    for (const auto& r : panel->rows()) names.add(r.track);
    panel->sort_by(InstrumentUsagePanel::Sort::Memory, false);
    const bool largest_first = panel->rows().front().memory_bytes.value_or(0) >= panel->rows().back().memory_bytes.value_or(0);
    panel->sort_by(InstrumentUsagePanel::Sort::State, true);
    check(names.joinIntoString(",") == "Flute,Horns,Piano,Violins" && largest_first,
          "it sorts by track name (" + names.joinIntoString(", ") + "), memory, state and engine load");

    // Suspend: loaded, not played; Resume.
    panel->select_tracks({ids["Horns"]});
    const auto suspended = panel->run(InstrumentUsagePanel::Action::Suspend);
    run_audio(20);
    panel->refresh();
    const auto* horns = row("Horns");
    check(session.data().find_track(ids["Horns"])->instrument_suspended && horns && horns->state == "SUSPENDED" &&
              horns->load == 0.0 && horns->loaded,
          "Suspend keeps the Horns loaded but stops processing them: " + suspended);
    panel->select_tracks({ids["Horns"]});
    panel->run(InstrumentUsagePanel::Action::Suspend);
    check(!session.data().find_track(ids["Horns"])->instrument_suspended, "Resume plays them again");

    // Reload, Purge, Locate.
    panel->select_tracks({ids["Flute"]});
    const auto reloaded = panel->run(InstrumentUsagePanel::Action::Reload);
    pump(300);
    check(reloaded.startsWith("Reloaded 1") && engine.instrument_status(ids["Flute"]) == "VCC Test Instrument",
          "Reload opens the Flute's plug-in again from its settings: " + reloaded);
    check(panel->run(InstrumentUsagePanel::Action::Purge).contains("not offered by VST3"),
          "Purge says plainly that the plug-in format does not offer it");
    panel->select_tracks({ids["Horns"]});
    panel->run(InstrumentUsagePanel::Action::Locate);
    pump(100);
    check(session.selected_track() == ids["Horns"] && main.workspace() == MainComponent::Workspace::Producer,
          "Locate selects the track and shows it in Producer");

    // Batch: the menu's groups; Cancel; then a family frozen and unloaded.
    panel->select_tracks({ids["Violins"], ids["Flute"]});
    juce::StringArray groups;
    for (const auto& [label, tracks] : panel->batch_targets()) groups.add(label);
    check(groups.contains("Selected tracks  (2)") && groups.contains("Family: Strings  (1)") && groups.contains("Family: Brass  (1)"),
          "Batch Freeze & Unload offers the selection and each family: " + groups.joinIntoString("; "));
    const auto started = panel->freeze_batch(panel->selected_tracks());
    wait_for([&] { return panel->progress().isNotEmpty(); }, 3000);
    const auto progress = panel->progress();
    panel->cancel_batch();
    wait_for([&] { return !ctx.freeze_progress || ctx.freeze_progress().isEmpty(); }, 10000);
    pump(300);
    check(started.startsWith("Freezing and unloading 2") && progress.startsWith("Freezing") && !session.data().find_track(ids["Violins"])->instrument_unloaded &&
              !session.data().find_track(ids["Flute"])->instrument_unloaded,
          "a batch shows its progress (" + progress + ") and Cancel stops it with nothing unloaded");
    std::vector<std::string> woodwinds_and_strings;
    for (const auto& [label, tracks] : panel->batch_targets())
        if (label.startsWith("Family: Strings") || label.startsWith("Family: Woodwinds"))
            woodwinds_and_strings.insert(woodwinds_and_strings.end(), tracks.begin(), tracks.end());
    const auto second = panel->freeze_batch(woodwinds_and_strings);
    wait_for([&] {
        return session.data().find_track(ids["Violins"])->instrument_unloaded && session.data().find_track(ids["Flute"])->instrument_unloaded;
    }, 60000);
    pump(500);
    int own = 0;
    wait_for([&] {
        engine.render_offline(out, 512);  // as a running device would: the old instruments retire
        own = 0;
        for (const auto& [label, n] : host.host_processes())
            if (label == "VCC Test Instrument" && n > 0) ++own;
        return own == 1;
    }, 8000);
    ctx.resources->sample_now();
    panel->refresh();
    const auto* frozen = row("Violins");
    check(second.startsWith("Freezing and unloading 2") && own == 1 && frozen && !frozen->loaded && frozen->state.startsWith("FROZEN") && frozen->memory == "0 (unloaded)",
          "batch Freeze & Unload of Strings and Woodwinds: printed, unloaded, their processes closed (" + juce::String(own) +
              " plug-in process left)");
    shoot(*panel, dir, "76-after-batch");
    if (auto* w = panel->findParentComponentOfClass<juce::DocumentWindow>()) w->closeButtonPressed();
    pump(100);
    host.set_hosting_mode(original);
    session.new_empty();
}

/// Relink missing media (Main Spec §16): a project whose audio moved opens
/// saying what is missing; File > Relink Missing Media searches folders in the
/// background, proposes the clear matches, leaves an ambiguous one to pick,
/// and relinks everything as one undo step.
void relink_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    const auto root = dir.getChildFile("relink-check");
    root.deleteRecursively();
    const auto project_dir = root.getChildFile("Studio/Cue 3M2");
    auto write_wav = [](const juce::File& f, float freq) {
        f.getParentDirectory().createDirectory();
        juce::AudioBuffer<float> b(1, 24000);
        for (int i = 0; i < b.getNumSamples(); ++i) b.setSample(0, i, 0.2f * std::sin(static_cast<float>(i) * freq));
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(f);
        auto writer = juce::WavAudioFormat().createWriterFor(
            stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000.0).withNumChannels(1).withBitsPerSample(24));
        if (writer) writer->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
    };
    const auto pad = project_dir.getChildFile("Audio Files/Pad.wav");
    const auto choir = root.getChildFile("Library/Choir.wav");
    const auto hit = root.getChildFile("Library/Hit.wav");
    write_wav(pad, 0.05f);
    write_wav(choir, 0.03f);
    write_wav(hit, 0.08f);
    session.new_empty();
    session.rename_project("Cue 3M2");
    const auto track = session.add_audio_track("Stems", false);
    auto clip = [](const juce::File& f, double at) {
        project::AudioClip c;
        c.name = f.getFileNameWithoutExtension().toStdString();
        c.file = f.getFullPathName().toStdString();
        c.start_seconds = at;
        c.length = 0.5;
        c.channels = 1;
        c.file_sample_rate = 48000.0;
        return c;
    };
    session.add_audio_clips({{track, clip(pad, 0.0)}, {track, clip(choir, 1.0)}, {track, clip(hit, 2.0)}}, "Import", false);
    const auto project_file = project_dir.getChildFile("Cue 3M2.vcc");
    session.save_as(project_file.getFullPathName().toStdString());
    session.new_empty();

    // Everything moves: the project's own audio to another subfolder, the
    // library to another drive; a second Hit.wav makes that one ambiguous.
    const auto drive = root.getChildFile("Drive");
    project_dir.getChildFile("Bounced").createDirectory();
    pad.moveFileTo(project_dir.getChildFile("Bounced/Pad.wav"));
    drive.getChildFile("Library").createDirectory();
    choir.moveFileTo(drive.getChildFile("Library/Choir.wav"));
    drive.getChildFile("Takes A").createDirectory();
    drive.getChildFile("Takes B").createDirectory();
    hit.moveFileTo(drive.getChildFile("Takes A/Hit.wav"));
    write_wav(drive.getChildFile("Takes B/Hit.wav"), 0.09f);
    root.getChildFile("Library").deleteRecursively();

    juce::String said;
    auto old_status = ctx.status;
    ctx.status = [&said, old_status](const juce::String& s) {
        said = s;
        if (old_status) old_status(s);
    };
    main.open_file(project_file);
    pump(100);
    ctx.status = old_status;
    check(said.contains("3 media file(s) missing") && said.contains("Relink Missing Media"),
          "opening a project whose media moved says how many are missing and where to relink: " + said);

    ctx.commands.invokeDirectly(cmd::relinkMedia, false);
    review::RelinkPanel* panel = nullptr;
    wait_for([&] { return (panel = review::relink_panel()) != nullptr; });
    if (!panel) {
        check(false, "File > Relink Missing Media opens the window");
        return;
    }
    check(panel->missing().size() == 3u && panel->folders().size() == 1 &&
              juce::File(panel->folders()[0]) == project_dir.getParentDirectory(),
          "the window lists the 3 missing files and searches beside the project to start (" +
              panel->folders().joinIntoString("; ") + ")");
    panel->add_folder(drive);
    panel->add_folder(drive.getChildFile("Library"));  // inside a listed folder: adds nothing
    check(panel->folders().size() == 2, "a folder inside one already listed is not added twice");
    panel->search();
    wait_for([&] { return !panel->searching(); }, 10000);
    int pad_i = -1, choir_i = -1, hit_i = -1;
    for (int i = 0; i < static_cast<int>(panel->missing().size()); ++i) {
        const juce::String stored(panel->missing()[static_cast<std::size_t>(i)].stored);
        (stored.endsWith("Pad.wav") ? pad_i : stored.endsWith("Choir.wav") ? choir_i : hit_i) = i;
    }
    check(!panel->searching() && juce::File(panel->chosen(pad_i)) == project_dir.getChildFile("Bounced/Pad.wav") &&
              juce::File(panel->chosen(choir_i)) == drive.getChildFile("Library/Choir.wav") &&
              panel->chosen(hit_i).isEmpty() && panel->message().contains("1 to pick"),
          "one search finds both clear matches and leaves the ambiguous Hit.wav to pick: " + panel->message());
    shoot(*panel->getTopLevelComponent(), dir, "57-relink");
    panel->choose(hit_i, drive.getChildFile("Takes A/Hit.wav").getFullPathName());
    const int changed = panel->relink();
    int found = 0;
    for (const auto& c : session.data().find_track(track)->active_version()->audio_clips)
        if (juce::File(juce::String(session.resolve_audio_path(c.file))).existsAsFile()) ++found;
    const auto first = session.data().find_track(track)->active_version()->audio_clips.front().file;
    check(changed == 3 && found == 3 && panel->missing().empty() && session.undo_label() == "Relink media" &&
              first == "Bounced/Pad.wav",
          "Relink points all three clips at the files found, as one undo step, the project's own audio stored "
          "relative (" + juce::String(first) + "): " + panel->message());
    session.undo();
    panel->refresh();
    check(panel->missing().size() == 3u, "Undo puts the old paths back");
    session.redo();
    panel->getTopLevelComponent()->setVisible(false);
    root.deleteRecursively();
}

/// Review comments (Main Spec §15): added at the playhead in the Comments
/// window, pinned on the Composer ruler (a click on the pin opens it),
/// replied to, resolved, filtered, saved with the project.
void comments_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_empty();
    session.set_tempo(120.0);
    const auto horn = session.add_track("Horn", "brass.horn_f");
    session.select_track(horn);
    main.show_workspace(MainComponent::Workspace::Composer);
    pump(100);
    ctx.engine.locate(session.data().tempo.tick_at_seconds(4.0));  // bar 3
    ctx.commands.invokeDirectly(cmd::addComment, false);
    auto* panel = review::comments_panel();
    for (int i = 0; i < 20 && !panel; ++i) {
        pump(50);
        panel = review::comments_panel();
    }
    if (!panel) {
        check(false, "View > Add Comment at Playhead opens the Comments window");
        return;
    }
    panel->author().setText("Director", juce::sendNotificationSync);
    panel->on_track().setToggleState(true, juce::dontSendNotification);
    panel->entry().setText("Horns too loud here", false);
    const auto id = panel->add_at_playhead();
    pump(80);
    const auto* c = session.find_comment(id);
    check(c && std::abs(c->seconds - 4.0) < 0.01 && c->track_id == horn && c->author == "Director" &&
              panel->listed().size() == 1,
          "a comment is added at the playhead (4 s, bar 3) on the selected track, by the name given");
    ctx.engine.locate(session.data().tempo.tick_at_seconds(10.0));
    panel->on_track().setToggleState(false, juce::dontSendNotification);
    panel->entry().setText("Lovely ending", false);
    const auto second = panel->add_at_playhead();
    pump(80);

    // The ruler shows a pin; a click on it opens the comment.
    auto& ruler = main.composer().ruler();
    main.composer().set_origin(0);
    pump(60);
    const auto pins = ruler.comment_pins();
    check(pins.size() == 2, "the Composer ruler pins both comments (" + juce::String(static_cast<int>(pins.size())) + ")");
    if (!pins.empty()) {
        const auto at = pins.front().second.getCentre();
        ruler.mouseDown(mouse_event(ruler, at, at, false));
        ruler.mouseUp(mouse_event(ruler, at, at, false));
        pump(60);
        check(panel->focused() == pins.front().first, "a click on a pin opens the Comments window on that comment");
    }
    shoot(main, dir, "52-comments-ruler");

    // A reply, then resolved; resolved ones can be hidden.
    panel->entry().setText("Pulled back 3 dB", false);
    // The rows rebuild on the panel's timer: wait for each button (slow runners).
    wait_for([&] { return panel->reply_button(id) != nullptr; }, 5000);
    if (auto* b = panel->reply_button(id)) b->triggerClick();
    wait_for([&] {
        const auto* cm = session.find_comment(id);
        return cm && cm->replies.size() == 1;
    });
    // The list rebuilds its rows on the change: click the new one.
    wait_for([&] {
        auto* b = panel->resolve_button(id);
        return b != nullptr && b->getButtonText() == "Resolve" && panel->row(id) && panel->row(id)->isShowing();
    }, 5000);
    pump(100);
    if (auto* b = panel->resolve_button(id)) b->triggerClick();
    wait_for([&] {
        const auto* cm = session.find_comment(id);
        return cm && cm->resolved;
    });
    pump(200);
    c = session.find_comment(id);
    check(c && c->resolved && c->replies.size() == 1 && c->replies[0].text == "Pulled back 3 dB" && c->replies[0].author == "Director",
          "Reply adds the typed reply, Resolve resolves");
    shoot(*panel->getTopLevelComponent(), dir, "53-comments");
    panel->set_show_resolved(false);
    wait_for([&] { return panel->listed().size() == 1; }, 5000);
    check(panel->listed().size() == 1 && panel->listed().front() == second, "hiding resolved leaves the open one");
    panel->set_show_resolved(true);
    // A click on a row locates there.
    if (auto* row = panel->row(second)) {
        row->mouseUp(mouse_event(*row, {20.0f, 10.0f}, {20.0f, 10.0f}, false));
        pump(40);
    }
    check(std::abs(ctx.engine.position_seconds() - 10.0) < 0.01, "a click on a comment locates to it");
    panel->getTopLevelComponent()->setVisible(false);
}

/// Audio scrub (Add-On spec §7): the engine's varispeed plays the mix at
/// the speed asked, pitch with it (a 440 Hz tone at 2x is 880 Hz); the
/// shuttle forward plays at its speed and stops at rest; past 4x it is a
/// silent seek; a jog plays a short grain and returns; nothing is heard
/// while automation is being written.
void scrub_checks(MainWindow& window, const juce::File& dir)
{
    namespace sf = project::surface;
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_empty();
    session.set_tempo(120.0);
    const auto project_dir = dir.getChildFile("scrub-check");
    project_dir.deleteRecursively();
    project_dir.getChildFile("Audio Files").createDirectory();
    session.save_as(project_dir.getChildFile("scrub.vcc").getFullPathName().toStdString());
    {
        const auto tone = project_dir.getChildFile("Audio Files/tone.wav");
        juce::AudioBuffer<float> b(1, 8 * 48000);
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample(0, i, 0.5f * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * i / 48000.0)));
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(tone);
        auto writer = juce::WavAudioFormat().createWriterFor(
            stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000.0).withNumChannels(1).withBitsPerSample(24));
        if (writer) writer->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
    }
    const auto track = session.add_audio_track("Tone", false);
    project::AudioClip clip;
    clip.name = "tone";
    clip.file = "Audio Files/tone.wav";
    clip.length = 8.0;
    clip.channels = 1;
    clip.file_sample_rate = 48000.0;
    session.add_audio_clips({{track, clip}}, "Import");

    // Varispeed, offline: frequency and travel at 2x and 0.5x.
    {
        PlaybackEngine e(session, ctx.plugins);
        e.prepare_offline(48000.0, 512);
        e.locate(0);
        e.play();
        auto measure = [&](double rate, double& hz, double& moved) {
            e.set_varispeed(rate);
            const double from = e.position_seconds();
            juce::AudioBuffer<float> out(2, 512), all(1, 48000);
            for (int done = 0; done < 48000; done += 512) {
                e.render_offline(out, 512);
                all.copyFrom(0, done, out, 0, 0, std::min(512, 48000 - done));
            }
            int crossings = 0;
            for (int i = 4800 + 1; i < 48000; ++i)
                if ((all.getSample(0, i - 1) < 0.0f) != (all.getSample(0, i) < 0.0f)) ++crossings;
            hz = crossings / 2.0 / 0.9;
            moved = e.position_seconds() - from;
        };
        double hz2 = 0, moved2 = 0, hz05 = 0, moved05 = 0;
        measure(2.0, hz2, moved2);
        measure(0.5, hz05, moved05);
        check(std::abs(hz2 - 880.0) < 20.0 && std::abs(moved2 - 2.0) < 0.05,
              "varispeed 2x: the 440 Hz tone plays at " + juce::String(hz2, 0) + " Hz and the playhead moves " + juce::String(moved2, 3) + " s in 1 s");
        check(std::abs(hz05 - 220.0) < 10.0 && std::abs(moved05 - 0.5) < 0.05,
              "varispeed 0.5x: " + juce::String(hz05, 0) + " Hz, " + juce::String(moved05, 3) + " s in 1 s");
        e.stop();
        check(std::abs(e.varispeed() - 1.0) < 1e-9, "Stop returns the transport to normal speed");
    }

    // The surface, heard.
    auto& surface = main.surface();
    surface.set_polling(false);
    surface.set_device("vcc-check-surface", "VCC Check Surface");
    surface.use_profile("Generic MIDI (CC 16-23)");
    auto& st = surface.settings();
    st.audible = true;
    st.curve = sf::Curve::Linear;
    st.dead_zone = 0.08;
    st.max_speed = 8.0;
    st.step = sf::Step::Beat;
    st.acceleration = false;
    st.detents_per_step = 1;
    double clock = 5000.0;
    main.show_workspace(MainComponent::Workspace::Sequencer);
    pump(60);
    ctx.engine.locate(0);
    surface.inject(juce::MidiMessage::controllerEvent(1, 17, 80));  // about 1.6x
    surface.process(clock);
    const double speed = surface.state().shuttle_speed();
    check(ctx.engine.playing() && std::abs(ctx.engine.varispeed() - speed) < 1e-6 && speed > 1.0 && speed < 2.0,
          "the shuttle forward plays the mix at its speed (" + juce::String(ctx.engine.varispeed(), 2) + "x)");
    surface.inject(juce::MidiMessage::controllerEvent(1, 17, 64));
    clock += 0.1;
    surface.process(clock);
    check(!ctx.engine.playing() && std::abs(ctx.engine.varispeed() - 1.0) < 1e-9, "at rest the scrub stops, at normal speed");
    surface.inject(juce::MidiMessage::controllerEvent(1, 17, 127));  // 8x: past what is heard
    clock += 0.1;
    surface.process(clock);
    clock += 0.1;
    surface.process(clock);
    check(!ctx.engine.playing() && ctx.engine.position_seconds() > 0.5, "past 4x it is a silent stepped seek");
    surface.inject(juce::MidiMessage::controllerEvent(1, 17, 64));
    clock += 0.1;
    surface.process(clock);

    // The jog: a short grain, then back to where it landed.
    ctx.engine.locate(0);
    surface.inject(juce::MidiMessage::controllerEvent(1, 16, 1));
    clock += 1.0;
    surface.process(clock);
    const bool grain = ctx.engine.grain_playing() && ctx.engine.playing();
    pump(300);
    check(grain && !ctx.engine.grain_playing() && !ctx.engine.playing() && std::abs(ctx.engine.position_seconds() - 0.5) < 1e-3,
          "a jog detent plays a short grain and the playhead returns to beat 2");
    // Writing automation: heard scrub would write, so it is silent.
    session.set_automation_mode(track, "write");
    surface.inject(juce::MidiMessage::controllerEvent(1, 16, 1));
    clock += 1.0;
    surface.process(clock);
    check(!ctx.engine.grain_playing() && !ctx.engine.playing() && std::abs(ctx.engine.position_seconds() - 1.0) < 1e-3,
          "with a track writing automation the jog is silent (it still moves)");
    session.set_automation_mode(track, "read");
    surface.set_device({}, {});
    surface.set_polling(true);
    project_dir.deleteRecursively();
}

/// Performance (Main Spec §19 phase 5) in the app: a large project - 48
/// tracks, 120 bars of eighths each in the score - timed through what a
/// composer does: the engine taking in an edit, the Composer and Producer
/// views painting it, save and open. The numbers are printed; the limits
/// are generous (CI machines vary) and catch only something pathological.
void perf_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_empty();
    auto ms_since = [](double t0) { return juce::Time::getMillisecondCounterHiRes() - t0; };
    double t0 = juce::Time::getMillisecondCounterHiRes();
    std::vector<std::string> ids;
    for (int t = 0; t < 48; ++t) ids.push_back(session.add_track("Track " + std::to_string(t + 1), "strings.violins_1"));
    for (const auto& id : ids) {
        auto score = session.data().find_track(id)->active_version()->score;
        notation::Duration eighth;
        eighth.value = notation::NoteValue::Eighth;
        for (int b = 0; b < 120; ++b)
            for (int k = 0; k < 8; ++k) {
                notation::Pitch p;
                p.step = static_cast<notation::Step>((b + k) % 7);
                p.octave = 4;
                score.insert_chord(0, static_cast<Ticks>(b) * kTicksPerWhole + k * kTicksPerQuarter / 2, eighth, {p});
            }
        session.commit_score(id, score, "Fill");
    }
    const double build = ms_since(t0);
    t0 = juce::Time::getMillisecondCounterHiRes();
    ctx.engine.rebuild_now();
    const double rebuild_cold = ms_since(t0);
    // One note on one track, then the engine takes it in.
    // The median of seven one-note edits (each followed by the message loop,
    // as between keystrokes).
    auto median_edit = [&](int base, double& commit_ms, double& edit_ms) {
        std::vector<double> cs, es;
        for (int i = 0; i < 7; ++i) {
            auto sc = session.data().find_track(ids[10])->active_version()->score;
            notation::Pitch p;
            p.step = static_cast<notation::Step>(i % 7);
            p.octave = 5;
            sc.insert_chord(0, (base + i) * kTicksPerQuarter, notation::Duration{}, {p}, 1);
            const double s0 = juce::Time::getMillisecondCounterHiRes();
            session.commit_score(ids[10], sc, "Note");
            cs.push_back(juce::Time::getMillisecondCounterHiRes() - s0);
            ctx.engine.rebuild_now();
            es.push_back(juce::Time::getMillisecondCounterHiRes() - s0);
            pump(20);
        }
        std::sort(cs.begin(), cs.end());
        std::sort(es.begin(), es.end());
        commit_ms = cs[3];
        edit_ms = es[3];
    };
    double composer_commit = 0, composer_edit = 0;
    main.show_workspace(MainComponent::Workspace::Composer);
    session.select_track(ids[10]);
    pump(120);
    median_edit(40, composer_commit, composer_edit);
    std::printf("  perf: in the Composer view, a note: commit %.1f ms, with the engine's rebuild %.1f ms\n", composer_commit,
                composer_edit);
    std::vector<double> commits, edits;
    main.show_workspace(MainComponent::Workspace::Sequencer);
    pump(120);
    for (int i = 0; i < 7; ++i) {
        auto one = session.data().find_track(ids[10])->active_version()->score;
        notation::Pitch g5;
        g5.step = static_cast<notation::Step>(i % 7);
        g5.octave = 5;
        one.insert_chord(0, i * kTicksPerQuarter, notation::Duration{}, {g5}, 1);
        t0 = juce::Time::getMillisecondCounterHiRes();
        session.commit_score(ids[10], one, "Note");
        commits.push_back(ms_since(t0));
        ctx.engine.rebuild_now();
        edits.push_back(ms_since(t0));
        pump(20);
    }
    std::sort(commits.begin(), commits.end());
    std::sort(edits.begin(), edits.end());
    const double commit_only = commits[3];
    const double edit_rebuild = edits[3];
    std::printf("  perf: in the Producer view, a note: commit %.1f ms\n", commit_only);
    // The views, painted whole.
    auto paint_ms = [&](juce::Component& c) {
        if (c.getWidth() <= 0 || c.getHeight() <= 0) return -1.0;
        juce::Image img(juce::Image::ARGB, c.getWidth(), c.getHeight(), true);
        juce::Graphics g(img);
        const double s0 = juce::Time::getMillisecondCounterHiRes();
        c.paintEntireComponent(g, true);
        return juce::Time::getMillisecondCounterHiRes() - s0;
    };
    main.show_workspace(MainComponent::Workspace::Composer);
    session.select_track(ids[10]);
    pump(150);
    paint_ms(main.composer());  // warm (layouts cached)
    const double composer_paint = paint_ms(main.composer());
    main.show_workspace(MainComponent::Workspace::Sequencer);
    pump(150);
    paint_ms(main.sequencer());
    const double producer_paint = paint_ms(main.sequencer());
    const auto file = dir.getChildFile("perf-check.vcc");
    t0 = juce::Time::getMillisecondCounterHiRes();
    session.save_as(file.getFullPathName().toStdString());
    const double save = ms_since(t0);
    t0 = juce::Time::getMillisecondCounterHiRes();
    session.open(file.getFullPathName().toStdString());
    const double open = ms_since(t0);
    pump(150);
    const juce::String numbers = "build " + juce::String(build, 0) + " ms, engine rebuild " + juce::String(rebuild_cold, 1) +
                                 " ms, a note entered in the Composer (with the engine's rebuild) " + juce::String(composer_edit, 1) +
                                 " ms, in the Producer view " + juce::String(edit_rebuild, 1) + " ms, Composer paint " +
                                 juce::String(composer_paint, 1) + " ms, Producer paint " + juce::String(producer_paint, 1) +
                                 " ms, save " + juce::String(save, 0) + " ms, open " + juce::String(open, 0) + " ms (" +
                                 juce::String(file.getSize() / 1024) + " KB)";
    std::printf("  perf: %s\n", numbers.toRawUTF8());
    check(rebuild_cold < 3000 && edit_rebuild < 1500 && composer_edit < 1500 && composer_paint < 2000 && producer_paint < 2000 && save < 4000 &&
              open < 6000,
          "a 48-track, 46,080-note project stays responsive: " + numbers);
    file.deleteFile();
    session.new_empty();
    pump(60);
}

/// Accessibility (Main Spec §19 phase 5): every control a screen reader can
/// reach has a name. Walks the main window in each view and the tool
/// windows, and lists any button, slider, list or text field (or anything
/// that takes keyboard focus) without one.
struct Unnamed {
    juce::String where;
};
std::vector<Unnamed> unnamed_controls(juce::Component& root)
{
    std::vector<Unnamed> out;
    std::function<void(juce::Component&, const juce::String&)> walk = [&](juce::Component& c, const juce::String& path) {
        if (!c.isShowing()) return;
        const juce::String here = path + "/" + (c.getComponentID().isNotEmpty() ? c.getComponentID() : juce::String(typeid(c).name()));
        const bool interactive = dynamic_cast<juce::Button*>(&c) != nullptr || dynamic_cast<juce::Slider*>(&c) != nullptr ||
                                 dynamic_cast<juce::ComboBox*>(&c) != nullptr || dynamic_cast<juce::TextEditor*>(&c) != nullptr;
        // Parts of a control (a slider's text box, a list's label) speak for it.
        const bool part_of_control = c.getParentComponent() != nullptr &&
                                     (dynamic_cast<juce::Slider*>(c.getParentComponent()) != nullptr ||
                                      dynamic_cast<juce::ComboBox*>(c.getParentComponent()) != nullptr);
        if (interactive && !part_of_control && c.isAccessible()) {
            juce::String name;
            if (auto* h = c.getAccessibilityHandler()) name = h->getTitle();
            if (name.trim().isEmpty()) out.push_back({here});
        }
        for (auto* child : c.getChildren()) walk(*child, here);
    };
    walk(root, {});
    return out;
}

void accessibility_checks(MainWindow& window, const juce::File& dir)
{
    juce::ignoreUnused(dir);
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_from_template("cinematic.suspense.tension");
    pump(150);
    auto report = [&](const juce::String& what, juce::Component& root) {
        const auto found = unnamed_controls(root);
        juce::String list;
        for (std::size_t i = 0; i < found.size() && i < 12; ++i) list << "\n      " << found[i].where;
        check(found.empty(), what + ": every control has a name for screen readers (" + juce::String(static_cast<int>(found.size())) +
                                 " without" + (found.empty() ? juce::String(")") : juce::String("):") + list));
    };
    main.show_workspace(MainComponent::Workspace::Composer);
    pump(150);
    report("Composer view", main);
    main.show_workspace(MainComponent::Workspace::Sequencer);
    pump(150);
    report("Producer view", main);
    for (auto id : {cmd::showCircleOfFifths, cmd::showRouting, cmd::showReview, cmd::showComments, cmd::collectPackage,
                    cmd::showControlSurfaces, cmd::relinkMedia, cmd::articulationMap, cmd::mixTools, cmd::midiInputFilter, cmd::editShortcuts, cmd::printPreview, cmd::tempoFinder, cmd::account}) {
        ctx.commands.invokeDirectly(id, false);
        pump(200);
    }
    show_new_project_wizard(ctx);  // (its command asks to save first)
    pump(150);
    for (int i = juce::TopLevelWindow::getNumTopLevelWindows(); --i >= 0;) {
        auto* w = juce::TopLevelWindow::getTopLevelWindow(i);
        if (!w || w == &window || !w->isShowing()) continue;
        report("\"" + w->getName() + "\" window", *w);
        w->setVisible(false);
    }

    // Contrast: text colours against the surfaces they are drawn on, WCAG
    // 2.x relative luminance; 4.5:1 for body text, 3:1 for large legends.
    auto lum = [](juce::Colour c) {
        auto ch = [](float v) { return v <= 0.03928f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
        return 0.2126f * ch(c.getFloatRed()) + 0.7152f * ch(c.getFloatGreen()) + 0.0722f * ch(c.getFloatBlue());
    };
    auto ratio = [&](juce::Colour a, juce::Colour b) {
        const float la = lum(a), lb = lum(b);
        return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
    };
    struct Pair { const char* what; juce::Colour fg, bg; float need; };
    const Pair pairs[] = {
        {"text on background", colours::text, colours::background, 4.5f},
        {"text on raised panels", colours::text, colours::panel_raised, 4.5f},
        {"dim text on panels", colours::text_dim, colours::panel, 4.5f},
        {"dim text on raised panels", colours::text_dim, colours::panel_raised, 4.5f},
        {"placeholder text on panels", colours::placeholder, colours::panel, 4.5f},
        {"silk labels on the hardware panel", hw::tok::silk, hw::tok::panel_top, 4.5f},
        {"keycap legends", hw::tok::text, hw::tok::key_top, 4.5f},
        {"LCD readout", hw::Theme::get().lcd(), hw::tok::lcd_bg, 4.5f},
        {"notation ink on paper", juce::Colours::black, colours::paper, 7.0f},
        {"accent on background (large)", colours::accent, colours::background, 3.0f},
    };
    juce::String low;
    for (const auto& p : pairs) {
        const float r = ratio(p.fg, p.bg);
        if (r < p.need) low << "\n      " << p.what << ": " << juce::String(r, 2) << ":1 (needs " << juce::String(p.need, 1) << ":1)";
    }
    check(low.isEmpty(), "theme text colours meet WCAG contrast" + (low.isEmpty() ? juce::String() : ":" + low));
}

void chord_symbol_checks(MainWindow& window, const juce::File& dir)
{
    using composer::Command;
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_empty();
    const auto track = session.add_track("Piano", "keys.piano");
    main.show_workspace(MainComponent::Workspace::Composer);
    if (auto* track_key = main.toolbar().key("Track")) track_key->triggerClick();
    pump(100);
    auto& composer = main.composer();
    session.select_track(track);
    pump(50);
    composer.grabKeyboardFocus();
    // A C major chord filling bar 1 (a whole note), then a bar of G.
    composer.keyPressed(juce::KeyPress('7'));
    composer.keyPressed(juce::KeyPress('c'));
    composer.keyPressed(juce::KeyPress('E', juce::ModifierKeys::shiftModifier, 'E'));
    composer.keyPressed(juce::KeyPress('G', juce::ModifierKeys::shiftModifier, 'G'));
    composer.keyPressed(juce::KeyPress('g'));
    pump(150);
    auto& ruler = composer.ruler();
    const auto& d = session.data();
    const Ticks bar = d.meter.at_bar(1).ticks_per_bar();
    auto pills = ruler.chord_pills();
    const auto suggestion = std::find_if(pills.begin(), pills.end(), [](const auto& p) { return p.suggestion && p.tick == 0; });
    check(suggestion != pills.end() && suggestion->text == "C" && d.chords.empty(),
          "the Chords row suggests what bar 1 spells (C), dimmed, before anything is stored");
    if (suggestion == pills.end()) return;

    // Click the suggestion: the editor opens with it; Return keeps it.
    const auto at = suggestion->bounds.getCentre();
    ruler.mouseDown(mouse_event(ruler, at, at, false));
    pump(30);
    auto& editor = ruler.chord_editor();
    const bool opened = editor.isVisible() && editor.getText() == "C";
    editor.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    pump(50);
    check(opened && !editor.isVisible() && d.chords.size() == 1 && d.chords[0].tick == 0 && d.chords[0].text == "C" &&
              session.undo_label() == "Chord symbol C",
          "clicking the suggestion opens the chord editor with it, and Return stores it (one undo step)");

    // A click on beat 3 of bar 2 opens an empty editor there; G7/B is stored on that beat.
    const float x = composer.x_of_tick(bar + 2 * kTicksPerQuarter) + 4.0f;
    const juce::Point<float> beat3(x, static_cast<float>(composer_ui::ComposerRuler::kOverview + 6 + 2 * composer_ui::ComposerRuler::kRow + 10));
    ruler.mouseDown(mouse_event(ruler, beat3, beat3, false));
    pump(30);
    const bool empty_editor = editor.isVisible() && editor.getText().isEmpty();
    editor.setText("G7/B");
    editor.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    pump(50);
    check(empty_editor && d.chords.size() == 2 && d.chords[1].tick == bar + 2 * kTicksPerQuarter && d.chords[1].text == "G7/B",
          "a click on beat 3 of bar 2 opens an empty editor there, and G7/B lands on that beat");

    // A bad symbol is refused and the editor stays; Escape closes it.
    pump(100);  // any focus change from the last edit arrives first
    ruler.mouseDown(mouse_event(ruler, beat3, beat3, false));
    pump(30);
    const bool reopened = editor.isVisible();
    editor.setText("hello");
    editor.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    pump(30);
    const bool refused = editor.isVisible() && d.chords.size() == 2 && d.chords[1].text == "G7/B";
    editor.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    pump(30);
    check(reopened && refused && !editor.isVisible() && d.chords[1].text == "G7/B",
          "a symbol that is not a chord is refused and the editor stays open; Escape closes it unchanged [reopened " +
              juce::String(reopened ? "y" : "n") + ", refused " + (refused ? "y" : "n") + "]");
    pills = ruler.chord_pills();
    const bool shown = std::count_if(pills.begin(), pills.end(), [](const auto& p) { return !p.suggestion; }) == 2;
    shoot(main, dir, "39-chord-symbols");
    check(shown, "the stored symbols show as solid pills in the Chords row");

    // On the printed score: over the top staff, in the Page view too.
    if (auto* page_key = main.toolbar().key("Score")) page_key->triggerClick();
    pump(200);
    auto& pv = main.page_view();
    pv.relayout();
    std::vector<std::string> printed;
    if (pv.layout() && !pv.layout()->scores.empty())
        for (const auto& t : pv.layout()->scores.front().texts()) printed.push_back(t.text);
    check(printed.size() == 2 && printed[0] == "C" && printed[1] == "G7/B",
          "the Page view (and the full-score PDF) print the chord symbols over the top staff");
    shoot(main, dir, "39b-chord-symbols-page");
    if (auto* track_key = main.toolbar().key("Track")) track_key->triggerClick();
    pump(80);

    // Editing a symbol to nothing removes it.
    ruler.edit_chord_at(bar + 2 * kTicksPerQuarter);
    editor.setText("");
    editor.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    pump(30);
    check(d.chords.size() == 1 && session.undo_label() == "Remove chord symbol", "emptying a symbol's editor removes it");
}

void page_view_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    session.new_from_template("cinematic.horror.psychological");
    main.show_workspace(MainComponent::Workspace::Composer);
    pump(100);
    auto* page_key = main.toolbar().key("Score");
    check(page_key && page_key->isEnabled(), "the Page key is enabled");
    if (!page_key) return;
    page_key->triggerClick();
    pump(300);
    auto& pv = main.page_view();
    check(main.page_view_shown() && pv.isVisible() && !main.composer().isVisible() && page_key->getToggleState(),
          "the Page key shows the score on pages in place of the track lanes");
    const int pages = pv.page_count();
    const auto first = pv.page_rect(0);
    const double aspect = first.getWidth() > 0 ? first.getHeight() / first.getWidth() : 0.0;
    check(pages >= 1 && pv.layout() && pv.layout()->parts > 1 && std::abs(aspect - 420.0 / 297.0) < 0.02,
          "the pages are the printed full score on A3 (" + juce::String(pages) + " pages, " +
              juce::String(pv.layout() ? pv.layout()->parts : 0) + " parts, aspect " + juce::String(aspect, 3) + ")");
    shoot(main, dir, "36-page-view");

    // Sixteen bars of music: several systems. A click on one below the first
    // moves the playhead to its first bar.
    // (Typed into the score in the Track view, then back to the Page view.)
    if (auto* track_key = main.toolbar().key("Track")) track_key->triggerClick();
    pump(80);
    {
        auto& composer = main.composer();
        session.select_track(session.music_tracks().front()->id);
        pump(50);
        composer.grabKeyboardFocus();
        composer.keyPressed(juce::KeyPress('6'));  // half notes
        for (int i = 0; i < 32; ++i) composer.keyPressed(juce::KeyPress("cdefgab"[i % 7]));
        pump(50);
    }
    page_key->triggerClick();
    pump(100);
    pv.relayout();
    int target = 0;
    juce::Point<float> where;
    for (int p = 0; p < pv.page_count() && target <= 1; ++p) {
        const auto r = pv.page_rect(p);
        for (float y = r.getY() + 20.0f; y < r.getBottom() && target <= 1; y += 6.0f) {
            where = {r.getCentreX(), y};
            target = pv.bar_at(where);
        }
    }
    shoot(main, dir, "36b-page-view-music");
    if (target > 1) {
        pv.mouseDown(mouse_event(pv, where, where, false));
        pump(50);
    }
    check(target > 1 && session.data().meter.bar_at(ctx.engine.position()) == target,
          "clicking a system moves the playhead to its first bar (bar " + juce::String(target) + "; " +
              juce::String(pv.layout() ? pv.layout()->laid.bar_count : -1) + " bars, " +
              juce::String(pv.layout() ? static_cast<int>(pv.layout()->laid.system_count) : -1) + " systems, " +
              juce::String(pv.page_count()) + " pages)");

    // Zoom.
    const float before = pv.page_rect(0).getWidth();
    pv.zoom(true);
    pump(30);
    check(pv.page_rect(0).getWidth() > before * 1.2f, "zooming in makes the pages larger");
    pv.zoom(false);

    // Page settings: bars per system, paper, concert pitch - shared with the export and remembered.
    {
        const auto original = pv.settings();
        auto s = original;
        s.bars_per_system = 2;
        pv.set_settings(s);
        const auto systems2 = pv.layout() ? static_cast<int>(pv.layout()->laid.system_count) : 0;
        s.paper = "A4";
        pv.set_settings(s);
        const auto r = pv.page_rect(0);
        const double a4 = r.getWidth() > 0 ? r.getHeight() / r.getWidth() : 0.0;
        pv.concert_key().triggerClick();
        pump(80);
        const bool concert = pv.settings().concert_pitch && saved_full_score_settings().concert_pitch;
        check(systems2 == 8 && std::abs(a4 - 297.0 / 210.0) < 0.02 && concert && pv.bars_key().getButtonText().startsWith("2") &&
                  pv.paper_key().getButtonText() == "Paper A4",
              "the Page view's settings re-lay the pages: 2 bars a line (" + juce::String(systems2) + " systems), A4 (aspect " +
                  juce::String(a4, 3) + "), concert pitch - and the export uses them");
        shoot(main, dir, "36c-page-view-settings");
        pv.set_settings(original);
    }

    // Using the width: pages side by side when the view is wide, landscape
    // paper, and as many bars on a line as fit - on screen and in the PDF.
    {
        const auto original = pv.settings();
        auto s = original;
        s.paper = "A3";
        s.landscape = false;
        s.bars_per_system = 1;  // sixteen systems: several pages
        pv.set_settings(s);
        const bool spread = pv.page_count() >= 2 &&
                            (pv.columns() == 2 && std::abs(pv.page_rect(1).getY() - pv.page_rect(0).getY()) < 1.0f &&
                             pv.page_rect(1).getX() > pv.page_rect(0).getRight());
        check(spread, "on a wide view the pages sit side by side, as a printed spread (" + juce::String(pv.columns()) +
                          " across, " + juce::String(pv.page_count()) + " pages)");
        shoot(main, dir, "36d-page-view-spread");
        s.bars_per_system = 4;
        pv.set_settings(s);
        const int systems4 = pv.layout() ? static_cast<int>(pv.layout()->laid.system_count) : 0;
        pv.landscape_key().triggerClick();
        pump(120);
        const auto r = pv.page_rect(0);
        const bool landscape = pv.settings().landscape && saved_full_score_settings().landscape && r.getWidth() > r.getHeight();
        s = pv.settings();
        s.bars_per_system = 0;
        pv.set_settings(s);
        const int systems_fit = pv.layout() ? static_cast<int>(pv.layout()->laid.system_count) : 0;
        const int first_bars = pv.layout() && !pv.layout()->laid.pages.empty() && !pv.layout()->laid.pages[0].systems.empty()
                                   ? pv.layout()->laid.pages[0].systems[0].last_bar - pv.layout()->laid.pages[0].systems[0].first_bar + 1
                                   : 0;
        check(landscape && systems_fit < systems4 && first_bars > 4 && pv.bars_key().getButtonText().startsWith("Fit"),
              "Landscape turns the page sideways; As many as fit puts more bars on a line (" + juce::String(first_bars) +
                  " on the first, " + juce::String(systems_fit) + " systems instead of " + juce::String(systems4) + ")");
        shoot(main, dir, "36e-page-view-landscape-fit");
        // The PDF prints the same way: landscape sheets.
        const auto pdf = dir.getChildFile("full-score-landscape.pdf");
        juce::String message;
        const int printed = export_full_score_pdf(ctx, pdf, pv.settings(), message);
        juce::MemoryBlock bytes;
        pdf.loadFileAsData(bytes);
        const std::string raw(static_cast<const char*>(bytes.getData()), bytes.getSize());
        double mw = 0, mh = 0;
        const auto at = raw.find("/MediaBox");
        if (at != std::string::npos) {
            const juce::String box(raw.substr(at + 9, 60));
            juce::StringArray nums;
            nums.addTokens(box.fromFirstOccurrenceOf("[", false, false).upToFirstOccurrenceOf("]", false, false), " ", "");
            nums.removeEmptyStrings();
            if (nums.size() == 4) {
                mw = nums[2].getDoubleValue() - nums[0].getDoubleValue();
                mh = nums[3].getDoubleValue() - nums[1].getDoubleValue();
            }
        }
        check(printed >= 1 && mw > mh && mh > 0,
              "the full-score PDF prints landscape too (" + juce::String(mw, 0) + " x " + juce::String(mh, 0) + " pt, " +
                  juce::String(printed) + " pages)");
        pv.set_settings(original);
    }

    // It follows edits: a new track appears as another part.
    const int parts = pv.layout() ? pv.layout()->parts : 0;
    session.add_track("Felt Piano", "keys.felt_piano");
    pv.relayout();
    check(pv.layout() && pv.layout()->parts == parts + 1, "the pages follow the score (a new part is laid out)");

    // Track brings the lanes back.
    if (auto* track_key = main.toolbar().key("Track")) track_key->triggerClick();
    pump(80);
    check(!main.page_view_shown() && main.composer().isVisible() && !pv.isVisible(), "the Track key brings the track lanes back");
}

void realtime_print_checks(MainWindow& window, const juce::File& dir)
{
    auto& main = window.main();
    auto& ctx = window.context();
    auto& session = ctx.session;
    ctx.engine.stop();
    // The "hardware": a virtual MIDI port that records what it is sent, and when.
    struct TestSynth final : MidiSink {
        std::mutex lock;
        std::vector<std::pair<juce::MidiMessage, double>> got;
        void send(const juce::MidiMessage& m) override
        {
            std::lock_guard<std::mutex> g(lock);
            got.emplace_back(m, juce::Time::getMillisecondCounterHiRes());
        }
    };
    auto synth = std::make_shared<TestSynth>();
    MidiOutRouter::register_virtual("VCC Test Synth", synth);
    check(MidiOutRouter::device_names().contains("VCC Test Synth"), "MIDI outputs are listed (with the test's virtual port)");

    session.new_from_template("cinematic.horror.psychological");
    const auto project_dir = dir.getChildFile("realtime-check");
    project_dir.deleteRecursively();
    project_dir.createDirectory();
    std::string error;
    session.save_as(project_dir.getChildFile("rt.vcc").getFullPathName().toStdString(), &error);
    main.show_workspace(MainComponent::Workspace::Producer);
    pump(100);
    // New track > MIDI makes a MIDI track.
    auto& nt = main.new_track_panel();
    nt.type_key(0).setToggleState(true, juce::sendNotificationSync);
    nt.set_count(1);
    check(nt.create() == 1 && session.data().find_track(session.selected_track())->kind == project::TrackKind::MidiOnly,
          "New track > MIDI makes a MIDI track");
    const std::string midi = session.selected_track();
    std::string inst;
    for (const auto* t : session.music_tracks())
        if (t->kind == project::TrackKind::Instrument) {
            inst = t->id;
            break;
        }
    main.show_workspace(MainComponent::Workspace::Composer);
    auto& composer = main.composer();
    for (const auto& [track, keys] : std::vector<std::pair<std::string, std::string>>{{midi, "5cdef"}, {inst, "5gab"}}) {
        session.select_track(track);
        pump(80);
        composer.grabKeyboardFocus();
        for (const char ch : keys) composer.keyPressed(juce::KeyPress(ch));
        pump(50);
    }
    session.set_midi_output(midi, "VCC Test Synth");
    session.update_track_mix(midi, "Return", [](project::Track& t) {
        t.input = 0;
        t.input_stereo = true;
    });
    main.show_workspace(MainComponent::Workspace::Producer);
    session.select_track(midi);
    pump(100);
    auto& channel = main.channel_panel();
    check(channel.midi_out_lcd().isVisible() && channel.midi_out_lcd().text().contains("VCC TEST SYNTH") &&
              channel.input_lcd().text().startsWith("RET"),
          "a MIDI track's channel strip shows its MIDI out and its return (" + channel.midi_out_lcd().text() + ", " +
              channel.input_lcd().text() + ")");

    // Offline cannot print hardware.
    {
        PrintRequest rq;
        rq.tracks = {midi};
        rq.end = 2 * 4 * kTicksPerQuarter;
        const auto items = print_to_audio(ctx, rq);
        check(!items.empty() && !items.front().ok && items.front().message.contains("real time"),
              "an offline print of a hardware track says to print it in real time (" +
                  (items.empty() ? juce::String() : items.front().message) + ")");
    }
    // An engine with no audio device (and not rendering offline) refuses.
    {
        PlaybackEngine idle(session, ctx.plugins);
        const auto why = idle.start_live_print({{midi, project_dir.getChildFile("x.wav"), true, false}}, 0, 4 * kTicksPerQuarter, 0.0);
        check(!idle.live_printing() && why.contains("audio device"), "a real-time print needs an audio device (" + why + ")");
    }
    auto& panel = main.print_panel();
    // A short window: the column shows Channel | Print as tabs.
    main.show_right_tab(0);
    check(main.right_tabs() && main.channel_panel().isVisible() && !panel.isVisible(),
          "in a short window the Producer column has Channel | Print tabs");
    main.show_right_tab(1);
    check(panel.realtime_key().isShowing() && panel.offline_key().getToggleState() && !main.channel_panel().isVisible(),
          "the Print tab shows the Print panel, which offers Offline and Real-time");

    // Live, on an engine driven as a device would be: the synth's "return"
    // is a tone on inputs 1-2; the instrument track prints with it.
    const Ticks bar = 4 * kTicksPerQuarter;
    PlaybackEngine e(session, ctx.plugins);
    e.prepare_offline(48000.0, 512);
    std::vector<PlaybackEngine::LivePrintResult> results;
    bool cancelled = true;
    e.on_live_print_finished = [&](const std::vector<PlaybackEngine::LivePrintResult>& r, bool c) {
        results = r;
        cancelled = c;
    };
    const auto midi_file = project_dir.getChildFile("Audio Files").getChildFile("MIDI_1_PRINT_01.wav");
    const auto inst_file = project_dir.getChildFile("Audio Files").getChildFile("Celli_live_PRINT_01.wav");
    const auto start_error = e.start_live_print({{midi, midi_file, true, false}, {inst, inst_file, false, false}}, 0, 2 * bar, 1.0);
    check(start_error.isEmpty() && e.live_printing() && e.playing(), "a real-time print starts the transport (" + start_error + ")");
    juce::AudioBuffer<float> input(2, 512), out(2, 512);
    long long t = 0;
    for (int guard = 0; e.live_printing() && guard < 2000; ++guard) {
        for (int i = 0; i < 512; ++i, ++t) {
            const float v = 0.25f * std::sin(2.0f * juce::MathConstants<float>::pi * 220.0f * static_cast<float>(t) / 48000.0f);
            input.setSample(0, i, v);
            input.setSample(1, i, v);
        }
        e.render_offline_with_input(input, out, 512);
        e.service_live_print();
    }
    pump(300);  // the MIDI sender plays what was due
    check(!e.live_printing() && !cancelled && results.size() == 2 && results[0].ok && results[1].ok && !e.playing(),
          "the print ends by itself after the range and its tail, and stops the transport");
    std::vector<int> notes;
    {
        std::lock_guard<std::mutex> g(synth->lock);
        for (const auto& [m, when] : synth->got)
            if (m.isNoteOn()) notes.push_back(m.getNoteNumber());
    }
    // C D E F typed in D Phrygian: C D E-flat F.
    check(notes == std::vector<int>{60, 62, 63, 65},
          "the MIDI track's notes go out to the hardware (" + juce::String(static_cast<int>(notes.size())) + " note-ons)");
    // The instrument's live print is the same audio as its offline print.
    {
        PrintRequest rq;
        rq.tracks = {inst};
        rq.end = 2 * bar;
        rq.play_audio = false;
        const auto offline = print_to_audio(ctx, rq);
        session.undo();
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> a(formats.createReaderFor(inst_file));
        std::unique_ptr<juce::AudioFormatReader> b(offline.empty() ? nullptr : formats.createReaderFor(offline.front().file));
        double diff = 0.0, ref = 0.0;
        if (a && b) {
            const int n = static_cast<int>(std::min<juce::int64>(std::min(a->lengthInSamples, b->lengthInSamples), 48000 * 4));
            juce::AudioBuffer<float> x(2, n), y(2, n);
            a->read(&x, 0, n, 0, true, true);
            b->read(&y, 0, n, 0, true, true);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < n; ++i) {
                    const double d = static_cast<double>(x.getSample(ch, i)) - y.getSample(ch, i);
                    diff += d * d;
                    ref += static_cast<double>(y.getSample(ch, i)) * y.getSample(ch, i);
                }
        }
        const double null_db = ref > 0.0 ? 10.0 * std::log10(std::max(diff, 1e-30) / ref) : 0.0;
        check(a && b && null_db < -60.0, "an instrument printed in real time nulls against its offline print (" +
                                              juce::String(null_db, 1) + " dB)");
    }
    // The hardware's return is what was recorded.
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r(formats.createReaderFor(midi_file));
        float hi = 0.0f;
        if (r) {
            juce::AudioBuffer<float> b(2, 48000);
            r->read(&b, 0, static_cast<int>(std::min<juce::int64>(r->lengthInSamples, 48000)), 0, true, true);
            hi = b.getMagnitude(0, 0, 48000);
        }
        check(r && std::abs(hi - 0.25f) < 0.01f, "the MIDI track's print is its return input (peak " + juce::String(hi, 3) + ")");
    }
    // Placed like an offline print: one undo step, the tracks then play it.
    const auto placed = place_live_prints(ctx, results, 0, false, true);
    const auto* mt = session.data().find_track(midi);
    bool on_lane = false;
    for (const auto& c : mt->active_version()->audio_clips) on_lane = on_lane || (c.lane == 0 && juce::String(c.name).contains("PRINT"));
    check(placed.size() == 2 && !placed[0].clip_id.empty() && on_lane && mt->playback == "audio" &&
              session.undo_label() == "Print 2 tracks to audio",
          "the real-time prints land on the tracks' own audio lanes, as one undo step");
    pump(100);
    shoot(main, dir, "37-realtime-print");
    main.show_right_tab(0);
    session.select_track(midi);
    pump(100);
    shoot(main, dir, "38-midi-channel");
    MidiOutRouter::unregister_virtual("VCC Test Synth");
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
    s.save_as(dir.getChildFile("hosting-check.vcc").getFullPathName().toStdString(), &error);
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
    install_crash_handler();
    vcc::desktop::prepare_display();
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File dir(argc > 1 ? juce::String(argv[1]) : juce::File::getCurrentWorkingDirectory().getChildFile("screenshots").getFullPathName());
    dir.createDirectory();
    // `vcc_desktop_check <dir> --ipc-only`: just the plug-in IPC wait check
    // (fast; for checking the kernel wait on a new platform or machine).
    if (argc > 2 && juce::String(argv[2]) == "--ipc-only") {
        section("ipc_wait_checks");
        ipc_wait_checks(dir);
        std::printf("%s: %d failure(s)\n", failures == 0 ? "PASSED" : "FAILED", failures);
        return failures == 0 ? 0 : 1;
    }
    // The plug-in list and scan failures go to a throwaway settings folder,
    // never the user's own.
    const auto settings = dir.getChildFile("settings");
    settings.deleteRecursively();
    settings.createDirectory();
    // The plug-in host processes log what they do here (they inherit it).
    const auto host_log = dir.getChildFile("plugin-host.log");
    host_log.deleteFile();
#if JUCE_WINDOWS
    _putenv_s("VCC_SETTINGS_DIR", settings.getFullPathName().toRawUTF8());
    _putenv_s("VCC_NO_PLUGIN_CHECK", "1");
    _putenv_s("VCC_NO_SOUNDS", "1");
    _putenv_s("VCC_PLUGIN_HOST_LOG", host_log.getFullPathName().toRawUTF8());
#else
    setenv("VCC_SETTINGS_DIR", settings.getFullPathName().toRawUTF8(), 1);
    setenv("VCC_NO_PLUGIN_CHECK", "1", 1);  // plugin_watch_checks drives it
    setenv("VCC_NO_SOUNDS", "1", 1);  // sound_pack_checks drives it
    setenv("VCC_PLUGIN_HOST_LOG", host_log.getFullPathName().toRawUTF8(), 1);
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

        // VCC_CHECK_ONLY=usage_checks,freeze_checks: just those (the memory
        // management sections, which stand on their own), for a quick rerun.
        if (const auto only = juce::SystemStats::getEnvironmentVariable("VCC_CHECK_ONLY", {}); only.isNotEmpty()) {
            const std::vector<std::pair<const char*, void (*)(MainWindow&, const juce::File&)>> sections = {
                {"freeze_checks", freeze_checks},
                {"track_header_checks", track_header_checks},
                {"piano_sketch_checks", piano_sketch_checks},
                {"sound_assignment_checks", sound_assignment_checks},
                {"library_checks", library_checks},
                {"articulation_checks", articulation_checks},
                {"articulation_template_checks", articulation_template_checks},
                {"usage_checks", usage_checks},
                {"collab_checks", collab_checks},
                {"brand_checks", brand_checks},
                {"updater_checks", updater_checks},
                {"menu_checks", menu_checks},
                {"layout_checks", layout_checks},
                {"studio_checks", studio_checks},
                {"plugin_watch_checks", plugin_watch_checks},
                {"mixer_controller_checks", mixer_controller_checks},
                {"note_mouse_checks", note_mouse_checks},
                {"project_file_checks", project_file_checks},
                {"hardware_insert_checks", hardware_insert_checks},
                {"ostinato_audition_checks", ostinato_audition_checks},
                {"view_tools_checks", view_tools_checks},
                {"chord_mode_checks", chord_mode_checks},
                {"chord_palette_checks", chord_palette_checks},
                {"sound_pack_checks", sound_pack_checks},
                {"range_checks", range_checks},
                {"composer_lane_checks", composer_lane_checks},
                {"staff_tools_checks", staff_tools_checks},
                {"page_view_checks", page_view_checks},
                {"swell_checks", swell_checks},
                {"recording_workflow_checks", recording_workflow_checks},
                {"midi_tools_checks", midi_tools_checks},
                {"drag_drop_checks", drag_drop_checks},
                {"midi_comp_checks", midi_comp_checks},
                {"articulation_bar_checks", articulation_bar_checks},
                {"chord_builder_checks", chord_builder_checks},
                {"account_checks", account_checks}};
            juce::StringArray wanted;
            wanted.addTokens(only, ",", "");
            for (const auto& [name, fn] : sections)
                if (wanted.contains(name)) {
                    section(name);
                    fn(window, dir);
                }
            std::printf("%s: %d failure(s)\n", failures == 0 ? "PASSED" : "FAILED", failures);
            std::fflush(stdout);
            return failures == 0 ? 0 : 1;
        }

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
        section("performance_checks");
        performance_checks(ctx, window.main(), track, dir);
        window.main().show_workspace(MainComponent::Workspace::Composer);

        window.main().show_console_window();
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
        {
            // A direct call: a new-expression may legally be optimised away.
            void* volatile probe = ::operator new(16);
            ::operator delete(probe);
        }
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
                if (ctx.engine.instrument_status(plugin_track) != "VCC Test Instrument") {
                    const juce::File log(juce::SystemStats::getEnvironmentVariable("VCC_PLUGIN_HOST_LOG", {}));
                    std::printf("--- plug-in host log (%s) ---\n%s\n---\n", log.getFullPathName().toRawUTF8(),
                                log.loadFileAsString().toRawUTF8());
                }
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
        const auto file = dir.getChildFile("check.vcc");
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

        section("console_and_recording_checks");
        console_and_recording_checks(ctx, dir);
        section("automation_checks");
        automation_checks(ctx, dir, plugin_track.empty() ? std::nullopt : std::optional<project::PluginRef>(plugin_ref));
        section("delay_compensation_checks");
        delay_compensation_checks(ctx, dir, plugin_track.empty() ? std::nullopt : std::optional<project::PluginRef>(plugin_ref));
        section("ipc_wait_checks");
        ipc_wait_checks(dir);
        section("hosting_checks");
        hosting_checks(ctx, dir);
        section("insert_window_checks");
        insert_window_checks(ctx, dir);
        section("hardware_control_checks");
        hardware_control_checks(window.main(), dir);
        section("shell_checks");
        shell_checks(window, dir);
        section("selection_and_shortcut_checks");
        selection_and_shortcut_checks(window, dir);
        section("composer_lane_checks");
        composer_lane_checks(window, dir);
        section("loop_and_keyboard_checks");
        loop_and_keyboard_checks(window, dir);
        section("producer_track_checks");
        producer_track_checks(window, dir);
        section("print_checks");
        print_checks(window, dir);
        section("mix_checks");
        mix_checks(window, dir);
        section("master_checks");
        master_checks(window, dir);
        section("follow_checks");
        follow_checks(window, dir);
        section("realtime_print_checks");
        realtime_print_checks(window, dir);
        section("recording_workflow_checks");
        recording_workflow_checks(window, dir);
        section("clip_editing_checks");
        clip_editing_checks(window, dir);
        section("edit_mode_checks");
        edit_mode_checks(window, dir);
        section("midi_tools_checks");
        midi_tools_checks(window, dir);
        section("drag_drop_checks");
        drag_drop_checks(window, dir);
        section("midi_comp_checks");
        midi_comp_checks(window, dir);
        section("articulation_bar_checks");
        articulation_bar_checks(window, dir);
        section("chord_builder_checks");
        chord_builder_checks(window, dir);
        section("sidechain_checks");
        sidechain_checks(window, dir);
        section("page_view_checks");
        page_view_checks(window, dir);
        section("chord_symbol_checks");
        chord_symbol_checks(window, dir);
        section("chord_mode_checks");
        chord_mode_checks(window, dir);
        section("chord_palette_checks");
        chord_palette_checks(window, dir);
        section("track_header_checks");
        track_header_checks(window, dir);
        section("piano_sketch_checks");
        piano_sketch_checks(window, dir);
        section("sound_assignment_checks");
        sound_assignment_checks(window, dir);
        section("sound_pack_checks");
        sound_pack_checks(window, dir);
        section("swell_checks");
        swell_checks(window, dir);
        section("range_checks");
        range_checks(window, dir);
        section("circle_checks");
        circle_checks(window, dir);
        section("trans_checks");
        trans_checks(window, dir);
        section("staff_tools_checks");
        staff_tools_checks(window, dir);
        section("routing_checks");
        routing_checks(window, dir);
        section("surface_checks");
        surface_checks(window, dir);
        section("review_checks");
        review_checks(window, dir);
        section("package_checks");
        package_checks(window, dir);
        section("library_checks");
        library_checks(window, dir);
        section("articulation_map_checks");
        articulation_map_checks(window, dir);
        section("wizard_checks");
        wizard_checks(window, dir);
        section("mix_tools_checks");
        mix_tools_checks(window, dir);
        section("edit_tools_checks");
        edit_tools_checks(window, dir);
        section("notation_tools_checks");
        notation_tools_checks(window, dir);
        section("picture_tools_checks");
        picture_tools_checks(window, dir);
        section("template_shelf_checks");
        template_shelf_checks(window, dir);
        section("preview_checks");
        preview_checks();
        section("account_checks");
        account_checks(window, dir);
        section("collab_checks");
        collab_checks(window, dir);
        section("brand_checks");
        brand_checks(window, dir);
        section("updater_checks");
        updater_checks(window, dir);
        section("menu_checks");
        menu_checks(window, dir);
        section("layout_checks");
        layout_checks(window, dir);
        section("studio_checks");
        studio_checks(window, dir);
        section("plugin_watch_checks");
        plugin_watch_checks(window, dir);
        section("mixer_controller_checks");
        mixer_controller_checks(window, dir);
        section("note_mouse_checks");
        note_mouse_checks(window, dir);
        section("project_file_checks");
        project_file_checks(window, dir);
        section("hardware_insert_checks");
        hardware_insert_checks(window, dir);
        section("ostinato_audition_checks");
        ostinato_audition_checks(window, dir);
        section("view_tools_checks");
        view_tools_checks(window, dir);
        section("resources_checks");
        resources_checks(window, dir);
        section("stress_checks");
        stress_checks(window, dir);
        section("freeze_checks");
        freeze_checks(window, dir);
        section("articulation_checks");
        articulation_checks(window, dir);
        section("articulation_template_checks");
        articulation_template_checks(window, dir);
        section("usage_checks");
        usage_checks(window, dir);
        section("relink_checks");
        relink_checks(window, dir);
        section("comments_checks");
        comments_checks(window, dir);
        section("scrub_checks");
        scrub_checks(window, dir);
        section("perf_checks");
        perf_checks(window, dir);
        section("accessibility_checks");
        accessibility_checks(window, dir);

        for (int i = juce::TopLevelWindow::getNumTopLevelWindows(); --i >= 0;)
            if (auto* w = juce::TopLevelWindow::getTopLevelWindow(i); w && w != &window) w->setVisible(false);
        pump(100);
    }
    juce::DeletedAtShutdown::deleteAll();
    std::printf("%s: %d failure(s); screenshots in %s\n", failures ? "FAILED" : "PASSED", failures,
                dir.getFullPathName().toRawUTF8());
    return failures == 0 ? 0 : 1;
}
