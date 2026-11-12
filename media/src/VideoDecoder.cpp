#include "vcc/media/VideoDecoder.h"

#include <algorithm>
#include <cmath>

#if VCC_WITH_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#endif

namespace vcc::media {

#if VCC_WITH_FFMPEG

bool available() noexcept { return true; }

struct VideoDecoder::Impl {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* decoded = nullptr;
    AVFrame* held = nullptr;       // last decoded frame with index <= target
    std::int64_t held_index = -1;
    SwsContext* sws = nullptr;
    int sws_w = 0, sws_h = 0, sws_src_w = 0, sws_src_h = 0, sws_src_fmt = -1;
    int stream = -1;
    AVRational time_base{1, 1};
    std::int64_t start_pts = 0;
    bool eof = false;
    VideoInfo info;
    Frame cache;  // last converted frame

    ~Impl() { close(); }

    void close()
    {
        if (sws) sws_freeContext(sws);
        sws = nullptr;
        av_frame_free(&decoded);
        av_frame_free(&held);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        if (format) avformat_close_input(&format);
        format = nullptr;
        stream = -1;
        held_index = -1;
        cache = {};
        info = {};
    }

    std::int64_t index_of(std::int64_t pts) const
    {
        const double seconds = static_cast<double>(pts - start_pts) * av_q2d(time_base);
        return static_cast<std::int64_t>(std::llround(seconds * info.fps));
    }

    std::int64_t pts_of(std::int64_t index) const
    {
        const double seconds = static_cast<double>(index) / info.fps;
        return start_pts + static_cast<std::int64_t>(std::llround(seconds / av_q2d(time_base)));
    }

    void seek(std::int64_t index)
    {
        av_seek_frame(format, stream, pts_of(std::max<std::int64_t>(0, index)), AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codec);
        held_index = -1;
        eof = false;
    }

    /// Decodes forward until a frame with index >= target is reached; leaves
    /// the best frame (index <= target, or the first one past a gap) in `held`.
    bool decode_to(std::int64_t target)
    {
        while (true) {
            const int r = avcodec_receive_frame(codec, decoded);
            if (r == 0) {
                const std::int64_t pts = decoded->best_effort_timestamp != AV_NOPTS_VALUE ? decoded->best_effort_timestamp
                                                                                         : decoded->pts;
                const std::int64_t idx = index_of(pts);
                if (idx <= target || held_index < 0) {
                    av_frame_unref(held);
                    av_frame_move_ref(held, decoded);
                    held_index = idx;
                }
                av_frame_unref(decoded);
                if (idx >= target) return true;
                continue;
            }
            if (r == AVERROR_EOF) {
                eof = true;
                return held_index >= 0;
            }
            if (r != AVERROR(EAGAIN)) return false;
            // Feed another packet of our stream (or drain at end of file).
            while (true) {
                const int rr = av_read_frame(format, packet);
                if (rr < 0) {
                    avcodec_send_packet(codec, nullptr);
                    break;
                }
                const bool ours = packet->stream_index == stream;
                if (ours) avcodec_send_packet(codec, packet);
                av_packet_unref(packet);
                if (ours) break;
            }
        }
    }

    bool convert(const AVFrame* f, std::int64_t index, int max_width, Frame& out)
    {
        int w = f->width, h = f->height;
        if (max_width > 0 && w > max_width) {
            h = std::max(1, static_cast<int>(std::lround(static_cast<double>(h) * max_width / w)));
            w = max_width;
        }
        if (cache.index == index && cache.width == w && cache.height == h) {
            out = cache;
            return true;
        }
        if (!sws || sws_w != w || sws_h != h || sws_src_w != f->width || sws_src_h != f->height || sws_src_fmt != f->format) {
            if (sws) sws_freeContext(sws);
            sws = sws_getContext(f->width, f->height, static_cast<AVPixelFormat>(f->format), w, h, AV_PIX_FMT_RGBA,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
            sws_w = w;
            sws_h = h;
            sws_src_w = f->width;
            sws_src_h = f->height;
            sws_src_fmt = f->format;
        }
        if (!sws) return false;
        cache.width = w;
        cache.height = h;
        cache.index = index;
        cache.rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
        std::uint8_t* dst[4] = {cache.rgba.data(), nullptr, nullptr, nullptr};
        int stride[4] = {w * 4, 0, 0, 0};
        sws_scale(sws, f->data, f->linesize, 0, f->height, dst, stride);
        out = cache;
        return true;
    }

    bool get(std::int64_t index, int max_width, Frame& out)
    {
        if (!format || index < 0 || (info.frame_count > 0 && index >= info.frame_count)) return false;
        if (held_index >= 0 && held_index == index) return convert(held, index, max_width, out);
        const bool jump = held_index < 0 || index < held_index || index > held_index + static_cast<std::int64_t>(2 * info.fps);
        if (jump) seek(index);
        else if (eof) return false;
        if (!decode_to(index)) return false;
        return convert(held, held_index, max_width, out);
    }
};

VideoDecoder::VideoDecoder() : impl_(std::make_unique<Impl>()) {}
VideoDecoder::~VideoDecoder() = default;

bool VideoDecoder::open(const std::string& path, std::string* error)
{
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        impl_->close();
        return false;
    };
    impl_->close();
    Impl& m = *impl_;
    if (avformat_open_input(&m.format, path.c_str(), nullptr, nullptr) < 0) return fail("the movie could not be opened");
    if (avformat_find_stream_info(m.format, nullptr) < 0) return fail("the movie's streams could not be read");
    m.stream = av_find_best_stream(m.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m.stream < 0) return fail("the file has no picture");
    AVStream* st = m.format->streams[m.stream];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) return fail("this build cannot decode the movie's video codec");
    m.codec = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(m.codec, st->codecpar);
    m.codec->thread_count = 0;  // automatic
    if (avcodec_open2(m.codec, dec, nullptr) < 0) return fail("the video decoder could not start");
    m.packet = av_packet_alloc();
    m.decoded = av_frame_alloc();
    m.held = av_frame_alloc();
    m.time_base = st->time_base;
    m.start_pts = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;

