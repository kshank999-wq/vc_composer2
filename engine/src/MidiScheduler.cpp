#include "vcc/engine/MidiScheduler.h"

#include <cstddef>
#include <cmath>

namespace vcc::engine {
namespace {

/// Insertion sort over the block's events. The count is small (a dense bar of
/// orchestral MIDI is tens of events, not thousands), and unlike a general
/// sort this allocates nothing and has no recursion, so it is safe to run on
/// the audio thread.
void sort_by_position(music::MidiEvent* events, std::size_t count) noexcept
{
    for (std::size_t i = 1; i < count; ++i) {
        const music::MidiEvent key = events[i];
        std::size_t j = i;
        while (j > 0 && events[j - 1].sample_position > key.sample_position) {
            events[j] = events[j - 1];
            --j;
        }
        events[j] = key;
    }
}

}  // namespace

bool place_in_block(const music::MidiEvent& event, const music::MusicalPosition& position,
                    double sample_rate, std::int64_t& sample_out) noexcept
{
    if (!position.sample_position_valid || !position.ppq_position_valid
        || !position.tempo_valid || !(position.tempo_bpm > 0.0) || !(sample_rate > 0.0)) {
        return false;
    }

    // From the block's own anchor, not from the session start: the host's
    // ppq and sample position are what it says about *this* block, and a
    // tempo change before it is already inside them. Only the stretch from
    // here to the event is at this block's tempo, and by the time the event
    // is due that stretch is at most one block long.
    const double quarters = static_cast<double>(event.sample_position)
                              / static_cast<double>(music::kTicksPerQuarter)
                          - position.ppq_position;
    const double seconds = quarters * 60.0 / position.tempo_bpm;
    const double samples = seconds * sample_rate;
    if (!std::isfinite(samples))
        return false;
    sample_out = position.sample_position + static_cast<std::int64_t>(std::llround(samples));
    return true;
}

MidiScheduler::MidiScheduler(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity)
    , pending_(new music::MidiEvent[capacity_])
{
}

void MidiScheduler::reset() noexcept
{
    count_ = 0;
}

std::size_t MidiScheduler::enqueue(const music::MidiEvent* events, std::size_t count) noexcept
{
    if (events == nullptr || count == 0)
        return 0;

    std::size_t accepted = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (count_ >= capacity_) {
            dropped_ += count - accepted;
            break;
        }
        if (!music::is_valid(events[i])) {
            // A malformed event from the wire is dropped here rather than
            // handed to the host, which would be free to misbehave on it.
            ++dropped_;
            continue;
        }
        pending_[count_++] = events[i];
        ++accepted;
    }
    return accepted;
}

std::size_t MidiScheduler::collect(std::int64_t block_start_sample, std::int32_t block_size,
                                   const music::MusicalPosition& position, double sample_rate,
                                   music::MidiEvent* out, std::size_t out_capacity) noexcept
{
    if (out == nullptr || out_capacity == 0 || count_ == 0)
        return 0;

    std::size_t emitted = 0;

    if (block_start_sample == music::kUnknownSamplePosition) {
        // No host timeline: emit as much as fits, now.
        const std::size_t take = count_ < out_capacity ? count_ : out_capacity;
        for (std::size_t i = 0; i < take; ++i) {
            out[i] = pending_[i];
            out[i].sample_position = 0;
            out[i].flags = static_cast<std::uint16_t>(out[i].flags & ~music::kMidiEventMusicalTime);
        }
        // Keep whatever did not fit, preserving order.
        for (std::size_t i = take; i < count_; ++i)
            pending_[i - take] = pending_[i];
        count_ -= take;
        return take;
    }

    const std::int64_t block_end = block_start_sample + (block_size > 0 ? block_size : 0);

    std::size_t keep = 0;
    for (std::size_t i = 0; i < count_; ++i) {
        const music::MidiEvent& event = pending_[i];

        // Where the event falls on the host's sample timeline. An event in
        // musical time is placed here, against this block's position and
        // tempo, and nowhere earlier; one the host has not given enough to
        // place is treated as having no position, and goes out now.
        std::int64_t due_sample = event.sample_position;
        if (music::is_in_musical_time(event)
            && !place_in_block(event, position, sample_rate, due_sample)) {
            due_sample = music::kUnknownSamplePosition;
        }

        // Due if it falls inside this block, or if its time has already
        // passed. The second clause is what a zero-length block reduces to,
        // and it keeps an overdue event from being stranded by one.
        const bool is_due = due_sample == music::kUnknownSamplePosition
                         || due_sample < block_end
                         || due_sample <= block_start_sample;

        if (!is_due) {
            pending_[keep++] = event;  // Still in the future; hold it.
            continue;
        }

        if (emitted >= out_capacity) {
            // The host cannot take any more events this block. Hold the rest
            // rather than dropping them; they go out next block, slightly
            // late, which is why late_ is a counter the UI can surface.
            pending_[keep++] = event;
            continue;
        }

        music::MidiEvent emitted_event = event;
        emitted_event.flags =
            static_cast<std::uint16_t>(emitted_event.flags & ~music::kMidiEventMusicalTime);
        if (due_sample == music::kUnknownSamplePosition || due_sample < block_start_sample) {
            emitted_event.sample_position = 0;
            ++late_;
        } else {
            emitted_event.sample_position = due_sample - block_start_sample;
        }

        out[emitted++] = emitted_event;
    }

    count_ = keep;
    sort_by_position(out, emitted);
    return emitted;
}

}  // namespace vcc::engine
