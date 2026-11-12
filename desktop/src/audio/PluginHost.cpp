#include "audio/PluginHost.h"

namespace vcc::desktop {

namespace {
juce::PropertiesFile::Options property_options()
{
    juce::PropertiesFile::Options o;
    o.applicationName = "VC Composer 2";
    o.filenameSuffix = ".settings";
    o.folderName = "VC Composer 2";
    o.osxLibrarySubFolder = "Application Support";
    o.storageFormat = juce::PropertiesFile::storeAsXML;
    return o;
}
}  // namespace

PluginHost::PluginHost()
{
    formats_.addDefaultFormats();
    properties_ = std::make_unique<juce::PropertiesFile>(property_options());
    if (auto xml = properties_->getXmlValue("knownPlugins")) known_.recreateFromXml(*xml);
}

PluginHost::~PluginHost() { save_list(); }

juce::File PluginHost::dead_mans_pedal() const
{
    return properties_->getFile().getSiblingFile("PluginScanCrashes.txt");
}

void PluginHost::save_list()
{
    if (auto xml = known_.createXml()) {
        properties_->setValue("knownPlugins", xml.get());
        properties_->saveIfNeeded();
    }
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
