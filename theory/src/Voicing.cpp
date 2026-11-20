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
        case VoicingStyle::TwoHands: return "two_hands";
    }
    return "closed";
}

std::optional<VoicingStyle> voicing_style_from_string(const std::string& t) noexcept
{
    for (VoicingStyle s : {VoicingStyle::Closed, VoicingStyle::Open, VoicingStyle::Drop2, VoicingStyle::Drop3,
                           VoicingStyle::Spread, VoicingStyle::TwoHands})
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

/// Two hands: the left hand's bass and fifth (or octave) in the bass clef,
/// the right hand's three or more notes from middle C up.
std::vector<Voicing> two_hand_candidates(const Chord& c)
{
    const std::vector<int> pcs = chord_pcs(c);
    const int bass_pc = c.bass_note().pitch_class();
    std::vector<int> upper_pcs;
    for (int pc : pcs)
        if (pc != bass_pc) upper_pcs.push_back(pc);
    // At least three in the right hand: the bass's pitch class joins a triad's two.
    if (upper_pcs.size() < 3) upper_pcs.push_back(bass_pc);
    while (upper_pcs.size() < 3) upper_pcs.push_back(upper_pcs.front());
    const bool has_fifth = std::find(pcs.begin(), pcs.end(), mod12(bass_pc + 7)) != pcs.end();
    std::vector<Voicing> out;
    for (int bass = 36 + mod12(bass_pc - 36); bass <= 52; bass += 12) {  // C2..E3
        const int second = has_fifth ? bass + 7 : bass + 12;
        if (second > 59) continue;  // the left hand stays in the bass clef
        for (std::size_t rot = 0; rot < upper_pcs.size(); ++rot) {
            std::vector<int> upper;
            int note = lowest_at_or_above(upper_pcs[rot], 60);
            upper.push_back(note);
            for (std::size_t k = 1; k < upper_pcs.size(); ++k) {
                note = lowest_at_or_above(upper_pcs[(rot + k) % upper_pcs.size()], note + 1);
                upper.push_back(note);
            }
            Voicing v;
            v.notes = {bass, second};
            v.notes.insert(v.notes.end(), upper.begin(), upper.end());
            out.push_back(v);
        }
    }
    return out;
}

std::vector<Voicing> candidates(const Chord& chord, int inversion, const VoicingOptions& opt)
{
    Chord c = chord;
    c.inversion = inversion;
    if (opt.style == VoicingStyle::TwoHands) return two_hand_candidates(c);
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


std::vector<Voicing> voice_root_bass(const std::vector<Chord>& chords, const VoicingOptions& options)
{
    // The upper structure: the chords without their bass, voice-led with
    // free inversions above the bass's range.
    std::vector<Chord> upper_chords;
    for (const auto& c : chords) {
        Chord u = c;
        u.bass.reset();
        u.inversion = 0;
        upper_chords.push_back(u);
    }
    VoicingOptions up = options;
    up.style = VoicingStyle::Closed;
    up.voice_lead = true;
    up.free_inversions = true;
    up.double_bass = false;
    up.low = std::max(options.low, options.center - 7);
    up.center = options.center + 4;
    const auto uppers = voice_progression(upper_chords, up);
    std::vector<Voicing> out;
    int prev_bass = -1;
    const int bass_high = std::max(options.low + 11, options.center - 5);
    for (std::size_t i = 0; i < chords.size(); ++i) {
        const auto& c = chords[i];
        // The bass: a slash chord's bass, an inversion's tone, else the root.
        int pc = c.root.pitch_class();
        if (c.bass) pc = c.bass->pitch_class();
        else if (c.inversion > 0) {
            const auto tones = c.tones();
            if (static_cast<std::size_t>(c.inversion) < tones.size()) pc = tones[static_cast<std::size_t>(c.inversion)].pitch_class();
        }
        pc = ((pc % 12) + 12) % 12;
        // The bass: that pitch class in range, nearest the last bass (or
        // the middle of the range for the first chord).
        const int target = prev_bass < 0 ? (options.low + bass_high) / 2 : prev_bass;
        int bass = -1;
        for (int n = options.low; n <= bass_high; ++n)
            if (((n % 12) + 12) % 12 == pc && (bass < 0 || std::abs(n - target) < std::abs(bass - target))) bass = n;
        if (bass < 0) bass = options.low + ((pc - options.low % 12) % 12 + 12) % 12;
        Voicing v;
        v.notes.push_back(bass);
        for (int n : uppers[i].notes) {
            int note = n;
            while (note <= bass) note += 12;  // the upper voices stay above the bass
            if (((note % 12) + 12) % 12 == pc && note - bass < 12 && uppers[i].notes.size() > 2) continue;  // no doubled bass right on top of it
            v.notes.push_back(note);
        }
        std::sort(v.notes.begin(), v.notes.end());
        v.notes.erase(std::unique(v.notes.begin(), v.notes.end()), v.notes.end());
        out.push_back(v);
        prev_bass = bass;
    }
    return out;
}

// --- Chord Mode entry voicing --------------------------------------------------

int VoicedTone::octave() const noexcept
{
    // The letter's natural pitch class, the alteration moving off it.
    static const int kNatural[7] = {0, 2, 4, 5, 7, 9, 11};
    const int natural = midi - pitch.alteration;
    const int letter_pc = kNatural[((pitch.letter % 7) + 7) % 7];
    return (natural - letter_pc) / 12 - 1;
}

std::vector<VoicedTone> voice_for_entry(const Chord& chord, int bass_octave, VoicingStyle style)
{
    const auto spelled = chord.tones();
    const auto intervals = chord.intervals();
    if (spelled.empty()) return {};
    auto midi_in_octave = [](const SpelledPitch& p, int octave) {
        static const int kNatural[7] = {0, 2, 4, 5, 7, 9, 11};
        return (octave + 1) * 12 + kNatural[((p.letter % 7) + 7) % 7] + p.alteration;
    };
    // The order the tones stack in: from the bass (an inversion starts on
    // its tone and wraps), the extensions above, in the order of their degree.
    std::vector<std::size_t> order;
    const std::size_t n = spelled.size();
    std::size_t core = 0;  // tones within the octave (degrees up to 7)
    for (const auto& t : intervals) core += t.degree <= 7 ? 1 : 0;
    const std::size_t inv = chord.bass ? 0 : static_cast<std::size_t>(std::clamp(chord.inversion, 0, 3));
    const std::size_t start = core > 0 ? std::min(inv, core - 1) : 0;
    for (std::size_t i = 0; i < core; ++i) order.push_back((start + i) % core);
    for (std::size_t i = core; i < n; ++i) order.push_back(i);

    std::vector<VoicedTone> out;
    int last = -1000;
    if (chord.bass) {
        out.push_back({*chord.bass, midi_in_octave(*chord.bass, bass_octave)});
        last = out.back().midi;
    }
    for (std::size_t k = 0; k < order.size(); ++k) {
        const auto& p = spelled[order[k]];
        int m = midi_in_octave(p, out.empty() ? bass_octave : bass_octave - 1);
        if (!out.empty())
            while (m <= last) m += 12;
        out.push_back({p, m});
        last = m;
    }
    auto sort = [&] { std::sort(out.begin(), out.end(), [](const VoicedTone& a, const VoicedTone& b) { return a.midi < b.midi; }); };
    switch (style) {
        case VoicingStyle::Closed: break;
        case VoicingStyle::Open:
            if (out.size() >= 3) out[1].midi += 12;
            break;
        case VoicingStyle::Drop2:
        case VoicingStyle::Drop3: {
            // On the structure above the bass.
            const std::size_t drop = style == VoicingStyle::Drop2 ? 2 : 3;
            if (out.size() >= drop + 2) {
                auto& v = out[out.size() - drop];
                v.midi -= 12;
                // Under the bass: the bass goes down an octave and stays the bass.
                while (v.midi <= out.front().midi) out.front().midi -= 12;
            }
            break;
        }
        case VoicingStyle::Spread:
        case VoicingStyle::TwoHands:
            if (out.size() >= 2) out.front().midi -= 12;
            break;
    }
    sort();
    return out;
}

}  // namespace vcc::theory
