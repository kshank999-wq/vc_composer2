// The mixer (Main Spec §12), laid out like a large-format console: a strip
// per track (input, filters, EQ, dynamics, inserts, sends, routing, pan,
// buttons, fader and meter), group and FX bus strips and the master section,
// in a window that can be detached to another monitor. Every change goes
// through Session, so the track panel and playback follow it immediately.
// The console itself lives in ui/console/.
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

/// The detachable mixer window. Closing hides it; it remembers its place
/// (across sessions too).
class MixerWindow final : public juce::DocumentWindow {
public:
    explicit MixerWindow(AppContext& context);
    ~MixerWindow() override;
    void closeButtonPressed() override;
    void visibilityChanged() override;

private:
    void save_state();
    bool placed_ = false;
};

}  // namespace vcc::desktop
