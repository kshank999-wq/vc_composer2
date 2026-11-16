#include "vcc/mobile/Handwriting.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "HandwritingImpl.h"

namespace vcc::mobile {

const char* to_string(StrokeShape s) noexcept
{
    switch (s) {
        case StrokeShape::FilledHead: return "filled head";
        case StrokeShape::HollowHead: return "hollow head";
        case StrokeShape::Stem: return "stem";
        case StrokeShape::Flag: return "flag";
        case StrokeShape::Beam: return "beam";
        case StrokeShape::Dot: return "dot";
        case StrokeShape::WholeRest: return "whole/half rest";
        case StrokeShape::QuarterRest: return "quarter rest";
        case StrokeShape::EighthRest: return "eighth rest";
        case StrokeShape::Flat: return "flat";
        case StrokeShape::AccidentalLine: return "accidental line";
        case StrokeShape::Unknown: break;
    }
    return "unknown";
}

int Recognition::doubtful_count() const noexcept
{
    int n = 0;
    for (const auto& i : items) n += i.doubtful ? 1 : 0;
    return n;
}

namespace detail {

Features measure(const Stroke& s) noexcept
{
    Features f;
    if (s.empty()) return f;
    double minx = s[0].x, maxx = s[0].x, miny = s[0].y, maxy = s[0].y;
    for (std::size_t i = 0; i < s.size(); ++i) {
        minx = std::min(minx, s[i].x), maxx = std::max(maxx, s[i].x);
        miny = std::min(miny, s[i].y), maxy = std::max(maxy, s[i].y);
        if (i > 0) f.length += std::hypot(s[i].x - s[i - 1].x, s[i].y - s[i - 1].y);
    }
    f.left = minx, f.right = maxx, f.top = miny, f.bottom = maxy;
    f.width = maxx - minx;
    f.height = maxy - miny;
    f.cx = (minx + maxx) / 2;
    f.cy = (miny + maxy) / 2;
    const double chord = std::hypot(s.back().x - s.front().x, s.back().y - s.front().y);
    f.straightness = f.length > 0 ? chord / f.length : 1.0;
    f.closure = chord;
    // Reversals of horizontal and vertical direction, ignoring jitter.
    auto reversals = [&](bool horizontal) {
        int n = 0, dir = 0;
        double run = 0;
        for (std::size_t i = 1; i < s.size(); ++i) {
            const double d = horizontal ? s[i].x - s[i - 1].x : s[i].y - s[i - 1].y;
            if (std::abs(d) < 1e-9) continue;
            const int now = d > 0 ? 1 : -1;
            if (now == dir) {
                run += std::abs(d);
            } else if (run > 0.12 || dir == 0) {
                if (dir != 0) ++n;
                dir = now;
                run = std::abs(d);
            } else {
                run += std::abs(d);
            }
        }
        return n;
    };
    f.x_reversals = reversals(true);
    f.y_reversals = reversals(false);
    const double area = std::max(0.05, f.width * f.height);
    f.density = f.length / std::sqrt(area);
    return f;
}

}  // namespace detail

StrokeShape classify(const Stroke& stroke) noexcept
{
    using detail::measure;
    const auto f = measure(stroke);
    if (stroke.size() < 2 || f.length < 0.25) return f.length < 0.6 ? StrokeShape::Dot : StrokeShape::Unknown;
    const double w = f.width, h = f.height, size = std::max(w, h);
    if (size < 0.55 && f.length < 1.2) return StrokeShape::Dot;
    // Lines.
    if (f.straightness > 0.88) {
        if (h > 1.8 && h > 3.0 * w) return StrokeShape::Stem;
        if (w > 1.3 && w > 2.0 * h) return h < 0.9 && w < 1.9 && f.length < 2.2 ? StrokeShape::AccidentalLine : StrokeShape::Beam;
        if (size <= 1.6) return StrokeShape::AccidentalLine;
    }
    // A notehead: about a space, round. Filled ones are scribbled in.
    if (w >= 0.55 && w <= 2.2 && h >= 0.45 && h <= 1.8) {
        const bool scribbled = f.density > 5.5 || (f.x_reversals + f.y_reversals) >= 4;
        // A whole or half rest is half a space tall; a head is an oval about
        // 1.2 by 0.8 spaces.
        if (scribbled) return h < 0.6 && h < 0.55 * w ? StrokeShape::WholeRest : StrokeShape::FilledHead;
        if (f.closure < 0.45 * size) return StrokeShape::HollowHead;
    }
    // A whole or half rest: a short, flat block.
    if (w >= 0.7 && w <= 1.8 && h < 0.6 && f.density > 3.0) return StrokeShape::WholeRest;
    // A quarter rest: tall and zigzag.
    if (h >= 1.8 && h <= 4.0 && w <= 1.8 && f.x_reversals >= 2) return StrokeShape::QuarterRest;
    // A flat: a straight vertical line from above, ending in a bowl at the
    // bottom - its upper half is a line, where a flag curves from the start.
    if (h >= 1.5 && h <= 3.6 && w <= 1.4 && f.straightness < 0.88 && stroke.front().y < stroke.back().y - 0.8 &&
        f.x_reversals >= 1) {
        double lo = 1e9, hi = -1e9;
        for (const auto& p : stroke)
            if (p.y < f.top + 0.5 * h) lo = std::min(lo, p.x), hi = std::max(hi, p.x);
        if (hi - lo < 0.3) return StrokeShape::Flat;
    }
    // A flag: from a stem's end, curving away, roughly a space or two.
    if (h >= 0.7 && h <= 3.0 && w >= 0.4 && w <= 1.8 && f.straightness < 0.95 && f.x_reversals <= 1)
    {
        // An eighth rest's diagonal runs down to the left of where it began
        // (its hook is at the top right); a flag curves down to the right of
        // its stem and ends right of its start.
        const bool down_left = stroke.back().x < stroke.front().x - 0.2 && stroke.back().y > stroke.front().y + 1.0;
        return down_left ? StrokeShape::EighthRest : StrokeShape::Flag;
    }
    return StrokeShape::Unknown;
}

namespace detail {

namespace {

std::int64_t duration_ticks(const InkContext& ctx, int value, int dots)
{
    if (ctx.duration_ticks) return ctx.duration_ticks(value, dots);
    // Without a context: 960 per quarter (tests of the grouping alone).
    long long t = 3840;
    for (int i = 1; i < value; ++i) t /= 2;
    long long add = t / 2, total = t;
    for (int i = 0; i < dots; ++i, add /= 2) total += add;
    return total;
}

}  // namespace

Recognition recognize(const std::vector<Stroke>& strokes, const InkContext& ctx)
{
    Recognition out;
    const std::size_t n = strokes.size();
    std::vector<Features> f(n);
    std::vector<StrokeShape> shape(n);
    std::vector<bool> used(n, false);
    for (std::size_t i = 0; i < n; ++i) {
        f[i] = measure(strokes[i]);
        shape[i] = classify(strokes[i]);
    }
    if (ctx.staff_top.empty()) {
        for (std::size_t i = 0; i < n; ++i) out.unused_strokes.push_back(i);
        out.summary = "No staff to write on";
        return out;
    }
    auto staff_of = [&](double y) {
        std::size_t best = 0;
        double d = std::numeric_limits<double>::max();
        for (std::size_t s = 0; s < ctx.staff_top.size(); ++s) {
            const double mid = ctx.staff_top[s] + 2.0;
            if (std::abs(y - mid) < d) d = std::abs(y - mid), best = s;
        }
        return best;
    };
    auto position_of = [&](std::size_t staff, double y) { return (ctx.staff_top[staff] + 4.0 - y) / 0.5; };

    // Barlines: long verticals spanning most of a staff with no head beside them.
    std::vector<double> barlines;

    struct Head {
        std::size_t stroke;
        bool hollow;
        double x, y;
    };
    std::vector<Head> heads;
    for (std::size_t i = 0; i < n; ++i)
        if (shape[i] == StrokeShape::FilledHead || shape[i] == StrokeShape::HollowHead)
            heads.push_back({i, shape[i] == StrokeShape::HollowHead, f[i].cx, f[i].cy});

    // Stems belong to the head they touch.
    struct Note {
        std::vector<std::size_t> heads;  // indices into `heads`
        int stem = -1;
        int flags = 0;
        int dots = 0;
        int alteration = Recognized::kNoAccidental;
        bool accidental_doubt = false;
        std::vector<std::size_t> strokes;
        double x = 0;
    };
    std::vector<Note> notes;
    std::vector<int> head_note(heads.size(), -1);
    for (std::size_t i = 0; i < n; ++i) {
        if (shape[i] != StrokeShape::Stem) continue;
        std::vector<std::size_t> touching;
        for (std::size_t h = 0; h < heads.size(); ++h) {
            const auto& hf = f[heads[h].stroke];
            const bool beside = f[i].cx >= hf.left - 0.45 && f[i].cx <= hf.right + 0.45;
            const bool along = heads[h].y >= f[i].top - 0.7 && heads[h].y <= f[i].bottom + 0.7;
            if (beside && along && head_note[h] < 0) touching.push_back(h);
        }
        if (touching.empty()) {
            // Spanning the staff: a barline.
            const auto st = staff_of(f[i].cy);
            if (f[i].top <= ctx.staff_top[st] + 0.6 && f[i].bottom >= ctx.staff_top[st] + 3.4) {
                barlines.push_back(f[i].cx);
                used[i] = true;
            }
            continue;
        }
        Note note;
        note.stem = static_cast<int>(i);
        note.strokes.push_back(i);
        used[i] = true;
        for (auto h : touching) {
            note.heads.push_back(h);
            note.strokes.push_back(heads[h].stroke);
            used[heads[h].stroke] = true;
            head_note[h] = static_cast<int>(notes.size());
        }
        notes.push_back(std::move(note));
    }
    // Heads without a stem: whole notes, or quarters missing a stem (doubtful).
    for (std::size_t h = 0; h < heads.size(); ++h) {
        if (head_note[h] >= 0) continue;
        // A chord member written without its own stem: the note just beside it.
        int chord = -1;
        for (std::size_t k = 0; k < notes.size(); ++k)
            for (auto other : notes[k].heads)
                if (std::abs(heads[other].x - heads[h].x) < 0.9 && heads[other].hollow == heads[h].hollow) chord = static_cast<int>(k);
        if (chord >= 0) {
            notes[static_cast<std::size_t>(chord)].heads.push_back(h);
            notes[static_cast<std::size_t>(chord)].strokes.push_back(heads[h].stroke);
        } else {
            Note note;
            note.heads.push_back(h);
            note.strokes.push_back(heads[h].stroke);
            head_note[h] = static_cast<int>(notes.size());
            notes.push_back(std::move(note));
        }
        used[heads[h].stroke] = true;
    }
    for (auto& note : notes) {
        double x = 0;
        for (auto h : note.heads) x += heads[h].x;
        note.x = x / static_cast<double>(note.heads.size());
    }
    // Flags at a stem's end; beams joining stems.
    for (std::size_t i = 0; i < n; ++i) {
        if (used[i]) continue;
        if (shape[i] == StrokeShape::Flag || shape[i] == StrokeShape::EighthRest) {
            for (auto& note : notes) {
                if (note.stem < 0) continue;
                const auto& sf = f[static_cast<std::size_t>(note.stem)];
                const bool at_end = std::abs(f[i].left - sf.cx) < 0.6 &&
                                    (std::abs(f[i].top - sf.top) < 1.4 || std::abs(f[i].bottom - sf.bottom) < 1.4);
                if (at_end) {
                    ++note.flags;
                    note.strokes.push_back(i);
                    used[i] = true;
                    break;
                }
            }
        } else if (shape[i] == StrokeShape::Beam) {
            int joined = 0;
            for (auto& note : notes) {
                if (note.stem < 0) continue;
                const auto& sf = f[static_cast<std::size_t>(note.stem)];
                const bool under = sf.cx >= f[i].left - 0.5 && sf.cx <= f[i].right + 0.5;
                const bool at_end = std::abs(f[i].cy - sf.top) < 1.0 || std::abs(f[i].cy - sf.bottom) < 1.0;
                if (under && at_end) {
                    ++note.flags;
                    note.strokes.push_back(i);
                    ++joined;
                }
            }
            used[i] = joined > 0;
        }
    }
    // Dots to a head's right; accidentals to its left.
    for (std::size_t i = 0; i < n; ++i) {
        if (used[i] || shape[i] != StrokeShape::Dot) continue;
        for (auto& note : notes) {
            const auto& hf = f[heads[note.heads.front()].stroke];
            if (f[i].cx > hf.right && f[i].cx < hf.right + 1.4 && std::abs(f[i].cy - hf.cy) < 0.8) {
                ++note.dots;
                note.strokes.push_back(i);
                used[i] = true;
                break;
            }
        }
    }
    for (auto& note : notes) {
        const auto& hf = f[heads[note.heads.front()].stroke];
        std::vector<std::size_t> lines;
        bool flat = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (used[i]) continue;
            const bool left = f[i].right < hf.left + 0.3 && f[i].right > hf.left - 2.6;
            const bool level = f[i].bottom > hf.cy - 2.5 && f[i].top < hf.cy + 2.5;
            if (!left || !level) continue;
            if (shape[i] == StrokeShape::Flat) {
                flat = true;
                note.strokes.push_back(i);
                used[i] = true;
            } else if (shape[i] == StrokeShape::AccidentalLine || shape[i] == StrokeShape::Stem ||
                       shape[i] == StrokeShape::Flag || shape[i] == StrokeShape::Unknown) {
                lines.push_back(i);
            }
        }
        if (flat) {
            note.alteration = -1;
        } else if (!lines.empty()) {
            // A sharp has two verticals and two cross-strokes; a natural, two
            // verticals and two short joins written as two angular strokes.
            int vertical = 0, slanted = 0;
            for (auto i : lines) (f[i].height > f[i].width ? vertical : slanted)++;
            if (lines.size() >= 3 && vertical >= 2 && slanted >= 1) {
                note.alteration = 1;
            } else if (lines.size() == 2 && (vertical >= 1)) {
                note.alteration = 0;
            } else {
                note.alteration = 1;
                note.accidental_doubt = true;
            }
            for (auto i : lines) {
                note.strokes.push_back(i);
                used[i] = true;
            }
        }
    }

