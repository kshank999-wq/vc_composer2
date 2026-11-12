#include "vcc/ostinato/Ostinato.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "vcc/core/Files.h"

namespace vcc::ostinato {

// --- Random -------------------------------------------------------------------

namespace {

std::uint64_t splitmix(std::uint64_t& x) noexcept
{
    std::uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

std::uint64_t rotl(std::uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }

}  // namespace

Rng::Rng(std::uint64_t seed) noexcept
{
    std::uint64_t x = seed;
    for (auto& s : s_) s = splitmix(x);
}

std::uint64_t Rng::next() noexcept
{
    const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
}

int Rng::range(int lo, int hi) noexcept
{
    if (hi <= lo) return lo;
    const std::uint64_t span = static_cast<std::uint64_t>(hi - lo) + 1;
    const std::uint64_t limit = UINT64_MAX - (UINT64_MAX % span);
    std::uint64_t r;
    do r = next(); while (r >= limit);
    return lo + static_cast<int>(r % span);
}

double Rng::unit() noexcept { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }

std::uint64_t derive_seed(std::uint64_t seed, const std::string& label) noexcept
{
    std::uint64_t h = 0xCBF29CE484222325ULL;
    for (unsigned char c : label) {
        h ^= c;
        h *= 0x100000001B3ULL;
    }
    std::uint64_t x = seed ^ h;
    return splitmix(x);
}

// --- Enums --------------------------------------------------------------------

Ticks grid_ticks(Grid g) noexcept
{
    switch (g) {
        case Grid::Whole: return kTicksPerWhole;
        case Grid::Half: return kTicksPerQuarter * 2;
        case Grid::Quarter: return kTicksPerQuarter;
        case Grid::Eighth: return kTicksPerQuarter / 2;
        case Grid::EighthTriplet: return kTicksPerQuarter / 3;
        case Grid::Sixteenth: return kTicksPerQuarter / 4;
        case Grid::ThirtySecond: return kTicksPerQuarter / 8;
    }
    return kTicksPerQuarter / 4;
}

namespace {
const std::pair<Grid, const char*> kGrids[] = {
    {Grid::Whole, "whole"}, {Grid::Half, "half"}, {Grid::Quarter, "quarter"}, {Grid::Eighth, "8th"},
    {Grid::EighthTriplet, "8th_triplet"}, {Grid::Sixteenth, "16th"}, {Grid::ThirtySecond, "32nd"}};
const std::pair<PitchMode, const char*> kPitchModes[] = {
    {PitchMode::RootOnly, "root_only"}, {PitchMode::ChordTones, "chord_tones"},
    {PitchMode::ScaleTones, "scale_tones"}, {PitchMode::PassingTones, "passing_tones"},
    {PitchMode::Pedal, "pedal"}, {PitchMode::ChromaticTension, "chromatic_tension"},
    {PitchMode::ArpeggioUp, "arpeggio_up"}, {PitchMode::ArpeggioUpDown, "arpeggio_updown"},
    {PitchMode::Alberti, "alberti"}, {PitchMode::Octave, "octave"}, {PitchMode::Drum, "drum"}};
const std::pair<Contour, const char*> kContours[] = {
    {Contour::Static, "static"}, {Contour::Up, "up"}, {Contour::Down, "down"},
    {Contour::UpDown, "updown"}, {Contour::RandomWalk, "random_walk"}};
const std::pair<AccentPattern, const char*> kAccents[] = {
    {AccentPattern::None, "none"}, {AccentPattern::Downbeats, "downbeats"},
    {AccentPattern::Offbeats, "offbeats"}, {AccentPattern::Backbeat, "backbeat"},
    {AccentPattern::Groups332, "groups_3_3_2"}, {AccentPattern::Syncopated, "syncopated"}};

template <typename E, std::size_t N>
const char* name_of(const std::pair<E, const char*> (&table)[N], E v) noexcept
{
    for (const auto& p : table)
        if (p.first == v) return p.second;
    return table[0].second;
}

template <typename E, std::size_t N>
std::optional<E> parse_of(const std::pair<E, const char*> (&table)[N], const std::string& t) noexcept
{
    for (const auto& p : table)
        if (t == p.second) return p.first;
    return std::nullopt;
}
}  // namespace

const char* to_string(Grid g) noexcept { return name_of(kGrids, g); }
std::optional<Grid> grid_from_string(const std::string& t) noexcept { return parse_of(kGrids, t); }
const char* to_string(PitchMode m) noexcept { return name_of(kPitchModes, m); }
std::optional<PitchMode> pitch_mode_from_string(const std::string& t) noexcept { return parse_of(kPitchModes, t); }
const char* to_string(Contour c) noexcept { return name_of(kContours, c); }
std::optional<Contour> contour_from_string(const std::string& t) noexcept { return parse_of(kContours, t); }
std::optional<AccentPattern> accent_pattern_from_string(const std::string& t) noexcept { return parse_of(kAccents, t); }

void OstinatoOptions::apply_preset(const json::Value& p)
{
    if (auto g = grid_from_string(p.get_string("grid"))) grid = *g;
    if (auto m = pitch_mode_from_string(p.get_string("pitch_mode"))) pitch_mode = *m;
    if (auto c = contour_from_string(p.get_string("contour"))) contour = *c;
    if (auto a = accent_pattern_from_string(p.get_string("accent_pattern"))) accents = *a;
    std::string r = p.get_string("register");
    if (r == "low") reg = Register::Low;
    else if (r == "high") reg = Register::High;
    else if (r == "mid") reg = Register::Mid;
    density = p.get_double("density", density);
    rest_probability = p.get_double("rest_probability", rest_probability);
    swing = p.get_double("swing", swing);
    cell_beats = p.get_double("cell_length_beats", cell_beats);
    note_length = p.get_double("note_length", note_length);
    interval_limit = static_cast<int>(p.get_int("interval_limit", interval_limit));
    articulation = p.get_string("articulation", articulation);
    percussion = p.get_bool("percussion", percussion);
    if (percussion) pitch_mode = PitchMode::Drum;
    if (const auto* v = p.get("velocity"); v && v->as_array().size() == 2) {
        velocity_low = static_cast<int>(v->as_array()[0].as_int(velocity_low));
        velocity_high = static_cast<int>(v->as_array()[1].as_int(velocity_high));
    }
}

// --- Cell -----------------------------------------------------------------------

namespace {

/// The repeating unit: per grid step, whether it sounds, which ladder offset
/// it plays, and whether it is accented.
struct Cell {
    std::vector<bool> on;
    std::vector<int> index;
    std::vector<bool> accent;
    int register_shift = 0;  ///< In octaves.
};

struct Seeds {
    std::uint64_t rhythm, pitch, accent, reg;
};

int triangle(int i, int span)
{
    if (span <= 1) return 0;
    int period = 2 * (span - 1);
    int k = i % period;
    return k < span ? k : period - k;
}

Cell make_cell(const OstinatoOptions& o, const Seeds& s, int steps, int mutation)
{
    Cell c;
    c.on.assign(static_cast<std::size_t>(steps), true);
    c.index.assign(static_cast<std::size_t>(steps), 0);
    c.accent.assign(static_cast<std::size_t>(steps), false);

    Rng rr(s.rhythm);
    for (int i = 0; i < steps; ++i) {
        bool on = rr.chance(std::clamp(o.density, 0.0, 1.0)) && !rr.chance(std::clamp(o.rest_probability, 0.0, 1.0));
        c.on[static_cast<std::size_t>(i)] = i == 0 ? true : on;
    }

    Rng pr(s.pitch);
    const int span = std::max(2, std::min(5, steps));
    int walk = 0;
    for (int i = 0; i < steps; ++i) {
        int idx = 0;
        switch (o.contour) {
            case Contour::Static: idx = 0; break;
            case Contour::Up: idx = i % span; break;
            case Contour::Down: idx = (span - 1) - (i % span); break;
            case Contour::UpDown: idx = triangle(i, span); break;
            case Contour::RandomWalk: {
                int max_step = std::max(1, std::min(3, o.interval_limit / 2));
                if (i > 0) walk += pr.range(-max_step, max_step);
                walk = std::clamp(walk, -3, 5);
                idx = walk;
                break;
            }
        }
        if (o.pitch_mode == PitchMode::Alberti) {
            static const int pattern[4] = {0, 2, 1, 2};
            idx = pattern[i % 4];
        } else if (o.pitch_mode == PitchMode::Octave) {
            idx = i % 2;
        }
        // Even static patterns get a seed-dependent starting offset so
        // variants of one preset differ in contour.
        c.index[static_cast<std::size_t>(i)] = idx;
    }
    if (o.contour != Contour::Static && o.pitch_mode != PitchMode::Alberti && o.pitch_mode != PitchMode::Octave) {
        int offset = pr.range(0, 2);
        for (auto& x : c.index) x += offset;
    }

    Rng ar(s.accent);
    for (int i = 0; i < steps; ++i) {
        bool a = false;
        switch (o.accents) {
            case AccentPattern::None: break;
            case AccentPattern::Downbeats: a = i == 0; break;
            case AccentPattern::Offbeats: a = i % 2 == 1; break;
            case AccentPattern::Backbeat: a = steps >= 4 && (i == steps / 4 || i == 3 * steps / 4); break;
            case AccentPattern::Groups332: { int k = i % 8; a = k == 0 || k == 3 || k == 6; break; }
            case AccentPattern::Syncopated: a = ar.chance(0.3) || i == 0; break;
        }
        c.accent[static_cast<std::size_t>(i)] = a;
    }

    Rng gr(s.reg);
    c.register_shift = gr.range(0, 3) == 0 ? (gr.chance(0.5) ? 1 : -1) : 0;

    // Mutation: deterministic perturbation used to separate otherwise
    // identical siblings. Flips rests and nudges pitch offsets.
    Rng mr(derive_seed(s.rhythm ^ s.pitch, "mutation"));
    for (int m = 0; m < mutation && steps > 1; ++m) {
        int k = mr.range(1, steps - 1);
        if (mr.chance(0.5)) c.on[static_cast<std::size_t>(k)] = !c.on[static_cast<std::size_t>(k)];
        else c.index[static_cast<std::size_t>(k)] += mr.chance(0.5) ? 1 : -1;
        if (m % 3 == 2) c.register_shift = c.register_shift == 0 ? 1 : 0;
    }
    return c;
}

/// Overlays locked steps from the parent cell.
void apply_locks(Cell& cell, const Cell& parent, const Locks& locks)
{
    const std::size_t n = std::min(cell.on.size(), parent.on.size());
    if (locks.rhythm) for (std::size_t i = 0; i < n; ++i) cell.on[i] = parent.on[i];
    if (locks.contour) for (std::size_t i = 0; i < n; ++i) cell.index[i] = parent.index[i];
    for (int s : locks.steps) {
        if (s < 0 || static_cast<std::size_t>(s) >= n) continue;
        auto i = static_cast<std::size_t>(s);
        cell.on[i] = parent.on[i];
        cell.index[i] = parent.index[i];
        cell.accent[i] = parent.accent[i];
    }
}

}  // namespace

// --- Generator -------------------------------------------------------------------

Generator::Generator(OstinatoOptions options) : opt_(std::move(options))
{
    if (opt_.range_low > opt_.range_high) std::swap(opt_.range_low, opt_.range_high);
    opt_.range_low = std::clamp(opt_.range_low, 0, 127);
    opt_.range_high = std::clamp(opt_.range_high, 0, 127);
    if (opt_.length_bars < 1) opt_.length_bars = 1;
    if (opt_.start_bar < 1) opt_.start_bar = 1;
    if (opt_.velocity_low > opt_.velocity_high) std::swap(opt_.velocity_low, opt_.velocity_high);
    opt_.velocity_low = std::clamp(opt_.velocity_low, 1, 127);
    opt_.velocity_high = std::clamp(opt_.velocity_high, 1, 127);
    if (opt_.percussion) opt_.pitch_mode = PitchMode::Drum;
}

namespace {

int cell_steps(const OstinatoOptions& o)
{
    TimeSignature sig = o.meter.at_bar(o.start_bar);
    Ticks cell = static_cast<Ticks>(std::llround(o.cell_beats * static_cast<double>(sig.beat_ticks())));
    Ticks step = grid_ticks(o.grid);
    return static_cast<int>(std::clamp<Ticks>(cell / step, 1, 64));
}

theory::Chord chord_at(const OstinatoOptions& o, Ticks t)
{
    for (const auto& h : o.harmony)
        if (t >= h.start && t < h.end) return h.chord;
    std::string err;
    auto tonic = theory::chord_from_roman(theory::is_minor_family(o.key.mode) ? "i" : "I", o.key, &err);
    if (tonic) return *tonic;
    theory::Chord fallback;
    fallback.root = o.key.tonic;
    fallback.quality = *theory::find_quality("maj");
    return fallback;
}

std::vector<int> scale_pcs(const OstinatoOptions& o)
{
    std::vector<int> pcs;
    if (o.custom_scale) {
        int tonic = o.key.tonic.pitch_class();
        for (double c : o.custom_scale->cents)
            pcs.push_back(((tonic + static_cast<int>(std::lround(c / 100.0))) % 12 + 12) % 12);
    } else {
        for (const auto& p : o.key.scale()) pcs.push_back(p.pitch_class());
    }
    return pcs;
}

/// All playable notes in range whose pitch class is in `pcs`, ascending.
std::vector<int> ladder(const OstinatoOptions& o, const std::vector<int>& pcs)
{
    std::vector<int> out;
    for (int n = o.range_low; n <= o.range_high; ++n)
        if (std::find(pcs.begin(), pcs.end(), n % 12) != pcs.end()) out.push_back(n);
    return out;
}

int register_anchor_note(const OstinatoOptions& o, int shift_octaves)
{
    const int lo = o.range_low, hi = o.range_high;
    const int span = hi - lo;
    int target;
    switch (o.reg) {
        case Register::Low: target = lo + std::min(7, span / 4); break;
        case Register::High: target = hi - std::max(12, span / 3); break;
        default: target = lo + span / 2 - 4; break;
    }
    target += shift_octaves * 12;
    return std::clamp(target, lo, hi);
}

int reflect(int i, int size)
{
    if (size <= 1) return 0;
    int period = 2 * (size - 1);
    int k = ((i % period) + period) % period;
    return k < size ? k : period - k;
}

int pick_pitch(const OstinatoOptions& o, const Cell& cell, int cell_index, int ladder_offset, Ticks t,
               bool strong, bool invert)
{
    const theory::Chord chord = chord_at(o, t);
    std::vector<int> chord_pcs;
    for (const auto& p : chord.tones()) chord_pcs.push_back(p.pitch_class());
    int root = chord.root.pitch_class();

    std::vector<int> pcs;
    switch (o.pitch_mode) {
        case PitchMode::RootOnly:
        case PitchMode::Octave: pcs = {root}; break;
        case PitchMode::Pedal: pcs = {o.harmony.empty() ? o.key.tonic.pitch_class() : root}; break;
        case PitchMode::ScaleTones: pcs = scale_pcs(o); break;
        case PitchMode::PassingTones: pcs = strong ? chord_pcs : scale_pcs(o); break;
        case PitchMode::ChromaticTension: pcs = {root, (root + 1) % 12, (root + 2) % 12, (root + 3) % 12}; break;
        default: pcs = chord_pcs; break;
    }
    auto lad = ladder(o, pcs);
    if (lad.empty()) {
        // The range cannot hold this pitch class set: fall back to any note.
        lad.push_back(std::clamp(register_anchor_note(o, 0), o.range_low, o.range_high));
    }
    int anchor_note = register_anchor_note(o, cell.register_shift);
    auto it = std::lower_bound(lad.begin(), lad.end(), anchor_note);
    int anchor = static_cast<int>(std::min<std::ptrdiff_t>(it - lad.begin(), static_cast<std::ptrdiff_t>(lad.size()) - 1));
    int idx = cell.index[static_cast<std::size_t>(cell_index)] + ladder_offset;
    if (invert) idx = -idx;
    return lad[static_cast<std::size_t>(reflect(anchor + idx, static_cast<int>(lad.size())))];
}

int drum_pitch(const OstinatoOptions& o, Ticks t, Ticks bar_start, const TimeSignature& sig, Rng& rng)
{
    auto note = [&](std::size_t i) { return o.drum_notes.empty() ? 36 : o.drum_notes[std::min(i, o.drum_notes.size() - 1)]; };
    Ticks in_bar = t - bar_start;
    Ticks beat = sig.beat_ticks();
    if (in_bar == 0) return note(0);
    if (sig.numerator >= 4 && !sig.is_compound() && in_bar % beat == 0 && (in_bar / beat) % 2 == 1) return note(1);
    if (sig.is_compound() && in_bar == beat * 3) return note(1);
    if (in_bar % beat == 0 && rng.chance(0.25)) return note(0);
    return rng.chance(0.12) ? note(3) : note(2);
}

}  // namespace

Variant Generator::build(Variant v) const
{
    const OstinatoOptions& o = opt_;
    const int steps = cell_steps(o);
    Seeds seeds{v.rhythm_seed, v.pitch_seed, v.accent_seed, v.register_seed};
    Cell base = make_cell(o, seeds, steps, v.mutation);
    if (v.parent_seeds.size() == 4) {
        Cell parent = make_cell(o, {v.parent_seeds[0], v.parent_seeds[1], v.parent_seeds[2], v.parent_seeds[3]},
                                steps, v.parent_mutation);
        apply_locks(base, parent, v.locks);
    }

    // Evolving 4-bar segments: each later segment re-rolls one aspect.
    std::vector<Cell> segments{base};
    const int segment_count = (o.length_bars + 3) / 4;
    for (int k = 1; k < segment_count; ++k) {
        if (!o.evolve) {
            segments.push_back(base);
            continue;
        }
        Seeds s = seeds;
        switch (k % 3) {
            case 1: s.pitch = derive_seed(seeds.pitch, "segment" + std::to_string(k)); break;
            case 2: s.rhythm = derive_seed(seeds.rhythm, "segment" + std::to_string(k)); break;
            default: s.accent = derive_seed(seeds.accent, "segment" + std::to_string(k)); break;
        }
        Cell c = make_cell(o, s, steps, v.mutation);
        // The first beat of the cell stays put so the figure stays recognisable.
        c.on[0] = base.on[0];
        c.index[0] = base.index[0];
        apply_locks(c, base, v.locks);
        segments.push_back(c);
    }

    v.start = o.meter.bar_start(o.start_bar);
    v.end = v.start + o.meter.span(o.start_bar, o.length_bars);
    const Ticks grid_step = grid_ticks(o.grid);
    Rng drum_rng(derive_seed(v.pitch_seed, "drums"));
    Rng vel_rng(derive_seed(v.accent_seed, "velocity"));

    struct Onset { Ticks t; int pitch; int velocity; };
    std::vector<Onset> onsets;
    for (int b = 0; b < o.length_bars; ++b) {
        const int bar = o.start_bar + b;
        const Ticks bar_start = o.meter.bar_start(bar);
        const TimeSignature sig = o.meter.at_bar(bar);
        const Ticks bar_len = sig.ticks_per_bar();
        // A grid longer than the bar (a whole-note drone in 7/8) sounds once per bar.
        const Ticks step = std::min(grid_step, bar_len);
        const Cell& cell = segments[static_cast<std::size_t>(b / 4)];
        const bool answer = o.call_response && (b % 2 == 1);
        const int steps_in_bar = static_cast<int>(bar_len / step);
        for (int s = 0; s < steps_in_bar; ++s) {
            // The cell restarts every bar, so odd meters (7/8, 5/4) stay
            // aligned to their own downbeats.
            const int ci = s % steps;
            if (!cell.on[static_cast<std::size_t>(ci)]) continue;
            Ticks t = bar_start + static_cast<Ticks>(s) * step;
            if (s % 2 == 1 && o.swing > 0)
                t += static_cast<Ticks>(std::llround(std::clamp(o.swing, 0.0, 0.5) * static_cast<double>(step)));
            if (t >= v.end) continue;
            const bool strong = (static_cast<Ticks>(s) * step) % sig.beat_ticks() == 0;
            int pitch = o.pitch_mode == PitchMode::Drum
                            ? drum_pitch(o, t, bar_start, sig, drum_rng)
                            : pick_pitch(o, cell, ci, 0, t, strong, answer);
            const bool accent = cell.accent[static_cast<std::size_t>(ci)];
            const int vlo = o.velocity_low, vhi = o.velocity_high;
            int vel = accent ? vhi - vel_rng.range(0, 5) : vlo + vel_rng.range(0, std::max(0, (vhi - vlo) / 2));
            onsets.push_back({t, pitch, std::clamp(vel, 1, 127)});
        }
    }

    v.notes.clear();
    for (std::size_t i = 0; i < onsets.size(); ++i) {
        const Ticks t = onsets[i].t;
        const Ticks next = i + 1 < onsets.size() ? onsets[i + 1].t : v.end;
        const Ticks step = std::min(grid_step, o.meter.at_tick(t).ticks_per_bar());
        Ticks dur = static_cast<Ticks>(std::llround(static_cast<double>(step) * std::clamp(o.note_length, 0.05, 1.0)));
        dur = std::min({dur, next - t, v.end - t});
        if (dur <= 0) continue;
        v.notes.push_back({t, dur, onsets[i].pitch, onsets[i].velocity, o.channel, o.articulation});
    }
    return v;
}

Variant Generator::generate(std::uint64_t seed, const std::string& id) const
{
    Variant v;
    v.id = id;
    v.seed = seed;
    v.rhythm_seed = derive_seed(seed, "rhythm");
    v.pitch_seed = derive_seed(seed, "pitch");
    v.accent_seed = derive_seed(seed, "accent");
    v.register_seed = derive_seed(seed, "register");
    return build(v);
}

std::vector<Variant> Generator::generate_variants(std::uint64_t seed, int count) const
{
    std::vector<Variant> out;
    for (int i = 0; i < count; ++i) {
        std::string id(1, static_cast<char>('A' + (i % 26)));
        if (i >= 26) id += std::to_string(i / 26);
        Variant v;
        for (int attempt = 0; attempt < 24; ++attempt) {
            v = generate(derive_seed(seed, "variant" + std::to_string(i) + "/" + std::to_string(attempt)), id);
            // Escalating mutation once plain reseeding stops producing
            // something new (a dense, static pulse has few degrees of freedom).
            if (attempt >= 3) {
                v.mutation = attempt - 2;
                v = build(v);
            }
            bool duplicate = std::any_of(out.begin(), out.end(), [&](const Variant& w) { return w.notes == v.notes; });
            if (!duplicate) break;
        }
        out.push_back(v);
    }
    return out;
}

Variant Generator::vary(const Variant& base, Aspect aspect, const std::string& new_id) const
{
    Variant v = base;
    v.id = new_id;
    v.parent_seeds = {base.rhythm_seed, base.pitch_seed, base.accent_seed, base.register_seed};
    v.parent_mutation = base.mutation;
    const std::string label = "vary:" + new_id;
    switch (aspect) {
        case Aspect::Rhythm: v.rhythm_seed = derive_seed(base.rhythm_seed, label); break;
        case Aspect::Pitch: v.pitch_seed = derive_seed(base.pitch_seed, label); break;
        case Aspect::Accents: v.accent_seed = derive_seed(base.accent_seed, label); break;
        case Aspect::Register: v.register_seed = derive_seed(base.register_seed, label); break;
    }
    return build(v);
}

Variant Generator::regenerate(const Variant& stored) const { return build(stored); }

// --- Persistence ------------------------------------------------------------------

namespace {
std::string hex64(std::uint64_t v)
{
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}
std::optional<std::uint64_t> parse_hex64(const std::string& s)
{
    if (s.empty() || s.size() > 16) return std::nullopt;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s.c_str(), &end, 16);
    if (!end || *end) return std::nullopt;
    return static_cast<std::uint64_t>(v);
}
}  // namespace

json::Value Variant::to_json() const
{
    json::Object o{{"id", id}, {"seed", hex64(seed)}, {"rhythm_seed", hex64(rhythm_seed)},
                   {"pitch_seed", hex64(pitch_seed)}, {"accent_seed", hex64(accent_seed)},
                   {"register_seed", hex64(register_seed)}, {"mutation", mutation}};
    if (!parent_seeds.empty()) {
        json::Array ps;
        for (auto s : parent_seeds) ps.emplace_back(hex64(s));
        o.set("parent_seeds", ps);
        o.set("parent_mutation", parent_mutation);
    }
    json::Array steps;
    for (int s : locks.steps) steps.emplace_back(s);
    o.set("locks", json::Object{{"steps", steps}, {"contour", locks.contour}, {"rhythm", locks.rhythm}});
    return o;
}

std::optional<Variant> Variant::from_json(const json::Value& j)
{
    Variant v;
    v.id = j.get_string("id");
    auto get = [&](const char* k, std::uint64_t& out) {
        auto x = parse_hex64(j.get_string(k));
        if (x) out = *x;
        return x.has_value();
    };
    if (!get("seed", v.seed) || !get("rhythm_seed", v.rhythm_seed) || !get("pitch_seed", v.pitch_seed) ||
        !get("accent_seed", v.accent_seed) || !get("register_seed", v.register_seed))
        return std::nullopt;
    v.mutation = static_cast<int>(j.get_int("mutation"));
    for (const auto& s : j.get_string_array("parent_seeds")) {
        auto x = parse_hex64(s);
        if (!x) return std::nullopt;
        v.parent_seeds.push_back(*x);
    }
    if (!v.parent_seeds.empty() && v.parent_seeds.size() != 4) return std::nullopt;
    v.parent_mutation = static_cast<int>(j.get_int("parent_mutation"));
    if (const auto* l = j.get("locks")) {
        if (const auto* st = l->get("steps"))
            for (const auto& s : st->as_array()) v.locks.steps.insert(static_cast<int>(s.as_int()));
        v.locks.contour = l->get_bool("contour");
        v.locks.rhythm = l->get_bool("rhythm");
    }
    return v;
}

// --- Presets ------------------------------------------------------------------------

bool PresetCatalog::load_file(const std::string& path, std::string* error)
{
    auto text = files::read_file(path, error);
    if (!text) return false;
    json::ParseError pe;
    auto doc = json::parse(*text, &pe);
    if (!doc) {
        if (error) *error = path + ": " + pe.to_string();
        return false;
    }
    return load(*doc, error);
}

bool PresetCatalog::load(const json::Value& doc, std::string* error)
{
    presets_.clear();
    for (const auto& p : doc.get("presets") ? doc.get("presets")->as_array() : json::Array{}) {
        std::string id = p.get_string("id");
        if (id.empty() || find(id)) {
            if (error) *error = "preset with empty or duplicate id '" + id + "'";
            return false;
        }
        if (!grid_from_string(p.get_string("grid")) || !pitch_mode_from_string(p.get_string("pitch_mode")) ||
            !contour_from_string(p.get_string("contour")) ||
            !accent_pattern_from_string(p.get_string("accent_pattern"))) {
            if (error) *error = "preset '" + id + "' has an unknown grid, pitch_mode, contour or accent_pattern";
            return false;
        }
        presets_.emplace_back(id, p);
    }
    return true;
}

const json::Value* PresetCatalog::find(const std::string& id) const noexcept
{
    for (const auto& p : presets_)
        if (p.first == id) return &p.second;
    return nullptr;
}

std::vector<std::string> PresetCatalog::ids() const
{
    std::vector<std::string> out;
    for (const auto& p : presets_) out.push_back(p.first);
    return out;
}

}  // namespace vcc::ostinato
