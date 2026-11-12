// The open document and everything the desktop app does to it.
//
// The windows (JUCE) never touch vcc::project directly to change it: they call
// Session, which makes each user action one undo step, marks the document
// dirty, and notifies every view so the Composer, Sequencer, track panel and
// Mixer show the same state (Main Spec §4, §20). Session has no framework
// dependency, so everything the app does is testable without a window.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vcc/ostinato/Ostinato.h"
#include "vcc/project/Project.h"
#include "vcc/templates/Templates.h"
#include "vcc/theory/Progression.h"

namespace vcc::session {

/// Shipped content: instruments, templates, progressions, ostinato presets.
struct Catalogs {
    templates::InstrumentRegistry instruments;
    std::unique_ptr<templates::TemplateRegistry> templates;
    theory::ProgressionCatalog progressions;
    ostinato::PresetCatalog ostinato_presets;
    std::vector<std::string> articulation_ids;
    std::vector<templates::Diagnostic> diagnostics;

    /// From a content/ directory on disk (development, CLI, tests).
    static std::unique_ptr<Catalogs> load_from_directory(const std::string& content_dir);
    /// From documents already in memory, keyed by their content-relative
    /// path ("instruments/core.json", "templates/cinematic.json",
    /// "progressions.json", ...). The desktop app embeds content this way so
    /// an installed copy cannot lose it.
    static std::unique_ptr<Catalogs> load_from_documents(const std::map<std::string, std::string>& documents);

    int error_count() const noexcept;
};

enum class Change {
    Document,   ///< Tracks, notes, mix or anything else in the project changed.
    Selection,  ///< The selected track changed.
    File,       ///< Path or dirty state changed (window title).
};

class Session {
public:
    explicit Session(const Catalogs& catalogs);

    const Catalogs& catalogs() const noexcept { return catalogs_; }
    const project::Project& project() const noexcept { return project_; }
    const project::ProjectData& data() const noexcept { return project_.data(); }

    // --- Listeners ------------------------------------------------------------
    using Listener = std::function<void(Change)>;
    int add_listener(Listener listener);
    void remove_listener(int id);

    // --- File -----------------------------------------------------------------
    const std::string& path() const noexcept { return path_; }
    bool dirty() const noexcept { return dirty_; }
    /// "Untitled" or the file name, with a trailing "*" when dirty.
    std::string title() const;
    void new_empty();
    /// A new project from a template (Create mode). Replaces the open
    /// document; the caller asks about unsaved changes first.
    templates::InstantiateResult new_from_template(const std::string& template_id,
                                                   const templates::InstantiateOptions& options = {});
    /// Adds a template to the open project (merge, never overwriting).
    templates::InstantiateResult add_template(const std::string& template_id,
                                              const templates::InstantiateOptions& options = {});
    bool open(const std::string& path, std::string* error = nullptr);
    bool save(std::string* error = nullptr);
    bool save_as(const std::string& path, std::string* error = nullptr);
    /// Crash protection (Main Spec §12): writes the document beside its file
    /// (or into `fallback_dir` when untitled) without changing path or dirty.
    bool write_autosave(const std::string& fallback_dir, std::string* error = nullptr) const;
    std::string autosave_path(const std::string& fallback_dir) const;
    /// True when an autosave newer than `path` exists (offer to recover it).
    static bool has_newer_autosave(const std::string& path);

    // --- Editing ----------------------------------------------------------------
    /// One undoable step. Returns false (and changes nothing) if `change` does.
    bool edit(const std::string& label, const std::function<bool(project::ProjectData&)>& change);
    bool undo();
    bool redo();
    bool can_undo() const noexcept { return project_.can_undo(); }
    bool can_redo() const noexcept { return project_.can_redo(); }
    std::string undo_label() const { return project_.undo_label(); }

    std::string add_track(const std::string& name, const std::string& instrument_standard_id);
    /// A temp-score / reference track: never in deliverables unless opted in.
    std::string add_reference_track(const std::string& name);
    bool remove_track(const std::string& track_id);
    bool rename_track(const std::string& track_id, const std::string& name);
    bool set_gain(const std::string& track_id, double gain_db);
    bool set_pan(const std::string& track_id, double pan);
    bool set_mute(const std::string& track_id, bool mute);
    bool set_solo(const std::string& track_id, bool solo);
    bool set_output_bus(const std::string& track_id, const std::string& bus_id);
    bool set_bus_gain(const std::string& bus_id, double gain_db);
    bool set_include_in_export(const std::string& track_id, bool include);
    bool set_active_version(const std::string& track_id, const std::string& version_id);
    std::optional<std::string> duplicate_version(const std::string& track_id);
    bool set_instrument_plugin(const std::string& track_id, const project::PluginRef& plugin);
    /// Stores a plug-in's state without an undo step (it changes whenever the
    /// composer touches the plug-in's own window).
    void store_plugin_state(const std::string& track_id, const std::string& state_base64);
    bool set_tempo(double bpm);
    bool set_meter(const TimeSignature& signature);
    bool set_key(const std::string& key);

    /// Replaces the active version's score with an edited copy (the Composer
    /// view edits a working copy and commits each change here).
    bool commit_score(const std::string& track_id, const notation::Score& score, const std::string& label);

    project::InsertResult insert_notes(const std::string& track_id, Ticks start, Ticks end,
                                       std::vector<NoteEvent> notes, project::CollisionPolicy policy,
                                       const std::string& source);
    /// Chord Explorer "Send to Track" (Supplement §9.1): one undo step.
    project::InsertResult send_progression(const std::string& track_id, const theory::ProgressionRender& render,
                                           const std::string& progression_id, project::CollisionPolicy policy);
    /// Ostinato Creator "Send to Track" (Supplement §10.1): one undo step.
    project::InsertResult send_ostinato(const std::string& track_id, const ostinato::Variant& variant,
                                        const std::string& preset_id, project::CollisionPolicy policy);