    // Rests.
    struct RestMark {
        std::size_t stroke;
        int value;
        double x;
    };
    std::vector<RestMark> rests;
    for (std::size_t i = 0; i < n; ++i) {
        if (used[i]) continue;
        const auto st = staff_of(f[i].cy);
        if (shape[i] == StrokeShape::QuarterRest) rests.push_back({i, 3, f[i].cx});
        else if (shape[i] == StrokeShape::EighthRest) rests.push_back({i, 4, f[i].cx});
        else if (shape[i] == StrokeShape::WholeRest) {
            // Hanging from the fourth line: whole; sitting on the middle line: half.
            const double line4 = ctx.staff_top[st] + 1.0, line3 = ctx.staff_top[st] + 2.0;
            rests.push_back({i, std::abs(f[i].top - line4) < std::abs(f[i].bottom - line3) ? 1 : 2, f[i].cx});
        } else {
            continue;
        }
        used[i] = true;
    }

    // In writing order (left to right), with rhythm from the values.
    struct Event {
        double x;
        int note = -1, rest = -1;
    };
    std::vector<Event> events;
    for (std::size_t k = 0; k < notes.size(); ++k) events.push_back({notes[k].x, static_cast<int>(k), -1});
    for (std::size_t k = 0; k < rests.size(); ++k) events.push_back({rests[k].x, -1, static_cast<int>(k)});
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.x < b.x; });
    std::sort(barlines.begin(), barlines.end());

    std::int64_t tick = 0;
    bool started = false;
    std::size_t next_bar = 0;
    int notes_n = 0, rests_n = 0;
    for (const auto& e : events) {
        Recognized r;
        if (!started) {
            tick = ctx.tick_for_x ? ctx.tick_for_x(e.x) : 0;
            started = true;
        }
        while (next_bar < barlines.size() && barlines[next_bar] < e.x) {
            // A barline written: the next symbol starts the next bar.
            if (ctx.bar_end_of) tick = ctx.bar_end_of(tick);
            ++next_bar;
        }
        if (e.note >= 0) {
            const auto& note = notes[static_cast<std::size_t>(e.note)];
            const auto& head = heads[note.heads.front()];
            const auto st = staff_of(head.y);
            r.kind = Recognized::Kind::Note;
            r.staff = static_cast<int>(st);
            for (auto h : note.heads) {
                const double p = position_of(st, heads[h].y);
                const int rounded = static_cast<int>(std::lround(p));
                r.positions.push_back(rounded);
                const double off = p - rounded;
                if (std::abs(off) > 0.3 && !r.has_alternative) {
                    r.has_alternative = true;
                    r.alternative_position = rounded + (off > 0 ? 1 : -1);
                    r.doubtful = true;
                    r.why = "between a line and a space";
                }
            }
            std::sort(r.positions.begin(), r.positions.end());
            r.positions.erase(std::unique(r.positions.begin(), r.positions.end()), r.positions.end());
            if (head.hollow) {
                r.value = note.stem >= 0 ? 2 : 1;
            } else if (note.stem < 0) {
                r.value = 3;
                r.doubtful = true;
                if (r.why.empty()) r.why = "no stem: read as a quarter";
            } else {
                r.value = std::min(6, 3 + note.flags);
            }
            r.dots = std::min(2, note.dots);
            r.alteration = note.alteration;
            if (note.accidental_doubt) {
                r.doubtful = true;
                if (r.why.empty()) r.why = "accidental unclear: read as a sharp";
            }
            r.x = head.x;
            r.y = head.y;
            r.strokes = note.strokes;
            ++notes_n;
        } else {
            const auto& rest = rests[static_cast<std::size_t>(e.rest)];
            r.kind = Recognized::Kind::Rest;
            r.value = rest.value;
            r.staff = static_cast<int>(staff_of(f[rest.stroke].cy));
            r.x = f[rest.stroke].cx;
            r.y = f[rest.stroke].cy;
            r.strokes = {rest.stroke};
            ++rests_n;
        }
        r.tick = tick;
        const auto length = duration_ticks(ctx, r.value, r.dots);
        if (ctx.bar_end_of && tick + length > ctx.bar_end_of(tick)) {
            r.doubtful = true;
            if (r.why.empty()) r.why = "the bar is too full";
        }
        tick += length;
        std::sort(r.strokes.begin(), r.strokes.end());
        out.items.push_back(std::move(r));
    }
    for (std::size_t i = 0; i < n; ++i)
        if (!used[i]) out.unused_strokes.push_back(i);
    out.summary = std::to_string(notes_n) + (notes_n == 1 ? " note" : " notes");
    if (rests_n > 0) out.summary += ", " + std::to_string(rests_n) + (rests_n == 1 ? " rest" : " rests");
    if (const int d = out.doubtful_count(); d > 0) out.summary += " - " + std::to_string(d) + " to check";
    if (!out.unused_strokes.empty())
        out.summary += " - " + std::to_string(out.unused_strokes.size()) + " stroke" +
                       (out.unused_strokes.size() == 1 ? "" : "s") + " not read";
    return out;
}

}  // namespace detail

}  // namespace vcc::mobile
