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

#include "vcc/interchange/MusicXml.h"
#include "vcc/ostinato/Ostinato.h"
#include "vcc/project/Performance.h"
#include "vcc/session/Notate.h"
#include "vcc/project/Project.h"
#include "vcc/project/Stems.h"
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

/// What a MusicXML import did (Session::import_musicxml).
struct ImportSummary {
    bool ok = false;
    std::string error;                    ///< Why nothing was imported.
    std::vector<std::string> track_ids;   ///< One instrument track per part, in file order.
    std::vector<std::string> warnings;    ///< What was approximated or left out.
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
    /// The same for an effect plug-in in an insert slot of a track or bus.
    void store_insert_plugin_state(const std::string& owner_id, int slot, const std::string& state_base64);
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

    // --- MusicXML (interchange with Finale, Sibelius, Dorico, MuseScore) ----------
    /// Imports a MusicXML document as one undo step ("Import MusicXML"): one
    /// instrument track per part, its score as the track's active version,
    /// the instrument guessed from the part name, transposition and MIDI
    /// program. With `replace_project` the existing tracks (all but video)
    /// are replaced and the project takes the file's title, composer, tempo,
    /// meter and key; without it the parts are added, and the file's tempo,
    /// meter and key are applied only when the project has no tracks yet.
    ImportSummary import_musicxml(const std::string& xml, bool replace_project);
    /// MusicXML 4.0 of the project; part abbreviations and MIDI programs not
    /// given in `options.part_info` come from the instrument registry.
    std::string export_musicxml(const interchange::ExportOptions& options = {},
                                std::vector<std::string>* warnings = nullptr) const;
    /// Registry id for an imported part (best effort; always an id that exists
    /// when the registry has any instruments).
    std::string guess_instrument(const interchange::ImportedPart& part) const;

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

    // --- Automation (Main Spec §12) ----------------------------------------------------
    // `owner_id` is a track id or a bus id (the master is "bus-master").
    /// off / read / touch / latch / write (see project::automation_modes()).
    bool set_automation_mode(const std::string& owner_id, const std::string& mode);
    /// The lane for `param`, created if needed (seeded with one point at the
    /// start holding the current static value, so creating a lane changes
    /// nothing audible). Also makes it shown. Returns the lane id, or empty
    /// for an unknown owner or parameter.
    std::string ensure_lane(const std::string& owner_id, const std::string& param, const std::string& name = {});
    /// Edits a lane's points or flags; the lane is normalised afterwards.
    /// Same-label calls coalesce (a breakpoint drag is one undo step).
    bool edit_lane(const std::string& owner_id, const std::string& lane_id, const std::string& label,
                   const std::function<void(project::AutomationLane&)>& change);
    bool remove_lane(const std::string& owner_id, const std::string& lane_id);
    /// One written pass from the console (touch/latch/write while playing).
    struct AutomationWrite {
        std::string owner_id;
        std::string param;
        std::string name;  ///< Lane name if the lane is new.
        Ticks from = 0;
        Ticks to = 0;
        std::vector<project::AutomationPoint> points;
    };
    /// Merges every lane written in a pass as one undo step ("Write
    /// automation"), creating lanes as needed.
    bool commit_automation_pass(const std::vector<AutomationWrite>& writes);
    /// The owner's lanes, or null for an unknown owner.
    const std::vector<project::AutomationLane>* lanes_of(const std::string& owner_id) const;

    // --- Performance overrides (Main Spec §8) ------------------------------------------
    // How written notes are played, per score note id, on the track's active
    // version. The score itself is never changed by these.
    /// The active version's overrides, or null for an unknown track.
    const std::map<notation::EventId, project::NoteOverride>* performance_of(const std::string& track_id) const;
    /// Changes the overrides of `notes` (each is passed its current override,
    /// empty when it has none). Ids that are not performable notes of the
    /// score are skipped. Same-label calls coalesce, so a drag is one undo
    /// step. False if nothing could be changed.
    bool edit_performance(const std::string& track_id, const std::vector<notation::EventId>& notes,
                          const std::string& label,
                          const std::function<void(notation::EventId, project::NoteOverride&)>& change);
    /// Returns notes to exactly how they are written (all notes when `notes`
    /// is empty). One undo step.
    bool clear_performance(const std::string& track_id, const std::vector<notation::EventId>& notes = {});
    /// Seeded humanize of `notes` (all performable notes when empty). One
    /// undo step.
    bool humanize(const std::string& track_id, const std::vector<notation::EventId>& notes,
                  const project::HumanizeSettings& settings);

    // --- MIDI -> notation (Main Spec §8) ---------------------------------------------------
    /// Plans a piano-roll edit of a score note (see Notate.h). Changes nothing.
    NotatePlan plan_note_edit(const std::string& track_id, notation::EventId note, const NoteTarget& target) const;
    /// Plans notating a MIDI region into the track's score. Changes nothing.
    NotatePlan plan_notate_region(const std::string& track_id, const std::string& region_id,
                                  const RegionNotateSettings& settings = {}) const;
    /// Applies one option of a plan as one undo step. Refuses (false) a plan
    /// made before the track's score last changed.
    /// With `coalesce`, same-label applies merge into one undo step (several
    /// notes edited by one gesture).
    bool apply_notate(const std::string& track_id, const NotatePlan& plan, std::size_t option, const std::string& label,
                      bool coalesce = false);

    // --- Stems (Main Spec §16) ---------------------------------------------------------
    /// The project's stems, or sensible defaults when it has none (group
    /// buses, then instrument families).
    std::vector<project::StemDef> stems() const;
    /// Replaces the stem definitions (one undo step). Stems without an id get one.
    bool set_stems(std::vector<project::StemDef> stems);

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
