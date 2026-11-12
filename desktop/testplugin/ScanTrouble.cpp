// Test plug-ins that misbehave as soon as they are loaded, for the check of
// out-of-process scanning. Built twice: VCC_SCAN_TROUBLE 1 crashes the
// process when the module loads, 2 hangs it. They are never installed and
// are only ever scanned by the headless check.
#include <chrono>
#include <csignal>
#include <thread>

namespace {
struct Trouble {
    Trouble()
    {
#if VCC_SCAN_TROUBLE == 1
        std::raise(SIGSEGV);
#elif VCC_SCAN_TROUBLE == 2
        std::this_thread::sleep_for(std::chrono::minutes(10));
#endif
    }
};
const Trouble trouble;
}  // namespace
