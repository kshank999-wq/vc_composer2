#include "system/ProcessStats.h"

#include <cstddef>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#else
#include <unistd.h>
#include <cstdio>
#include <cstring>
#endif

namespace vcc::desktop::stats {

std::uint64_t process_memory_bytes() noexcept
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc))
        return static_cast<std::uint64_t>(pmc.WorkingSetSize);
    return 0;
#elif defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return static_cast<std::uint64_t>(info.phys_footprint);
    return 0;
#else
    long pages = 0, resident = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        std::fclose(f);
    }
    return static_cast<std::uint64_t>(resident) * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
#endif
}

SystemMemory system_memory() noexcept
{
    SystemMemory m;
#if defined(_WIN32)
    MEMORYSTATUSEX s{};
    s.dwLength = sizeof s;
    if (GlobalMemoryStatusEx(&s)) {
        m.total = s.ullTotalPhys;
        m.available = s.ullAvailPhys;
    }
#elif defined(__APPLE__)
    std::uint64_t total = 0;
    std::size_t len = sizeof total;
    if (sysctlbyname("hw.memsize", &total, &len, nullptr, 0) == 0) m.total = total;
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page = 0;
    host_page_size(mach_host_self(), &page);
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS)
        // Free, plus what the system gives back on demand (inactive and purgeable pages).
        m.available = (static_cast<std::uint64_t>(vm.free_count) + vm.inactive_count + vm.purgeable_count) * page;
#else
    if (FILE* f = std::fopen("/proc/meminfo", "r")) {
        char key[64];
        unsigned long long kb = 0;
        char unit[16];
        while (std::fscanf(f, "%63s %llu %15s", key, &kb, unit) == 3) {
            if (std::strcmp(key, "MemTotal:") == 0) m.total = kb * 1024ull;
            if (std::strcmp(key, "MemAvailable:") == 0) m.available = kb * 1024ull;
        }
        std::fclose(f);
    }
#endif
    return m;
}

}  // namespace vcc::desktop::stats
