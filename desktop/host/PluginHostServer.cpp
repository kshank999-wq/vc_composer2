// vcc_plugin_host: runs plug-in instances for VC Composer 2 outside the app
// (Main Spec §20), so a plug-in that crashes, hangs or corrupts memory takes
// only this process down. How many instances share one process is the app's
// hosting mode: one per instance (safest), one per plug-in, one per
// manufacturer, or all together (lightest).
//
// Each instance has its own shared-memory block and its own audio thread,
// so instances in one process process in parallel exactly as they would in
// separate ones. Threads: the connection thread receives control messages
// and hands them to the message thread (plug-ins expect their loading,
// state and editor calls there); each instance's audio thread waits on its
// block, runs it and signals it done. A control call that changes an
// instance's processing (prepare, state) holds that instance's `lock`, and a
// block arriving meanwhile is answered without processing (`busy`).
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <array>
#include <cstdlib>
#include <map>

#if JUCE_LINUX || JUCE_MAC
#include <unistd.h>
#endif

#include "audio/PluginIpc.h"

namespace {

using vcc::ipc::Message;

/// One hosted plug-in.
class Instance final : private juce::AudioProcessorParameter::Listener, private juce::Thread, private juce::Timer {
public:
    Instance() : juce::Thread("VCC plug-in audio") {}
    ~Instance() override { shut_down(); }

    /// Loads the plug-in (message thread). Fills `out` with the reply.
    void load(juce::AudioPluginFormatManager& formats, juce::MemoryInputStream& in, juce::MemoryOutputStream& out);
    void handle(const juce::String& command, juce::MemoryInputStream& in, juce::MemoryOutputStream& out);
    void shut_down();

private:
    void run() override;
    // Latency can change between blocks (a VST3 plug-in's report reaches its
    // host on the message thread), so it is published on a timer too.
    void timerCallback() override
    {
        if (plugin_ && shared_.get()) shared_.get()->latency.store(plugin_->getLatencySamples(), std::memory_order_relaxed);
    }
    void process_block() noexcept;
    /// Applies the app's queued parameter changes. Called with `lock` held,
    /// by the audio thread before a block and by control calls before they
    /// read or change state, so a change is never left behind a request.
    void apply_pending_params() noexcept;
    void parameterValueChanged(int index, float value) override { queue({index, 0, value}); }
    void parameterGestureChanged(int index, bool starting) override { queue({index, starting ? 1 : 2, 0.0f}); }
    void queue(const vcc::ipc::ParamEvent& e)
    {
        if (applying_) return;  // the change came from the app: no echo
        const juce::SpinLock::ScopedLockType hold(out_lock_);
        if (out_count_ < static_cast<int>(out_.size())) out_[static_cast<size_t>(out_count_++)] = e;
    }

