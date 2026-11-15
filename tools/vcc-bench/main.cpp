// A performance benchmark for the document layer (Main Spec §19 phase 5,
// "performance"): a large project - 64 instrument tracks, 200 bars of
// eighth notes each in the score (102,400 notes) - and the operations a
// composer repeats all day, timed. `vcc-bench [tracks] [bars]`.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

#include "vcc/project/Render.h"
#include "vcc/project/RenderCache.h"
#include "vcc/session/Session.h"

using namespace vcc;
using Clock = std::chrono::steady_clock;

namespace {
/// Resident memory in MB (Linux; 0 elsewhere).
double rss_mb()
{
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return 0.0;
    char line[256];
    double kb = 0.0;
    while (std::fgets(line, sizeof line, f))
        if (std::sscanf(line, "VmRSS: %lf kB", &kb) == 1) break;
    std::fclose(f);
    return kb / 1024.0;
}

double time_ms(const std::function<void()>& f, int repeat = 1)
{
    const auto t0 = Clock::now();
    for (int i = 0; i < repeat; ++i) f();
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count() / repeat;
}
}  // namespace

int main(int argc, char** argv)
{
    const int tracks = argc > 1 ? std::atoi(argv[1]) : 64;
    const int bars = argc > 2 ? std::atoi(argv[2]) : 200;
    auto catalogs = session::Catalogs::load_from_directory(VCC_CONTENT_DIR);
    session::Session s(*catalogs);
    s.new_empty();
    std::vector<std::string> ids;
    const double build = time_ms([&] {
        for (int t = 0; t < tracks; ++t) ids.push_back(s.add_track("Track " + std::to_string(t + 1), "strings.violins_1"));
        for (const auto& id : ids) {
            auto score = s.data().find_track(id)->active_version()->score;
            notation::Duration eighth;
            eighth.value = notation::NoteValue::Eighth;
            for (int b = 0; b < bars; ++b)
                for (int k = 0; k < 8; ++k) {
                    notation::Pitch p;
                    p.step = static_cast<notation::Step>((b + k) % 7);
                    p.octave = 4;
                    score.insert_chord(0, static_cast<Ticks>(b) * kTicksPerWhole + k * kTicksPerQuarter / 2, eighth, {p});
                }
            s.commit_score(id, score, "Fill");
        }
    });
    std::size_t notes = 0;
    for (const auto& t : s.data().tracks) notes += t.active_version()->score.total_note_count();
    std::printf("project: %d tracks x %d bars, %zu notes (built in %.0f ms), %.0f MB resident\n", tracks, bars, notes, build,
                rss_mb());

    // An everyday edit: one note on one track (the Composer's commit).
    auto one = s.data().find_track(ids[3])->active_version()->score;
    const double commit = time_ms([&] {
        auto sc = one;
        notation::Duration q;
        notation::Pitch p;
        p.step = notation::Step::G;
        p.octave = 5;
        sc.insert_chord(0, 0, q, {p}, 1);
        s.commit_score(ids[3], sc, "Note");
    }, 10);
    const double mute = time_ms([&] { s.set_mute(ids[5], !s.data().find_track(ids[5])->mute); }, 10);
    const double fader = time_ms([&] { s.set_gain(ids[5], -3.0 - (std::rand() % 10)); }, 10);
    const double undo = time_ms([&] { s.undo(); s.redo(); }, 10);
    const double render = time_ms([&] { (void)project::render_track(*s.data().find_track(ids[3])); }, 5);
    project::RenderCache cache;
    const double cache_cold = time_ms([&] {
        for (const auto& t : s.data().tracks) (void)cache.render(t);
    });
    const double cache_warm = time_ms([&] {
        for (const auto& t : s.data().tracks) (void)cache.render(t);
    }, 5);
    const double to_json = time_ms([&] { (void)s.project().to_json(); });
    const auto doc = s.project().to_json();
    std::string text;
    const double write_json = time_ms([&] { text = json::write(doc); });
    const double parse_json = time_ms([&] { (void)json::parse(text); });
    const auto parsed = json::parse(text);
    const double from_json = time_ms([&] { (void)project::Project::from_json(*parsed); });
    std::printf("json text              %8.1f MB\n", static_cast<double>(text.size()) / (1024.0 * 1024.0));
    const std::string path = "/tmp/vcc-bench.vcc2";
    const double save = time_ms([&] { s.save_as(path); });
    session::Session r(*catalogs);
    const double load = time_ms([&] { r.open(path); });
    std::printf("commit one note        %8.2f ms\n", commit);
    std::printf("mute (an undo step)    %8.2f ms\n", mute);
    std::printf("fader move (coalesced) %8.2f ms\n", fader);
    std::printf("undo + redo            %8.2f ms\n", undo);
    std::printf("render one track       %8.2f ms\n", render);
    std::printf("render cache, cold     %8.2f ms\n", cache_cold);
    std::printf("render cache, no change%8.2f ms\n", cache_warm);
    std::printf("to_json (build values) %8.2f ms\n", to_json);
    std::printf("json::write (text)     %8.2f ms\n", write_json);
    std::printf("json::parse            %8.2f ms\n", parse_json);
    std::printf("from_json (to model)   %8.2f ms\n", from_json);
    std::printf("save                   %8.2f ms\n", save);
    std::printf("open                   %8.2f ms\n", load);
    std::printf("undo steps held        %8zu\n", s.project().history_size());
    // Memory per edit: 100 one-note edits on one track.
    const double before = rss_mb();
    for (int i = 0; i < 300; ++i) {
        auto sc = one;
        notation::Duration q;
        notation::Pitch p;
        p.step = static_cast<notation::Step>(i % 7);
        p.octave = 5;
        sc.insert_chord(0, i * kTicksPerQuarter, q, {p}, 1);
        s.commit_score(ids[3], sc, "Note");
    }
    std::printf("memory for 300 edits   %8.0f MB (%.0f MB resident in all)\n", rss_mb() - before, rss_mb());
    return 0;
}
