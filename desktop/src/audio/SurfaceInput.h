// The MIDI side of a control surface (Add-On Features spec §7): the
// engine's MIDI input callback offers every message here first. Messages
// from the surface's device that its profile maps (or, while learning, any
// control from that device) are taken off the instrument path - a jog wheel
// must not play notes or land in a recording - and queued for the message
// thread. Everything else goes on to play as before.
//
// take() runs on the MIDI thread; configure() and drain() on the message
// thread. The queue is lock-free; the configuration is swapped under a
// spin lock held only for a copy.
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <functional>

#include "vcc/project/ControlSurface.h"

namespace vcc::desktop {

class SurfaceInput {
public:
    struct Raw {
        std::uint8_t status = 0, data1 = 0, data2 = 0;
    };

    /// The device the surface listens to (a MIDI input identifier; empty
    /// for none), its profile, and whether it is learning a control.
    void configure(const juce::String& device_id, const project::surface::Profile& profile, bool learning)
    {
        const juce::SpinLock::ScopedLockType lock(config_lock_);
        device_id_ = device_id;
        profile_ = profile;
        learning_ = learning;
    }

    /// MIDI thread: true when the message is the surface's (then it is
    /// queued and must not play).
    bool take(const juce::String& device_id, const juce::MidiMessage& m)
    {
        if (m.getRawDataSize() < 2 || m.getRawDataSize() > 3) return false;
        const auto* d = m.getRawData();
        const Raw raw{d[0], d[1], m.getRawDataSize() > 2 ? d[2] : std::uint8_t(0)};
        {
            const juce::SpinLock::ScopedTryLockType lock(config_lock_);
            if (!lock.isLocked()) return false;  // being reconfigured: let it play
            if (device_id_.isEmpty() || device_id != device_id_) return false;
            const bool mine = learning_ ? project::surface::source_of(raw.status, raw.data1, raw.data2).has_value() ||
                                              (raw.status & 0xF0) == 0x80
                                        : project::surface::claims(profile_, raw.status, raw.data1, raw.data2);
            if (!mine) return false;
        }
        int start1, size1, start2, size2;
        fifo_.prepareToWrite(1, start1, size1, start2, size2);
        if (size1 > 0) buffer_[static_cast<std::size_t>(start1)] = raw;
        fifo_.finishedWrite(size1 + size2);
        received_.fetch_add(1, std::memory_order_relaxed);
        return true;  // taken even when the queue is full: never play it
    }

    /// Message thread: every queued message, oldest first.
    void drain(const std::function<void(const Raw&)>& f)
    {
        int start1, size1, start2, size2;
        fifo_.prepareToRead(fifo_.getNumReady(), start1, size1, start2, size2);
        for (int i = 0; i < size1; ++i) f(buffer_[static_cast<std::size_t>(start1 + i)]);
        for (int i = 0; i < size2; ++i) f(buffer_[static_cast<std::size_t>(start2 + i)]);
        fifo_.finishedRead(size1 + size2);
    }

    /// Messages taken so far (an activity light).
    int received() const noexcept { return received_.load(std::memory_order_relaxed); }

private:
    juce::SpinLock config_lock_;
    juce::String device_id_;
    project::surface::Profile profile_;
    bool learning_ = false;
    static constexpr int kSize = 512;
    juce::AbstractFifo fifo_{kSize};
    std::array<Raw, kSize> buffer_{};
    std::atomic<int> received_{0};
};

}  // namespace vcc::desktop
