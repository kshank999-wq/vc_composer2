// Mix > Groups, VCAs and Monitor... (Main Spec §12):
//
//   - VCAs: a fader and mute per VCA that trim its member tracks without
//     summing their audio; members picked from a checklist.
//   - Mix groups: members' faders move together (relative), and mutes,
//     solos and record arms as linked; a group can be switched off.
//   - Monitor: what the speakers hear - Dim (by a set amount), Mono, Mute,
//     and the output pair the mix plays on. Renders and exports never hear
//     it. Remembered between sessions.
//   - Cue mixes: headphone buses fed by pre-fader sends, each to its own
//     device outputs, with a send level per track.
//
// Mix > Bounce in Place renders the selected track (an audio track's clips
// through its channel strip and inserts, or an instrument's sound) onto it,
// the clips it replaces kept on a take lane. Mix > Save on Transport Stop
// saves the project whenever playback stops.
#pragma once

#include <memory>

#include "ui/AppContext.h"

namespace vcc::desktop {

class MixToolsPanel final : public juce::Component, private juce::Timer {
public:
    explicit MixToolsPanel(AppContext& context);
    ~MixToolsPanel() override;

    /// New VCA / mix group with the selected track's folder (or the track).
    std::string add_vca();
    std::string add_group();
    std::string add_cue();
    void show_tab(int index);
    /// The monitor controls (applied to the engine and remembered).
    void set_monitor(bool dim, bool mono, bool mute, int pair);
    void rebuild();

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    class GroupsTab;
    class MonitorTab;
    void timerCallback() override;

    AppContext& context_;
    juce::TabbedComponent tabs_{juce::TabbedButtonBar::TabsAtTop};
    std::unique_ptr<GroupsTab> groups_;
    std::unique_ptr<MonitorTab> monitor_;
};

void show_mix_tools(AppContext& context, int tab = 0);
MixToolsPanel* mix_tools_panel();

/// The saved monitor settings, applied to the engine (at startup).
void restore_monitor(AppContext& context);

/// Bounces the selected track in place (Mix > Bounce in Place): the
/// selection's range, else the whole song, with its strip and inserts.
juce::String bounce_in_place(AppContext& context, const std::string& track_id);

}  // namespace vcc::desktop
