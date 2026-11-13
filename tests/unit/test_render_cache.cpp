// The rendered cache (Main Spec §8): re-rendering by dirty time range gives
// exactly what a whole render gives, and an edit costs its own size.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>

#include "vcc/project/RenderCache.h"
#include "vcc_test.h"

using namespace vcc;
using project::RenderStats;

namespace {
constexpr Ticks Q = kTicksPerQuarter;

project::Track make_track(int staves = 2)
{
    project::Track t;
    t.id = "trk";
    t.name = "Part";
    project::TrackVersion v;
    v.id = "ver";
    if (staves == 2) v.score.make_grand_staff();
    t.versions.push_back(v);
    t.active_version_id = "ver";
    return t;
}

notation::Score& score(project::Track& t) { return t.active_version()->score; }

bool same(const project::RenderedTrack& a, const project::RenderedTrack& b, std::string& why)
{
    auto notes_equal = [](const std::vector<NoteEvent>& x, const std::vector<NoteEvent>& y) {
        if (x.size() != y.size()) return false;
        for (std::size_t i = 0; i < x.size(); ++i)
            if (!(x[i] == y[i]) || x[i].articulation != y[i].articulation) return false;
        return true;
    };
    if (!notes_equal(a.notes, b.notes)) return why = "notes", false;
    if (a.note_ids != b.note_ids) return why = "note ids", false;
    if (!notes_equal(a.keyswitches, b.keyswitches)) return why = "keyswitches", false;
    if (a.controllers.size() != b.controllers.size()) return why = "controller count", false;
    for (std::size_t i = 0; i < a.controllers.size(); ++i) {
        const auto& x = a.controllers[i];
        const auto& y = b.controllers[i];
        if (x.tick != y.tick || x.channel != y.channel || x.controller != y.controller || x.value != y.value)
            return why = "controllers", false;
    }
    if (a.warnings != b.warnings) return why = "warnings", false;
    return true;
}

notation::ScoreNote note_at(Ticks start, notation::Duration d, int step, int octave, int alteration = 0)
{
    notation::ScoreNote n;
    n.start_tick = start;
    n.duration = d;
    n.pitch.step = static_cast<notation::Step>(step);
    n.pitch.octave = static_cast<std::int8_t>(octave);
    n.pitch.alteration = static_cast<std::int8_t>(alteration);
    return n;
}
}  // namespace

TEST_CASE("an unchanged track is served from the cache; overrides only refinish")
{
    auto t = make_track(1);
    for (int i = 0; i < 8; ++i) score(t).insert_note(0, note_at(i * Q, notation::quarter(), i % 7, 4));
    project::RenderCache cache;
    const auto first = cache.render(t);
    CHECK(cache.last().kind == RenderStats::Kind::Full);
    std::string why;
    CHECK(same(cache.render(t), first, why));
    CHECK(cache.last().kind == RenderStats::Kind::Unchanged);
    CHECK_EQ(cache.last().events_rendered, 0u);

    const auto id = score(t).notes(0)[3].id;
    t.active_version()->performance[id].onset = 30;
    const auto r = cache.render(t);
    CHECK(cache.last().kind == RenderStats::Kind::Refinish);
    CHECK(same(r, project::render_track(t), why));
    // Both readings are cached.
    CHECK(same(cache.render(t, false), project::render_track(t, false), why));
}

