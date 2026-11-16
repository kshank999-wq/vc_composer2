// Memory readings for the resource monitor (Memory Management spec §6):
// this process's resident memory, and the machine's physical memory. Plain
// OS calls, cheap enough to sample every second; used by the app and by the
// plug-in host processes (each reports itself).
#pragma once

#include <cstdint>

namespace vcc::desktop::stats {

/// Physical memory this process is using now, in bytes (Windows: working
/// set; macOS: physical footprint, as Activity Monitor shows; Linux: resident
/// set). 0 when the OS does not say.
std::uint64_t process_memory_bytes() noexcept;

struct SystemMemory {
    std::uint64_t total = 0;      ///< Installed physical memory.
    std::uint64_t available = 0;  ///< What new allocations can have without swapping.
    std::uint64_t used() const noexcept { return total > available ? total - available : 0; }
};
SystemMemory system_memory() noexcept;

}  // namespace vcc::desktop::stats
