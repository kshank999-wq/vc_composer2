// The project document: one track list shared by the Composer, Sequencer and
// Mixer views (Main Spec §4, §8, §12, §14, §15, §17).
//
// Everything a view shows is read from ProjectData; everything a view changes
// goes through Project::edit, which makes the change a single undoable step.
// Inserting a 40-track template, sending 8 bars of chords to a track, or
// switching a track to Version 2 is each one entry in the history.
//
// Each track version owns a notation score (the composer's written intent,
// from the notation engine ported from the original VC Composer) plus MIDI
// regions (performance material and generated MIDI not yet notated). The
// project's meter map and key are canonical: every edit pushes them into
// every score, so the Composer view and the Sequencer can never disagree
// about where a bar starts.
//
// Undo currently keeps a copy of the document per step. That is simple and
// obviously correct, which is what the foundation needs; when the notation
// score model from the original VC Composer is ported in, note-level edits
// move to its journal of inverses so long scores stay cheap to edit.
//
// IDs come from an allocator that lives outside the undoable data, so an ID
// is never handed out twice - not even after an undo.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "vcc/core/Ids.h"
#include "vcc/core/Json.h"
#include "vcc/core/Midi.h"
#include "vcc/core/Time.h"
#include "vcc/notation/Score.h"

namespace vcc::project {

constexpr int kProjectSchemaVersion = 1;

enum class TrackKind {
    Instrument,  ///< Score staff + mirrored sequencer track (the default).
    MidiOnly,
    Audio,
    Aux,
    Reference,   ///< Temp score / reference audio: excluded from deliverables by default.
    Video,
    Folder,
};
const char* to_string(TrackKind kind) noexcept;
std::optional<TrackKind> track_kind_from_string(const std::string& text) noexcept;

/// How the composer's instrument library satisfied a track's request
/// (Main Spec §11 "mapping health indicator"; Supplement §8 instrument
/// resolution). Nothing is ever resolved silently: the status and message
/// are shown in the track inspector.
enum class ResolutionStatus {
    Exact,        ///< A mapped patch for the requested instrument.
    Fallback,     ///< A mapped patch for an approved fallback instrument.
    Placeholder,  ///< No patch: built-in General MIDI placeholder plays the part.
    Unresolved,   ///< Nothing assigned yet.
};
const char* to_string(ResolutionStatus status) noexcept;
std::optional<ResolutionStatus> resolution_status_from_string(const std::string& text) noexcept;

struct InstrumentAssignment {
    ResolutionStatus status = ResolutionStatus::Unresolved;
    std::string resolved_instrument_id;  ///< Which instrument the patch is for.
    std::string patch_id;                ///< User mapping-pack patch, empty for placeholder.
    std::string patch_display;
    int placeholder_program = 0;         ///< GM program used when status is Placeholder.
    std::vector<std::string> unsupported_articulations;
    std::string message;                 ///< Human-readable explanation.
};

struct MidiRegion {
    std::string id;
    Ticks start = 0;
    Ticks end = 0;
    int lane = 0;  ///< Take lane; 0 is the main lane.
    std::vector<NoteEvent> notes;
    std::string source;  ///< "chord_explorer:pop_uplift", "ostinato:urgent_string_spiccato/B", "manual"...
};

/// A non-destructive alternative of a track's content (Main Spec §15).
struct TrackVersion {
    std::string id;
    std::string name;  ///< "Version 1", renamable.
    std::string notes; ///< Composer's notes about this version.
    /// Written notation: staves, notes, articulations, dynamics, slurs.
    notation::Score score;
    std::vector<MidiRegion> regions;
    std::vector<std::string> articulation_overrides;
    std::optional<double> gain_db;  ///< Optional per-version mix setting.
};

/// A hosted instrument plug-in on a track (Main Spec §10.6: "persist plugin
/// state via supported state chunks"). The state is the plug-in's own opaque
/// chunk, base64-encoded; the host never interprets it.
struct PluginRef {
    std::string format;      ///< "VST3", "AudioUnit".
    std::string identifier;  ///< Host's identifier string (format + path + uid).
    std::string name;        ///< "Kontakt 8", shown when the plug-in is missing.
    std::string vendor;
    std::string state_base64;
    bool empty() const noexcept { return identifier.empty(); }
};

struct Track {
    std::string id;
    std::string name;
    TrackKind kind = TrackKind::Instrument;
    std::string folder_id;
    std::string family;
    std::string role;
    std::string color = "#808080";
    std::string instrument_standard_id;
    std::vector<std::string> articulation_intents;
    std::vector<std::string> fallback_instruments;
    std::vector<std::string> plugin_requirements;
    InstrumentAssignment assignment;
    /// How notation becomes MIDI for this track's patch: "default" or
    /// "orchestral_cc" (built in), or a user map by name.
    std::string expression_map = "default";
    /// Instrument plug-in; empty means the built-in placeholder plays.
    PluginRef instrument_plugin;
    std::string output_bus_id;
    std::vector<std::pair<std::string, double>> sends;  ///< bus id, level dB
    double gain_db = 0.0;
    double pan = 0.0;
    bool mute = false;
    bool solo = false;
    bool include_in_export = true;  ///< False for Reference tracks unless the user opts in.
    int midi_channel = 0;           ///< 0-15; 9 for drum policy.
    std::string template_origin;    ///< template_id/track id it came from, if any.
    std::vector<TrackVersion> versions;
    std::string active_version_id;