TEST_CASE("one edit in a long part re-renders its own bars")
{
    auto t = make_track(1);
    const int bars = 600;
    for (int i = 0; i < bars * 4; ++i) {
        auto n = note_at(i * Q, notation::quarter(), i % 7, 4 + (i / 7) % 2);
        if (i % 9 == 0) n.tied_to_next = false;
        score(t).insert_note(0, n);
    }
    score(t).add_dynamic({0, notation::DynamicLevel::Piano, 0, {}, 0});
    project::RenderCache cache;
    cache.render(t);
    const auto total = cache.last().events_total;

    // Change one note in bar 300, then others, timing each against a
    // whole render (medians: the machine may be busy).
    std::vector<double> cached_ms, full_ms;
    std::string why;
    std::size_t rendered = 0;
    for (int k = 0; k < 30; ++k) {
        auto n = score(t).notes(0)[static_cast<std::size_t>((300 + k) * 4 + 1)];
        n.pitch.alteration = static_cast<std::int8_t>(n.pitch.alteration ? 0 : 1);
        REQUIRE(score(t).modify_note(n.id, n));
        const auto a = std::chrono::steady_clock::now();
        const auto& cached = cache.render(t);
        const auto b = std::chrono::steady_clock::now();
        const auto full = project::render_track(t);
        const auto c = std::chrono::steady_clock::now();
        CHECK(cache.last().kind == RenderStats::Kind::Partial);
        rendered = std::max(rendered, cache.last().events_rendered);
        CHECK(same(cached, full, why));
        cached_ms.push_back(std::chrono::duration<double, std::milli>(b - a).count());
        full_ms.push_back(std::chrono::duration<double, std::milli>(c - b).count());
    }
    CHECK(rendered * 50 < total);
    std::sort(cached_ms.begin(), cached_ms.end());
    std::sort(full_ms.begin(), full_ms.end());
    std::printf("    %zu notes: an edit re-rendered at most %zu of %zu events (median: cache %.2f ms, whole render %.2f ms)\n",
                score(t).notes(0).size(), rendered, total, cached_ms[15], full_ms[15]);

    // A tie that reaches into the window brings its whole chain in.
    auto head = score(t).notes(0)[200 * 4 + 3];
    head.tied_to_next = true;
    auto next = score(t).notes(0)[200 * 4 + 4];
    next.pitch = head.pitch;
    REQUIRE(score(t).modify_note(head.id, head));
    REQUIRE(score(t).modify_note(next.id, next));
    CHECK(same(cache.render(t), project::render_track(t), why));
    CHECK(cache.last().kind == RenderStats::Kind::Partial);

    // A dynamic re-renders from where it changes to the end.
    score(t).add_dynamic({500 * 4 * Q, notation::DynamicLevel::Forte, 0, {}, 0});
    CHECK(same(cache.render(t), project::render_track(t), why));
    CHECK(cache.last().kind == RenderStats::Kind::Partial);
    CHECK(cache.last().from > 400 * 4 * Q);
}

TEST_CASE("an edit to one track of many costs that track, not the project")
{
    // What the playback engine does on every edit: render every track.
    std::vector<project::Track> tracks;
    for (int k = 0; k < 24; ++k) {
        auto t = make_track(1);
        t.id = "trk" + std::to_string(k);
        for (int i = 0; i < 1200; ++i) score(t).insert_note(0, note_at(i * Q / 2, notation::eighth(), (i + k) % 7, 4));
        tracks.push_back(std::move(t));
    }
    project::RenderCache cache;
    for (const auto& t : tracks) cache.render(t);
    std::vector<double> cached_ms, full_ms;
    for (int round = 0; round < 15; ++round) {
        auto& t = tracks[static_cast<std::size_t>(round) % tracks.size()];
        auto n = score(t).notes(0)[static_cast<std::size_t>(600 + round)];
        n.pitch.alteration = static_cast<std::int8_t>(n.pitch.alteration ? 0 : 1);
        REQUIRE(score(t).modify_note(n.id, n));
        int unchanged = 0;
        const auto a = std::chrono::steady_clock::now();
        for (const auto& x : tracks) {
            cache.render(x);
            if (cache.last().kind == RenderStats::Kind::Unchanged) ++unchanged;
        }
        const auto b = std::chrono::steady_clock::now();
        for (const auto& x : tracks) project::render_track(x);
        const auto c = std::chrono::steady_clock::now();
        CHECK_EQ(unchanged, 23);
        cached_ms.push_back(std::chrono::duration<double, std::milli>(b - a).count());
        full_ms.push_back(std::chrono::duration<double, std::milli>(c - b).count());
    }
    std::sort(cached_ms.begin(), cached_ms.end());
    std::sort(full_ms.begin(), full_ms.end());
    std::printf("    24 tracks x 1200 notes, one edit: cache %.2f ms, whole renders %.2f ms (medians)\n", cached_ms[7],
                full_ms[7]);
    CHECK(cached_ms[7] * 3 < full_ms[7]);
}