    std::unique_ptr<juce::AudioPluginInstance> plugin_;
    vcc::ipc::SharedBlock shared_;
    std::unique_ptr<juce::DocumentWindow> editor_window_;
    juce::CriticalSection lock_;
    juce::MidiBuffer midi_;
    // Parameter events from the plug-in (its editor, or itself), any thread,
    // handed to the audio thread which alone writes the shared ring.
    juce::SpinLock out_lock_;
    std::array<vcc::ipc::ParamEvent, 1024> out_{};
    int out_count_ = 0;
    static thread_local bool applying_;
    int ins_ = 0, outs_ = 2;
#if JUCE_LINUX || JUCE_MAC
    pid_t parent_ = getppid();
#endif
};

thread_local bool Instance::applying_ = false;

void Instance::shut_down()
{
    stopTimer();
    stopThread(2000);
    editor_window_.reset();
    if (plugin_) {
        for (auto* p : plugin_->getParameters()) p->removeListener(this);
        plugin_->releaseResources();
    }
    plugin_.reset();
}

void Instance::run()
{
    auto& sh = *shared_.get();
    std::uint32_t seen = sh.request.load(std::memory_order_acquire);
    while (!threadShouldExit()) {
        if (!vcc::ipc::wait_for_change(sh.request, seen, 100.0)) {
#if JUCE_LINUX || JUCE_MAC
            // The app is gone (killed, or crashed hard): leave at once
            // rather than wait for the connection's ping timeout.
            if (getppid() != parent_) std::_Exit(0);
#endif
            continue;
        }
        seen = sh.request.load(std::memory_order_acquire);
        process_block();
        sh.done.store(seen, std::memory_order_release);
        vcc::ipc::wake(sh.done);
    }
}

void Instance::apply_pending_params() noexcept
{
    if (!plugin_ || shared_.get() == nullptr) return;
    applying_ = true;
    vcc::ipc::ParamEvent e{};
    const auto& params = plugin_->getParameters();
    while (shared_.get()->to_plugin.pop(e))
        if (e.index >= 0 && e.index < params.size() && e.kind == 0) params[e.index]->setValue(e.value);
    applying_ = false;
}

void Instance::process_block() noexcept
{
    auto& sh = *shared_.get();
    const juce::ScopedTryLock hold(lock_);
    if (!hold.isLocked() || !plugin_) {
        sh.busy.store(1, std::memory_order_relaxed);
        return;
    }
    sh.busy.store(0, std::memory_order_relaxed);
    const int n = juce::jlimit(0, vcc::ipc::kMaxBlock, sh.num_samples.load(std::memory_order_relaxed));
    if (n == 0) return;
    // The app's automation, applied before the block it belongs to.
    apply_pending_params();

    midi_.clear();
    const int events = juce::jlimit(0, vcc::ipc::kMaxMidi, sh.midi_count.load(std::memory_order_relaxed));
    for (int i = 0; i < events; ++i) {
        const auto& m = sh.midi[i];
        if (m.size >= 1 && m.size <= 3) midi_.addEvent(m.bytes, m.size, juce::jlimit(0, n - 1, m.offset));
    }
    const int channels = juce::jmax(ins_, outs_);
    float* ptrs[vcc::ipc::kMaxChannels];
    for (int ch = 0; ch < channels; ++ch) ptrs[ch] = sh.audio[ch];
    juce::AudioBuffer<float> view(ptrs, channels, n);
    for (int ch = ins_; ch < channels; ++ch) view.clear(ch, 0, n);
    if (sh.bypassed.load(std::memory_order_relaxed)) plugin_->processBlockBypassed(view, midi_);
    else plugin_->processBlock(view, midi_);
    sh.latency.store(plugin_->getLatencySamples(), std::memory_order_relaxed);

    const juce::SpinLock::ScopedLockType out_hold(out_lock_);
    int sent = 0;
    while (sent < out_count_ && sh.to_host.push(out_[static_cast<size_t>(sent)])) ++sent;
    if (sent > 0) {
        std::copy(out_.begin() + sent, out_.begin() + out_count_, out_.begin());
        out_count_ -= sent;
    }
}

void Instance::load(juce::AudioPluginFormatManager& formats, juce::MemoryInputStream& in, juce::MemoryOutputStream& out)
{
    const auto description_xml = in.readString();
    const double sr = in.readDouble();
    const int block = in.readInt();
    const auto shm_path = in.readString();
    const auto state = in.readString();

    auto fail = [&](const juce::String& why) {
        out.writeBool(false);
        out.writeString(why);
    };
    juce::PluginDescription description;
    auto xml = juce::parseXML(description_xml);
    if (!xml || !description.loadFromXml(*xml)) return fail("bad plug-in description");
    if (!shared_.open(juce::File(shm_path), false)) return fail("could not open the shared audio block");
    juce::String error;
    plugin_ = formats.createPluginInstance(description, sr, block, error);
    if (!plugin_) return fail(error.isEmpty() ? juce::String("the plug-in could not be created") : error);
    plugin_->enableAllBuses();
    if (state.isNotEmpty()) {
        juce::MemoryBlock data;
        if (data.fromBase64Encoding(state) && data.getSize() > 0)
            plugin_->setStateInformation(data.getData(), static_cast<int>(data.getSize()));
    }
    ins_ = juce::jmin(vcc::ipc::kMaxChannels, plugin_->getTotalNumInputChannels());
    outs_ = juce::jmin(vcc::ipc::kMaxChannels, plugin_->getTotalNumOutputChannels());
    plugin_->setRateAndBufferSizeDetails(sr, block);
    plugin_->prepareToPlay(sr, block);
    for (auto* p : plugin_->getParameters()) p->addListener(this);
    shared_.get()->latency.store(plugin_->getLatencySamples());
    startThread(juce::Thread::Priority::highest);
    startTimerHz(30);

    out.writeBool(true);
    out.writeString(plugin_->getName());
    out.writeInt(ins_);
    out.writeInt(outs_);
    out.writeInt(plugin_->getLatencySamples());
    out.writeBool(plugin_->acceptsMidi());
    out.writeBool(plugin_->hasEditor());
    out.writeDouble(plugin_->getTailLengthSeconds());
    const auto& params = plugin_->getParameters();
    out.writeInt(params.size());
    for (auto* p : params) {
        out.writeString(p->getName(64));
        out.writeString(p->getLabel());
        out.writeFloat(p->getDefaultValue());
        out.writeFloat(p->getValue());
        out.writeInt(p->getNumSteps());
        out.writeBool(p->isDiscrete());
        out.writeBool(p->isBoolean());
        out.writeBool(p->isAutomatable());
        juce::String id = juce::String(p->getParameterIndex());
        if (auto* hp = dynamic_cast<juce::HostedAudioProcessorParameter*>(p)) id = hp->getParameterID();
        out.writeString(id);
    }
}

void Instance::handle(const juce::String& command, juce::MemoryInputStream& in, juce::MemoryOutputStream& out)
{
    if (!plugin_) {
        out.writeBool(false);
        return;
    }
    if (command == "prepare") {
        const double sr = in.readDouble();
        const int block = in.readInt();
        const juce::ScopedLock hold(lock_);
        apply_pending_params();
        plugin_->releaseResources();
        plugin_->setRateAndBufferSizeDetails(sr, block);
        plugin_->prepareToPlay(sr, block);
        shared_.get()->latency.store(plugin_->getLatencySamples());
        out.writeBool(true);
    } else if (command == "release") {
        const juce::ScopedLock hold(lock_);
        plugin_->releaseResources();
        out.writeBool(true);
    } else if (command == "getState") {
        juce::MemoryBlock data;
        {
            const juce::ScopedLock hold(lock_);
            apply_pending_params();
            plugin_->getStateInformation(data);
        }
        out.writeBool(true);
        out.writeString(data.toBase64Encoding());
    } else if (command == "setState") {
        juce::MemoryBlock data;
        if (data.fromBase64Encoding(in.readString()) && data.getSize() > 0) {
            const juce::ScopedLock hold(lock_);
            apply_pending_params();
            plugin_->setStateInformation(data.getData(), static_cast<int>(data.getSize()));
        }
        // The parameters after the new state, so the app's copies agree.
        out.writeBool(true);
        const auto& params = plugin_->getParameters();
        out.writeInt(params.size());
        for (auto* p : params) out.writeFloat(p->getValue());
        shared_.get()->latency.store(plugin_->getLatencySamples());
    } else if (command == "editor") {
        const bool show = in.readBool();
        if (show) {
            if (!editor_window_) {
                auto* editor = plugin_->hasEditor() ? plugin_->createEditorIfNeeded() : nullptr;
                if (editor == nullptr) editor = new juce::GenericAudioProcessorEditor(*plugin_);
                class Window final : public juce::DocumentWindow {
                public:
                    explicit Window(const juce::String& name)
                        : juce::DocumentWindow(name, juce::Colours::black, juce::DocumentWindow::closeButton)
                    {
                    }
                    void closeButtonPressed() override { setVisible(false); }
                };
                editor_window_ = std::make_unique<Window>(plugin_->getName() + " (outside VC Composer)");
                editor_window_->setUsingNativeTitleBar(true);
                editor_window_->setContentOwned(editor, true);
                editor_window_->setResizable(editor->isResizable(), false);
                editor_window_->centreWithSize(editor_window_->getWidth(), editor_window_->getHeight());
            }
            editor_window_->setVisible(true);
            editor_window_->toFront(true);
        } else if (editor_window_) {
            editor_window_->setVisible(false);
        }
        out.writeBool(true);
    } else {
        out.writeBool(false);
    }
}

/// The process: a registry of instances behind one connection.
class Server final : public juce::ChildProcessWorker {
public:
    Server() { formats_.addDefaultFormats(); }

