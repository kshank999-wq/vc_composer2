// Copyright (c) VC Composer. All rights reserved.
//
// Sample-accurate delivery of VC Composer's MIDI into host process blocks
// (spec 7: "Preserve musical timing and avoid jitter when transferring
// generated MIDI into the Pro Tools workflow").
//
// VC Composer sends events ahead of time, stamped with absolute positions.
// The bridge's I/O thread hands them over whenever they arrive - which is
// not aligned to block boundaries - so this class holds them until the block
// that actually contains their position, then emits them at the correct
// offset within it. Delivery jitter on the IPC link therefore does not
// become timing jitter in the host.
//
// A position is a host sample, or a score tick (`music::kMidiEventMusicalTime`).
// A tick is placed against the host's musical position *for the block being
// collected*, every block, so the sample it lands on is not decided until it
// is due - which is what makes a phrase sent ahead of a ritardando follow
// the ritardando. Deciding it any earlier, on either side of the bridge,
// bakes in whatever tempo was current at the time (docs/aax/12-notation-to-host.md).
//
// Every operation is real-time safe: fixed capacity allocated up front, no
// locks, no allocation, and a bounded linear scan per block.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "vcc/music/MidiEvent.h"
#include "vcc/music/MusicalTime.h"

namespace vcc::engine {

/// The host sample a musical-time event lands on, from the host's position
/// at the start of a block. False when the host has not reported enough to
/// place it: no sample position, no musical position, or no tempo.
///
/// A free function so a test can pin the arithmetic without a scheduler,
/// and so the scheduler has exactly one way of doing it.
bool place_in_block(const music::MidiEvent& event, const music::MusicalPosition& position,
                    double sample_rate, std::int64_t& sample_out) noexcept;

class MidiScheduler {
public:
    /// `capacity` is the largest number of events that may be pending at
    /// once. Allocated in the constructor, on a non-real-time thread.
    explicit MidiScheduler(std::size_t capacity);

    MidiScheduler(const MidiScheduler&) = delete;
    MidiScheduler& operator=(const MidiScheduler&) = delete;

    /// Drops everything pending. Called on transport stop, session reload and
    /// reconnect, where stale future events would otherwise fire later
    /// (spec 5: handle transport and lifecycle changes safely).
    void reset() noexcept;

    /// Accepts events stamped with absolute positions - host samples, or
    /// score ticks for events flagged `music::kMidiEventMusicalTime`. Returns
    /// the number stored; a shortfall means the scheduler was full and the
    /// remainder was dropped (counted, never silent). Real-time safe.
    std::size_t enqueue(const music::MidiEvent* events, std::size_t count) noexcept;

    /// Emits the events belonging to the block starting at
    /// `block_start_sample` and spanning `block_size` samples, rewritten to
    /// block-relative offsets and sorted by position.
    ///
    /// An event in musical time is first placed with `place_in_block` from
    /// `position` and `sample_rate` - the host's position at the start of
    /// *this* block, at this block's tempo. One the host has not given
    /// enough to place is emitted now, at offset 0, for the same reason an
    /// event with no position is: without a timeline there is nothing to
    /// hold it against, and a note-off held forever is a note that never
    /// ends.
    ///
    /// Events whose position has already passed are emitted at offset 0 - the
    /// alternative is dropping a note VC Composer asked for - and counted in
    /// `late_events()`. Events beyond this block stay pending. An event
    /// exactly on the end boundary belongs to the next block.
    ///
    /// When `block_start_sample` is `kUnknownSamplePosition` the host is not
    /// reporting a timeline (a stopped transport in some hosts), so
    /// everything pending is emitted immediately at offset 0: without a
    /// timeline there is nothing to schedule against.
    ///
    /// What comes out is in block offsets, whatever went in: the musical-time
    /// flag is cleared on the way.
    ///
    /// Returns the number written to `out`. Real-time safe.
    std::size_t collect(std::int64_t block_start_sample, std::int32_t block_size,
                        const music::MusicalPosition& position, double sample_rate,
                        music::MidiEvent* out, std::size_t out_capacity) noexcept;

    /// The same, for a host that reports no musical position: every event
    /// in musical time is emitted at once.
    std::size_t collect(std::int64_t block_start_sample, std::int32_t block_size,
                        music::MidiEvent* out, std::size_t out_capacity) noexcept
    {
        return collect(block_start_sample, block_size, music::MusicalPosition{}, 0.0, out,
                       out_capacity);
    }

    std::size_t pending() const noexcept { return count_; }
    std::size_t capacity() const noexcept { return capacity_; }

    /// Events dropped because the scheduler was full.
    std::uint64_t dropped_events() const noexcept { return dropped_; }
    /// Events that arrived after the block they were scheduled for.
    std::uint64_t late_events() const noexcept { return late_; }

    void reset_counters() noexcept
    {
        dropped_ = 0;
        late_ = 0;
    }

private:
    std::size_t capacity_;
    std::unique_ptr<music::MidiEvent[]> pending_;
    std::size_t count_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t late_ = 0;
};

}  // namespace vcc::engine
