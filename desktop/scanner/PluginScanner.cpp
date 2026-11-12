// vcc_plugin_scanner: loads one plug-in file in its own process and reports
// what it contains (Main Spec §20: "a failed scan cannot crash the whole
// project"). VC Composer 2 runs one of these per plug-in file; if the plug-in
// crashes, hangs or corrupts memory while it is loaded, only this process
// dies and the app records why.
//
//   vcc_plugin_scanner --format VST3 --target /path/Plugin.vst3 --out result.xml
//
// The result is written to a file rather than stdout because plug-ins print
// freely. It is written to a temporary name and renamed when complete, so a
// crash part-way can never leave a half-written result that reads as valid.
// Exit codes: 0 scanned (even if nothing was found), 2 bad arguments, 3 no
// such format, 4 the result could not be written.
#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdio>

namespace {

juce::String argument(const juce::StringArray& args, const juce::String& name)
{
    const int i = args.indexOf(name);
    return i >= 0 && i + 1 < args.size() ? args[i + 1] : juce::String();
}

}  // namespace

int main(int argc, char* argv[])
{
    juce::StringArray args;
    for (int i = 1; i < argc; ++i) args.add(juce::String::fromUTF8(argv[i]));
    const auto format_name = argument(args, "--format");
    const auto target = argument(args, "--target");
    const auto out_path = argument(args, "--out");
    if (format_name.isEmpty() || target.isEmpty() || out_path.isEmpty() || !juce::File::isAbsolutePath(out_path)) {
        std::fprintf(stderr, "usage: vcc_plugin_scanner --format NAME --target FILE_OR_ID --out ABSOLUTE_RESULT_FILE\n");
        return 2;
    }

    // Plug-ins expect a message thread; this one never shows a window.
    juce::ScopedJuceInitialiser_GUI juce_runtime;
    juce::AudioPluginFormatManager formats;
    formats.addDefaultFormats();
    juce::AudioPluginFormat* format = nullptr;
    for (auto* f : formats.getFormats())
        if (f->getName() == format_name) format = f;
    if (format == nullptr) {
        std::fprintf(stderr, "no plug-in format named %s in this build\n", format_name.toRawUTF8());
        return 3;
    }

    juce::OwnedArray<juce::PluginDescription> found;
    format->findAllTypesForFile(found, target);

    juce::XmlElement result("VCC_SCAN_RESULT");
    result.setAttribute("format", format_name);
    result.setAttribute("target", target);
    for (const auto* d : found) result.addChildElement(d->createXml().release());

    const juce::File out(out_path);
    const auto partial = out.getSiblingFile(out.getFileName() + ".partial");
    if (!result.writeTo(partial) || !partial.moveFileTo(out)) {
        std::fprintf(stderr, "could not write %s\n", out_path.toRawUTF8());
        return 4;
    }
    return 0;
}