    void handleMessageFromCoordinator(const juce::MemoryBlock& block) override
    {
        Message m;
        if (!Message::decode(block, m)) return;
        // Plug-ins expect the message thread for everything but audio.
        juce::MessageManager::callAsync([this, m] { handle(m); });
    }

    void handleConnectionLost() override
    {
        // The app quit or crashed: nothing to save, leave at once (some
        // plug-ins misbehave in their destructors; this process is spent).
        std::_Exit(0);
    }

    void shut_down() { instances_.clear(); }

private:
    void handle(const Message& m)
    {
        juce::MemoryInputStream in(m.payload, false);
        juce::MemoryOutputStream out;
        if (m.command == "load") {
            auto instance = std::make_unique<Instance>();
            instance->load(formats_, in, out);
            instances_[m.instance] = std::move(instance);
        } else if (m.command == "unload") {
            instances_.erase(m.instance);
            out.writeBool(true);
        } else if (auto it = instances_.find(m.instance); it != instances_.end()) {
            it->second->handle(m.command, in, out);
        } else {
            out.writeBool(false);
        }
        Message r;
        r.id = m.id;
        r.instance = m.instance;
        r.command = m.command;
        r.payload = out.getMemoryBlock();
        sendMessageToCoordinator(r.encode());
    }

    juce::AudioPluginFormatManager formats_;
    std::map<int, std::unique_ptr<Instance>> instances_;
};

}  // namespace

int main(int argc, char* argv[])
{
    juce::StringArray args;
    for (int i = 1; i < argc; ++i) args.add(juce::String::fromUTF8(argv[i]));
    juce::ScopedJuceInitialiser_GUI juce_runtime;
    Server server;
    // The app pings; without a ping for 8 s this process assumes the app has
    // gone and leaves.
    if (!server.initialiseFromCommandLine(args.joinIntoString(" "), vcc::ipc::kProcessId, 8000)) return 1;
    juce::MessageManager::getInstance()->runDispatchLoop();
    server.shut_down();
    return 0;
}
