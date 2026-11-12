// What every view and dialog is given: the open document, the audio engine,
// plug-in hosting, commands and shared resources. Views change the document
// only through Session, and redraw when Session tells them it changed.
#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "AppResources.h"
#include "audio/PlaybackEngine.h"
#include "audio/PluginHost.h"
#include "vcc/composer/ShortcutProfile.h"
#include "vcc/session/Session.h"

namespace vcc::desktop {

struct AppContext {
    session::Catalogs& catalogs;  ///< mutable: user templates are added at run time
    session::Session& session;
    PlaybackEngine& engine;
    PluginHost& plugins;
    juce::ApplicationCommandManager& commands;
    composer::ShortcutProfile shortcuts;
    /// Shows a message in the status bar.
    std::function<void(const juce::String&)> status;
    /// Opens a hosted plug-in's own window for a track.
    std::function<void(const std::string& track_id)> show_plugin_window;
};

/// Application commands (menus, toolbar buttons and key bindings).
namespace cmd {
enum : juce::CommandID {
    newProject = 0x1000,
    newFromTemplate,
    open,
    save,
    saveAs,
    exportMidi,
    exportScoreSvg,
    saveAsTemplate,
    undo = 0x1100,
    redo,
    play = 0x1200,
    rewind,
    toggleMetronome,
    showComposer = 0x1300,
    showSequencer,
    showMixer,
    chordExplorer = 0x1400,
    ostinatoCreator,
    addTemplate,
    addTrack,
    duplicateVersion,
    removeTrack,
    audioSettings = 0x1500,
    scanPlugins,
    loadInstrument,
    openInstrumentWindow,
    about = 0x1600,
};
}  // namespace cmd

}  // namespace vcc::desktop
