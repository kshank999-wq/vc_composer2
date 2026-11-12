// Copyright (c) VC Composer. All rights reserved.
//
// Lock-free single-producer/single-consumer queue (spec 6, 8).
//
// This is the only structure that crosses the audio-thread boundary. It
// allocates once, in the constructor, on a non-real-time thread; every
// push/pop afterwards is wait-free and allocation-free, and a full queue
// drops with an explicit, countable policy rather than blocking.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace vcc::audio {

/// Fixed-capacity lock-free queue for trivially copyable elements.
///
/// Exactly one thread may call `push`/`push_bulk` and exactly one other may
/// call `pop`/`pop_bulk`. Capacity is rounded up to a power of two and one
/// slot is reserved to distinguish full from empty, so `capacity()` reports
/// the usable count.
template <typename T>
class SpscQueue {
    static_assert(std::is_trivially_copyable<T>::value,
                  "SpscQueue elements cross a real-time boundary and must be trivially copyable");
    static_assert(std::is_trivially_destructible<T>::value,
                  "SpscQueue must never run a destructor on the audio thread");

public:
    explicit SpscQueue(std::size_t minimum_capacity)
        : slot_count_(round_up_pow2(minimum_capacity + 1))
        , mask_(slot_count_ - 1)
        , slots_(new T[slot_count_]())
    {
    }

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    /// Usable element count.
    std::size_t capacity() const noexcept { return slot_count_ - 1; }

    /// Elements currently readable. Safe to call from either thread, but the
    /// result is a lower bound for the consumer and an upper bound for the
    /// producer, so use it for diagnostics rather than control flow.
    std::size_t size() const noexcept
    {
        const std::size_t w = write_index_.load(std::memory_order_acquire);
        const std::size_t r = read_index_.load(std::memory_order_acquire);
        return (w - r) & mask_;
    }

    bool empty() const noexcept
    {
        return write_index_.load(std::memory_order_acquire)
            == read_index_.load(std::memory_order_acquire);
    }

    /// Producer side. Returns false and increments the drop counter when the
    /// queue is full - it never blocks and never allocates.
    bool push(const T& value) noexcept
    {
        const std::size_t w = write_index_.load(std::memory_order_relaxed);
        const std::size_t next = (w + 1) & mask_;
        if (next == read_index_.load(std::memory_order_acquire)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        slots_[w] = value;
        write_index_.store(next, std::memory_order_release);
        return true;
    }

    /// Pushes up to `count` elements, stopping at the first failure. Returns
    /// the number accepted; the remainder is counted as dropped.
    std::size_t push_bulk(const T* values, std::size_t count) noexcept
    {
        std::size_t written = 0;
        while (written < count && push(values[written]))
            ++written;
        return written;
    }

    /// Consumer side. Returns false when empty.
    bool pop(T& out) noexcept
    {
        const std::size_t r = read_index_.load(std::memory_order_relaxed);
        if (r == write_index_.load(std::memory_order_acquire))
            return false;
        out = slots_[r];
        read_index_.store((r + 1) & mask_, std::memory_order_release);
        return true;
    }

    /// Pops up to `count` elements into `out`; returns how many were read.
    std::size_t pop_bulk(T* out, std::size_t count) noexcept
    {
        std::size_t read = 0;
        while (read < count && pop(out[read]))
            ++read;
        return read;
    }

    /// Discards everything readable. Consumer side only.
    void clear() noexcept
    {
        read_index_.store(write_index_.load(std::memory_order_acquire), std::memory_order_release);
    }

    /// Total elements rejected because the queue was full, for the
    /// diagnostics panel (spec 9) and the back-pressure policy (spec 6).
    std::uint64_t dropped_count() const noexcept
    {
        return dropped_.load(std::memory_order_relaxed);
    }

    void reset_dropped_count() noexcept { dropped_.store(0, std::memory_order_relaxed); }

private:
    static std::size_t round_up_pow2(std::size_t v) noexcept
    {
        std::size_t p = 2;
        while (p < v)
            p <<= 1;
        return p;
    }

    const std::size_t slot_count_;
    const std::size_t mask_;
    std::unique_ptr<T[]> slots_;

    // Kept on separate cache lines: the producer writes one index on the
    // audio thread while the consumer writes the other on the I/O thread.
    alignas(64) std::atomic<std::size_t> write_index_{0};
    alignas(64) std::atomic<std::size_t> read_index_{0};
    alignas(64) std::atomic<std::uint64_t> dropped_{0};
};

}  // namespace vcc::audio
