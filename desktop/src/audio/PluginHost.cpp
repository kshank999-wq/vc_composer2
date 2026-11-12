#include "audio/PluginHost.h"

namespace vcc::desktop {

namespace {
juce::PropertiesFile::Options property_options()
{
    juce::PropertiesFile::Options o;
    o.applicationName = "VC Composer 2";
    o.filenameSuffix = ".settings";
    o.folderName = "VC Composer 2";
    // An absolute folder here keeps the plug-in list somewhere else (the
    // headless check uses a throwaway one, never the user's).
    if (const auto dir = juce::SystemStats::getEnvironmentVariable("VCC_SETTINGS_DIR", {}); dir.isNotEmpty())
        o.folderName = dir;
    o.osxLibrarySubFolder = "Application Support";
    o.storageFormat = juce::PropertiesFile::storeAsXML;
    return o;
}

/// Plugs the scanner process into JUCE's scanning (the plug-in list window
/// and PluginDirectoryScanner call it, possibly from several threads).
class OutOfProcessScanner final : public juce::KnownPluginList::CustomScanner {
public:
    explicit OutOfProcessScanner(PluginHost& host) : host_(host) {}
    bool findPluginTypesFor(juce::AudioPluginFormat& format, juce::OwnedArray<juce::PluginDescription>& result,
                            const juce::String& file_or_id) override
    {
        return host_.scan_file(format, file_or_id, result, [this] { return shouldExit(); });
    }

private:
    PluginHost& host_;
};

}  // namespace

PluginHost::PluginHost()
{
    formats_.addDefaultFormats();
    properties_ = std::make_unique<juce::PropertiesFile>(property_options());
    if (auto xml = properties_->getXmlValue("knownPlugins")) known_.recreateFromXml(*xml);
    if (auto xml = properties_->getXmlValue("scanFailures"))
        for (auto* e : xml->getChildIterator()) failures_[e->getStringAttribute("target")] = e->getStringAttribute("reason");
    known_.setCustomScanner(std::make_unique<OutOfProcessScanner>(*this));
}

namespace {

juce::String tool_name()
{
#if JUCE_WINDOWS
    return "vcc_plugin_scanner.exe";
#else
    return "vcc_plugin_scanner";
#endif
}

}  // namespace

juce::File PluginHost::scanner_tool() const
{
    std::lock_guard<std::mutex> lock(scan_lock_);
    if (scanner_override_ != juce::File()) return scanner_override_;
    if (const auto env = juce::SystemStats::getEnvironmentVariable("VCC_PLUGIN_SCANNER", {}); env.isNotEmpty())
        return juce::File(env);
    // Beside the executable (Contents/MacOS in a macOS bundle).
    return juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile(tool_name());
}

void PluginHost::set_scanner_tool(const juce::File& tool)
{
    std::lock_guard<std::mutex> lock(scan_lock_);
    scanner_override_ = tool;
}

std::map<juce::String, juce::String> PluginHost::scan_failures() const
{
    std::lock_guard<std::mutex> lock(scan_lock_);
    return failures_;
}

void PluginHost::clear_scan_failure(const juce::String& file_or_id)
{
    {
        std::lock_guard<std::mutex> lock(scan_lock_);
        failures_.erase(file_or_id);
    }
    known_.removeFromBlacklist(file_or_id);
}

void PluginHost::record_failure(const juce::String& file_or_id, const juce::String& reason)
{
    std::lock_guard<std::mutex> lock(scan_lock_);
    failures_[file_or_id] = reason;
}

bool PluginHost::scan_file(juce::AudioPluginFormat& format, const juce::String& file_or_id,
                           juce::OwnedArray<juce::PluginDescription>& found, const std::function<bool()>& cancelled)
{
    const juce::File tool = scanner_tool();
    if (!tool.existsAsFile()) {
        // Last resort: in this process (the dead-man's pedal still marks a crasher).
        format.findAllTypesForFile(found, file_or_id);
        return true;
    }
    const auto out = juce::File::createTempFile(".vccscan");
    out.deleteFile();
    juce::ChildProcess child;
    // Output is not captured (it goes to /dev/null): a chatty plug-in can
    // never fill a pipe and stall the scan.
    if (!child.start(juce::StringArray{tool.getFullPathName(), "--format", format.getName(), "--target", file_or_id,
                                       "--out", out.getFullPathName()},
                     0)) {
        record_failure(file_or_id, "the scanner tool could not be started");
        return false;
    }
    const auto started = juce::Time::getMillisecondCounter();
    const auto limit = static_cast<juce::uint32>(std::max(500, scan_timeout_ms()));
    while (child.isRunning()) {
        if (cancelled && cancelled()) {
            child.kill();
            return true;  // abandoned, not failed: scanned again next time
        }
        if (juce::Time::getMillisecondCounter() - started > limit) {
            child.kill();
            record_failure(file_or_id, "did not finish loading within " + juce::String(limit / 1000.0, 1) +
                                           " s (it may be waiting for a licence dialog or hung)");
            out.deleteFile();
            return false;
        }
        juce::Thread::sleep(15);
    }
    const auto code = child.getExitCode();
    std::unique_ptr<juce::XmlElement> xml = out.existsAsFile() ? juce::parseXML(out) : nullptr;
    out.deleteFile();
    if (!xml || !xml->hasTagName("VCC_SCAN_RESULT")) {
        record_failure(file_or_id, code == 0 ? juce::String("crashed while it was being loaded")
                                             : "crashed while it was being loaded (scanner exit " + juce::String(code) + ")");
        return false;
    }
    for (auto* e : xml->getChildIterator()) {
        auto d = std::make_unique<juce::PluginDescription>();
        if (d->loadFromXml(*e)) found.add(d.release());
    }
    {
        std::lock_guard<std::mutex> lock(scan_lock_);
        failures_.erase(file_or_id);
    }
    return true;
}

PluginHost::~PluginHost() { save_list(); }

juce::File PluginHost::dead_mans_pedal() const
{
    return properties_->getFile().getSiblingFile("PluginScanCrashes.txt");
}

void PluginHost::save_list()
{
    if (auto xml = known_.createXml()) properties_->setValue("knownPlugins", xml.get());
    juce::XmlElement failures("SCAN_FAILURES");
    for (const auto& [target, reason] : scan_failures()) {
        auto* e = failures.createNewChildElement("FAILURE");
        e->setAttribute("target", target);
        e->setAttribute("reason", reason);
    }
    properties_->setValue("scanFailures", &failures);
    properties_->saveIfNeeded();
}

juce::Array<juce::PluginDescription> PluginHost::instruments() const
{
    juce::Array<juce::PluginDescription> out;
    for (const auto& d : known_.getTypes())
        if (d.isInstrument) out.add(d);
    std::sort(out.begin(), out.end(), [](const juce::PluginDescription& a, const juce::PluginDescription& b) {
        return a.manufacturerName == b.manufacturerName ? a.name < b.name : a.manufacturerName < b.manufacturerName;
    });
    return out;
}

project::PluginRef PluginHost::ref_for(const juce::PluginDescription& d) const
{
    project::PluginRef r;
    r.format = d.pluginFormatName.toStdString();
    r.identifier = d.createIdentifierString().toStdString();
    r.name = d.name.toStdString();
    r.vendor = d.manufacturerName.toStdString();
    return r;
}

std::unique_ptr<juce::AudioPluginInstance> PluginHost::create(const project::PluginRef& ref, double sample_rate,
                                                              int block_size, juce::String& error)
{
    auto description = known_.getTypeForIdentifierString(juce::String(ref.identifier));
    if (description == nullptr) {
        error = juce::String(ref.name) + " is not installed (or not scanned) on this computer";
        return nullptr;
    }
    auto instance = formats_.createPluginInstance(*description, sample_rate, block_size, error);
    if (instance == nullptr) return nullptr;
    instance->enableAllBuses();
    if (!ref.state_base64.empty()) restore_state(*instance, ref.state_base64);
    return instance;
}

std::string PluginHost::state_of(juce::AudioPluginInstance& instance)
{
    juce::MemoryBlock block;
    instance.getStateInformation(block);
    return block.toBase64Encoding().toStdString();
}

void PluginHost::restore_state(juce::AudioPluginInstance& instance, const std::string& state)
{
    juce::MemoryBlock block;
    if (block.fromBase64Encoding(juce::String(state)) && block.getSize() > 0)
        instance.setStateInformation(block.getData(), static_cast<int>(block.getSize()));
}

// --- PluginInstrument ----------------------------------------------------------------

PluginInstrument::PluginInstrument(std::unique_ptr<juce::AudioPluginInstance> instance) : instance_(std::move(instance))
{
    for (int ch = 1; ch <= 16; ++ch) {
        panic_.addEvent(juce::MidiMessage::allNotesOff(ch), 0);
        panic_.addEvent(juce::MidiMessage::allSoundOff(ch), 0);
    }
}

PluginInstrument::~PluginInstrument()
{
    if (instance_) instance_->releaseResources();
}

void PluginInstrument::prepare(double sample_rate, int max_block_size)
{
    instance_->releaseResources();
    instance_->setRateAndBufferSizeDetails(sample_rate, max_block_size);
    instance_->prepareToPlay(sample_rate, max_block_size);
    const int channels = juce::jmax(2, instance_->getTotalNumInputChannels(), instance_->getTotalNumOutputChannels());
    buffer_.setSize(channels, juce::jmax(1, max_block_size));
}

void PluginInstrument::process(juce::AudioBuffer<float>& out, juce::MidiBuffer& midi, int num_samples) noexcept
{
    if (num_samples > buffer_.getNumSamples()) return;  // never reallocate on the audio thread
    if (panic_pending_.exchange(false)) midi.addEvents(panic_, 0, 1, 0);
    juce::AudioBuffer<float> view(buffer_.getArrayOfWritePointers(), buffer_.getNumChannels(), num_samples);
    view.clear();
    instance_->processBlock(view, midi);
    const int outs = juce::jmin(2, instance_->getTotalNumOutputChannels());
    for (int ch = 0; ch < 2 && ch < out.getNumChannels(); ++ch)
        out.addFrom(ch, 0, view, outs == 1 ? 0 : ch, 0, num_samples);
}

void PluginInstrument::all_notes_off() noexcept { panic_pending_ = true; }

juce::String PluginInstrument::display_name() const { return instance_->getName(); }

}  // namespace vcc::desktop
