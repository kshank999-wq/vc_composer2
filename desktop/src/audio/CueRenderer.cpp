#include "audio/CueRenderer.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include "audio/PlaybackEngine.h"
#include "ui/AppContext.h"
#include "vcc/cues/Cues.h"

namespace vcc::desktop {

std::vector<CueRenderResult> render_cues(AppContext& context, const CueRenderRequest& request)
{
    std::vector<CueRenderResult> results;
    const auto& d = context.session.data();
    request.folder.createDirectory();
    const auto date = juce::Time::getCurrentTime().formatted("%Y-%m-%d").toStdString();

    // A private engine: offline, no device, its own plug-in instances
    // restored from the project's saved states.
    PlaybackEngine engine(context.session, context.plugins);
    constexpr int block = 512;
    engine.set_export_mode(true);  // temp/reference tracks are not part of a deliverable
    engine.prepare_offline(request.sample_rate, block);

    juce::WavAudioFormat wav;
    juce::AudioBuffer<float> buffer(2, block);

    for (const auto& id : request.cue_ids) {
        CueRenderResult r;
        r.cue_id = id;
        const project::Cue* cue = d.find_cue(id);
        if (!cue) {
            r.message = "no such cue";
            results.push_back(r);
            continue;
        }
        const auto name = cues::render_name(request.name_preset, *cue, d, date);
        r.file = request.folder.getChildFile(juce::String(name) + ".wav");
        const bool protected_cue = cue->status == "approved" || cue->status == "locked";
        if (protected_cue && r.file.existsAsFile() && !request.overwrite_protected) {
            r.skipped_protected = true;
            r.message = "not overwritten: " + juce::String(cue->code) + " is " + juce::String(cue->status);
            results.push_back(r);
            continue;
        }

        const double start = d.tempo.seconds_at(cue->start);
        const double end = d.tempo.seconds_at(cue->end) + request.tail_seconds;
        const auto total = static_cast<std::int64_t>((end - start) * request.sample_rate);

        // Write beside the target and move into place only when complete.
        const auto temp = r.file.getSiblingFile(r.file.getFileName() + ".partial");
        temp.deleteFile();
        std::unique_ptr<juce::AudioFormatWriter> writer;
        if (std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(temp);
            static_cast<juce::FileOutputStream*>(stream.get())->openedOk()) {
            const auto options = juce::AudioFormatWriterOptions{}
                                     .withSampleRate(request.sample_rate)
                                     .withNumChannels(2)
                                     .withBitsPerSample(request.bit_depth == 16 ? 16 : 24);
            writer = wav.createWriterFor(stream, options);  // takes the stream on success
        }
        if (!writer) {
            r.message = "could not write to that folder";
            results.push_back(r);
            continue;
        }

        engine.stop();
        engine.locate(cue->start);
        engine.play();
        float peak = 0.0f;
        bool ok = true;
        for (std::int64_t done = 0; done < total && ok; done += block) {
            const int n = static_cast<int>(std::min<std::int64_t>(block, total - done));
            engine.render_offline(buffer, n);
            peak = std::max({peak, buffer.getMagnitude(0, 0, n), buffer.getMagnitude(1, 0, n)});
            ok = writer->writeFromAudioSampleBuffer(buffer, 0, n);
        }
        engine.stop();
        writer.reset();  // flushes and closes
        if (!ok || !temp.moveFileTo(r.file)) {
            temp.deleteFile();
            r.message = "writing the file failed";
            results.push_back(r);
            continue;
        }
        r.ok = true;
        r.peak = peak;
        r.message = juce::String(cue->code) + " -> " + r.file.getFileName();
        if (peak < 1.0e-4f) r.message << " (silent: nothing plays in this cue's range)";
        else if (peak > 1.0f) r.message << " (clips: peak " << juce::String(juce::Decibels::gainToDecibels(peak), 1) << " dBFS)";
        results.push_back(r);
    }
    return results;
}

}  // namespace vcc::desktop
