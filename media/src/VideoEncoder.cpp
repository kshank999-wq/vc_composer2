#include "vcc/media/VideoEncoder.h"

#include <algorithm>
#include <cstring>
#include <vector>

#if VCC_WITH_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif

namespace vcc::media {

#if VCC_WITH_FFMPEG

bool encoding_available() noexcept { return true; }

struct VideoEncoder::Impl {
    AVFormatContext* format = nullptr;
    AVCodecContext* video = nullptr;
    AVCodecContext* audio = nullptr;
    AVStream* video_stream = nullptr;
    AVStream* audio_stream = nullptr;
    AVFrame* picture = nullptr;
    AVFrame* sound = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* sws = nullptr;
    int sws_src_w = 0, sws_src_h = 0;
    EncodeSettings settings;
    std::string codec_name;
    std::string path;
    std::int64_t frames = 0;
    std::int64_t audio_samples = 0;
    std::vector<float> pending;  // interleaved, waiting for a full AAC frame
    bool header_written = false;
    bool failed = false;
    std::string error;

    ~Impl() { release(); }

    void release()
    {
        if (sws) sws_freeContext(sws);
        sws = nullptr;
        av_frame_free(&picture);
        av_frame_free(&sound);
        av_packet_free(&packet);
        avcodec_free_context(&video);
        avcodec_free_context(&audio);
        if (format) {
            if (format->pb) avio_closep(&format->pb);
            avformat_free_context(format);
        }
        format = nullptr;
    }

    bool fail(const std::string& why)
    {
        failed = true;
        if (error.empty()) error = why;
        return false;
    }

    bool drain(AVCodecContext* codec, AVStream* stream)
    {
        for (;;) {
            const int r = avcodec_receive_packet(codec, packet);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
            if (r < 0) return fail("the encoder failed");
            av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
            packet->stream_index = stream->index;
            if (av_interleaved_write_frame(format, packet) < 0) return fail("writing the movie failed");
        }
    }

    bool send(AVCodecContext* codec, AVStream* stream, AVFrame* frame)
    {
        if (avcodec_send_frame(codec, frame) < 0) return fail("the encoder refused a frame");
        return drain(codec, stream);
    }

