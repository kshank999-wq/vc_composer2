// MP3 encoding for Export master, through LAME (LGPL), loaded at run time.
//
// The app is not linked against LAME: the library is opened when an MP3 is
// exported (libmp3lame.so.0 / libmp3lame.0.dylib / libmp3lame.dll, beside
// the application or where the system keeps libraries), so it stays a
// separately replaceable library as the LGPL asks and the build needs no
// headers. Without it, MP3 export says how to get it and the rest works.
// The MP3 patents expired in 2017; LAME is free.
#pragma once

#include <memory>
#include <vector>

#include <juce_core/juce_core.h>

namespace vcc::desktop {

class Mp3Encoder {
public:
    /// Whether LAME could be loaded; `why` says what to do when it cannot.
    static bool available(juce::String* why = nullptr);
    /// The LAME version, e.g. "3.100", or empty.
    static juce::String version();

    Mp3Encoder();
    ~Mp3Encoder();
    Mp3Encoder(const Mp3Encoder&) = delete;
    Mp3Encoder& operator=(const Mp3Encoder&) = delete;

    /// `kbps`: a constant bit rate (320, 256, 192...), or 0 for VBR V0 (the
    /// highest-quality variable rate). 44.1 or 48 kHz. `title` goes in an
    /// ID3v2 tag.
    bool open(const juce::File& file, int sample_rate, int kbps, const juce::String& title, juce::String& error);
    /// Stereo float samples (-1..1).
    bool write(const float* left, const float* right, int n);
    /// Flushes the encoder and writes the LAME/Xing header frame (exact
    /// length and gapless playback). True when the file is complete.
    bool close();

private:
    void* gf_ = nullptr;
    std::unique_ptr<juce::FileOutputStream> out_;
    std::vector<unsigned char> buffer_;
    juce::int64 first_frame_ = 0;  ///< Where the header frame goes (after the ID3 tag).
    bool failed_ = false;
};

}  // namespace vcc::desktop
