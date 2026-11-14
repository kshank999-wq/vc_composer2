#include "audio/MidiOut.h"

#include <map>
#include <mutex>

namespace vcc::desktop {

namespace {
std::mutex& virtual_mutex()
{
    static std::mutex m;
    return m;
}
std::map<juce::String, std::shared_ptr<MidiSink>>& virtual_sinks()
{
    static std::map<juce::String, std::shared_ptr<MidiSink>> sinks;
    return sinks;
}

class DeviceSink final : public MidiSink {
public:
    explicit DeviceSink(std::unique_ptr<juce::MidiOutput> out) : out_(std::move(out)) {}
    void send(const juce::MidiMessage& m) override { out_->sendMessageNow(m); }

private:
    std::unique_ptr<juce::MidiOutput> out_;
};
}  // namespace

void MidiOutRouter::register_virtual(const juce::String& name, std::shared_ptr<MidiSink> sink)
{
    std::lock_guard<std::mutex> lock(virtual_mutex());
    virtual_sinks()[name] = std::move(sink);
}

void MidiOutRouter::unregister_virtual(const juce::String& name)
{
    std::lock_guard<std::mutex> lock(virtual_mutex());
    virtual_sinks().erase(name);
}

juce::StringArray MidiOutRouter::device_names()
{
    juce::StringArray names;
    {
        std::lock_guard<std::mutex> lock(virtual_mutex());
        for (const auto& [name, sink] : virtual_sinks()) names.add(name);
    }
    for (const auto& d : juce::MidiOutput::getAvailableDevices()) names.addIfNotAlreadyThere(d.name);
    return names;
}

// The sender thread starts with the first destination, so projects without
// external MIDI (and the many offline engines bounces create) pay nothing.
MidiOutRouter::MidiOutRouter() : juce::Thread("VCC MIDI out") {}

MidiOutRouter::~MidiOutRouter() { stopThread(1000); }

int MidiOutRouter::sink_for(const juce::String& name, bool hardware)
{
    if (name.isEmpty()) return -1;
    for (int i = 0; i < count_; ++i)
        if (names_[static_cast<std::size_t>(i)] == name) return i;
    if (count_ >= kMaxSinks) return -1;
    std::shared_ptr<MidiSink> sink;
    {
        std::lock_guard<std::mutex> lock(virtual_mutex());
        if (auto it = virtual_sinks().find(name); it != virtual_sinks().end()) sink = it->second;
    }
    if (!sink && hardware)
        for (const auto& d : juce::MidiOutput::getAvailableDevices())
            if (d.name == name)
                if (auto out = juce::MidiOutput::openDevice(d.identifier)) sink = std::make_shared<DeviceSink>(std::move(out));
    if (!sink) return -1;
    // Published before any event can name it (the queue orders the two).
    sinks_[static_cast<std::size_t>(count_)] = sink;
    names_[static_cast<std::size_t>(count_)] = name;
    if (!isThreadRunning()) startThread(juce::Thread::Priority::highest);
    return count_++;
}

void MidiOutRouter::push(int sink, const juce::MidiMessage& m, double due_ms) noexcept
{
    if (sink < 0 || sink >= kMaxSinks || m.getRawDataSize() > 3) return;
    Event e;
    e.sink = sink;
    e.size = m.getRawDataSize();
    for (int i = 0; i < e.size; ++i) e.bytes[i] = m.getRawData()[i];
    e.due = due_ms;
    if (!queue_.push(e)) dropped_.fetch_add(1, std::memory_order_relaxed);
}

void MidiOutRouter::run()
{
    Event e;
    while (!threadShouldExit()) {
        if (!queue_.pop(e)) {
            wait(1);
            continue;
        }
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (e.due > now + 0.5) juce::Time::waitForMillisecondCounter(static_cast<juce::uint32>(e.due));
        if (auto& s = sinks_[static_cast<std::size_t>(e.sink)])
            s->send(juce::MidiMessage(e.bytes, e.size, e.due * 0.001));
    }
}

}  // namespace vcc::desktop
