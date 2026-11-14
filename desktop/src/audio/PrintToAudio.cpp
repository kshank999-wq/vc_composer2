#include "audio/PrintToAudio.h"

#include <algorithm>
#include <cmath>

#include <juce_audio_formats/juce_audio_formats.h>

#include "audio/PlaybackEngine.h"
#include "ui/AppContext.h"

namespace vcc::desktop {

namespace {
constexpr int kBlock = 512;
constexpr float kSilence = 1.0e-4f;  // -80 dBFS

juce::File audio_folder(const AppContext& context)
{
    const auto folder = context.session.audio_folder();
    if (!folder.empty()) return juce::File(juce::String(folder));
    return juce::File::getSpecialLocation(juce::File::userMusicDirectory)
        .getChildFile("VC Composer 2")
        .getChildFile("Untitled Audio Files");
}
}  // namespace

juce::File print_file_for(const AppContext& context, const juce::String& track_name)
{
    // "Celli — Sul Pont./Trem." prints as "Celli_PRINT_01.wav".
    juce::String base = track_name.upToFirstOccurrenceOf(juce::String::fromUTF8(" \xe2\x80\x94"), false, false).trim();
    base = juce::File::createLegalFileName(base).replaceCharacters(" ./", "___").trimCharactersAtEnd("_");
    if (base.isEmpty()) base = "Track";
    const auto folder = audio_folder(context);
    for (int n = 1; n < 1000; ++n) {
        const auto f = folder.getChildFile(base + "_PRINT_" + juce::String(n).paddedLeft('0', 2) + ".wav");
        if (!f.existsAsFile()) return f;
    }
    return folder.getChildFile(base + "_PRINT_" + juce::Uuid().toString().substring(0, 6) + ".wav");
}

struct PrintJob::Render {
    std::unique_ptr<PlaybackEngine> engine;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    juce::File temp;
    juce::AudioBuffer<float> block{2, kBlock};
    std::int64_t skip = 0;       ///< The tap's latency, rendered and dropped.
    std::int64_t samples = 0;    ///< To write: the range and the tail.
    std::int64_t range_samples = 0;
    std::int64_t written = 0;
    std::int64_t last_loud = 0;  ///< One past the last sample above silence.
};

PrintJob::PrintJob(AppContext& context, PrintRequest request) : context_(context), request_(std::move(request))
{
    if (request_.end <= request_.start) request_.end = request_.start + kTicksPerWhole;
    for (const auto& id : request_.tracks) {
        PrintItem item;
        item.track_id = id;
        if (const auto* t = context_.session.data().find_track(id)) item.track_name = juce::String(t->name);
        items_.push_back(item);
    }
}

PrintJob::~PrintJob() = default;

double PrintJob::progress() const
{
    if (items_.empty() || finished_) return 1.0;
    double part = 0.0;
    if (render_ && render_->samples > 0) part = static_cast<double>(render_->written) / static_cast<double>(render_->samples);
    return (static_cast<double>(index_) + part) / static_cast<double>(items_.size());
}

juce::String PrintJob::status() const
{
    if (finished_) return cancelled_ ? "PRINT CANCELLED" : "PRINT DONE";
    if (index_ >= static_cast<int>(items_.size())) return "PLACING";
    const auto& d = context_.session.data();
    juce::String name = items_[static_cast<std::size_t>(index_)].track_name.upToFirstOccurrenceOf(
        juce::String::fromUTF8(" \xe2\x80\x94"), false, false);
    Ticks at = request_.start;
    if (render_ && render_->written > 0)
        at = d.tempo.tick_at_seconds(d.tempo.seconds_at(request_.start) +
                                     static_cast<double>(render_->written) / request_.sample_rate);
    return name.toUpperCase() + "  " + juce::String(index_ + 1) + " / " + juce::String(items_.size()) + "  BAR " +
           juce::String(d.meter.bar_at(std::min(at, request_.end)));
}

bool PrintJob::begin_track()
{
    auto& item = items_[static_cast<std::size_t>(index_)];
    const auto* t = context_.session.data().find_track(item.track_id);
    if (!t || t->kind == project::TrackKind::Audio || t->kind == project::TrackKind::Aux ||
        t->kind == project::TrackKind::Reference) {
        item.message = "not an instrument or MIDI track";
        return false;
    }
    if (!t->midi_output.empty()) {
        // Hardware cannot be rendered offline: it has to play.
        item.message = "plays external MIDI (" + juce::String(t->midi_output) + "): print it in real time";
        return false;
    }
    item.file = print_file_for(context_, item.track_name);
    if (!item.file.getParentDirectory().createDirectory()) {
        item.message = "cannot create " + item.file.getParentDirectory().getFullPathName();
        return false;
    }
    auto r = std::make_unique<Render>();
    r->engine = std::make_unique<PlaybackEngine>(context_.session, context_.plugins);
    r->engine->prepare_offline(request_.sample_rate, kBlock);
    r->engine->set_print_tap(PlaybackEngine::PrintTap{item.track_id, request_.include_fx});
    r->skip = r->engine->print_tap_latency();
    const auto& tempo = context_.session.data().tempo;
    const double seconds = tempo.seconds_at(request_.end) - tempo.seconds_at(request_.start);
    r->range_samples = static_cast<std::int64_t>(std::llround(seconds * request_.sample_rate));
    r->samples = r->range_samples + static_cast<std::int64_t>(std::llround(request_.tail_seconds * request_.sample_rate));
    r->temp = item.file.getSiblingFile(item.file.getFileName() + ".partial");
    r->temp.deleteFile();
    if (std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(r->temp);
        static_cast<juce::FileOutputStream*>(stream.get())->openedOk()) {
        const auto options = juce::AudioFormatWriterOptions{}
                                 .withSampleRate(request_.sample_rate)
                                 .withNumChannels(2)
                                 .withBitsPerSample(request_.bit_depth == 16 ? 16 : 24);
        r->writer = juce::WavAudioFormat().createWriterFor(stream, options);
    }
    if (!r->writer) {
        item.message = "could not write " + item.file.getFileName();
        return false;
    }
    r->engine->locate(request_.start);
    r->engine->play();
    // The tap's latency: rendered and dropped, so the print lines up.
    for (std::int64_t left = r->skip; left > 0;) {
        const int n = static_cast<int>(std::min<std::int64_t>(kBlock, left));
        r->engine->render_offline(r->block, n);
        left -= n;
    }
    render_ = std::move(r);
    return true;
}

void PrintJob::finish_track()
{
    auto& item = items_[static_cast<std::size_t>(index_)];
    auto& r = *render_;
    r.engine->stop();
    r.writer.reset();
    item.seconds = static_cast<double>(r.samples) / request_.sample_rate;
    if (item.peak < kSilence) {
        r.temp.deleteFile();
        item.message = "nothing sounds in that range";
    } else if (!r.temp.moveFileTo(item.file)) {
        r.temp.deleteFile();
        item.message = "writing " + item.file.getFileName() + " failed";
    } else {
        item.ok = true;
        item.message = item.file.getFileName();
        if (item.peak > 1.0f)
            item.message << " (clips: " << juce::String(juce::Decibels::gainToDecibels(item.peak), 1) << " dBFS)";
    }
    render_.reset();
}

bool PrintJob::step(double budget_ms)
{
    if (finished_) return false;
    const double until = juce::Time::getMillisecondCounterHiRes() + budget_ms;
    while (juce::Time::getMillisecondCounterHiRes() < until) {
        if (cancelled_) {
            if (render_) {
                render_->engine->stop();
                render_->writer.reset();
                render_->temp.deleteFile();
                render_.reset();
            }
            finished_ = true;
            return false;
        }
        if (index_ >= static_cast<int>(items_.size())) {
            place();
            finished_ = true;
            return false;
        }
        if (!render_) {
            if (!begin_track()) ++index_;
            continue;
        }
        auto& r = *render_;
        auto& item = items_[static_cast<std::size_t>(index_)];
        const int n = static_cast<int>(std::min<std::int64_t>(kBlock, r.samples - r.written));
        if (n <= 0) {
            finish_track();
            ++index_;
            continue;
        }
        r.engine->render_offline(r.block, n);
        const float peak = std::max(r.block.getMagnitude(0, 0, n), r.block.getMagnitude(1, 0, n));
        item.peak = std::max(item.peak, peak);
        if (peak >= kSilence) r.last_loud = r.written + n;
        if (!r.writer->writeFromAudioSampleBuffer(r.block, 0, n)) {
            item.message = "writing " + item.file.getFileName() + " failed";
            item.peak = 0.0f;
            r.written = r.samples;
        }
        r.written += n;
        // Trailing silence after the range is not kept in the clip.
        if (r.written >= r.samples) {
            const auto sound = std::min(r.samples, r.last_loud + request_.sample_rate / 10);
            const auto keep = request_.whole_range ? std::max(r.range_samples, sound) : sound;
            r.samples = keep;  // the clip's length; the file keeps the rest
        }
    }
    return true;
}

void PrintJob::place()
{
    const auto& d = context_.session.data();
    const double start = d.tempo.seconds_at(request_.start);
    const auto folder = context_.session.audio_folder();
    std::vector<std::pair<std::string, project::AudioClip>> prints;
    std::vector<std::size_t> which;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        auto& item = items_[i];
        if (!item.ok) continue;
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(item.file));
        if (!reader) {
            item.ok = false;
            item.message = "cannot read back " + item.file.getFileName();
            continue;
        }
        project::AudioClip c;
        // Inside a saved project the clip names its file relative to it.
        c.file = folder.empty() ? item.file.getFullPathName().toStdString()
                                : ("Audio Files/" + item.file.getFileName()).toStdString();
        c.name = item.file.getFileNameWithoutExtension().toStdString();
        c.start_seconds = start;
        c.channels = static_cast<int>(reader->numChannels);
        c.file_sample_rate = reader->sampleRate;
        const double file_seconds = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
        // The clip ends where the sound does (or the range does, if later).
        c.length = std::min(file_seconds, item.seconds > 0.0 ? item.seconds : file_seconds);
        prints.emplace_back(item.track_id, c);
        which.push_back(i);
    }
    if (prints.empty()) return;
    const auto ids = context_.session.place_prints(prints, request_.include_fx, request_.play_audio);
    for (std::size_t k = 0; k < ids.size() && k < which.size(); ++k) items_[which[k]].clip_id = ids[k];
}

