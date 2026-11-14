#include "audio/Consolidate.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include <juce_audio_formats/juce_audio_formats.h>

#include "ui/AppContext.h"

namespace vcc::desktop {

namespace {

juce::File consolidate_file(const AppContext& context, const juce::String& track_name)
{
    juce::File folder{juce::String(context.session.audio_folder())};
    if (context.session.audio_folder().empty())
        folder = juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                     .getChildFile("VC Composer 2")
                     .getChildFile("Untitled Audio Files");
    folder.createDirectory();
    juce::String base = juce::File::createLegalFileName(track_name).replaceCharacters(" ./", "___").trimCharactersAtEnd("_");
    if (base.isEmpty()) base = "Track";
    for (int n = 1; n < 1000; ++n) {
        const auto f = folder.getChildFile(base + "_CONSOL_" + juce::String(n).paddedLeft('0', 2) + ".wav");
        if (!f.existsAsFile()) return f;
    }
    return folder.getChildFile(base + "_CONSOL_" + juce::Uuid().toString().substring(0, 6) + ".wav");
}

}  // namespace

std::vector<std::string> touching_clips(const AppContext& context, const std::string& track_id, const std::string& clip_id)
{
    const auto* t = context.session.data().find_track(track_id);
    const auto* v = t ? t->active_version() : nullptr;
    if (!v) return {};
    const project::AudioClip* seed = nullptr;
    for (const auto& c : v->audio_clips)
        if (c.id == clip_id) seed = &c;
    if (!seed) return {};
    std::vector<const project::AudioClip*> lane;
    for (const auto& c : v->audio_clips)
        if (c.lane == seed->lane) lane.push_back(&c);
    std::sort(lane.begin(), lane.end(), [](auto* a, auto* b) { return a->start_seconds < b->start_seconds; });
    // Grow the span both ways while the next clip touches it (1 ms tolerance).
    double from = seed->start_seconds, to = seed->end_seconds();
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto* c : lane)
            if (c->start_seconds <= to + 0.001 && c->end_seconds() >= from - 0.001 &&
                (c->start_seconds < from || c->end_seconds() > to)) {
                from = std::min(from, c->start_seconds);
                to = std::max(to, c->end_seconds());
                grew = true;
            }
    }
    std::vector<std::string> ids;
    for (const auto* c : lane)
        if (c->start_seconds >= from - 0.001 && c->end_seconds() <= to + 0.001) ids.push_back(c->id);
    return ids;
}

