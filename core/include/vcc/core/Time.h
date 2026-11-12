// Musical time, meter, tempo and timecode.
//
// Ticks are exact integers at 2880 per quarter note, the same resolution the
// original VC Composer notation engine uses. 2880 divides by 2, 3, 4, 5, 6, 8,
// 9, 10, 12, 16 and 64-th triplets, so ordinary and nested tuplets land on
// whole ticks and no position ever drifts. MIDI files and plug-ins that want
// 960 PPQ are converted where the data leaves the model.
//
// Templates Supplement §9.3: "Quantize at DAW musical ticks (PPQ) with
// time-signature-aware bar arithmetic; tempo changes do not move MIDI event
// bar positions." That is why bars are computed from the meter map alone, and
// seconds are a separate projection through the tempo map.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vcc {

using Ticks = std::int64_t;
constexpr Ticks kTicksPerQuarter = 2880;
constexpr Ticks kTicksPerWhole = kTicksPerQuarter * 4;
constexpr Ticks kMidiFilePpq = 960;

struct TimeSignature {
    int numerator = 4;
    int denominator = 4;  ///< Power of two: 1, 2, 4, 8, 16, 32.

    bool is_valid() const noexcept;
    Ticks ticks_per_bar() const noexcept { return numerator * beat_ticks(); }
    /// The written beat unit (a quarter in 4/4, an eighth in 7/8).
    Ticks beat_ticks() const noexcept { return kTicksPerWhole / denominator; }
    /// True for 6/8, 9/8, 12/8 and the like, where the felt beat is a
    /// dotted note grouping three written beats.
    bool is_compound() const noexcept { return numerator % 3 == 0 && numerator > 3 && denominator >= 8; }
    /// Beat grouping used for accents and beaming: 7/8 -> {2,2,3}, 6/8 -> {3,3},
    /// 5/4 -> {3,2}, 4/4 -> {1,1,1,1}. Values are in written beats.
    std::vector<int> default_grouping() const;
    std::string to_string() const;
    static std::optional<TimeSignature> parse(const std::string& text);
};

bool operator==(const TimeSignature& a, const TimeSignature& b) noexcept;
bool operator!=(const TimeSignature& a, const TimeSignature& b) noexcept;

/// Meter changes indexed by bar. Bar numbers are 1-based, as a composer reads
/// them, and the first entry is always bar 1.
class MeterMap {
public:
    MeterMap();
    explicit MeterMap(TimeSignature initial);

    /// Sets the meter from `bar` onwards (replacing any change at that bar).
    void set(int bar, TimeSignature sig);
    TimeSignature at_bar(int bar) const noexcept;
    TimeSignature at_tick(Ticks tick) const noexcept;

    /// Tick at the start of a 1-based bar.
    Ticks bar_start(int bar) const noexcept;
    /// 1-based bar containing `tick` (ticks before 0 are bar 1).
    int bar_at(Ticks tick) const noexcept;
    /// Total length of `count` bars starting at `first_bar`.
    Ticks span(int first_bar, int count) const noexcept;

    struct Change {
        int bar = 1;
        TimeSignature signature;
    };
    const std::vector<Change>& changes() const noexcept { return changes_; }

private:
    std::vector<Change> changes_;  ///< Sorted by bar; changes_[0].bar == 1.
};

/// Tempo changes indexed by tick. Converts musical time to seconds, which is
/// what audio, video and timecode need. Changes are instantaneous steps;
/// ramps are a later extension.
class TempoMap {
public:
    TempoMap();
    explicit TempoMap(double initial_bpm);

    void set(Ticks tick, double quarter_bpm);
    double bpm_at(Ticks tick) const noexcept;
    double seconds_at(Ticks tick) const noexcept;
    Ticks tick_at_seconds(double seconds) const noexcept;

    struct Change {
        Ticks tick = 0;
        double bpm = 120.0;
    };
    const std::vector<Change>& changes() const noexcept { return changes_; }

private:
    std::vector<Change> changes_;
};

/// Film and TV frame rates (Main Spec §13), including the fractional NTSC
/// rates and their drop-frame counting.
enum class FrameRate : std::uint8_t {
    Fps23_976,
    Fps24,
    Fps25,
    Fps29_97_NonDrop,
    Fps29_97_Drop,
    Fps30,
    Fps47_952,
    Fps48,
    Fps50,
    Fps59_94_NonDrop,
    Fps59_94_Drop,
    Fps60,
};

const char* to_string(FrameRate rate) noexcept;
std::optional<FrameRate> frame_rate_from_string(const std::string& text) noexcept;
/// Real frames per second (23.976... = 24000/1001).
double frames_per_second(FrameRate rate) noexcept;
/// Integer frames counted per timecode second (24 for 23.976, 30 for 29.97).
int nominal_fps(FrameRate rate) noexcept;
bool is_drop_frame(FrameRate rate) noexcept;

struct Timecode {
    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    int frames = 0;
    bool negative = false;

    /// "01:00:00:00", or "01:00:00;00" for drop frame.
    std::string to_string(FrameRate rate) const;
    /// Accepts ':' or ';' as the frame separator.
    static std::optional<Timecode> parse(const std::string& text);
};

/// Converts a frame count since 00:00:00:00 to timecode, applying drop-frame
/// numbering where the rate uses it.
Timecode frames_to_timecode(std::int64_t frame_count, FrameRate rate) noexcept;
/// Inverse of frames_to_timecode. Returns nullopt for a label drop-frame
/// skips (e.g. 00:01:00;00 at 29.97 DF) or out-of-range fields.
std::optional<std::int64_t> timecode_to_frames(const Timecode& tc, FrameRate rate) noexcept;

/// Wall-clock seconds of a frame boundary, relative to 00:00:00:00.
double frames_to_seconds(std::int64_t frame_count, FrameRate rate) noexcept;
/// Frame containing `seconds` (floor).
std::int64_t seconds_to_frames(double seconds, FrameRate rate) noexcept;

}  // namespace vcc
