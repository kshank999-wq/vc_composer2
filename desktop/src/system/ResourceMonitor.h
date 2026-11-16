// The resource monitor behind the status strip's CPU and RAM readout
// (Memory Management spec §6): what VC Composer is using - this process and
// every plug-in host process - and the machine's physical memory, sampled on
// its own thread about once a second (the plug-in hosts answer a short
// "memory" call each), so neither the audio nor the message thread waits.
//
// Plug-ins run in host processes, so memory is measured per process, never
// guessed from file sizes: an instrument alone in its process is measured
// exactly; plug-ins sharing a process are reported together.
#pragma once

#include <juce_core/juce_core.h>

#include "audio/PluginHost.h"
#include "system/ProcessStats.h"

namespace vcc::desktop {

class ResourceMonitor final : private juce::Thread {
public:
    struct Snapshot {
        std::uint64_t app_bytes = 0;  ///< This process (the app and its in-app plug-ins).
        std::vector<PluginHost::HostMemory> hosts;
        stats::SystemMemory system;
        juce::Time taken;
        /// The app plus every host process.
        std::uint64_t total_bytes() const noexcept
        {
            std::uint64_t t = app_bytes;
            for (const auto& h : hosts) t += h.bytes;
            return t;
        }
        int plugin_count() const noexcept
        {
            int n = 0;
            for (const auto& h : hosts) n += h.instances;
            return n;
        }
    };

    explicit ResourceMonitor(PluginHost& plugins, int interval_ms = 1000);
    ~ResourceMonitor() override;

    Snapshot latest() const;
    /// Samples now, on the calling thread (checks; the first reading).
    void sample_now();
    void set_interval(int ms) noexcept { interval_ms_.store(juce::jlimit(250, 10000, ms)); }
    int interval() const noexcept { return interval_ms_.load(); }

private:
    void run() override;

    PluginHost& plugins_;
    std::atomic<int> interval_ms_;
    mutable juce::CriticalSection lock_;
    Snapshot latest_;
};

/// "1.4 GB", "820 MB".
juce::String format_bytes(std::uint64_t bytes);

}  // namespace vcc::desktop
