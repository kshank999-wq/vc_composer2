#include "ui/composer/ChordDetect.h"

#include <algorithm>
#include <array>

namespace vcc::desktop::composer_ui {

namespace {
struct Shape {
    const char* suffix;
    std::array<int, 4> tones;  // semitones above the root; -1 unused
};
// Most specific first: a seventh chord beats the triad inside it.
const Shape kShapes[] = {
    {"maj7", {0, 4, 7, 11}}, {"7", {0, 4, 7, 10}},  {"m7", {0, 3, 7, 10}},  {"m7b5", {0, 3, 6, 10}},
    {"dim7", {0, 3, 6, 9}},  {"", {0, 4, 7, -1}},   {"m", {0, 3, 7, -1}},   {"dim", {0, 3, 6, -1}},
    {"aug", {0, 4, 8, -1}},  {"sus4", {0, 5, 7, -1}}, {"sus2", {0, 2, 7, -1}}, {"5", {0, 7, -1, -1}},
};

const char* root_name(int pc, bool flats)
{
    static const char* sharps[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static const char* flat_names[] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
    return (flats ? flat_names : sharps)[pc];
}
}  // namespace

std::string chord_symbol(const std::vector<NoteEvent>& notes, Ticks from, Ticks to, bool flats)
{
    std::array<double, 12> weight{};
    int lowest = 128;
    double total = 0.0;
    for (const auto& n : notes) {
        const Ticks a = std::max(from, n.start), b = std::min(to, n.end());
        if (b <= a || n.channel == 9) continue;
        weight[static_cast<std::size_t>(n.pitch % 12)] += static_cast<double>(b - a);
        total += static_cast<double>(b - a);
        lowest = std::min(lowest, n.pitch);
    }
    if (total <= 0.0) return {};
    int distinct = 0;
    for (double w : weight) distinct += w > total * 0.04 ? 1 : 0;
    if (distinct < 2) return {};

    double best = 0.0;
    std::string symbol;
    for (int root = 0; root < 12; ++root) {
        for (const auto& shape : kShapes) {
            double covered = 0.0;
            int tones = 0;
            bool complete = true;
            for (int t : shape.tones) {
                if (t < 0) continue;
                ++tones;
                const double w = weight[static_cast<std::size_t>((root + t) % 12)];
                if (w <= total * 0.04 && t != 7) complete = false;  // a missing fifth is allowed
                covered += w;
            }
            if (!complete || weight[static_cast<std::size_t>(root)] <= 0.0) continue;
            // How much of what sounds the chord explains, a little credit for
            // the bass being the root, and simpler shapes win a tie.
            double score = covered / total;
            if (lowest < 128 && lowest % 12 == root) score += 0.08;
            score -= 0.01 * tones;
            if (score > best + 1e-9) {
                best = score;
                symbol = std::string(root_name(root, flats)) + shape.suffix;
            }
        }
    }
    return best >= 0.6 ? symbol : std::string{};
}

std::vector<std::string> chord_symbols(const std::vector<NoteEvent>& notes, const MeterMap& meter, int bars, bool flats)
{
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(std::max(0, bars)));
    for (int bar = 1; bar <= bars; ++bar)
        out.push_back(chord_symbol(notes, meter.bar_start(bar), meter.bar_start(bar + 1), flats));
    return out;
}

}  // namespace vcc::desktop::composer_ui
