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

class LibraryService;
class ResourceMonitor;

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
    /// Opens an insert's window: a hosted effect's own editor, or the editor
    /// for a built-in processor. `owner_id` is a track or bus id.
    std::function<void(const std::string& owner_id, int slot)> show_insert_window;
    /// Follow (docs/spec/ui, the edit window's title bar): while playing, the
    /// views scroll with the playhead and the keyboard brings lit notes
    /// into view. Remembered between sessions.
    bool follow = true;
    /// The Circle of Fifths' "Send to Chord Mode": fills the Composer's chord
    /// palette with a chord and opens it (set by the main window).
    std::function<void(const std::string& chord_symbol)> send_to_chord_mode;
    /// Stores every hosted plug-in's current state in the project (before a
    /// package copies it; set by the main window).
    std::function<void()> capture_plugin_states;
    /// Opens the Comments window on a comment (empty: just the window).
    std::function<void(const std::string& comment_id)> show_comment;
    /// The library browser's library (Main Spec §10; owned by the main window).
    LibraryService* library = nullptr;
    /// CPU/RAM sampling for the status strip (Memory Management spec §6; owned by the main window).
    ResourceMonitor* resources = nullptr;
    /// Shows the library browser's candidates for the selected track (set
    /// by the main window).
    std::function<void()> show_library_for_track;
    /// Freeze (Memory Management spec §5): prints the tracks through their
    /// inserts and plays the prints; with `unload`, then closes their
    /// instruments to give the memory back. Re-render is this again.
    std::function<void(const std::vector<std::string>& tracks, bool unload)> freeze_tracks;
    /// "LIVE", "RENDERING", "FROZEN", "FROZEN · UNLOADED", "STALE", "MISSING PLUG-IN".
    std::function<juce::String(const std::string& track_id)> track_state;
    /// A freeze in progress: "Violins I  42%" (empty when none); and stop it.
    std::function<juce::String()> freeze_progress;
    std::function<void()> cancel_freeze;
    /// Opens the instrument usage panel (Memory Management spec §6.1).
    std::function<void()> show_instrument_usage;
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
    importMusicXml = 0x1020,
    exportMusicXml,
    exportPartsPdf,
    exportFullScorePdf,
    exportStems,
    saveAsTemplate,
    undo = 0x1100,
    redo,
    play = 0x1200,
    rewind,
    toggleMetronome,
    toggleRecord,
    togglePunch,
    toggleLoop,
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
    reloadCrashedPlugins,
    showControlGallery = 0x1320,
    toggleView,
    toggleFollow = 0x1330,
    zoomIn,
    zoomOut,
    showVideo = 0x1310,
    showCueManager,
    importPicture,
    addHit,
    addAudioTrack = 0x1420,
    importAudio,
    addAuxBus,
    addGroupBus,
    about = 0x1600,
    editShuffle = 0x1700,
    editSlip,
    editSpot,
    editGrid,
    toggleSmartTool,
    showCircleOfFifths,
    toggleStaffTools,
    detachStaffTools,
    showRouting,
    showControlSurfaces,
    jogForward,
    jogBack,
    showReview,
    collectPackage,
    openPackage,
    showComments,
    addComment,
    exportReviewMixes,
    relinkMedia,
    articulationMap,
    missingInstruments,
    newProjectWizard,
    mixTools,
    bounceInPlace,
    saveOnStop,
    monitorDim,
    reverseClips,
    timeStretchClips,
    groupClips,
    ungroupClips,
    captureMidi,
    midiInputFilter,
    lyrics,
    explodeTracks,
    implodeTracks,
    addRepeat,
    unrollRepeats,
    editShortcuts,
    printPreview,
    exportPicture,
    tempoFinder,
    account,
    freezeTrack,
    freezeUnload,
    unfreezeTrack,
    rerenderTrack,
    expandArticulations,
    consolidateArticulations,
    instrumentUsage,
    sharedProjects,
    shareProject,
    syncShared,
    lockPart,
    unlockPart,
    checkUpdates,
    toggleDock,
    toggleWorkView,
    saveLayout,
    midiSetup,
    mixerController,
    scoreView,
    trackList,
    hearEntry,
    builtinSounds,
    credits,
};
}  // namespace cmd

}  // namespace vcc::desktop
