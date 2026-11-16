// Offline audio processing for clip edits (Main Spec §9 "reverse, warp/time
// stretch"): written to a new file by the app, so the original recording is
// never touched and undo simply points the clip back at it.
//
// Time stretch is WSOLA (waveform-similarity overlap-add): the input is cut
// into Hann-windowed frames of ~40 ms laid down at a fixed output hop; each
// frame is taken from near its nominal input position, at the offset that
// best continues the waveform already written (cross-correlation), so the
// pitch is kept and the joins do not phase. Good for speech, sustained
// instruments and moderate changes (about 0.5x-2x); transient-heavy material
// at large ratios smears, as WSOLA does. It is our own implementation: no
// third-party licence is involved.
#pragma once

#include <vector>

namespace vcc::audio {

using Channels = std::vector<std::vector<float>>;

/// Returns the audio stretched to `ratio` times its length (2.0 = twice as
/// long, half as fast), pitch unchanged. Every channel moves together (the
/// frame offsets are chosen on their sum), so the stereo image holds.
/// `ratio` is clamped to 0.25..4.
Channels time_stretch(const Channels& in, double ratio, double sample_rate);

/// The audio backwards.
Channels reversed(const Channels& in);

}  // namespace vcc::audio
