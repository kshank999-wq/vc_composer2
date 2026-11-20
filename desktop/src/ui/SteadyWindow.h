// Floating windows that redraw often (the popped-out mixer, the racks with
// their meters and analyser): drawn with the software renderer. On Windows,
// JUCE's default Direct2D renderer can leave grey patches and flicker in a
// second top-level window whose parts repaint many times a second; the
// software renderer draws them steadily. No change where it is the only one.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace vcc::desktop {

/// Call once the window is on screen (it has a peer).
inline void use_steady_renderer(juce::Component& window)
{
    auto* peer = window.getPeer();
    if (peer == nullptr) return;
    const auto engines = peer->getAvailableRenderingEngines();
    const int software = engines.indexOf("Software Renderer");
    if (software >= 0 && engines.size() > 1 && peer->getCurrentRenderingEngine() != software)
        peer->setCurrentRenderingEngine(software);
}

}  // namespace vcc::desktop