    const TrackVersion* active_version() const noexcept;
    TrackVersion* active_version() noexcept;
};

struct InsertSlot {
    std::string kind;        ///< Generic processor kind: "eq", "compressor", "reverb", "limiter".
    std::string plugin_ref;  ///< Installed plug-in assigned, empty while a suggestion.
    bool suggestion = true;
    bool bypass = false;
};

struct Bus {
    std::string id;
    std::string name;
    std::string kind = "group";  ///< "group", "fx", "master".
    std::string output;          ///< Bus id or "master"/"" for the master bus itself.
    std::vector<std::pair<std::string, double>> sends;
    std::vector<InsertSlot> inserts;
    double gain_db = 0.0;
};

/// Spec §14 hierarchy: Production -> Reel/Episode -> Scene -> Cue.
struct Production {
    std::string title;     ///< The film or series.
    std::string client;
    std::string director;
    std::string composer;  ///< Default composer for new cues.
};

struct Reel {
    std::string id;
    std::string name;  ///< "Reel 1", "Episode 104".
};

struct Scene {
    std::string id;
    std::string reel_id;
    std::string name;  ///< "Sc. 42 - the warehouse".
};

struct Deliverable {
    std::string name;  ///< "Stereo mix", "Stems", "Score PDF", "Parts".
    bool done = false;
};

/// A cue (Main Spec §14). The music range is in ticks, so the cue follows the
/// bars it was written in. The spotting range is the timecode the cue was
/// spotted at, which belongs to the picture. The cue manager shows both and
/// says when they disagree.
struct Cue {
    std::string id;
    std::string code;   ///< "1M1".
    std::string title;
    Ticks start = 0;
    Ticks end = 0;
    /// spotted, writing, sent, awaiting_feedback, revisions, approved, locked.
    std::string status = "writing";
    std::string notes;  ///< Director's / spotting notes.
    std::string description;
    std::string reel_id;
    std::string scene_id;
    std::string spot_in;   ///< Timecode, e.g. "01:02:03:04"; empty when not spotted.
    std::string spot_out;
    int revision = 1;
    std::string deadline;  ///< ISO date, "2026-12-01".
    std::string composer;
    std::string template_id;
    /// Cue-sheet usage: BI (background instrumental), BV, VI (visual
    /// instrumental), VV, MT (main title), ET (end title), SRC (source).
    std::string usage = "BI";
    std::vector<Deliverable> deliverables;
    std::string transition_in;
    std::string transition_out;

    bool locked() const noexcept { return status == "locked"; }
};

/// Statuses in workflow order.
const std::vector<std::string>& cue_statuses();

/// A picture cut the score is written against (Main Spec §13). Earlier cuts
/// are kept when a new one is imported, so the composer can compare.
struct FlaggedRegion {
    double start_seconds = 0.0;  ///< Project time.
    double end_seconds = 0.0;
    std::string note;
};

struct PictureVersion {
    std::string id;
    std::string name;  ///< "Cut 3 (locked)".
    std::string path;  ///< As chosen; relative to the project file when it can be.
    FrameRate rate = FrameRate::Fps24;
    std::string start_timecode = "01:00:00:00";  ///< Timecode of the movie's first frame.
    double offset_seconds = 0.0;                 ///< User-approved conform offset.
    std::int64_t duration_frames = 0;
    int width = 0;
    int height = 0;
    std::vector<FlaggedRegion> changed_regions;
    std::string notes;
};

/// A point in picture time (hit, sync point, streamer, punch, note). Kept in
/// seconds, not ticks, so tempo edits never move a picture event (Main Spec
/// §13: "anchor musical beats to picture events without accidental sync drift").
struct Marker {
    std::string id;
    std::string kind = "hit";  ///< hit, sync, streamer, punch, note.
    double seconds = 0.0;      ///< Project time (0 = the project's start timecode).
    std::string label;
    std::string cue_id;
    double streamer_seconds = 2.0;  ///< Length of the streamer leading into it.
};

/// A named recall of which version each track plays (Main Spec §15 cue
/// snapshots for director A/B).
struct Snapshot {
    std::string id;
    std::string name;
    std::map<std::string, std::string> track_versions;  ///< track id -> version id
};

struct ProjectData {
    std::string name = "Untitled";
    std::string key = "C major";
    MeterMap meter;
    TempoMap tempo;
    FrameRate frame_rate = FrameRate::Fps24;
    std::string start_timecode = "01:00:00:00";
    int sample_rate = 48000;
    std::vector<Track> tracks;
    std::vector<Bus> buses;
    Production production;
    std::vector<Reel> reels;
    std::vector<Scene> scenes;
    std::vector<Cue> cues;
    std::vector<PictureVersion> pictures;
    std::string active_picture_id;
    std::vector<Marker> markers;
    std::vector<Snapshot> snapshots;
    std::vector<std::string> applied_templates;

