#include "audio/Mp3Encoder.h"

#include <mutex>

namespace vcc::desktop {

namespace {

// The LAME API this uses (lame.h), declared here so no headers are needed.
using lame_t = void*;
struct Lame {
    juce::DynamicLibrary lib;
    bool ok = false;
    juce::String where;
    lame_t (*init)() = nullptr;
    int (*close)(lame_t) = nullptr;
    int (*set_in_samplerate)(lame_t, int) = nullptr;
    int (*set_out_samplerate)(lame_t, int) = nullptr;
    int (*set_num_channels)(lame_t, int) = nullptr;
    int (*set_brate)(lame_t, int) = nullptr;
    int (*set_VBR)(lame_t, int) = nullptr;
    int (*set_VBR_q)(lame_t, int) = nullptr;
    int (*set_quality)(lame_t, int) = nullptr;
    int (*set_mode)(lame_t, int) = nullptr;
    int (*set_bWriteVbrTag)(lame_t, int) = nullptr;
    void (*set_write_id3tag_automatic)(lame_t, int) = nullptr;
    int (*init_params)(lame_t) = nullptr;
    int (*encode_float)(lame_t, const float*, const float*, int, unsigned char*, int) = nullptr;
    int (*encode_flush)(lame_t, unsigned char*, int) = nullptr;
    size_t (*get_lametag_frame)(lame_t, unsigned char*, size_t) = nullptr;
    size_t (*get_id3v2_tag)(lame_t, unsigned char*, size_t) = nullptr;
    void (*id3tag_init)(lame_t) = nullptr;
    void (*id3tag_add_v2)(lame_t) = nullptr;
    void (*id3tag_set_title)(lame_t, const char*) = nullptr;
    void (*id3tag_set_comment)(lame_t, const char*) = nullptr;
    const char* (*get_lame_version)() = nullptr;
};

constexpr int kVbrOff = 0, kVbrDefault = 4, kJointStereo = 1;

Lame& lame()
{
    static Lame l;
    static std::once_flag once;
    std::call_once(once, [] {
        juce::StringArray names;
#if JUCE_WINDOWS
        names = {"libmp3lame.dll", "mp3lame.dll"};
#elif JUCE_MAC
        names = {"libmp3lame.0.dylib", "libmp3lame.dylib", "/opt/homebrew/lib/libmp3lame.dylib", "/usr/local/lib/libmp3lame.dylib"};
#else
        names = {"libmp3lame.so.0", "libmp3lame.so"};
#endif
        // Beside the application first (a bundled copy), then the system's.
        const auto here = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
        juce::StringArray tries;
        for (const auto& n : names)
            if (!juce::File::isAbsolutePath(n)) tries.add(here.getChildFile(n).getFullPathName());
        tries.addArray(names);
        for (const auto& name : tries)
            if (l.lib.open(name)) {
                l.where = name;
                break;
            }
        if (l.where.isEmpty()) return;
        auto get = [&](auto& fn, const char* symbol) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(l.lib.getFunction(symbol));
            return fn != nullptr;
        };
        l.ok = get(l.init, "lame_init") && get(l.close, "lame_close") && get(l.set_in_samplerate, "lame_set_in_samplerate") &&
               get(l.set_out_samplerate, "lame_set_out_samplerate") && get(l.set_num_channels, "lame_set_num_channels") &&
               get(l.set_brate, "lame_set_brate") && get(l.set_VBR, "lame_set_VBR") && get(l.set_VBR_q, "lame_set_VBR_q") &&
               get(l.set_quality, "lame_set_quality") && get(l.set_mode, "lame_set_mode") &&
               get(l.set_bWriteVbrTag, "lame_set_bWriteVbrTag") &&
               get(l.set_write_id3tag_automatic, "lame_set_write_id3tag_automatic") && get(l.init_params, "lame_init_params") &&
               get(l.encode_float, "lame_encode_buffer_ieee_float") && get(l.encode_flush, "lame_encode_flush") &&
               get(l.get_lametag_frame, "lame_get_lametag_frame") && get(l.get_id3v2_tag, "lame_get_id3v2_tag") &&
               get(l.id3tag_init, "id3tag_init") && get(l.id3tag_add_v2, "id3tag_add_v2") &&
               get(l.id3tag_set_title, "id3tag_set_title") && get(l.id3tag_set_comment, "id3tag_set_comment") &&
               get(l.get_lame_version, "get_lame_version");
    });
    return l;
}

}  // namespace

