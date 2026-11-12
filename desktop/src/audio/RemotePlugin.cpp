#include "audio/RemotePlugin.h"

#include <algorithm>
#include <cstring>

namespace vcc::desktop {

// --- Host processes ---------------------------------------------------------------------------

struct HostProcess::Pending {
    juce::WaitableEvent done;
    juce::MemoryBlock reply;
    bool ok = false;
};

class HostProcess::Connection final : public juce::ChildProcessCoordinator {
public:
    explicit Connection(HostProcess& owner) : owner_(owner) {}
    ~Connection() override { killWorkerProcess(); }
    void handleMessageFromWorker(const juce::MemoryBlock& block) override { owner_.received(block); }
    void handleConnectionLost() override { owner_.lost(); }

private:
    HostProcess& owner_;
};

std::shared_ptr<HostProcess> HostProcess::start(const juce::File& host_tool, const juce::String& label)
{
    std::shared_ptr<HostProcess> p(new HostProcess());
    p->label_ = label;
    p->connection_ = std::make_unique<Connection>(*p);
    // Output is not captured: a chatty plug-in can never fill a pipe and stall.
    if (!p->connection_->launchWorkerProcess(host_tool, ipc::kProcessId, 8000, 0)) return nullptr;
    return p;
}

HostProcess::~HostProcess() { connection_.reset(); }

bool HostProcess::call(int instance, const juce::String& command, const juce::MemoryBlock& payload,
                       juce::MemoryBlock& reply, int timeout_ms)
{
    if (crashed_ || !connection_) return false;
    auto pending = std::make_shared<Pending>();
    ipc::Message m;
    m.id = next_id_++;
    m.instance = instance;
    m.command = command;
    m.payload = payload;
    {
        std::lock_guard<std::mutex> lock(pending_lock_);
        pending_[m.id] = pending;
    }
    const bool sent = connection_->sendMessageToWorker(m.encode());
    const bool answered = sent && pending->done.wait(timeout_ms);
    {
        std::lock_guard<std::mutex> lock(pending_lock_);
        pending_.erase(m.id);
    }
    if (!answered || !pending->ok) return false;
    reply = pending->reply;
    return true;
}

void HostProcess::received(const juce::MemoryBlock& block)
{
    ipc::Message m;
    if (!ipc::Message::decode(block, m)) return;
    std::lock_guard<std::mutex> lock(pending_lock_);
    if (auto it = pending_.find(m.id); it != pending_.end()) {
        it->second->reply = m.payload;
        it->second->ok = true;
        it->second->done.signal();
    }
}

void HostProcess::lost()
{
    crashed_ = true;
    std::lock_guard<std::mutex> lock(pending_lock_);
    for (auto& [id, p] : pending_) p->done.signal();
}

void HostProcess::mark_crashed()
{
    lost();
    if (connection_) connection_->killWorkerProcess();
}

class RemotePluginInstance::Parameter final : public juce::HostedAudioProcessorParameter {
public:
    Parameter(RemotePluginInstance& owner, int index) : owner_(owner), index_(index) {}

    float getValue() const override { return value.load(std::memory_order_relaxed); }
    void setValue(float v) override
    {
        value.store(v, std::memory_order_relaxed);
        owner_.push_param(index_, v);
    }
    float getDefaultValue() const override { return default_value; }
    juce::String getName(int max) const override { return name.substring(0, max); }
    juce::String getLabel() const override { return label; }
    int getNumSteps() const override { return steps; }
    bool isDiscrete() const override { return discrete; }
    bool isBoolean() const override { return boolean; }
    bool isAutomatable() const override { return automatable; }
    juce::String getText(float v, int max) const override
    {
        if (boolean) return v >= 0.5f ? "On" : "Off";
        return (juce::String(v, 3) + (label.isEmpty() ? "" : " " + label)).substring(0, max);
    }
    float getValueForText(const juce::String& text) const override { return juce::jlimit(0.0f, 1.0f, text.getFloatValue()); }
    juce::String getParameterID() const override { return id; }