    const Track* find_track(const std::string& id) const noexcept;
    Track* find_track(const std::string& id) noexcept;
    const Cue* find_cue(const std::string& id) const noexcept;
    Cue* find_cue(const std::string& id) noexcept;
    const PictureVersion* active_picture() const noexcept;
    PictureVersion* active_picture() noexcept;
    const Bus* find_bus(const std::string& id) const noexcept;
    Bus* find_bus(const std::string& id) noexcept;
    const Track* find_track_by_name(const std::string& wanted) const noexcept;
    const Bus* find_bus_by_name(const std::string& wanted) const noexcept;
    /// "Horn" -> "Horn" if free, else "Horn (2)", "Horn (3)"...
    std::string unique_track_name(const std::string& wanted) const;
    std::string unique_bus_name(const std::string& wanted) const;

    /// Pushes the project's meter map and key into every track version's
    /// score. Called after every edit; cheap when nothing changed.
    void sync_scores();
};

/// What to do when Send to Track lands on material already there
/// (Supplement §10.1, test T08). Ask is the default: nothing is written and
/// the caller is told a decision is needed.
enum class CollisionPolicy { Ask, Replace, Merge, NewLane };

struct InsertResult {
    bool ok = false;
    bool needs_decision = false;  ///< Ask policy and the range is occupied.
    std::string region_id;
    std::string error;
};

class Project {
public:
    Project();
    explicit Project(ProjectData data, std::uint64_t next_id = 1);

    const ProjectData& data() const noexcept { return data_; }
    IdAllocator& ids() noexcept { return ids_; }

    /// Applies `change` to a copy of the document. If it returns true the copy
    /// becomes current and one undo step labelled `label` is recorded; if it
    /// returns false (or throws) the document is unchanged.
    bool edit(const std::string& label, const std::function<bool(ProjectData&)>& change);

    /// Like edit(), but when the previous undo step has the same label the
    /// change folds into it: a fader drag is one undo step, not hundreds.
    bool edit_coalescing(const std::string& label, const std::function<bool(ProjectData&)>& change);

    /// Changes the current document without recording an undo step, for
    /// state that is not the composer's edit (a plug-in's own state chunk).
    void modify_unrecorded(const std::function<void(ProjectData&)>& change);

    bool can_undo() const noexcept { return !undo_.empty(); }
    bool can_redo() const noexcept { return !redo_.empty(); }
    std::string undo_label() const;
    bool undo();
    bool redo();
    std::size_t history_size() const noexcept { return undo_.size(); }

    // --- Common edits (each one undo step) -----------------------------------

    /// Adds an instrument track with a "Version 1". Returns its id.
    std::string add_track(const std::string& name, TrackKind kind = TrackKind::Instrument,
                          const std::string& instrument_standard_id = {});

    /// Writes notes into the active version of a track over [start, end).
    InsertResult insert_notes(const std::string& track_id, Ticks start, Ticks end, std::vector<NoteEvent> notes,
                              CollisionPolicy policy, const std::string& source = {});

    /// Duplicates the active version (regions included) and makes the copy active.
    std::optional<std::string> duplicate_version(const std::string& track_id, const std::string& name = {});
    bool set_active_version(const std::string& track_id, const std::string& version_id);
    bool rename_version(const std::string& track_id, const std::string& version_id, const std::string& name);

    /// Records every track's active version under a name.
    std::string create_snapshot(const std::string& name);
    /// Recalls a snapshot as one undo step. Tracks added since are left alone.
    bool recall_snapshot(const std::string& snapshot_id);

    // --- Persistence ------------------------------------------------------------

    json::Value to_json() const;
    /// Loads any supported schema version, migrating older ones forward.
    /// A newer-than-supported file is refused rather than half-read.
    static std::optional<Project> from_json(const json::Value& doc, std::string* error = nullptr);
    bool save(const std::string& path, std::string* error = nullptr) const;
    static std::optional<Project> load(const std::string& path, std::string* error = nullptr);

    /// Tracks a deliverable export includes: reference/temp tracks are out
    /// unless explicitly opted in, and folders never render.
    std::vector<const Track*> export_tracks() const;

private:
    ProjectData data_;
    IdAllocator ids_;
    struct Step {
        std::string label;
        ProjectData before;
        ProjectData after;
    };
    std::vector<Step> undo_;
    std::vector<Step> redo_;
};

}  // namespace vcc::project