    const AVRational rate = av_guess_frame_rate(m.format, st, nullptr);
    if (rate.num <= 0 || rate.den <= 0) return fail("the movie's frame rate is unknown");
    m.info.fps_num = rate.num;
    m.info.fps_den = rate.den;
    m.info.fps = av_q2d(rate);
    m.info.width = st->codecpar->width;
    m.info.height = st->codecpar->height;
    m.info.codec = dec->name;
    if (st->duration != AV_NOPTS_VALUE) m.info.duration_seconds = static_cast<double>(st->duration) * av_q2d(st->time_base);
    else if (m.format->duration != AV_NOPTS_VALUE) m.info.duration_seconds = static_cast<double>(m.format->duration) / AV_TIME_BASE;
    m.info.frame_count = st->nb_frames > 0 ? st->nb_frames
                                           : static_cast<std::int64_t>(std::llround(m.info.duration_seconds * m.info.fps));
    if (const AVDictionaryEntry* tc = av_dict_get(st->metadata, "timecode", nullptr, 0)) m.info.timecode = tc->value;
    else if (const AVDictionaryEntry* tc2 = av_dict_get(m.format->metadata, "timecode", nullptr, 0)) m.info.timecode = tc2->value;
    else
        for (unsigned i = 0; i < m.format->nb_streams; ++i)
            if (const AVDictionaryEntry* t = av_dict_get(m.format->streams[i]->metadata, "timecode", nullptr, 0)) {
                m.info.timecode = t->value;
                break;
            }
    m.info.has_audio = av_find_best_stream(m.format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) >= 0;
    return true;
}

void VideoDecoder::close() { impl_->close(); }
bool VideoDecoder::is_open() const noexcept { return impl_->format != nullptr; }
const VideoInfo& VideoDecoder::info() const noexcept { return impl_->info; }

bool VideoDecoder::frame(std::int64_t index, Frame& out, int max_width) { return impl_->get(index, max_width, out); }

bool VideoDecoder::frame_at(double seconds, Frame& out, int max_width)
{
    if (!is_open() || seconds < 0.0) return false;
    const auto index = static_cast<std::int64_t>(std::floor(seconds * impl_->info.fps + 1e-6));
    return impl_->get(index, max_width, out);
}

std::optional<std::uint64_t> VideoDecoder::signature(std::int64_t index)
{
    Frame f;
    if (!impl_->get(index, 0, f)) return std::nullopt;
    // Average hash over an 8x8 grid of luma.
    double cells[64] = {};
    double sum = 0;
    for (int gy = 0; gy < 8; ++gy)
        for (int gx = 0; gx < 8; ++gx) {
            const int x0 = gx * f.width / 8, x1 = std::max(x0 + 1, (gx + 1) * f.width / 8);
            const int y0 = gy * f.height / 8, y1 = std::max(y0 + 1, (gy + 1) * f.height / 8);
            double acc = 0;
            int n = 0;
            for (int y = y0; y < y1; y += 2)
                for (int x = x0; x < x1; x += 2) {
                    const std::uint8_t* p = &f.rgba[(static_cast<std::size_t>(y) * f.width + x) * 4];
                    acc += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
                    ++n;
                }
            cells[gy * 8 + gx] = n ? acc / n : 0;
            sum += cells[gy * 8 + gx];
        }
    const double mean = sum / 64.0;
    std::uint64_t h = 0;
    for (int i = 0; i < 64; ++i)
        if (cells[i] > mean) h |= (1ULL << i);
    // Flat frames (all one level) hash to their brightness band instead of zero.
    if (h == 0) h = static_cast<std::uint64_t>(mean) * 0x0101010101010101ULL;
    return h;
}

#else  // no FFmpeg

bool available() noexcept { return false; }

struct VideoDecoder::Impl {
    VideoInfo info;
};

VideoDecoder::VideoDecoder() : impl_(std::make_unique<Impl>()) {}
VideoDecoder::~VideoDecoder() = default;
bool VideoDecoder::open(const std::string&, std::string* error)
{
    if (error) *error = "this build of VC Composer 2 has no video decoder (built without FFmpeg)";
    return false;
}
void VideoDecoder::close() {}
bool VideoDecoder::is_open() const noexcept { return false; }
const VideoInfo& VideoDecoder::info() const noexcept { return impl_->info; }
bool VideoDecoder::frame(std::int64_t, Frame&, int) { return false; }
bool VideoDecoder::frame_at(double, Frame&, int) { return false; }
std::optional<std::uint64_t> VideoDecoder::signature(std::int64_t) { return std::nullopt; }

#endif

}  // namespace vcc::media