    /// A change made in the plug-in's process (its own window).
    void from_plugin(const ipc::ParamEvent& e)
    {
        if (e.kind == 1) beginChangeGesture();
        else if (e.kind == 2) endChangeGesture();
        else {
            value.store(e.value, std::memory_order_relaxed);
            sendValueChangedMessageToListeners(e.value);
        }
    }

    std::atomic<float> value{0.0f};
    float default_value = 0.0f;
    juce::String name, label, id;
    int steps = 0x7fffffff;
    bool discrete = false, boolean = false, automatable = true;

private:
    RemotePluginInstance& owner_;
    int index_;
};

// --- Creation -----------------------------------------------------------------------------------

namespace {

juce::File shared_file()
{
    // /dev/shm keeps the block in memory on Linux; elsewhere the temporary
    // folder's page cache does the same job.
    const juce::File shm("/dev/shm");
    const auto dir = shm.isDirectory() ? shm : juce::File::getSpecialLocation(juce::File::tempDirectory);
    return dir.getChildFile("vcc-plugin-" + juce::Uuid().toString());
}

}  // namespace

RemotePluginInstance::RemotePluginInstance(const BusesProperties& buses, juce::PluginDescription description,
                                           std::shared_ptr<HostProcess> process)
    : juce::AudioPluginInstance(buses), description_(std::move(description)), process_(std::move(process))
{
    instance_ = process_->new_instance_id();
    ++process_->instances_;
}

std::unique_ptr<RemotePluginInstance> RemotePluginInstance::create(std::shared_ptr<HostProcess> process,
                                                                   const juce::PluginDescription& description,
                                                                   const std::string& state_base64,
                                                                   double sample_rate, int block_size,
                                                                   juce::String& error)
{
    if (!process || process->crashed()) {
        error = "the plug-in host process is not running";
        return nullptr;
    }
    std::unique_ptr<RemotePluginInstance> p(new RemotePluginInstance(
        BusesProperties()
            .withInput("Input", juce::AudioChannelSet::stereo(), true)
            .withOutput("Output", juce::AudioChannelSet::stereo(), true),
        description, std::move(process)));
    p->name_ = description.name;
    p->sample_rate_ = sample_rate;
    p->last_state_ = state_base64;
    if (!p->shared_.open(shared_file(), true)) {
        error = "could not create the shared audio block for " + description.name;
        return nullptr;
    }
    juce::MemoryOutputStream out;
    out.writeString(description.createXml()->toString(juce::XmlElement::TextFormat().singleLine()));
    out.writeDouble(sample_rate);
    out.writeInt(block_size);
    out.writeString(p->shared_.file().getFullPathName());
    out.writeString(juce::String(state_base64));
    juce::MemoryBlock reply;
    if (!p->call("load", out.getMemoryBlock(), reply, 60000)) {
        error = p->crashed() ? description.name + " crashed while loading outside VC Composer"
                             : description.name + " did not finish loading within 60 s";
        return nullptr;
    }
    juce::MemoryInputStream in(reply, false);
    if (!in.readBool()) {
        error = in.readString();
        return nullptr;
    }
    p->loaded_ = true;
    p->name_ = in.readString();
    p->ins_ = in.readInt();
    p->outs_ = in.readInt();
    const int latency = in.readInt();
    p->accepts_midi_ = in.readBool();
    in.readBool();  // has its own editor: either way, the window opens in its process
    p->tail_ = in.readDouble();
    const int count = in.readInt();
    for (int i = 0; i < count && !in.isExhausted(); ++i) {
        auto param = std::make_unique<Parameter>(*p, i);
        param->name = in.readString();
        param->label = in.readString();
        param->default_value = in.readFloat();
        param->value.store(in.readFloat());
        param->steps = in.readInt();
        param->discrete = in.readBool();
        param->boolean = in.readBool();
        param->automatable = in.readBool();
        param->id = in.readString();
        p->params_.push_back(param.get());
        p->addHostedParameter(std::move(param));
    }
    p->setPlayConfigDetails(p->ins_, p->outs_, sample_rate, block_size);
    p->setLatencySamples(latency);
    // Its state as loaded, so even a crash before the first save leaves
    // something to save (saves and autosaves refresh it).
    juce::MemoryBlock state;
    p->getStateInformation(state);
    p->startTimerHz(30);
    return p;
}

RemotePluginInstance::~RemotePluginInstance()
{
    stopTimer();
    // Out of a shared process; the process itself stops with its last instance.
    if (!process_->crashed()) {
        juce::MemoryBlock reply;
        call("unload", {}, reply, loaded_ ? 10000 : 1000);
    }
    --process_->instances_;
    shared_.file().deleteFile();
}

// --- Control ------------------------------------------------------------------------------------

void RemotePluginInstance::timerCallback()
{
    if (auto* sh = shared_.get(); sh && !crashed())
        if (const int latency = sh->latency.load(std::memory_order_relaxed); latency != getLatencySamples())
            setLatencySamples(latency);  // the engine notices on its own timer and re-aligns
}

const juce::String RemotePluginInstance::getName() const
{
    return crashed() ? name_ + " (crashed)" : name_;
}

void RemotePluginInstance::prepareToPlay(double sample_rate, int block_size)
{
    sample_rate_ = sample_rate;
    juce::MemoryOutputStream out;
    out.writeDouble(sample_rate);
    out.writeInt(block_size);
    juce::MemoryBlock reply;
    call("prepare", out.getMemoryBlock(), reply, 20000);
    if (auto* sh = shared_.get()) setLatencySamples(sh->latency.load());
}

void RemotePluginInstance::releaseResources()
{
    juce::MemoryBlock reply;
    call("release", {}, reply, 5000);
}

void RemotePluginInstance::getStateInformation(juce::MemoryBlock& data)
{
    juce::MemoryBlock reply;
    if (call("getState", {}, reply, 10000)) {
        juce::MemoryInputStream in(reply, false);
        if (in.readBool()) last_state_ = in.readString().toStdString();
    }
    // A crashed plug-in gives back what it last had, so saving never loses it.
    data.reset();
    data.fromBase64Encoding(juce::String(last_state_));
}

void RemotePluginInstance::setStateInformation(const void* data, int size)
{
    last_state_ = juce::MemoryBlock(data, static_cast<size_t>(std::max(0, size))).toBase64Encoding().toStdString();
    juce::MemoryOutputStream out;
    out.writeString(juce::String(last_state_));
    juce::MemoryBlock reply;
    if (call("setState", out.getMemoryBlock(), reply, 10000)) {
        juce::MemoryInputStream in(reply, false);
        if (in.readBool()) {
            const int count = in.readInt();
            for (int i = 0; i < count && i < static_cast<int>(params_.size()) && !in.isExhausted(); ++i)
                params_[static_cast<size_t>(i)]->value.store(in.readFloat());
        }
        if (auto* sh = shared_.get()) setLatencySamples(sh->latency.load());
    }
}

bool RemotePluginInstance::show_editor(bool show)
{
    juce::MemoryOutputStream out;
    out.writeBool(show);
    juce::MemoryBlock reply;
    return call("editor", out.getMemoryBlock(), reply, 10000);
}

juce::AudioProcessorEditor* RemotePluginInstance::createEditor()
{
    // The plug-in's own window lives in its process; this stands in for it.
    class Editor final : public juce::AudioProcessorEditor {
    public:
        explicit Editor(RemotePluginInstance& p) : juce::AudioProcessorEditor(p), plugin_(p)
        {
            text_.setText(p.crashed() ? p.getName() + " has crashed. Reload it from the Tools menu."
                                      : p.getName() + "'s window is open in its own process, so a crash in the "
                                                      "plug-in cannot take VC Composer down.",
                          juce::dontSendNotification);
            text_.setJustificationType(juce::Justification::centred);
            addAndMakeVisible(text_);
            show_.setButtonText("Show the plug-in's window");
            show_.onClick = [this] { plugin_.show_editor(true); };
            addAndMakeVisible(show_);
            setSize(420, 130);
            plugin_.show_editor(true);
        }
        ~Editor() override { plugin_.show_editor(false); }
        void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff1e2126)); }
        void resized() override
        {
            auto r = getLocalBounds().reduced(12);
            show_.setBounds(r.removeFromBottom(30).withSizeKeepingCentre(240, 30));
            text_.setBounds(r);
        }

    private:
        RemotePluginInstance& plugin_;
        juce::Label text_;
        juce::TextButton show_;
    };
    return new Editor(*this);
}

