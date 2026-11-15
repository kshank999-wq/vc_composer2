// A refresh that happens once, on the next pass of the message loop, however
// many times it was asked for: a view that rebuilds on every document change
// (the mixer deck, the track list, the rulers) is asked by each edit, and a
// note entered must not wait for all of them, nor a burst of edits pay for
// each (Main Spec §19 phase 5: a 48-track project's views took 48 ms per
// keystroke rebuilding what a note edit had not changed).
#pragma once

#include <functional>

#include <juce_events/juce_events.h>

namespace vcc::desktop {

class Deferred final : private juce::AsyncUpdater {
public:
    explicit Deferred(std::function<void()> refresh) : refresh_(std::move(refresh)) {}
    ~Deferred() override { cancelPendingUpdate(); }

    /// Refresh on the next pass of the message loop (once).
    void request() { triggerAsyncUpdate(); }
    /// Refresh now if one is pending (before work that needs it current).
    void flush() { handleUpdateNowIfNeeded(); }
    bool pending() const noexcept { return isUpdatePending(); }

private:
    void handleAsyncUpdate() override { refresh_(); }
    std::function<void()> refresh_;
};

}  // namespace vcc::desktop