juce::String PrintJob::summary() const
{
    if (cancelled_) return "Print to audio cancelled; nothing was placed";
    int ok = 0;
    juce::StringArray problems;
    for (const auto& item : items_) {
        if (item.ok) ++ok;
        else problems.add(item.track_name.upToFirstOccurrenceOf(juce::String::fromUTF8(" \xe2\x80\x94"), false, false) + ": " +
                          item.message);
    }
    juce::String s = "Printed " + juce::String(ok) + " of " + juce::String(items_.size()) +
                     (items_.size() == 1 ? " track" : " tracks") + " to audio";
    if (ok > 0 && request_.play_audio) s << " - now playing the printed audio";
    if (!problems.isEmpty()) s << " (" << problems.joinIntoString("; ") << ")";
    return s;
}

std::vector<PrintItem> place_live_prints(AppContext& context, const std::vector<PlaybackEngine::LivePrintResult>& results,
                                         Ticks start, bool include_fx, bool play_audio)
{
    std::vector<PrintItem> items;
    std::vector<std::pair<std::string, project::AudioClip>> prints;
    std::vector<std::size_t> which;
    const auto folder = context.session.audio_folder();
    const double at = context.session.data().tempo.seconds_at(start);
    for (const auto& r : results) {
        PrintItem item;
        item.track_id = r.track_id;
        if (const auto* t = context.session.data().find_track(r.track_id)) item.track_name = juce::String(t->name);
        item.file = r.file;
        item.ok = r.ok;
        item.peak = r.peak;
        item.seconds = r.length;
        item.message = r.message;
        if (r.ok) {
            project::AudioClip c;
            const bool inside = !folder.empty() && r.file.isAChildOf(juce::File(juce::String(folder)));
            c.file = inside ? ("Audio Files/" + r.file.getFileName()).toStdString() : r.file.getFullPathName().toStdString();
            c.name = r.file.getFileNameWithoutExtension().toStdString();
            c.start_seconds = at;
            c.source_offset = r.source_offset;
            c.length = r.length;
            c.channels = 2;
            c.file_sample_rate = r.sample_rate;
            prints.emplace_back(r.track_id, c);
            which.push_back(items.size());
        }
        items.push_back(item);
    }
    if (!prints.empty()) {
        const auto ids = context.session.place_prints(prints, include_fx, play_audio);
        for (std::size_t k = 0; k < ids.size() && k < which.size(); ++k) items[which[k]].clip_id = ids[k];
    }
    return items;
}

std::vector<PrintItem> print_to_audio(AppContext& context, const PrintRequest& request)
{
    PrintJob job(context, request);
    while (job.step(1000.0)) {
    }
    return job.items();
}

}  // namespace vcc::desktop
