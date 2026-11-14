#include "audio/MasterExport.h"

#include <algorithm>
#include <cmath>

#include <juce_audio_formats/juce_audio_formats.h>

#include "audio/PlaybackEngine.h"
#include "ui/AppContext.h"

namespace vcc::desktop {

namespace {
constexpr int kBlock = 512;
}

MasterExportJob::MasterExportJob(AppContext& context, MasterExportRequest request)
    : context_(context), request_(std::move(request)), random_(0x5eed)
{
    if (request_.end <= request_.start) request_.end = request_.start + kTicksPerWhole;
    request_.bit_depth = request_.bit_depth == 16 ? 16 : 24;
}

MasterExportJob::~MasterExportJob()
{
    if (writer_) {
        writer_.reset();
        temp_.deleteFile();
    }
}

double MasterExportJob::progress() const
{
    if (finished_) return 1.0;
    return samples_ > 0 ? static_cast<double>(written_) / static_cast<double>(samples_) : 0.0;
}

bool MasterExportJob::begin()
{
    started_ = true;
    engine_ = std::make_unique<PlaybackEngine>(context_.session, context_.plugins);
    engine_->prepare_offline(request_.sample_rate, kBlock);
    engine_->set_export_mode(true);
    const auto& tempo = context_.session.data().tempo;
    const double seconds = tempo.seconds_at(request_.end) - tempo.seconds_at(request_.start) + request_.tail_seconds;
    samples_ = static_cast<std::int64_t>(std::llround(std::max(0.0, seconds) * request_.sample_rate));
    analyser_.prepare(request_.sample_rate, 2);
    meter_.reset();
    if (request_.file != juce::File()) {
        if (!request_.file.getParentDirectory().createDirectory()) {
            result_.message = "cannot create " + request_.file.getParentDirectory().getFullPathName();
            return false;
        }
        temp_ = request_.file.getSiblingFile(request_.file.getFileName() + ".partial");
        temp_.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(temp_);
        if (!static_cast<juce::FileOutputStream*>(stream.get())->openedOk()) {
            result_.message = "cannot write to " + request_.file.getParentDirectory().getFullPathName();
            return false;
        }
        const auto options = juce::AudioFormatWriterOptions{}
                                 .withSampleRate(request_.sample_rate)
                                 .withNumChannels(2)
                                 .withBitsPerSample(request_.bit_depth);
        if (request_.format == MasterExportRequest::Format::Flac) writer_ = juce::FlacAudioFormat().createWriterFor(stream, options);
        else writer_ = juce::WavAudioFormat().createWriterFor(stream, options);
        if (!writer_) {
            result_.message = "the format cannot write that (" + juce::String(request_.bit_depth) + "-bit, " +
                              juce::String(request_.sample_rate) + " Hz)";
            return false;
        }
    }
    engine_->locate(request_.start);
    engine_->play();
    return true;
}

bool MasterExportJob::step(double budget_ms)
{
    if (finished_) return false;
    if (!started_ && !begin()) {
        finish();
        return false;
    }
    juce::AudioBuffer<float> block(2, kBlock);
    const double until = juce::Time::getMillisecondCounterHiRes() + budget_ms;
    // TPDF dither: two uniform sources of one LSB each, before the writer
    // rounds to 16 bits.
    const float lsb = request_.dither && request_.bit_depth == 16 ? 1.0f / 32768.0f : 0.0f;
    while (juce::Time::getMillisecondCounterHiRes() < until) {
        if (cancelled_) {
            result_.cancelled = true;
            finish();
            return false;
        }
        const int n = static_cast<int>(std::min<std::int64_t>(kBlock, samples_ - written_));
        if (n <= 0) {
            finish();
            return false;
        }
        engine_->render_offline(block, n);
        const float* ch[] = {block.getReadPointer(0), block.getReadPointer(1)};
        analyser_.process(ch, n, [this](const audio::LoudnessBlock& b) { meter_.add(b); });
        if (writer_) {
            if (lsb > 0.0f)
                for (int c = 0; c < 2; ++c) {
                    float* d = block.getWritePointer(c);
                    for (int i = 0; i < n; ++i) d[i] += (random_.nextFloat() - random_.nextFloat()) * lsb;
                }
            if (!writer_->writeFromAudioSampleBuffer(block, 0, n)) {
                result_.message = "writing " + request_.file.getFileName() + " failed (disk full?)";
                finish();
                return false;
            }
        }
        written_ += n;
    }
    return true;
}

void MasterExportJob::finish()
{
    finished_ = true;
    if (engine_) engine_->stop();
    result_.integrated = meter_.integrated();
    result_.true_peak = meter_.true_peak_db();
    result_.sample_peak = meter_.sample_peak_db();
    result_.max_momentary = meter_.max_momentary();
    result_.seconds = static_cast<double>(written_) / request_.sample_rate;
    const bool complete = written_ >= samples_ && samples_ > 0 && !result_.cancelled && result_.message.isEmpty();
    if (writer_) {
        writer_.reset();
        if (complete && temp_.moveFileTo(request_.file)) {
            result_.ok = true;
        } else {
            temp_.deleteFile();
            if (result_.message.isEmpty()) result_.message = result_.cancelled ? "cancelled" : "could not finish the file";
        }
    } else {
        result_.ok = complete;
    }
    engine_.reset();
    if (result_.ok && result_.message.isEmpty()) {
        result_.message = (request_.file == juce::File() ? juce::String("Analysed") : request_.file.getFileName()) + ": " +
                          (result_.integrated <= audio::LoudnessMeter::kSilence ? juce::String("silent")
                                                                                  : juce::String(result_.integrated, 1) + " LUFS, " +
                                                                                        juce::String(result_.true_peak, 1) + " dBTP");
    }
}

MasterExportResult export_master(AppContext& context, const MasterExportRequest& request)
{
    MasterExportJob job(context, request);
    while (job.step(1000.0)) {
    }
    return job.result();
}

}  // namespace vcc::desktop