    bool open_video(const AVCodec* codec)
    {
        avcodec_free_context(&video);
        video = avcodec_alloc_context3(codec);
        if (!video) return false;
        video->width = settings.width;
        video->height = settings.height;
        video->time_base = AVRational{settings.fps_den, settings.fps_num};
        video->framerate = AVRational{settings.fps_num, settings.fps_den};
        video->gop_size = 12;
        video->bit_rate = settings.video_bitrate;
        // The first pixel format the encoder takes, preferring 4:2:0.
        AVPixelFormat fmt = AV_PIX_FMT_YUV420P;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
        const void* formats = nullptr;
        int count = 0;
        if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &formats, &count) >= 0 && formats &&
            count > 0) {
            const auto* list = static_cast<const AVPixelFormat*>(formats);
            fmt = list[0];
            for (int i = 0; i < count; ++i)
                if (list[i] == AV_PIX_FMT_YUV420P) fmt = AV_PIX_FMT_YUV420P;
        }
#else
        if (codec->pix_fmts) {
            fmt = codec->pix_fmts[0];
            for (const AVPixelFormat* p = codec->pix_fmts; *p != AV_PIX_FMT_NONE; ++p)
                if (*p == AV_PIX_FMT_YUV420P) fmt = AV_PIX_FMT_YUV420P;
        }
#endif
        video->pix_fmt = fmt;
        if (std::strcmp(codec->name, "mpeg4") == 0) {
            // High fixed quality: a review copy, not a delivery master.
            video->flags |= AV_CODEC_FLAG_QSCALE;
            video->global_quality = FF_QP2LAMBDA * 3;
            video->max_b_frames = 0;
        }
        if (format->oformat->flags & AVFMT_GLOBALHEADER) video->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(video, codec, nullptr) < 0) {
            avcodec_free_context(&video);
            return false;
        }
        return true;
    }

    bool open(const std::string& file, const EncodeSettings& s)
    {
        settings = s;
        // FFmpeg's encoders print statistics at the default level.
        if (av_log_get_level() > AV_LOG_ERROR) av_log_set_level(AV_LOG_ERROR);
        settings.width = std::max(2, s.width & ~1);
        settings.height = std::max(2, s.height & ~1);
        if (settings.fps_num <= 0 || settings.fps_den <= 0) return fail("no frame rate");
        if (settings.channels < 1 || settings.channels > 2) return fail("mono or stereo audio only");
        path = file;
        if (avformat_alloc_output_context2(&format, nullptr, nullptr, file.c_str()) < 0 || !format)
            return fail("that movie type cannot be written (use .mov or .mp4)");

        for (const char* name : {"h264_videotoolbox", "h264_mf", "mpeg4"}) {
            const AVCodec* codec = avcodec_find_encoder_by_name(name);
            if (codec && open_video(codec)) {
                codec_name = name;
                break;
            }
        }
        if (!video) return fail("no video encoder available");
        video_stream = avformat_new_stream(format, nullptr);
        avcodec_parameters_from_context(video_stream->codecpar, video);
        video_stream->time_base = video->time_base;
        video_stream->avg_frame_rate = video->framerate;
        if (!settings.timecode.empty()) av_dict_set(&video_stream->metadata, "timecode", settings.timecode.c_str(), 0);

        const AVCodec* aac = avcodec_find_encoder_by_name("aac");
        if (!aac) return fail("no AAC encoder available");
        audio = avcodec_alloc_context3(aac);
        audio->sample_fmt = AV_SAMPLE_FMT_FLTP;
        audio->sample_rate = settings.sample_rate;
        av_channel_layout_default(&audio->ch_layout, settings.channels);
        audio->bit_rate = 256000;
        audio->time_base = AVRational{1, settings.sample_rate};
        if (format->oformat->flags & AVFMT_GLOBALHEADER) audio->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(audio, aac, nullptr) < 0) return fail("the AAC encoder could not start");
        audio_stream = avformat_new_stream(format, nullptr);
        avcodec_parameters_from_context(audio_stream->codecpar, audio);
        audio_stream->time_base = audio->time_base;

        if (!(format->oformat->flags & AVFMT_NOFILE) && avio_open(&format->pb, file.c_str(), AVIO_FLAG_WRITE) < 0)
            return fail("could not write to that folder");
        AVDictionary* options = nullptr;
        if (!settings.timecode.empty()) av_dict_set(&options, "write_tmcd", "1", 0);
        const int r = avformat_write_header(format, &options);
        av_dict_free(&options);
        if (r < 0) return fail("the movie header could not be written");
        header_written = true;

        packet = av_packet_alloc();
        picture = av_frame_alloc();
        picture->format = video->pix_fmt;
        picture->width = settings.width;
        picture->height = settings.height;
        if (av_frame_get_buffer(picture, 0) < 0) return fail("out of memory");
        sound = av_frame_alloc();
        sound->format = AV_SAMPLE_FMT_FLTP;
        sound->sample_rate = settings.sample_rate;
        av_channel_layout_copy(&sound->ch_layout, &audio->ch_layout);
        sound->nb_samples = audio->frame_size > 0 ? audio->frame_size : 1024;
        if (av_frame_get_buffer(sound, 0) < 0) return fail("out of memory");
        return true;
    }

    bool add_frame(const Frame& f)
    {
        if (failed || !video) return false;
        if (f.width <= 0 || f.height <= 0 || f.rgba.size() < static_cast<std::size_t>(f.width) * f.height * 4)
            return fail("a frame had no picture");
        if (!sws || sws_src_w != f.width || sws_src_h != f.height) {
            if (sws) sws_freeContext(sws);
            sws = sws_getContext(f.width, f.height, AV_PIX_FMT_RGBA, settings.width, settings.height, video->pix_fmt,
                                 SWS_BICUBIC, nullptr, nullptr, nullptr);
            sws_src_w = f.width;
            sws_src_h = f.height;
            if (!sws) return fail("the frame could not be converted");
        }
        if (av_frame_make_writable(picture) < 0) return fail("out of memory");
        const std::uint8_t* src[1] = {f.rgba.data()};
        const int stride[1] = {f.width * 4};
        sws_scale(sws, src, stride, 0, f.height, picture->data, picture->linesize);
        picture->pts = frames++;
        return send(video, video_stream, picture);
    }

    bool flush_audio(bool final)
    {
        const int ch = settings.channels;
        const int size = sound->nb_samples;
        std::size_t at = 0;
        while (pending.size() - at >= static_cast<std::size_t>(size * ch) || (final && pending.size() > at)) {
            const int n = static_cast<int>(std::min<std::size_t>(size, (pending.size() - at) / ch));
            if (av_frame_make_writable(sound) < 0) return fail("out of memory");
            for (int c = 0; c < ch; ++c) {
                auto* out = reinterpret_cast<float*>(sound->data[c]);
                for (int i = 0; i < size; ++i) out[i] = i < n ? pending[at + static_cast<std::size_t>(i * ch + c)] : 0.0f;
            }
            sound->pts = audio_samples;
            audio_samples += size;
            at += static_cast<std::size_t>(n * ch);
            if (!send(audio, audio_stream, sound)) return false;
            if (n < size) break;
        }
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(at));
        return true;
    }

    bool add_audio(const float* interleaved, int count)
    {
        if (failed || !audio) return false;
        pending.insert(pending.end(), interleaved, interleaved + static_cast<std::ptrdiff_t>(count) * settings.channels);
        return flush_audio(false);
    }

    bool finish()
    {
        if (!header_written) return false;
        if (!failed) flush_audio(true);
        if (!failed) send(video, video_stream, nullptr);
        if (!failed) send(audio, audio_stream, nullptr);
        if (av_write_trailer(format) < 0) fail("the movie could not be finished");
        header_written = false;
        release();
        return !failed;
    }
};

