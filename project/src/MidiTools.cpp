#include "vcc/project/MidiTools.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace vcc::project {

Ticks quantize_tick(Ticks t, const QuantizeSettings& q)
{
    if (q.grid <= 0) return t;
    const double g = static_cast<double>(q.grid);
    const double late = std::clamp(q.swing, 0.0, 1.0) * g;
    // The nearest of the straight points and their swung neighbours.
    const auto k = static_cast<long long>(std::floor(static_cast<double>(t) / g));
    Ticks best = t;
    double best_d = 1e300;
    for (long long i = k - 1; i <= k + 2; ++i) {
        const double p = static_cast<double>(i) * g + ((i % 2 != 0) ? late : 0.0);
        const double d = std::abs(p - static_cast<double>(t));
        if (d < best_d) {
            best_d = d;
            best = static_cast<Ticks>(std::llround(p));
        }
    }
    return std::max<Ticks>(0, best);
}

bool remove_overlaps(std::vector<NoteEvent>& notes)
{
    bool changed = false;
    std::stable_sort(notes.begin(), notes.end(), [](const NoteEvent& a, const NoteEvent& b) {
        if (a.channel != b.channel) return a.channel < b.channel;
        if (a.pitch != b.pitch) return a.pitch < b.pitch;
        return a.start < b.start;
    });
    std::vector<NoteEvent> out;
    out.reserve(notes.size());
    for (const auto& n : notes) {
        if (!out.empty() && out.back().pitch == n.pitch && out.back().channel == n.channel) {
            auto& prev = out.back();
            if (n.start == prev.start) {
                changed = true;
                continue;
            }
            if (n.start < prev.end()) {
                prev.duration = n.start - prev.start;
                changed = true;
            }
        }
        out.push_back(n);
    }
    std::stable_sort(out.begin(), out.end());
    notes = std::move(out);
    return changed;
}

int quantize_notes(std::vector<NoteEvent>& notes, const QuantizeSettings& q)
{
    if (q.grid <= 0) return 0;
    const double s = std::clamp(q.strength, 0.0, 1.0);
    int moved = 0;
    for (auto& n : notes) {
        const Ticks end = n.end();
        const Ticks target = quantize_tick(n.start, q);
        const Ticks start = n.start + static_cast<Ticks>(std::llround(s * static_cast<double>(target - n.start)));
        Ticks new_end = end + (start - n.start);  // keeps its length...
        if (q.ends) {
            const Ticks target_end = quantize_tick(end, q);
            new_end = end + static_cast<Ticks>(std::llround(s * static_cast<double>(target_end - end)));
        }
        new_end = std::max(new_end, start + std::max<Ticks>(1, q.grid / 2));  // ...never shorter than half a step
        if (start != n.start || new_end != end) ++moved;
        n.start = start;
        n.duration = new_end - start;
    }
    remove_overlaps(notes);
    return moved;
}

int change_velocities(std::vector<NoteEvent*>& notes, const VelocityChange& change)
{
    if (notes.empty()) return 0;
    double mean = 0.0;
    for (const auto* n : notes) mean += n->velocity;
    mean /= static_cast<double>(notes.size());
    const double c = std::clamp(change.compress, 0.0, 1.0);
    int changed = 0;
    for (auto* n : notes) {
        double v = n->velocity;
        if (change.set >= 1) {
            v = change.set;
        } else {
            v = mean + (v - mean) * (1.0 - c);
            v = v * change.scale + change.offset;
        }
        const int nv = std::clamp(static_cast<int>(std::lround(v)), 1, 127);
        if (nv != n->velocity) ++changed;
        n->velocity = nv;
    }
    return changed;
}

}  // namespace vcc::project
