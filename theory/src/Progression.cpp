#include "vcc/theory/Progression.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "vcc/core/Files.h"

namespace vcc::theory {

namespace {

std::string lower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

std::optional<std::vector<Chord>> Progression::chords(const Key& key, std::string* error) const
{
    std::vector<Chord> out;
    for (const auto& s : steps) {
        auto c = chord_from_roman(s.numeral, key, error);
        if (!c) return std::nullopt;
        out.push_back(*c);
    }
    return out;
}

std::string Progression::roman_text() const
{
    std::string s;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (i) s += "\xE2\x80\x93";  // en dash
        s += steps[i].numeral;
    }
    return s;
}

// --- Catalog ------------------------------------------------------------------

bool ProgressionCatalog::load(const json::Value& doc, std::string* error)
{
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (doc.get_int("schema_version") != 1) return fail("progressions: schema_version must be 1");
    std::vector<Progression> items;
    for (const auto& p : doc.get("progressions") ? doc.get("progressions")->as_array() : json::Array{}) {
        Progression g;
        g.id = p.get_string("id");
        if (g.id.empty()) return fail("progression without id");
        if (std::any_of(items.begin(), items.end(), [&](const Progression& x) { return x.id == g.id; }))
            return fail("duplicate progression id " + g.id);
        g.name = p.get_string("name", g.id);
        g.family = p.get_string("family");
        g.tonality = p.get_string("tonality");
        auto mode = mode_from_string(p.get_string("default_mode", "major"));
        if (!mode) return fail(g.id + ": unknown default_mode");
        g.default_mode = *mode;
        g.bars = static_cast<int>(p.get_int("bars", 4));
        g.fixed_length = p.get_bool("fixed_length");
        g.cadence = p.get_string("cadence");
        g.tags = p.get_string_array("tags");
        g.notes = p.get_string("notes");
        if (const auto* lens = p.get("allowed_bar_lengths"))
            for (const auto& l : lens->as_array()) g.allowed_bar_lengths.push_back(static_cast<int>(l.as_int()));
        double total = 0;
        if (const auto* ch = p.get("chords")) {
            for (const auto& c : ch->as_array()) {
                ProgressionStep s{c.get_string("numeral"), c.get_double("bars", 1.0)};
                std::string err;
                if (!parse_roman(s.numeral, &err)) return fail(g.id + ": " + err);
                if (s.bars <= 0) return fail(g.id + ": non-positive chord length");
                total += s.bars;
                g.steps.push_back(s);
            }
        }
        if (g.steps.empty()) return fail(g.id + ": no chords");
        if (std::fabs(total - g.bars) > 1e-9) return fail(g.id + ": chord lengths do not add up to bars");
        if (g.allowed_bar_lengths.empty()) g.allowed_bar_lengths.push_back(g.bars);
        items.push_back(std::move(g));
    }
    items_ = std::move(items);
    return true;
}

bool ProgressionCatalog::load_file(const std::string& path, std::string* error)
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

const Progression* ProgressionCatalog::find(const std::string& id) const noexcept
{
    for (const auto& p : items_)
        if (p.id == id) return &p;
    return nullptr;
}

std::vector<const Progression*> ProgressionCatalog::search(const std::string& query) const
{
    std::string q = lower(query);
    std::vector<const Progression*> out;
    for (const auto& p : items_) {
        bool hit = q.empty() || lower(p.id).find(q) != std::string::npos ||
                   lower(p.name).find(q) != std::string::npos || lower(p.family).find(q) != std::string::npos;
        for (const auto& t : p.tags) hit = hit || lower(t).find(q) != std::string::npos;
        if (hit) out.push_back(&p);
    }
    return out;
}

// --- Rendering ----------------------------------------------------------------

const char* to_string(ChordRhythm r) noexcept
{
    switch (r) {
        case ChordRhythm::Block: return "block";
        case ChordRhythm::BeatPulse: return "pulse";
        case ChordRhythm::Arpeggio: return "arpeggio";
        case ChordRhythm::Strum: return "strum";
    }
    return "block";
}

std::optional<ChordRhythm> chord_rhythm_from_string(const std::string& t) noexcept
{
    for (ChordRhythm r : {ChordRhythm::Block, ChordRhythm::BeatPulse, ChordRhythm::Arpeggio, ChordRhythm::Strum})
        if (t == to_string(r)) return r;
    return std::nullopt;
}

namespace {

/// Tick of a position measured in bars from `start_bar` (fractional bars
/// are fractions of the bar they fall in).
Ticks tick_at(const MeterMap& meter, int start_bar, double bars_from_start)
{
    double whole = std::floor(bars_from_start + 1e-9);
    double frac = bars_from_start - whole;
    if (frac < 1e-9) frac = 0;
    int bar = start_bar + static_cast<int>(whole);
    Ticks base = meter.bar_start(bar);
    return base + static_cast<Ticks>(std::llround(frac * static_cast<double>(meter.at_bar(bar).ticks_per_bar())));
}

void emit(std::vector<NoteEvent>& out, Ticks start, Ticks slot, double legato, int pitch, int velocity,
          int channel, Ticks limit)
{
    Ticks dur = std::max<Ticks>(1, static_cast<Ticks>(std::llround(static_cast<double>(slot) * legato)));
    if (start + dur > limit) dur = limit - start;
    if (dur <= 0 || pitch < 0 || pitch > 127) return;
    out.push_back({start, dur, pitch, std::clamp(velocity, 1, 127), channel, {}});
}

}  // namespace

std::optional<ProgressionRender> render_progression(const Progression& prog, const MeterMap& meter,
                                                    const ProgressionRenderOptions& opt, std::string* error)
{
    auto fail = [&](const std::string& why) -> std::optional<ProgressionRender> {
        if (error) *error = why;
        return std::nullopt;
    };
    if (opt.start_bar < 1) return fail("start bar must be 1 or later");
    if (opt.length_bars <= 0) return fail("length must be positive");
    if (opt.length_bars % prog.bars != 0) {
        std::string allowed;
        for (int l : prog.allowed_bar_lengths) allowed += (allowed.empty() ? "" : ", ") + std::to_string(l);
        return fail("'" + prog.name + "' is a " + std::to_string(prog.bars) + "-bar pattern and cannot fill " +
                    std::to_string(opt.length_bars) + " bars without truncation (allowed: " + allowed + ")");
    }
    const int factor = opt.length_bars / prog.bars;
    if (prog.fixed_length && factor > 1 && opt.expand == ExpandMode::Stretch)
        return fail("'" + prog.name + "' is a fixed form; repeat it rather than stretching it");

    auto chords = prog.chords(opt.key, error);
    if (!chords) return std::nullopt;

    // Expand the pattern to the requested length.
    struct Slot { std::size_t step; double bars; };
    std::vector<Slot> slots;
    if (opt.expand == ExpandMode::Repeat) {
        for (int r = 0; r < factor; ++r)
            for (std::size_t i = 0; i < prog.steps.size(); ++i) slots.push_back({i, prog.steps[i].bars});
    } else {
        for (std::size_t i = 0; i < prog.steps.size(); ++i) slots.push_back({i, prog.steps[i].bars * factor});
    }

    std::vector<Chord> seq;
    std::vector<std::optional<int>> locks;
    for (const auto& s : slots) {
        seq.push_back((*chords)[s.step]);
        locks.push_back(s.step < opt.locked_inversions.size() ? opt.locked_inversions[s.step] : std::nullopt);
    }
    auto voicings = voice_progression(seq, opt.voicing, locks);

    ProgressionRender out;
    out.start = meter.bar_start(opt.start_bar);
    out.end = out.start + meter.span(opt.start_bar, opt.length_bars);
    double pos = 0;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        RealizedChord rc;
        rc.numeral = prog.steps[slots[i].step].numeral;
        rc.chord = seq[i];
        rc.voicing = voicings[i];
        rc.start = tick_at(meter, opt.start_bar, pos);
        pos += slots[i].bars;
        // The last boundary is the region end exactly, so rounding can never
        // leave a gap or overhang.
        Ticks end = i + 1 == slots.size() ? out.end : tick_at(meter, opt.start_bar, pos);
        rc.duration = end - rc.start;
        out.chords.push_back(rc);
    }