ConsolidateResult consolidate_clips(AppContext& context, const std::string& track_id,
                                    const std::vector<std::string>& clip_ids)
{
    ConsolidateResult result;
    auto& session = context.session;
    const auto* t = session.data().find_track(track_id);
    const auto* v = t ? t->active_version() : nullptr;
    if (!v) {
        result.message = "Consolidate: no such track";
        return result;
    }
    std::vector<project::AudioClip> clips;
    for (const auto& c : v->audio_clips)
        if (std::find(clip_ids.begin(), clip_ids.end(), c.id) != clip_ids.end()) clips.push_back(c);
    if (clips.size() < 2) {
        result.message = "Consolidate needs two or more clips that touch";
        return result;
    }
    const int lane = clips.front().lane;
    double from = 1e300, to = 0.0, rate = 0.0;
    int channels = 1;
    for (const auto& c : clips) {
        if (c.lane != lane) {
            result.message = "Consolidate works on one lane at a time";
            return result;
        }
        from = std::min(from, c.start_seconds);
        to = std::max(to, c.end_seconds());
        rate = std::max(rate, c.file_sample_rate);
        channels = std::max(channels, std::clamp(c.channels, 1, 2));
    }
    if (rate <= 0.0) rate = 48000.0;
    const auto total = static_cast<int>(std::llround((to - from) * rate));
    if (total <= 0 || total > static_cast<int>(rate * 60.0 * 60.0)) {
        result.message = "Consolidate: the clips are too long to render into one file";
        return result;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    juce::AudioBuffer<float> mix(channels, total);
    mix.clear();
    juce::StringArray problems;
    for (const auto& c : clips) {
        if (c.muted) continue;
        const juce::File file{juce::String(session.resolve_audio_path(c.file))};
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (!reader) {
            problems.add(file.getFileName() + " could not be read");
            continue;
        }
        const double file_rate = reader->sampleRate > 0.0 ? reader->sampleRate : rate;
        const double ratio = file_rate / rate;  // file samples per output sample
        const auto out_len = static_cast<int>(std::llround(c.length * rate));
        const auto at = static_cast<int>(std::llround((c.start_seconds - from) * rate));
        const auto src_start = static_cast<juce::int64>(std::llround(c.source_offset * file_rate));
        const int src_len = static_cast<int>(std::ceil(out_len * ratio)) + 8;
        const int src_channels = static_cast<int>(std::min<unsigned>(reader->numChannels, 2));
        juce::AudioBuffer<float> src(std::max(1, src_channels), src_len);
        src.clear();
        reader->read(&src, 0, src_len, src_start, true, src_channels > 1);
        juce::AudioBuffer<float> piece(channels, out_len);
        for (int ch = 0; ch < channels; ++ch) {
            const float* in = src.getReadPointer(std::min(ch, src.getNumChannels() - 1));
            float* out = piece.getWritePointer(ch);
            if (std::abs(ratio - 1.0) < 1e-9) {
                juce::FloatVectorOperations::copy(out, in, std::min(out_len, src_len));
            } else {
                juce::LagrangeInterpolator interp;
                interp.process(ratio, in, out, out_len, src_len, 0);
            }
        }
        // Gain and fades as playback applies them (equal-power sine fades).
        const float gain = juce::Decibels::decibelsToGain(static_cast<float>(c.gain_db));
        const auto fade_in = static_cast<int>(std::llround(c.fade_in * rate));
        const auto fade_out = static_cast<int>(std::llround(c.fade_out * rate));
        const float half_pi = juce::MathConstants<float>::halfPi;
        for (int k = 0; k < out_len; ++k) {
            float g = gain;
            if (k < fade_in) g *= std::sin(half_pi * (static_cast<float>(k) + 0.5f) / static_cast<float>(fade_in));
            const int remaining = out_len - k;
            if (remaining < fade_out) g *= std::sin(half_pi * (static_cast<float>(remaining) + 0.5f) / static_cast<float>(fade_out));
            const int dst = at + k;
            if (dst < 0 || dst >= total) continue;
            for (int ch = 0; ch < channels; ++ch) mix.addSample(ch, dst, piece.getSample(ch, k) * g);
        }
    }
    if (!problems.isEmpty()) {
        result.message = "Consolidate failed: " + problems.joinIntoString("; ");
        return result;
    }

    const auto file = consolidate_file(context, juce::String(t->name));
    {
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
        auto options = juce::AudioFormatWriterOptions{}
                           .withSampleRate(rate)
                           .withNumChannels(channels)
                           .withBitsPerSample(24);
        auto writer = juce::WavAudioFormat().createWriterFor(stream, options);
        if (!writer || !writer->writeFromAudioSampleBuffer(mix, 0, total)) {
            file.deleteFile();
            result.message = "Consolidate: could not write " + file.getFullPathName();
            return result;
        }
    }
    project::AudioClip whole;
    whole.name = file.getFileNameWithoutExtension().toStdString();
    const juce::File project_dir =
        session.path().empty() ? juce::File() : juce::File(juce::String(session.path())).getParentDirectory();
    whole.file = project_dir != juce::File() && file.isAChildOf(project_dir)
                     ? file.getRelativePathFrom(project_dir).replaceCharacter('\\', '/').toStdString()
                     : file.getFullPathName().toStdString();
    whole.start_seconds = from;
    whole.length = static_cast<double>(total) / rate;
    whole.lane = lane;
    whole.channels = channels;
    whole.file_sample_rate = rate;
    result.clip_id = session.replace_audio_clips(track_id, clip_ids, whole, "Consolidate");
    if (result.clip_id.empty()) {
        file.deleteFile();
        result.message = "Consolidate: the clips changed; nothing was replaced";
        return result;
    }
    result.ok = true;
    result.file = file;
    result.message = "Consolidated " + juce::String(static_cast<int>(clips.size())) + " clips into " + file.getFileName();
    return result;
}

}  // namespace vcc::desktop
