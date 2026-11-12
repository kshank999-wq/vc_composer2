#include "audio/StemRenderer.h"

#include <cmath>
#include <limits>

#include <juce_audio_formats/juce_audio_formats.h>

#include "audio/PlaybackEngine.h"
#include "ui/AppContext.h"
#include "vcc/cues/Cues.h"
#include "vcc/project/Stems.h"

namespace vcc::desktop {

namespace {

constexpr int kBlock = 512;
/// Above this the sum check would hold too much audio in memory; files
/// still print.
constexpr double kMaxSumCheckSeconds = 20.0 * 60.0;

std::string file_name(const StemRenderRequest& rq, const StemRange& range, const std::string& stem,
                      const project::ProjectData& d, const std::string& date)
{
    std::string preset = rq.name_preset.empty() ? "{production}_{code}_{stem}_v{rev}" : rq.name_preset;
    for (std::size_t at = preset.find("{stem}"); at != std::string::npos; at = preset.find("{stem}", at + stem.size()))
        preset.replace(at, 6, stem);
    if (const auto* cue = d.find_cue(range.cue_id)) return cues::render_name(preset, *cue, d, date);
    project::Cue stand_in;
    stand_in.code = range.label;
    stand_in.title = range.label;
    stand_in.status = "writing";
    return cues::render_name(preset, stand_in, d, date);
}

/// Master-bus processing that does not add up linearly.
std::vector<juce::String> master_notes(const project::ProjectData& d)
{
    std::vector<juce::String> notes;
    const project::Bus* m = d.find_bus("bus-master");
    if (!m) return notes;
    if (m->strip.comp_on || m->strip.gate_on)
        notes.push_back("The master bus compressor/gate is in: stems will not add back up to the mix exactly");
    for (const auto& ins : m->inserts)
        if (!ins.bypass && !ins.suggestion &&
            (ins.kind == "compressor" || ins.kind == "limiter" || ins.kind == "gate" || ins.kind == "saturation" ||
             ins.kind == "plugin"))
            notes.push_back("Master insert '" + juce::String(ins.kind == "plugin" ? ins.plugin.name : ins.kind) +
                            "' is not linear: stems will not add back up to the mix exactly");
    return notes;
}

}  // namespace

StemRange whole_project_range(const AppContext& context)
{
    StemRange r;
    r.label = "Full";
    r.start = 0;
    r.end = std::max<Ticks>(context.session.content_end(), kTicksPerWhole);
    return r;
}

std::vector<StemRange> cue_ranges(const AppContext& context, const std::vector<std::string>& cue_ids)
{
    std::vector<StemRange> out;
    for (const auto& c : context.session.data().cues) {
        if (!cue_ids.empty() && std::find(cue_ids.begin(), cue_ids.end(), c.id) == cue_ids.end()) continue;
        if (c.end <= c.start) continue;
        out.push_back({c.id, c.code.empty() ? c.title : c.code, c.start, c.end});
    }
    return out;
}

StemRenderResult render_stems(AppContext& context, const StemRenderRequest& rq)
{
    StemRenderResult result;
    const auto& d = context.session.data();
    std::vector<project::StemDef> stems;
    for (const auto& st : context.session.stems())
        if (rq.stem_ids.empty() || std::find(rq.stem_ids.begin(), rq.stem_ids.end(), st.id) != rq.stem_ids.end())
            stems.push_back(st);

    const auto coverage = project::stem_coverage(context.session.stems(), d);
    for (const auto& n : coverage.missing) result.warnings.push_back("'" + juce::String(n) + "' is in no stem");
    for (const auto& n : coverage.doubled) result.warnings.push_back("'" + juce::String(n) + "' is in more than one stem");
    for (const auto& n : coverage.empty) result.warnings.push_back("Stem '" + juce::String(n) + "' has nothing in it");
    for (auto& n : master_notes(d)) result.warnings.push_back(n);

    rq.folder.createDirectory();
    const auto date = juce::Time::getCurrentTime().formatted("%Y-%m-%d").toStdString();
    juce::WavAudioFormat wav;
    const std::size_t total = rq.ranges.size() * (stems.size() + (rq.include_mix ? 1u : 0u));
    std::size_t done = 0;

    for (const auto& range : rq.ranges) {
        const project::Cue* cue = d.find_cue(range.cue_id);
        const bool protected_cue = cue && (cue->status == "approved" || cue->status == "locked");
        const double start_s = d.tempo.seconds_at(range.start);
        const double end_s = d.tempo.seconds_at(range.end) + rq.tail_seconds;
        const auto samples = static_cast<std::int64_t>(std::max(0.0, end_s - start_s) * rq.sample_rate);
        const bool check_sum = rq.include_mix && !stems.empty() && end_s - start_s <= kMaxSumCheckSeconds;
        juce::AudioBuffer<float> mix_audio, stem_sum;
        if (check_sum) {
            mix_audio.setSize(2, static_cast<int>(samples));
            stem_sum.setSize(2, static_cast<int>(samples));
            mix_audio.clear();
            stem_sum.clear();
        }
        bool range_ok = true;

        // The mix first, then each stem; every file through a fresh engine.
        for (int which = rq.include_mix ? -1 : 0; which < static_cast<int>(stems.size()); ++which) {
            const std::string stem_name = which < 0 ? "Mix" : stems[static_cast<std::size_t>(which)].name;
            if (rq.progress &&
                !rq.progress(total ? static_cast<double>(done) / static_cast<double>(total) : 0.0,
                             juce::String(range.label) + ": " + juce::String(stem_name))) {
                result.cancelled = true;
                return result;
            }
            ++done;
            StemFileResult fr;
            fr.range_label = range.label;
            fr.stem = stem_name;
            fr.file = rq.folder.getChildFile(juce::String(file_name(rq, range, stem_name, d, date)) + ".wav");
            if (protected_cue && fr.file.existsAsFile() && !rq.overwrite_protected) {
                fr.skipped_protected = true;
                fr.message = "not overwritten: " + juce::String(range.label) + " is " + juce::String(cue->status);
                result.files.push_back(fr);
                range_ok = false;
                continue;
            }

            PlaybackEngine engine(context.session, context.plugins);
            engine.prepare_offline(rq.sample_rate, kBlock);
            engine.set_export_mode(true);
            if (which >= 0) {
                const auto& st = stems[static_cast<std::size_t>(which)];
                PlaybackEngine::RenderOverride over;
                over.solo = st.sources;
                over.mute_effects = !st.include_effects;
                engine.set_render_override(over);
            }

            const auto temp = fr.file.getSiblingFile(fr.file.getFileName() + ".partial");
            temp.deleteFile();
            std::unique_ptr<juce::AudioFormatWriter> writer;
            if (std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(temp);
                static_cast<juce::FileOutputStream*>(stream.get())->openedOk()) {
                const auto options = juce::AudioFormatWriterOptions{}
                                         .withSampleRate(rq.sample_rate)
                                         .withNumChannels(2)
                                         .withBitsPerSample(rq.bit_depth == 16 ? 16 : 24);
                writer = wav.createWriterFor(stream, options);
            }
            if (!writer) {
                fr.message = "could not write to that folder";
                result.files.push_back(fr);
                range_ok = false;
                continue;
            }
            juce::AudioBuffer<float> block(2, kBlock);
            engine.locate(range.start);
            engine.play();
            bool ok = true;
            for (std::int64_t pos = 0; pos < samples && ok; pos += kBlock) {
                const int n = static_cast<int>(std::min<std::int64_t>(kBlock, samples - pos));
                engine.render_offline(block, n);
                fr.peak = std::max({fr.peak, block.getMagnitude(0, 0, n), block.getMagnitude(1, 0, n)});
                if (check_sum)
                    for (int ch = 0; ch < 2; ++ch)
                        (which < 0 ? mix_audio : stem_sum).addFrom(ch, static_cast<int>(pos), block, ch, 0, n);
                ok = writer->writeFromAudioSampleBuffer(block, 0, n);
            }
            engine.stop();
            writer.reset();
            if (!ok || !temp.moveFileTo(fr.file)) {
                temp.deleteFile();
                fr.message = "writing the file failed";
                result.files.push_back(fr);
                range_ok = false;
                continue;
            }
            fr.ok = true;
            fr.message = fr.file.getFileName();
            if (fr.peak < 1.0e-4f) fr.message << " (silent)";
            else if (fr.peak > 1.0f)
                fr.message << " (clips: " << juce::String(juce::Decibels::gainToDecibels(fr.peak), 1) << " dBFS)";
            result.files.push_back(fr);
        }

        StemRangeSummary summary;
        summary.label = range.label;
        if (check_sum && range_ok) {
            double residual = 0.0, reference = 0.0;
            for (int ch = 0; ch < 2; ++ch) {
                const float* m = mix_audio.getReadPointer(ch);
                const float* s = stem_sum.getReadPointer(ch);
                for (int i = 0; i < mix_audio.getNumSamples(); ++i) {
                    const double diff = static_cast<double>(m[i]) - static_cast<double>(s[i]);
                    residual += diff * diff;
                    reference += static_cast<double>(m[i]) * static_cast<double>(m[i]);
                }
            }
            summary.sum_checked = reference > 0.0;
            summary.sum_error_db = residual <= 0.0 ? -std::numeric_limits<double>::infinity()
                                                   : 10.0 * std::log10(residual / std::max(reference, 1e-30));
        }
        result.ranges.push_back(summary);

        // A cue whose stems all printed has its "Stems" deliverable done.
        if (rq.mark_deliverables && cue && range_ok && !stems.empty() && !cue->locked())
            context.session.update_cue(cue->id, [](project::Cue& c) {
                for (auto& del : c.deliverables)
                    if (del.name == "Stems") del.done = true;
            });
    }
    if (rq.progress) rq.progress(1.0, "Done");
    return result;
}

}  // namespace vcc::desktop