TEST_CASE("random edits: the cache always equals a whole render")
{
    std::mt19937 rng(20261113);
    auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
    const notation::Duration values[] = {notation::quarter(), notation::eighth(), notation::sixteenth(),
                                         notation::half(), notation::quarter(1),
                                         notation::triplet(notation::NoteValue::Eighth)};
    const notation::Articulation arts[] = {notation::Articulation::None,     notation::Articulation::Staccato,
                                           notation::Articulation::Accent,   notation::Articulation::Legato,
                                           notation::Articulation::Pizzicato, notation::Articulation::Trill,
                                           notation::Articulation::Tremolo,  notation::Articulation::Harmonic};

    for (int round = 0; round < 6; ++round) {
        auto t = make_track(round % 2 ? 2 : 1);
        if (round >= 3) t.expression_map = "orchestral_cc";
        project::RenderCache cache;
        cache.render(t);
        int partial = 0, mismatches = 0;
        for (int step = 0; step < 220; ++step) {
            auto& sc = score(t);
            const std::size_t staff = static_cast<std::size_t>(pick(static_cast<int>(sc.staff_count())));
            const auto& notes = sc.notes(staff);
            const int op = pick(20);
            if (op < 8 || notes.empty()) {
                auto n = note_at(pick(64) * Q / 3, values[pick(6)], pick(7), 3 + pick(3), pick(3) - 1);
                n.voice = static_cast<std::uint8_t>(pick(2));
                const auto art = arts[pick(8)];
                if (art != notation::Articulation::None)
                    n.articulations = notation::to_mask(art);
                if (pick(6) == 0) {
                    n.grace = true;
                    n.duration = notation::eighth();
                    n.grace_order = static_cast<std::uint8_t>(pick(2));
                } else if (pick(4) == 0) {
                    n.tied_to_next = true;
                    auto cont = n;
                    cont.tied_to_next = false;
                    cont.articulations = 0;
                    cont.start_tick = n.end_tick();
                    sc.insert_note(staff, cont);
                }
                sc.insert_note(staff, n);
            } else if (op < 11) {
                sc.remove_note(notes[static_cast<std::size_t>(pick(static_cast<int>(notes.size())))].id);
            } else if (op < 15) {
                auto n = notes[static_cast<std::size_t>(pick(static_cast<int>(notes.size())))];
                switch (pick(4)) {
                    case 0: n.start_tick = std::max<Ticks>(0, n.start_tick + (pick(5) - 2) * Q / 2); break;
                    case 1: n.pitch.step = static_cast<notation::Step>(pick(7)); break;
                    case 2: if (!n.grace) n.tied_to_next = !n.tied_to_next; break;
                    default: n.articulations = static_cast<notation::ArticulationMask>(pick(64)); break;
                }
                sc.modify_note(n.id, n);
            } else if (op == 15) {
                sc.add_dynamic({pick(20) * Q, static_cast<notation::DynamicLevel>(pick(6)), 0, {},
                                static_cast<std::uint8_t>(staff)});
            } else if (op == 16 && !sc.dynamics().empty()) {
                const auto d = sc.dynamics()[static_cast<std::size_t>(pick(static_cast<int>(sc.dynamics().size())))];
                sc.remove_dynamic(d.tick, d.voice, d.staff);
            } else if (op == 17) {
                notation::Hairpin h;
                h.start_tick = pick(16) * Q;
                h.end_tick = h.start_tick + (1 + pick(6)) * Q;
                h.kind = pick(2) ? notation::HairpinKind::Crescendo : notation::HairpinKind::Diminuendo;
                h.staff = static_cast<std::uint8_t>(staff);
                sc.add_hairpin(h);
            } else if (op == 18 && !notes.empty()) {
                const auto id = notes[static_cast<std::size_t>(pick(static_cast<int>(notes.size())))].id;
                auto& o = t.active_version()->performance[id];
                o.onset = pick(81) - 40;
                o.velocity = pick(21) - 10;
                o.cc = pick(11) - 5;
            } else {
                project::MidiRegion r;
                r.id = "rgn" + std::to_string(step);
                r.start = pick(8) * 4 * Q;
                r.end = r.start + 4 * Q;
                r.notes = {{r.start + pick(4) * Q, Q, 60 + pick(12), 90, 0, {}}};
                t.active_version()->regions.push_back(r);
            }
            if (step % 50 == 49) {
                auto st = sc.staff(0);
                st.transposition_semitones = static_cast<std::int8_t>(st.transposition_semitones == 0 ? -2 : 0);
                sc.set_staff(0, st);
            }
            const bool perf = pick(2) == 0;
            std::string why;
            const auto cached = cache.render(t, perf);
            if (cache.last().kind == RenderStats::Kind::Partial) ++partial;
            if (!same(cached, project::render_track(t, perf), why)) {
                if (mismatches++ < 3)
                    std::fprintf(stderr, "    round %d step %d (op %d, %s): %s differ (window %lld-%lld)\n", round, step,
                                 op, to_string(cache.last().kind), why.c_str(),
                                 static_cast<long long>(cache.last().from), static_cast<long long>(cache.last().to));
            }
        }
        CHECK_EQ(mismatches, 0);
        CHECK(partial > 50);
    }
}
