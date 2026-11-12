// What VC Composer 2 and its plug-in host process (vcc_plugin_host) share.
//
// Control messages (load, prepare, state, editor) travel over JUCE's
// child-process connection. Audio does not: each hosted instance has a
// shared-memory block holding one audio block, its MIDI, two lock-free
// parameter rings (host -> plug-in for automation, plug-in -> host for the
// plug-in's own editor), and a request/done sequence pair the two sides wait
// on. On Linux the waits are futexes on the shared words, so a round trip
// costs microseconds; elsewhere they spin briefly and then sleep in short
// steps.
//
// Everything here is plain data in a mapped file: no pointers, no
// constructors that matter (zeroed memory is the initial state).
#pragma once

#include <atomic>
#include <cstdint>

#include <juce_core/juce_core.h>

namespace vcc::ipc {

constexpr std::uint32_t kMagic = 0x56434332;  // "VCC2"
constexpr std::uint32_t kVersion = 2;
constexpr int kMaxChannels = 16;
constexpr int kMaxBlock = 8192;
constexpr int kMaxMidi = 4096;
constexpr int kRingSize = 2048;
/// The command-line token both sides agree on (JUCE child-process handshake).
inline const char* const kProcessId = "vcc-plugin-host-2";

struct MidiEvent {
    std::int32_t offset;
    std::uint8_t size;
    std::uint8_t bytes[3];
};

struct ParamEvent {
    std::int32_t index;
    std::int32_t kind;  ///< 0 value, 1 gesture begins, 2 gesture ends
    float value;
};

/// Single-producer, single-consumer ring of parameter events.
struct Ring {
    std::atomic<std::uint32_t> write;
    std::atomic<std::uint32_t> read;
    ParamEvent events[kRingSize];

    bool push(const ParamEvent& e) noexcept
    {
        const auto w = write.load(std::memory_order_relaxed);
        if (w - read.load(std::memory_order_acquire) >= static_cast<std::uint32_t>(kRingSize)) return false;
        events[w % kRingSize] = e;
        write.store(w + 1, std::memory_order_release);
        return true;
    }
    bool pop(ParamEvent& e) noexcept
    {
        const auto r = read.load(std::memory_order_relaxed);
        if (r == write.load(std::memory_order_acquire)) return false;
        e = events[r % kRingSize];
        read.store(r + 1, std::memory_order_release);
        return true;
    }
};

struct Shared {
    std::atomic<std::uint32_t> magic;
    std::atomic<std::uint32_t> version;
    std::atomic<std::uint32_t> request;  ///< Host bumps it to ask for a block.
    std::atomic<std::uint32_t> done;     ///< The plug-in process sets it to `request` when finished.
    std::atomic<std::int32_t> num_samples;
    std::atomic<std::int32_t> bypassed;
    std::atomic<std::int32_t> latency;   ///< The plug-in's reported latency, kept current.
    std::atomic<std::int32_t> busy;      ///< Prepare/state change in progress: the block was skipped.
    std::atomic<std::int32_t> midi_count;
    MidiEvent midi[kMaxMidi];
    Ring to_plugin;
    Ring to_host;
    float audio[kMaxChannels][kMaxBlock];
};

static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "shared-memory atomics must be lock free");

/// Waits until `word` is no longer `seen`, or `timeout_ms` passes (< 0:
/// forever). Returns true when it changed.
bool wait_for_change(std::atomic<std::uint32_t>& word, std::uint32_t seen, double timeout_ms) noexcept;
/// Wakes whoever waits on `word` (after it has been changed).
void wake(std::atomic<std::uint32_t>& word) noexcept;

/// A shared-memory block backed by a file both processes map.
class SharedBlock {
public:
    /// Creates (host) or opens (plug-in process) the block at `file`.
    bool open(const juce::File& file, bool create);
    Shared* get() const noexcept { return shared_; }
    const juce::File& file() const noexcept { return file_; }

private:
    juce::File file_;
    std::unique_ptr<juce::MemoryMappedFile> map_;
    Shared* shared_ = nullptr;
};

/// A message: request id, the instance it is for, command, then the
/// command's fields. One host process can carry many instances.
struct Message {
    std::int32_t id = 0;
    std::int32_t instance = 0;
    juce::String command;
    juce::MemoryBlock payload;  ///< The fields, written with MemoryOutputStream.

    juce::MemoryBlock encode() const;
    static bool decode(const juce::MemoryBlock& block, Message& out);
};

}  // namespace vcc::ipc