    // --- Picture, markers and cues (Main Spec §13, §14) ------------------------------
    bool set_frame_rate(FrameRate rate);
    bool set_start_timecode(const std::string& timecode);
    /// Adds a picture version and makes it the active cut; earlier cuts are kept.
    std::string import_picture(project::PictureVersion picture);
    bool set_active_picture(const std::string& picture_id);
    bool set_picture_offset(const std::string& picture_id, double offset_seconds);
    bool set_picture_start_timecode(const std::string& picture_id, const std::string& timecode);
    bool set_changed_regions(const std::string& picture_id, std::vector<project::FlaggedRegion> regions);
    bool remove_picture(const std::string& picture_id);

    std::string add_marker(double project_seconds, const std::string& kind, const std::string& label = {});
    bool update_marker(const std::string& marker_id, const std::function<void(project::Marker&)>& change);
    bool remove_marker(const std::string& marker_id);
    /// Hit-point tool: refits the tempo between `from_tick` and `target_tick`
    /// so that `target_tick` sounds exactly on the marker (one undo step).
    bool fit_tempo_to_marker(const std::string& marker_id, Ticks target_tick, Ticks from_tick,
                             std::string* error = nullptr);

    bool set_production(const project::Production& production);
    std::string add_reel(const std::string& name);
    std::string add_scene(const std::string& reel_id, const std::string& name);
    /// A new cue over a music range, with the next free code in its reel.
    std::string add_cue(Ticks start, Ticks end, const std::string& reel_id = {}, const std::string& title = {});
    /// Edits a cue as one undo step. A locked cue is refused unless
    /// `allow_locked` (the caller has asked the composer first, Main Spec §15).
    bool update_cue(const std::string& cue_id, const std::function<void(project::Cue&)>& change,
                    const std::string& label = "Edit cue", bool allow_locked = false);
    bool remove_cue(const std::string& cue_id, bool allow_locked = false);
    /// Records where a cue was spotted; with `move_music` the music range is
    /// moved to the spotted timecodes through the tempo map.
    bool spot_cue(const std::string& cue_id, const std::string& in_tc, const std::string& out_tc, bool move_music);

    // --- Console and recording (Main Spec §11, §12) ---------------------------------------
    /// Edits a track's console settings (strip, inserts, sends, routing,
    /// input). Repeated calls with the same `label` merge into one undo step,
    /// so a knob drag is undone in one go; give each control its own label.
    bool update_track_mix(const std::string& track_id, const std::string& label,
                          const std::function<void(project::Track&)>& change);
    /// The same for a bus (including the master bus).
    bool update_bus(const std::string& bus_id, const std::string& label,
                    const std::function<void(project::Bus&)>& change);
    /// A new bus feeding the master: kind "group" (subgroup/stem) or "fx"
    /// (an aux return fed by sends). Returns its id.
    std::string add_bus(const std::string& name, const std::string& kind);
    /// Removes a bus; whatever fed it is re-routed to the master and sends to
    /// it are dropped. The master bus cannot be removed.
    bool remove_bus(const std::string& bus_id);
    /// A new audio track (mono or stereo input) with an empty playlist.
    std::string add_audio_track(const std::string& name, bool stereo);
    /// Record-arms a track. Not an undo step and does not dirty the project.
    void set_record_armed(const std::string& track_id, bool armed);
    /// Places clips (one recording pass, or an import) as one undo step. With
    /// `as_takes`, clips already on lane 0 that a new clip overlaps move to
    /// the next free take lane, so nothing recorded is ever lost. Ids are
    /// assigned here and returned in order.
    std::vector<std::string> add_audio_clips(const std::vector<std::pair<std::string, project::AudioClip>>& clips,
                                             const std::string& label, bool as_takes = true);
    bool update_audio_clip(const std::string& track_id, const std::string& clip_id, const std::string& label,
                           const std::function<void(project::AudioClip&)>& change);
    bool remove_audio_clip(const std::string& track_id, const std::string& clip_id);
    /// Where recordings go: "<project folder>/Audio Files", or empty while the
    /// project is untitled (the app then uses a scratch folder).
    std::string audio_folder() const;
    /// Resolves a clip's stored file name to a path on disk.
    std::string resolve_audio_path(const std::string& file) const;

    // --- Selection ----------------------------------------------------------------
    const std::string& selected_track() const noexcept { return selected_track_; }
    void select_track(const std::string& track_id);

    // --- Queries for views ----------------------------------------------------------
    /// Instrument tracks in display order (folders and video excluded).
    std::vector<const project::Track*> music_tracks() const;
    /// Whether a track is audible given mute and solo across the project.
    bool audible(const project::Track& track) const;
    /// Total length worth showing: the last note or region end, at least 32 bars.
    Ticks content_end() const;
    /// Instrument definition for a track, if it names one in the registry.
    const templates::InstrumentDef* instrument_for(const project::Track& track) const;
    /// Sounding range for a track (instrument range, or full piano).
    std::pair<int, int> range_for(const project::Track& track) const;

private:
    void notify(Change change);
    void document_changed();
    void reset(project::Project project, const std::string& path);

    const Catalogs& catalogs_;
    project::Project project_;
    std::string path_;
    bool dirty_ = false;
    std::string selected_track_;
    std::map<int, Listener> listeners_;
    int next_listener_ = 1;
};

}  // namespace vcc::session