bool Mp3Encoder::available(juce::String* why)
{
    const auto& l = lame();
    if (!l.ok && why) {
#if JUCE_WINDOWS
        *why = "MP3 needs LAME (free): put libmp3lame.dll beside VC Composer 2";
#elif JUCE_MAC
        *why = "MP3 needs LAME (free): brew install lame";
#else
        *why = "MP3 needs LAME (free): install libmp3lame (e.g. apt install libmp3lame0)";
#endif
        if (l.where.isNotEmpty()) *why = l.where + " is not a usable LAME library";
    }
    return l.ok;
}

juce::String Mp3Encoder::version()
{
    const auto& l = lame();
    return l.ok ? juce::String(l.get_lame_version()) : juce::String();
}

Mp3Encoder::Mp3Encoder() = default;

Mp3Encoder::~Mp3Encoder()
{
    if (gf_) lame().close(gf_);
}

bool Mp3Encoder::open(const juce::File& file, int sample_rate, int kbps, const juce::String& title, juce::String& error)
{
    auto& l = lame();
    if (!available(&error)) return false;
    if (sample_rate != 44100 && sample_rate != 48000) {
        error = "MP3 masters are 44.1 or 48 kHz";
        return false;
    }
    gf_ = l.init();
    if (!gf_) {
        error = "LAME would not start";
        return false;
    }
    l.set_in_samplerate(gf_, sample_rate);
    l.set_out_samplerate(gf_, sample_rate);
    l.set_num_channels(gf_, 2);
    l.set_mode(gf_, kJointStereo);
    l.set_quality(gf_, 2);  // high quality (0 best, 9 fastest)
    if (kbps > 0) {
        l.set_VBR(gf_, kVbrOff);
        l.set_brate(gf_, kbps);
    } else {
        l.set_VBR(gf_, kVbrDefault);
        l.set_VBR_q(gf_, 0);  // V0
    }
    l.set_bWriteVbrTag(gf_, 1);
    // The ID3v2 tag is written here, so the header frame's place is known.
    l.set_write_id3tag_automatic(gf_, 0);
    l.id3tag_init(gf_);
    l.id3tag_add_v2(gf_);
    if (title.isNotEmpty()) l.id3tag_set_title(gf_, title.toRawUTF8());
    l.id3tag_set_comment(gf_, "VC Composer 2");
    if (l.init_params(gf_) < 0) {
        error = "LAME refused those settings (" + juce::String(kbps) + " kbps, " + juce::String(sample_rate) + " Hz)";
        return false;
    }
    out_ = std::make_unique<juce::FileOutputStream>(file);
    if (!out_->openedOk() || !out_->setPosition(0) || !out_->truncate().wasOk()) {
        error = "cannot write " + file.getFullPathName();
        return false;
    }
    buffer_.resize(1 << 16);
    const auto tag = l.get_id3v2_tag(gf_, buffer_.data(), buffer_.size());
    if (tag > 0 && tag <= buffer_.size()) out_->write(buffer_.data(), tag);
    first_frame_ = out_->getPosition();
    return true;
}

bool Mp3Encoder::write(const float* left, const float* right, int n)
{
    if (!gf_ || !out_ || failed_) return false;
    const auto need = static_cast<std::size_t>(n) * 5 / 4 + 7200;
    if (buffer_.size() < need) buffer_.resize(need);
    const int bytes = lame().encode_float(gf_, left, right, n, buffer_.data(), static_cast<int>(buffer_.size()));
    if (bytes < 0 || (bytes > 0 && !out_->write(buffer_.data(), static_cast<std::size_t>(bytes)))) failed_ = true;
    return !failed_;
}

bool Mp3Encoder::close()
{
    if (!gf_ || !out_) return false;
    auto& l = lame();
    const int bytes = l.encode_flush(gf_, buffer_.data(), static_cast<int>(buffer_.size()));
    if (bytes < 0 || (bytes > 0 && !out_->write(buffer_.data(), static_cast<std::size_t>(bytes)))) failed_ = true;
    // The LAME/Xing header frame, over the placeholder LAME left at the first frame.
    const auto tag = l.get_lametag_frame(gf_, buffer_.data(), buffer_.size());
    if (!failed_ && tag > 0 && tag <= buffer_.size()) {
        const auto end = out_->getPosition();
        if (out_->setPosition(first_frame_)) out_->write(buffer_.data(), tag);
        out_->setPosition(end);
    }
    out_->flush();
    if (out_->getStatus().failed()) failed_ = true;
    out_.reset();
    l.close(gf_);
    gf_ = nullptr;
    return !failed_;
}

}  // namespace vcc::desktop