// --- Audio --------------------------------------------------------------------------------------

void RemotePluginInstance::push_param(int index, float value) noexcept
{
    auto* sh = shared_.get();
    if (!sh || crashed()) return;
    const juce::SpinLock::ScopedLockType hold(param_lock_);
    sh->to_plugin.push({index, 0, value});
}

void RemotePluginInstance::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    run_block(buffer, midi, false);
}

void RemotePluginInstance::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    run_block(buffer, midi, true);
}

void RemotePluginInstance::run_block(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, bool bypassed) noexcept
{
    auto* sh = shared_.get();
    const int n = buffer.getNumSamples();
    if (!sh || crashed() || n <= 0 || n > ipc::kMaxBlock) {
        buffer.clear();
        return;
    }
    // Offline renders run on the message thread (or mark the instance
    // non-realtime): they wait for every block. The device's audio thread
    // never waits longer than the block lasts.
    const bool offline = isNonRealtime() || juce::MessageManager::existsAndIsCurrentThread();
    auto wait_done = [&]() {
        if (!offline) {
            const double budget = 1000.0 * n / std::max(1.0, sample_rate_);
            return ipc::wait_for_change(sh->done, sent_ - 1, budget) || sh->done.load() == sent_;
        }
        // Offline: as long as it takes, unless the process dies (or stops
        // answering for a long time, then it is treated as crashed).
        const auto start = juce::Time::getMillisecondCounter();
        while (sh->done.load(std::memory_order_acquire) != sent_) {
            if (crashed()) return false;
            ipc::wait_for_change(sh->done, sent_ - 1, 100.0);
            if (juce::Time::getMillisecondCounter() - start > 60000) {
                process_->mark_crashed();  // hung: treated as crashed (with its process)
                return false;
            }
        }
        return true;
    };
    // A late block from last time still running: this block cannot be sent.
    if (sh->done.load(std::memory_order_acquire) != sent_) {
        if (!offline || !wait_done()) {
            late_.fetch_add(1);
            buffer.clear();
            return;
        }
    }

    const int chans = std::min(buffer.getNumChannels(), ipc::kMaxChannels);
    for (int ch = 0; ch < ins_; ++ch) {
        if (ch < chans) std::memcpy(sh->audio[ch], buffer.getReadPointer(ch), sizeof(float) * static_cast<size_t>(n));
        else std::memset(sh->audio[ch], 0, sizeof(float) * static_cast<size_t>(n));
    }
    int count = 0;
    for (const auto meta : midi) {
        if (count >= ipc::kMaxMidi) break;
        if (meta.numBytes < 1 || meta.numBytes > 3) continue;  // sysex is not passed on
        auto& e = sh->midi[count++];
        e.offset = meta.samplePosition;
        e.size = static_cast<std::uint8_t>(meta.numBytes);
        std::memcpy(e.bytes, meta.data, static_cast<size_t>(meta.numBytes));
    }
    sh->midi_count.store(count, std::memory_order_relaxed);
    sh->num_samples.store(n, std::memory_order_relaxed);
    sh->bypassed.store(bypassed ? 1 : 0, std::memory_order_relaxed);
    sh->request.store(++sent_, std::memory_order_release);
    ipc::wake(sh->request);

    if (!wait_done()) {
        late_.fetch_add(1);
        buffer.clear();
        return;
    }
    if (sh->busy.load(std::memory_order_relaxed)) {
        buffer.clear();  // the plug-in was being prepared or given a new state
    } else {
        for (int ch = 0; ch < chans; ++ch) {
            if (ch < outs_) std::memcpy(buffer.getWritePointer(ch), sh->audio[ch], sizeof(float) * static_cast<size_t>(n));
            else buffer.clear(ch, 0, n);
        }
    }
    ipc::ParamEvent e{};
    while (sh->to_host.pop(e))
        if (e.index >= 0 && e.index < static_cast<int>(params_.size())) params_[static_cast<size_t>(e.index)]->from_plugin(e);
}

}  // namespace vcc::desktop
