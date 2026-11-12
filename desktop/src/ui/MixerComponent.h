// The mixer (Main Spec §12): a channel strip per track (fader, pan, mute,
// solo, output routing, meter), bus strips and the master, in a window that
// can be detached to another monitor. Every change goes through Session, so
// the track panel and playback follow it immediately.
#pragma once

#include "ui/AppContext.h"

namespace vcc::desktop {

class MixerComponent final : public juce::Component {
public:
    explicit MixerComponent(AppContext& context);
    ~MixerComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    AppContext& context_;
};

/// The detachable mixer window. Closing hides it; it remembers its place.
class MixerWindow final : public juce::DocumentWindow {
public:
    explicit MixerWindow(AppContext& context);
    void closeButtonPressed() override;
};

}  // namespace vcc::desktop
