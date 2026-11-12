#include "vcc/theory/Voicing.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace vcc::theory {

const char* to_string(VoicingStyle s) noexcept
{
    switch (s) {
        case VoicingStyle::Closed: return "closed";
        case VoicingStyle::Open: return "open";
        case VoicingStyle::Drop2: return "drop2";
        case VoicingStyle::Drop3: return "drop3";
        case VoicingStyle::Spread: return "spread";
    }
    return "closed";
}

std::optional<VoicingStyle> voicing_style_from_string(const std::string& t) noexcept
{
    for (VoicingStyle s : {VoicingStyle::Closed, VoicingStyle::Open, VoicingStyle::Drop2, VoicingStyle::Drop3,
                           VoicingStyle::Spread})
        if (t == to_string(s)) return s;
    return std::nullopt;
}

int voice_movement(const Voicing& from, const Voicing& to)
{
    if (from.notes.empty() || to.notes.empty()) return 0;
    if (from.notes.size() == to.notes.size()) {
        int sum = 0;
        for (std::size_t i = 0; i < from.notes.size(); ++i) sum += std::abs(from.notes[i] - to.notes[i]);
        return sum;
    }
    auto nearest = [](const std::vector<int>& set, int n) {
        int best = std::numeric_limits<int>::max();
        for (int x : set) best = std::min(best, std::abs(x - n));
        return best;
    };
    int sum = 0;
    for (int n : to.notes) sum += nearest(from.notes, n);
    for (int n : from.notes) sum += nearest(to.notes, n);
    return sum / 2;
}

namespace {

int mod12(int a) { return ((a % 12) + 12) % 12; }

/// Pitch classes in chord order, without duplicates.
std::vector<int> chord_pcs(const Chord& c)
{
    std::vector<int> pcs;
    for (const auto& t : c.tones()) {
        int pc = t.pitch_class();
        if (std::find(pcs.begin(), pcs.end(), pc) == pcs.end()) pcs.push_back(pc);
    }
    return pcs;
}

int lowest_at_or_above(int pc, int floor_note)
{
    int n = floor_note + mod12(pc - floor_note);
    return n;
}

std::vector<int> apply_style(std::vector<int> upper, VoicingStyle style)
{
    std::sort(upper.begin(), upper.end());
    const std::size_t n = upper.size();
    switch (style) {
        case VoicingStyle::Open:
            for (std::size_t i = 1; i < n; i += 2) upper[i] += 12;
            break;
        case VoicingStyle::Drop2:
            if (n >= 3) upper[n - 2] -= 12;
            break;
        case VoicingStyle::Drop3:
            if (n >= 4) upper[n - 3] -= 12;
            else if (n >= 3) upper[n - 2] -= 12;
            break;
        default: break;
    }
    std::sort(upper.begin(), upper.end());
    return upper;
}

struct Candidate {
    Voicing voicing;
    double score = 0;
};

std::vector<Voicing> candidates(const Chord& chord, int inversion, const VoicingOptions& opt)
{
    Chord c = chord;
    c.inversion = inversion;
    std::vector<int> pcs = chord_pcs(c);
    int bass_pc = c.bass_note().pitch_class();
    std::vector<int> upper_pcs;
    for (int pc : pcs)
        if (pc != bass_pc) upper_pcs.push_back(pc);
    if (upper_pcs.empty()) upper_pcs.push_back(bass_pc);

    std::vector<Voicing> out;
    const std::size_t n = upper_pcs.size();
    for (std::size_t rot = 0; rot < n; ++rot) {
        for (int octave = 1; octave <= 8; ++octave) {
            std::vector<int> upper;
            int note = lowest_at_or_above(upper_pcs[rot], octave * 12);
            upper.push_back(note);
            for (std::size_t k = 1; k < n; ++k) {
                note = lowest_at_or_above(upper_pcs[(rot + k) % n], note + 1);
                upper.push_back(note);
            }
            upper = apply_style(upper, opt.style);
            int ceiling = opt.style == VoicingStyle::Spread ? upper.front() - 12 : upper.front() - 1;
            int bass = ceiling - mod12(ceiling - bass_pc);
            if (bass < 0) continue;
            Voicing v;
            if (opt.double_bass && bass - 12 >= opt.low) v.notes.push_back(bass - 12);
            v.notes.push_back(bass);
            v.notes.insert(v.notes.end(), upper.begin(), upper.end());
            if (v.notes.back() > 127) continue;
            out.push_back(v);
        }
    }
    return out;
}

double range_penalty(const Voicing& v, const VoicingOptions& opt)
{
    double p = 0;
    for (int n : v.notes) {
        if (n < opt.low) p += 1000.0 * (opt.low - n);
        if (n > opt.high) p += 1000.0 * (n - opt.high);
    }
    return p;
}

double center_distance(const Voicing& v, const VoicingOptions& opt)
{
    // Centre on the upper structure: the bass has its own register.
    double sum = 0;
    std::size_t count = 0;
    for (std::size_t i = 1; i < v.notes.size(); ++i, ++count) sum += v.notes[i];
    if (count == 0) return 0;
    return std::fabs(sum / static_cast<double>(count) - opt.center);
}

Voicing best(const Chord& chord, const VoicingOptions& opt, const Voicing* previous,
             std::optional<int> locked_inversion)
{
    std::vector<int> inversions;
    int max_inv = std::min<int>(3, static_cast<int>(chord.tones().size()) - 1);
    if (locked_inversion) inversions.push_back(std::clamp(*locked_inversion, 0, max_inv));
    else if (opt.free_inversions && !chord.bass) for (int i = 0; i <= max_inv; ++i) inversions.push_back(i);
    else inversions.push_back(std::clamp(chord.inversion, 0, max_inv));

    Candidate winner;
    bool have = false;
    for (int inv : inversions) {
        for (const auto& v : candidates(chord, inv, opt)) {
            double score = range_penalty(v, opt);
            if (previous && opt.voice_lead) {
                score += voice_movement(*previous, v);
                score += 0.25 * center_distance(v, opt);  // resist drifting away
            } else {
                score += center_distance(v, opt);
            }
            if (inv != chord.inversion) score += 0.5;
            // Deterministic tie-break: lower overall voicing first.
            if (!have || score < winner.score - 1e-9 ||
                (std::fabs(score - winner.score) <= 1e-9 && v.notes < winner.voicing.notes)) {
                winner = {v, score};
                have = true;
            }
        }
    }
    return winner.voicing;
}

}  // namespace

Voicing voice_chord(const Chord& chord, const VoicingOptions& options)
{
    return best(chord, options, nullptr, std::nullopt);
}

std::vector<Voicing> voice_progression(const std::vector<Chord>& chords, const VoicingOptions& options,
                                       const std::vector<std::optional<int>>& locked)
{
    std::vector<Voicing> out;
    for (std::size_t i = 0; i < chords.size(); ++i) {
        std::optional<int> lock = i < locked.size() ? locked[i] : std::nullopt;
        const Voicing* prev = out.empty() ? nullptr : &out.back();
        out.push_back(best(chords[i], options, prev, lock));
    }
    return out;
}

}  // namespace vcc::theory
