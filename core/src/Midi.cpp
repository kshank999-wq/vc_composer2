#include "vcc/core/Midi.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace vcc {

bool operator==(const NoteEvent& a, const NoteEvent& b) noexcept
{
    return a.start == b.start && a.duration == b.duration && a.pitch == b.pitch &&
           a.velocity == b.velocity && a.channel == b.channel && a.articulation == b.articulation;
}
bool operator!=(const NoteEvent& a, const NoteEvent& b) noexcept { return !(a == b); }

bool operator<(const NoteEvent& a, const NoteEvent& b) noexcept
{
    return std::tie(a.start, a.pitch, a.duration, a.channel, a.velocity) <
           std::tie(b.start, b.pitch, b.duration, b.channel, b.velocity);
}

std::string validate_notes(const std::vector<NoteEvent>& notes, Ticks range_start, Ticks range_end)
{
    std::map<std::pair<int, int>, Ticks> last_end;  // (channel, pitch) -> end
    std::vector<NoteEvent> sorted = notes;
    std::sort(sorted.begin(), sorted.end());
    for (const auto& n : sorted) {
        std::string at = " at tick " + std::to_string(n.start) + " pitch " + std::to_string(n.pitch);
        if (n.duration <= 0) return "non-positive duration" + at;
        if (n.pitch < 0 || n.pitch > 127) return "pitch out of range" + at;
        if (n.velocity < 1 || n.velocity > 127) return "velocity out of range" + at;
        if (n.channel < 0 || n.channel > 15) return "channel out of range" + at;
        if (n.start < range_start) return "note before range start" + at;
        if (n.end() > range_end) return "note past range end" + at;
        auto key = std::make_pair(n.channel, n.pitch);
        auto it = last_end.find(key);
        if (it != last_end.end() && n.start < it->second) return "overlapping same-pitch notes" + at;
        last_end[key] = n.end();
    }
    return {};
}

namespace {

struct RawEvent {
    std::int64_t tick;  // at 960 PPQ
    int order;          // tie-break: meta < note-off < note-on
    std::vector<std::uint8_t> bytes;
};

void put_varlen(std::string& out, std::uint32_t v)
{
    std::uint8_t buf[5];
    int n = 0;
    buf[n++] = static_cast<std::uint8_t>(v & 0x7F);
    while ((v >>= 7) != 0) buf[n++] = static_cast<std::uint8_t>(0x80 | (v & 0x7F));
    while (n > 0) out += static_cast<char>(buf[--n]);
}

void put_be(std::string& out, std::uint32_t v, int bytes)
{
    for (int i = bytes - 1; i >= 0; --i) out += static_cast<char>((v >> (8 * i)) & 0xFF);
}

std::int64_t to_file_ticks(Ticks t)
{
    const Ticks ratio = kTicksPerQuarter / kMidiFilePpq;
    return (t + ratio / 2) / ratio;
}

std::string track_chunk(std::vector<RawEvent> events)
{
    std::stable_sort(events.begin(), events.end(), [](const RawEvent& a, const RawEvent& b) {
        return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
    });
    std::string data;
    std::int64_t prev = 0;
    for (const auto& e : events) {
        put_varlen(data, static_cast<std::uint32_t>(std::max<std::int64_t>(0, e.tick - prev)));
        prev = std::max(prev, e.tick);
        for (auto b : e.bytes) data += static_cast<char>(b);
    }
    data += std::string("\x00\xFF\x2F\x00", 4);  // end of track
    std::string chunk = "MTrk";
    put_be(chunk, static_cast<std::uint32_t>(data.size()), 4);
    return chunk + data;
}

RawEvent meta_text(std::int64_t tick, std::uint8_t type, const std::string& text)
{
    RawEvent e{tick, 0, {0xFF, type}};
    std::string len;
    put_varlen(len, static_cast<std::uint32_t>(text.size()));
    for (char c : len) e.bytes.push_back(static_cast<std::uint8_t>(c));
    for (char c : text) e.bytes.push_back(static_cast<std::uint8_t>(c));
    return e;
}

}  // namespace

std::string write_midi_file(const std::vector<MidiFileTrack>& tracks, const MidiFileOptions& options)
{
    std::string out = "MThd";
    put_be(out, 6, 4);
    put_be(out, 1, 2);
    put_be(out, static_cast<std::uint32_t>(tracks.size() + 1), 2);
    put_be(out, static_cast<std::uint32_t>(kMidiFilePpq), 2);

    std::vector<RawEvent> conductor;
    for (const auto& c : options.tempo.changes()) {
        auto usec = static_cast<std::uint32_t>(std::lround(60000000.0 / c.bpm));
        conductor.push_back({to_file_ticks(c.tick), 0,
                             {0xFF, 0x51, 0x03, static_cast<std::uint8_t>(usec >> 16),
                              static_cast<std::uint8_t>(usec >> 8), static_cast<std::uint8_t>(usec)}});
    }
    for (const auto& c : options.meter.changes()) {
        std::uint8_t pow2 = 0;
        for (int d = c.signature.denominator; d > 1; d >>= 1) ++pow2;
        conductor.push_back({to_file_ticks(options.meter.bar_start(c.bar)), 0,
                             {0xFF, 0x58, 0x04, static_cast<std::uint8_t>(c.signature.numerator), pow2, 24, 8}});
    }
    for (const auto& m : options.markers) conductor.push_back(meta_text(to_file_ticks(m.first), 0x06, m.second));
    out += track_chunk(std::move(conductor));

    for (const auto& t : tracks) {
        std::vector<RawEvent> ev;
        ev.push_back(meta_text(0, 0x03, t.name));
        int channel = t.notes.empty() ? 0 : std::clamp(t.notes.front().channel, 0, 15);
        if (t.program >= 0 && t.program <= 127)
            ev.push_back({0, 0, {static_cast<std::uint8_t>(0xC0 | channel), static_cast<std::uint8_t>(t.program)}});
        for (const auto& c : t.controllers) {
            // Controllers sort before notes at the same tick, so a dynamic
            // is in force when the note it shapes starts.
            ev.push_back({to_file_ticks(c.tick), 0,
                          {static_cast<std::uint8_t>(0xB0 | std::clamp(c.channel, 0, 15)),
                           static_cast<std::uint8_t>(std::clamp(c.controller, 0, 127)),
                           static_cast<std::uint8_t>(std::clamp(c.value, 0, 127))}});
        }
        for (const auto& n : t.notes) {
            auto ch = static_cast<std::uint8_t>(std::clamp(n.channel, 0, 15));
            auto p = static_cast<std::uint8_t>(std::clamp(n.pitch, 0, 127));
            auto v = static_cast<std::uint8_t>(std::clamp(n.velocity, 1, 127));
            std::int64_t on = to_file_ticks(n.start);
            std::int64_t off = std::max(on + 1, to_file_ticks(n.end()));
            ev.push_back({on, 2, {static_cast<std::uint8_t>(0x90 | ch), p, v}});
            ev.push_back({off, 1, {static_cast<std::uint8_t>(0x80 | ch), p, 0}});
        }
        out += track_chunk(std::move(ev));
    }
    return out;
}

}  // namespace vcc
