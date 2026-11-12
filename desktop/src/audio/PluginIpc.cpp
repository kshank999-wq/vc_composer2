#include "audio/PluginIpc.h"

#include <algorithm>
#include <chrono>
#include <thread>

#if JUCE_LINUX
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <climits>
#include <ctime>
#endif

namespace vcc::ipc {

namespace {
std::uint32_t* raw(std::atomic<std::uint32_t>& word) noexcept { return reinterpret_cast<std::uint32_t*>(&word); }
}  // namespace

bool wait_for_change(std::atomic<std::uint32_t>& word, std::uint32_t seen, double timeout_ms) noexcept
{
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
#if JUCE_LINUX
        timespec ts;
        const double wait = std::min(left, 100.0);
        ts.tv_sec = static_cast<time_t>(wait / 1000.0);
        ts.tv_nsec = static_cast<long>((wait - static_cast<double>(ts.tv_sec) * 1000.0) * 1.0e6);
        // Not FUTEX_PRIVATE: the word lives in memory shared with another process.
        syscall(SYS_futex, raw(word), FUTEX_WAIT, seen, &ts, nullptr, 0);
#else
        if (elapsed_ms() < 2.0) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::microseconds(100));
#endif
    }
}

void wake(std::atomic<std::uint32_t>& word) noexcept
{
#if JUCE_LINUX
    syscall(SYS_futex, raw(word), FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
#else
    juce::ignoreUnused(word);
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
    out.writeString(command);
    out.writeInt64(static_cast<juce::int64>(payload.getSize()));
    out.write(payload.getData(), payload.getSize());
    return out.getMemoryBlock();
}

bool Message::decode(const juce::MemoryBlock& block, Message& out)
{
    juce::MemoryInputStream in(block, false);
    out.id = in.readInt();
    out.command = in.readString();
    const auto size = in.readInt64();
    if (size < 0 || size > in.getNumBytesRemaining()) return false;
    out.payload.setSize(static_cast<size_t>(size));
    return in.read(out.payload.getData(), static_cast<int>(size)) == static_cast<int>(size);
}

}  // namespace vcc::ipc
