// The picture window (Main Spec §13): the active cut locked to the transport,
// with timecode burn-in, cue and hit overlays, streamers and punches, and
// the tools for spotting against picture. It can be detached to another
// display and made full screen.
#pragma once

#include <memory>

#include "ui/AppContext.h"
#include "vcc/media/VideoDecoder.h"

namespace vcc::desktop {

/// Opens (or brings forward) the picture window.
void show_video_window(AppContext& context);
/// Destroys the picture window (before the session it watches goes away).
void close_video_window();
/// The window, when open (for screenshots and tests).
juce::DocumentWindow* video_window_if_open();

/// Imports a movie as a new cut, reading its rate, size and source timecode.
/// Returns false with a reason when the file cannot be decoded.
bool import_picture_file(AppContext& context, const juce::File& file, juce::String& error);

/// Maps a decoded frame rate (and drop-frame flag) to a timecode rate.
FrameRate frame_rate_for(double fps, bool drop_frame);

/// Resolves a picture's stored path, falling back to the same file name
/// beside the project when the original location is gone (relink).
juce::File resolve_picture_file(const AppContext& context, const project::PictureVersion& picture);

}  // namespace vcc::desktop
