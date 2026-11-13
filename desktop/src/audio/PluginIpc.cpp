#include "audio/PluginIpc.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#if JUCE_LINUX
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <climits>
#include <ctime>
#elif JUCE_MAC
#include <cerrno>
#if __has_include(<os/os_sync_wait_on_address.h>)
#include <os/clock.h>
#include <os/os_sync_wait_on_address.h>
#define VCC_HAVE_OS_SYNC 1
#endif
// The calls os_sync_wait_on_address is built on, in libSystem since macOS
// 10.12: the fallback for systems older than 14.4. Weakly linked, so a
// system without them falls back to polling instead of failing to start.
extern "C" int __ulock_wait(std::uint32_t operation, void* addr, std::uint64_t value, std::uint32_t timeout_us)
    __attribute__((weak_import));
extern "C" int __ulock_wake(std::uint32_t operation, void* addr, std::uint64_t wake_value) __attribute__((weak_import));
#elif JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace vcc::ipc {

namespace {

std::uint32_t* raw(std::atomic<std::uint32_t>& word) noexcept { return reinterpret_cast<std::uint32_t*>(&word); }

#if JUCE_MAC
constexpr std::uint32_t kUlCompareAndWaitShared = 3;  // UL_COMPARE_AND_WAIT_SHARED
constexpr std::uint32_t kUlfWakeAll = 0x100;          // ULF_WAKE_ALL

bool has_os_sync() noexcept
{
#if VCC_HAVE_OS_SYNC
    if (__builtin_available(macOS 14.4, *)) return true;
#endif
    return false;
}

bool has_ulock() noexcept { return __ulock_wait != nullptr && __ulock_wake != nullptr; }
#endif

#if JUCE_WINDOWS
/// The name both processes give a block's event, from the block's file.
std::wstring event_name(const juce::File& file, const char* which)
{
    return (juce::String("Local\\vcc-ipc-") + file.getFileName() + "-" + which).toWideCharPointer();
}
#endif

/// Blocks the calling thread until `word` may differ from `seen`, for at
/// most `ms` (spurious and early returns are fine: the caller re-checks).
void block_on(std::atomic<std::uint32_t>& word, std::uint32_t seen, double ms, void* event) noexcept
{
    juce::ignoreUnused(word, seen, ms, event);
#if JUCE_LINUX
    timespec ts;
    ts.tv_sec = static_cast<time_t>(ms / 1000.0);
    ts.tv_nsec = static_cast<long>((ms - static_cast<double>(ts.tv_sec) * 1000.0) * 1.0e6);
    // Not FUTEX_PRIVATE: the word lives in memory shared with another process.
    syscall(SYS_futex, raw(word), FUTEX_WAIT, seen, &ts, nullptr, 0);
#elif JUCE_MAC
#if VCC_HAVE_OS_SYNC
    if (__builtin_available(macOS 14.4, *)) {
        os_sync_wait_on_address_with_timeout(raw(word), seen, sizeof(std::uint32_t), OS_SYNC_WAIT_ON_ADDRESS_SHARED,
                                             OS_CLOCK_MACH_ABSOLUTE_TIME,
                                             static_cast<std::uint64_t>(ms * 1.0e6));
        return;
    }
#endif
    if (has_ulock()) {
        // A timeout of 0 would mean "forever": always at least a microsecond.
        __ulock_wait(kUlCompareAndWaitShared, raw(word), seen,
                     static_cast<std::uint32_t>(std::max(1.0, ms * 1000.0)));
        return;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
#elif JUCE_WINDOWS
    if (event != nullptr) {
        WaitForSingleObject(static_cast<HANDLE>(event), static_cast<DWORD>(std::max(1.0, std::ceil(ms))));
        return;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
#else
    std::this_thread::sleep_for(std::chrono::microseconds(100));
#endif
}

}  // namespace

const char* wait_mechanism() noexcept
{
#if JUCE_LINUX
    return "futex";
#elif JUCE_MAC
    if (has_os_sync()) return "os_sync";
    return has_ulock() ? "ulock" : "polling";
#elif JUCE_WINDOWS
    return "events";
#else
    return "polling";
#endif
}

bool SharedBlock::wait(Word which, std::uint32_t seen, double timeout_ms) noexcept
{
    auto& word = word_ref(which);
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    auto elapsed_ms = [&] { return std::chrono::duration<double, std::milli>(clock::now() - start).count(); };
    // A short spin first: a plug-in block is often done in microseconds.
    for (int i = 0; i < 200; ++i)
        if (word.load(std::memory_order_acquire) != seen) return true;
    for (;;) {
        if (word.load(std::memory_order_acquire) != seen) return true;
        const double left = timeout_ms < 0.0 ? 100.0 : timeout_ms - elapsed_ms();
        if (left <= 0.0) return false;
        block_on(word, seen, std::min(left, 100.0), events_[which == Word::request ? 0 : 1]);
    }
}

void SharedBlock::wake(Word which) noexcept
{
    auto& word = word_ref(which);
    juce::ignoreUnused(word);
#if JUCE_LINUX
    syscall(SYS_futex, raw(word), FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
#elif JUCE_MAC
#if VCC_HAVE_OS_SYNC
    if (__builtin_available(macOS 14.4, *)) {
        os_sync_wake_by_address_all(raw(word), sizeof(std::uint32_t), OS_SYNC_WAKE_BY_ADDRESS_SHARED);
        return;
    }
#endif
    if (has_ulock()) __ulock_wake(kUlCompareAndWaitShared | kUlfWakeAll, raw(word), 0);
#elif JUCE_WINDOWS
    // Auto-reset: a wake that lands before the other side waits stays set,
    // so it is never lost; a stale one costs one extra re-check.
    if (auto* e = events_[which == Word::request ? 0 : 1]) SetEvent(static_cast<HANDLE>(e));
#endif
}

SharedBlock::~SharedBlock()
{
#if JUCE_WINDOWS
    for (auto*& e : events_)
        if (e != nullptr) {
            CloseHandle(static_cast<HANDLE>(e));
            e = nullptr;
        }
#endif
}

bool SharedBlock::open(const juce::File& file, bool create)
{
    file_ = file;
    const auto size = static_cast<juce::int64>(sizeof(Shared));
    if (create) {
        file.deleteFile();
        juce::FileOutputStream out(file);
        if (!out.openedOk()) return false;
        // Zeroed memory is the initial state of every field.
        juce::HeapBlock<char> zeros(1 << 16, true);
        for (juce::int64 written = 0; written < size;) {
            const auto n = static_cast<int>(std::min<juce::int64>(1 << 16, size - written));
            if (!out.write(zeros.get(), static_cast<size_t>(n))) return false;
            written += n;
        }
        out.flush();
    }
    if (file.getSize() < size) return false;
    map_ = std::make_unique<juce::MemoryMappedFile>(file, juce::Range<juce::int64>(0, size),
                                                    juce::MemoryMappedFile::readWrite, false);
    if (map_->getData() == nullptr || static_cast<juce::int64>(map_->getSize()) < size) return false;
    shared_ = static_cast<Shared*>(map_->getData());
#if JUCE_WINDOWS
    // The events both sides wait on, named after the block. The host
    // creates them; the plug-in process opens the same ones.
    const char* names[2] = {"request", "done"};
    for (int i = 0; i < 2; ++i) {
        const auto name = event_name(file, names[i]);
        events_[i] = create ? CreateEventW(nullptr, FALSE, FALSE, name.c_str())
                            : OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name.c_str());
        if (events_[i] == nullptr) return false;
    }
#endif
    if (create) {
        shared_->version.store(kVersion);
        shared_->magic.store(kMagic, std::memory_order_release);
    }
    return shared_->magic.load(std::memory_order_acquire) == kMagic && shared_->version.load() == kVersion;
}

juce::MemoryBlock Message::encode() const
{
    juce::MemoryOutputStream out;
    out.writeInt(id);
    out.writeInt(instance);
    out.writeString(command);
    out.writeInt64(static_cast<juce::int64>(payload.getSize()));
    out.write(payload.getData(), payload.getSize());
    return out.getMemoryBlock();
}

bool Message::decode(const juce::MemoryBlock& block, Message& out)
{
    juce::MemoryInputStream in(block, false);
    out.id = in.readInt();
    out.instance = in.readInt();
    out.command = in.readString();
    const auto size = in.readInt64();
    if (size < 0 || size > in.getNumBytesRemaining()) return false;
    out.payload.setSize(static_cast<size_t>(size));
    return in.read(out.payload.getData(), static_cast<int>(size)) == static_cast<int>(size);
}

}  // namespace vcc::ipc