VideoEncoder::VideoEncoder() : impl_(std::make_unique<Impl>()) {}

VideoEncoder::~VideoEncoder()
{
    if (impl_->header_written) impl_->finish();
}

bool VideoEncoder::open(const std::string& path, const EncodeSettings& settings, std::string* error)
{
    impl_ = std::make_unique<Impl>();
    const bool ok = impl_->open(path, settings);
    if (!ok) {
        if (error) *error = impl_->error;
        impl_->release();
    }
    return ok;
}

bool VideoEncoder::add_frame(const Frame& frame) { return impl_->add_frame(frame); }
bool VideoEncoder::add_audio(const float* interleaved, int frames) { return impl_->add_audio(interleaved, frames); }

bool VideoEncoder::finish(std::string* error)
{
    const bool ok = impl_->finish();
    if (!ok && error) *error = impl_->error.empty() ? "the movie was not opened" : impl_->error;
    return ok;
}

const std::string& VideoEncoder::video_codec() const noexcept { return impl_->codec_name; }
std::int64_t VideoEncoder::frames_written() const noexcept { return impl_->frames; }

#else

bool encoding_available() noexcept { return false; }

struct VideoEncoder::Impl {
    std::string codec_name;
};

VideoEncoder::VideoEncoder() : impl_(std::make_unique<Impl>()) {}
VideoEncoder::~VideoEncoder() = default;
bool VideoEncoder::open(const std::string&, const EncodeSettings&, std::string* error)
{
    if (error) *error = "this build cannot write movies (no FFmpeg)";
    return false;
}
bool VideoEncoder::add_frame(const Frame&) { return false; }
bool VideoEncoder::add_audio(const float*, int) { return false; }
bool VideoEncoder::finish(std::string* error)
{
    if (error) *error = "this build cannot write movies (no FFmpeg)";
    return false;
}
const std::string& VideoEncoder::video_codec() const noexcept { return impl_->codec_name; }
std::int64_t VideoEncoder::frames_written() const noexcept { return 0; }

#endif

}  // namespace vcc::media
