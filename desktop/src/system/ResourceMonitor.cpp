#include "system/ResourceMonitor.h"

namespace vcc::desktop {

ResourceMonitor::ResourceMonitor(PluginHost& plugins, int interval_ms)
    : juce::Thread("VC Composer resource monitor"), plugins_(plugins), interval_ms_(juce::jlimit(250, 10000, interval_ms))
{
    startThread(juce::Thread::Priority::low);
}

ResourceMonitor::~ResourceMonitor() { stopThread(3000); }

ResourceMonitor::Snapshot ResourceMonitor::latest() const
{
    const juce::ScopedLock sl(lock_);
    return latest_;
}

void ResourceMonitor::sample_now()
{
    Snapshot s;
    s.app_bytes = stats::process_memory_bytes();
    s.system = stats::system_memory();
    s.hosts = plugins_.host_memory();
    // A host that did not answer this time keeps its last reading, so the
    // total does not flicker while a plug-in is busy loading.
    {
        const juce::ScopedLock sl(lock_);
        for (auto& h : s.hosts)
            if (h.bytes == 0)
                for (const auto& before : latest_.hosts)
                    if (before.label == h.label) h.bytes = before.bytes;
    }
    s.taken = juce::Time::getCurrentTime();
    const juce::ScopedLock sl(lock_);
    latest_ = std::move(s);
}

void ResourceMonitor::run()
{
    while (!threadShouldExit()) {
        sample_now();
        wait(interval_ms_.load());
    }
}

juce::String format_bytes(std::uint64_t bytes)
{
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (mb >= 10240.0) return juce::String(juce::roundToInt(mb / 1024.0)) + " GB";
    if (mb >= 1000.0) return juce::String(mb / 1024.0, 1) + " GB";
    return juce::String(juce::roundToInt(mb)) + " MB";
}

}  // namespace vcc::desktop