    for (const auto& rc : out.chords) {
        const Ticks end = rc.start + rc.duration;
        const auto& notes = rc.voicing.notes;
        switch (opt.rhythm) {
            case ChordRhythm::Block:
                for (int p : notes) emit(out.notes, rc.start, rc.duration, opt.legato, p, opt.velocity, opt.channel, end);
                break;
            case ChordRhythm::Strum: {
                const Ticks roll = kTicksPerQuarter / 24;  // ~20 ms at 120 bpm per string
                for (std::size_t k = 0; k < notes.size(); ++k) {
                    Ticks s = rc.start + roll * static_cast<Ticks>(k);
                    if (s >= end) break;
                    emit(out.notes, s, end - s, opt.legato, notes[k],
                         opt.velocity - static_cast<int>(k) * 2, opt.channel, end);
                }
                break;
            }
            case ChordRhythm::BeatPulse: {
                Ticks t = rc.start;
                while (t < end) {
                    TimeSignature sig = meter.at_tick(t);
                    Ticks beat = sig.is_compound() ? sig.beat_ticks() * 3 : sig.beat_ticks();
                    Ticks bar0 = meter.bar_start(meter.bar_at(t));
                    bool downbeat = t == bar0;
                    Ticks slot = std::min(beat, end - t);
                    for (int p : notes)
                        emit(out.notes, t, slot, opt.legato, p, opt.velocity + (downbeat ? 8 : 0), opt.channel, end);
                    t += slot;
                }
                break;
            }
            case ChordRhythm::Arpeggio: {
                const Ticks step = kTicksPerQuarter / 2;
                std::size_t k = 0;
                for (Ticks t = rc.start; t < end && !notes.empty(); t += step, ++k) {
                    Ticks slot = std::min(step, end - t);
                    emit(out.notes, t, slot, std::max(opt.legato, 0.98), notes[k % notes.size()],
                         opt.velocity - (k % notes.size() == 0 ? 0 : 6), opt.channel, end);
                }
                break;
            }
        }
    }
    std::sort(out.notes.begin(), out.notes.end());
    return out;
}

}  // namespace vcc::theory
