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

/// A piece of an audio file placed on the timeline (Main Spec §11). Audio is
/// placed in project seconds, not ticks, so a tempo edit never moves or
/// stretches a recording. The file is never modified (non-destructive).
struct AudioClip {
    std::string id;
    std::string name;
    /// Relative to the project's folder when inside it ("Audio Files/Vln_01.wav"),
    /// otherwise absolute.
    std::string file;
    double start_seconds = 0.0;   ///< Timeline position, project seconds.
    double source_offset = 0.0;   ///< Seconds into the file where the clip starts.
    double length = 0.0;          ///< Seconds.
    double gain_db = 0.0;
    double fade_in = 0.0;         ///< Seconds.
    double fade_out = 0.0;
    int lane = 0;                 ///< Take lane; 0 is the main lane.
    bool muted = false;
    int channels = 0;             ///< Of the file, for display (1 or 2).
    double file_sample_rate = 0.0;
    double end_seconds() const noexcept { return start_seconds + length; }
};

/// How one written note is played (Main Spec §8 performance overrides).
/// Offsets, not values: the score keeps the musical intent, and this layer
/// only says how far the performance departs from it, so a later score edit
/// (a new dynamic, a longer note) still reaches the performed note.
struct NoteOverride {
    Ticks onset = 0;   ///< Added to the rendered start (early < 0 < late).
    Ticks length = 0;  ///< Added to the rendered duration.
    int velocity = 0;  ///< Added to the rendered velocity.
    /// Added to the controllers the note sends (its articulation CCs), and,
    /// when the expression map shapes dynamics with a controller, to that
    /// controller while the note sounds.
    int cc = 0;
    bool empty() const noexcept { return onset == 0 && length == 0 && velocity == 0 && cc == 0; }
};
bool operator==(const NoteOverride& a, const NoteOverride& b) noexcept;
bool operator!=(const NoteOverride& a, const NoteOverride& b) noexcept;

/// A phrase block (docs/spec/ui, "Phrase blocks"): a named, coloured span
/// of a staff ("FLUTE · CHORUS MELODY"), the Composer view's clip. Moving a
/// block moves the notes inside it; trimming it changes only its span.
struct Phrase {
    std::string id;
    std::string name;
    Ticks start = 0;
    Ticks end = 0;  ///< Exclusive.
};

/// A non-destructive alternative of a track's content (Main Spec §15).
struct TrackVersion {
    std::string id;
    std::string name;  ///< "Version 1", renamable.
    std::string notes; ///< Composer's notes about this version.
    /// Written notation: staves, notes, articulations, dynamics, slurs.
    notation::Score score;
    std::vector<MidiRegion> regions;
    /// Performance overrides by score note id, kept apart from the score: a
    /// tie chain is keyed by its first note. Overrides of notes that no
    /// longer exist are ignored and are not saved.
    std::map<notation::EventId, NoteOverride> performance;
    std::vector<std::string> articulation_overrides;
    std::optional<double> gain_db;  ///< Optional per-version mix setting.
    /// Audio on this version's playlist (audio and reference tracks; any
    /// track may carry printed audio).
    std::vector<AudioClip> audio_clips;
    /// Phrase blocks over the score, in time order.
    std::vector<Phrase> phrases;
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

/// One breakpoint of an automation lane. Automation is placed in ticks, so a
/// swell stays with the notes it shapes when the tempo is edited.
struct AutomationPoint {
    Ticks tick = 0;
    double value = 0.0;  ///< In the parameter's own unit (dB, -1..+1 pan, 0/1 switch, Hz...).
    /// How the value travels to the next point: "linear", "step" (hold, then
    /// jump) or "smooth" (an S-curve, for swells).
    std::string shape = "linear";
};

/// A lane of automation for one parameter of a track or bus (Main Spec §12).
/// Parameter ids (see vcc/project/Automation.h for ranges and defaults):
///   "volume" (dB), "pan", "mute" (0/1), "send:<bus id>" (dB),
///   "strip.<field>" (any numeric ChannelStrip field, e.g. "strip.hmf.gain_db",
///   "strip.comp_threshold_db", switches as 0/1 e.g. "strip.eq_on"),
///   "insert:<slot>:<param>" (a built-in insert's param),
///   "plugin:<index>" (the instrument plug-in's parameter, normalised 0..1),
///   "insert_plugin:<slot>:<index>" (an effect plug-in's parameter, 0..1).
struct AutomationLane {
    std::string id;
    std::string param;
    std::string name;  ///< Display name, e.g. "HMF Gain" or the plug-in's parameter name.
    std::vector<AutomationPoint> points;  ///< Sorted by tick; at most one point per tick.
    bool enabled = true;  ///< Off: the lane is kept but not played.
    bool shown = true;    ///< Shown under the track in the Sequencer.
};

/// Console automation modes, per track or bus.
///   off   - lanes are ignored; controls are free.
///   read  - lanes play; controls follow them.
///   touch - lanes play; while a control is held it writes, and on release
///           returns to the lane.
///   latch - like touch, but after release it keeps writing the last value
///           until the transport stops.
///   write - writes every automatable control the strip has a lane for, for
///           the whole pass.
inline const std::vector<std::string>& automation_modes()
{
    static const std::vector<std::string> modes = {"off", "read", "touch", "latch", "write"};
    return modes;
}

/// One equaliser band of a console channel.
struct EqBand {
    bool on = true;
    double freq = 1000.0;  ///< Hz
    double gain_db = 0.0;
    double q = 0.7;
    bool bell = false;     ///< LF/HF: shelf unless `bell`.
};

/// The processing built into every console channel, modelled on a large-
/// format analogue console strip (Main Spec §12): input trim and polarity,
/// filters, a four-band EQ and a dynamics section. The engine runs it before
/// the inserts; each section has its own in/out switch.
struct ChannelStrip {
    double trim_db = 0.0;        ///< Input gain, -20..+20 dB.
    bool phase_invert = false;
    bool hpf_on = false;
    double hpf_hz = 80.0;        ///< 20..400 Hz, 12 dB/oct.
    bool lpf_on = false;
    double lpf_hz = 12000.0;     ///< 3k..20k Hz.
    bool eq_on = false;
    EqBand lf{true, 100.0, 0.0, 0.7, false};
    EqBand lmf{true, 400.0, 0.0, 1.0, true};
    EqBand hmf{true, 2500.0, 0.0, 1.0, true};
    EqBand hf{true, 8000.0, 0.0, 0.7, false};
    bool comp_on = false;
    double comp_threshold_db = -18.0;
    double comp_ratio = 3.0;
    double comp_attack_ms = 10.0;
    double comp_release_ms = 120.0;
    double comp_makeup_db = 0.0;
    bool gate_on = false;
    double gate_threshold_db = -50.0;
    double gate_range_db = 40.0;  ///< How far a closed gate turns down.
    double gate_release_ms = 150.0;
    bool dynamics_pre_eq = false; ///< Dynamics before EQ (console "DYN PRE").
};

/// A send from a channel or bus to an aux/fx bus.
struct Send {
    std::string bus_id;
    double level_db = -12.0;
    bool pre_fader = false;
    bool mute = false;
    double pan = 0.0;  ///< -1..+1; follows the channel pan when `pan_follow`.
    bool pan_follow = true;
    Send() = default;
    Send(std::string bus, double level) : bus_id(std::move(bus)), level_db(level) {}
};

/// An insert slot: a built-in processor ("eq", "compressor", "reverb",
/// "delay", "limiter", "gate") or a hosted effect plug-in ("plugin").
struct InsertSlot {
    std::string kind;        ///< Generic processor kind, or "plugin".
    std::string plugin_ref;  ///< Installed plug-in assigned, empty while a suggestion.
    bool suggestion = true;
    bool bypass = false;
    PluginRef plugin;        ///< kind == "plugin": the hosted effect and its state.
    std::map<std::string, double> params;  ///< Built-in processor settings by name.
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
    std::vector<Send> sends;
    ChannelStrip strip;
    std::vector<InsertSlot> inserts;
    double gain_db = 0.0;
    double pan = 0.0;
    bool mute = false;
    bool solo = false;
    // Recording (Main Spec §11). Arming is not an undo step.
    int input = 0;              ///< First device input channel (0-based); -1 = none.
    bool input_stereo = false;  ///< Records input, input + 1.
    bool record_armed = false;
    std::string monitor = "auto";  ///< "auto" (input while armed and stopped/recording), "input", "off".
    /// What an instrument or MIDI track plays (docs/spec/ui, Producer view):
    /// "midi" (its notes through its instrument), "audio" (only its own
    /// audio lane - say, the score printed to audio) or "both".
    std::string playback = "both";
    /// The audio lane was printed with the channel strip and inserts: while
    /// the track plays its audio lane they are bypassed, so they are not
    /// heard twice. Set by Print to audio with effects.
    bool printed_fx = false;
    std::vector<AutomationLane> automation;
    std::string automation_mode = "read";
    bool include_in_export = true;  ///< False for Reference tracks unless the user opts in.
    int midi_channel = 0;           ///< 0-15; 9 for drum policy.
    std::string template_origin;    ///< template_id/track id it came from, if any.
    std::vector<TrackVersion> versions;
    std::string active_version_id;

    const TrackVersion* active_version() const noexcept;
    TrackVersion* active_version() noexcept;
};

struct Bus {
    std::string id;
    std::string name;
    std::string kind = "group";  ///< "group", "fx", "master".
    std::string output;          ///< Bus id or "master"/"" for the master bus itself.
    std::vector<Send> sends;
    std::vector<InsertSlot> inserts;
    double gain_db = 0.0;
    double pan = 0.0;
    bool mute = false;
    bool solo = false;
    ChannelStrip strip;
    std::string color;  ///< Empty: the console's colour for the bus kind.
    std::vector<AutomationLane> automation;
    std::string automation_mode = "read";
};

/// A stem (Main Spec §16): a named part of the mix printed to its own file,
/// so a dubbing mixer can rebalance the score. Sources are track ids and/or
/// bus ids (a group bus brings every track that feeds it).
struct StemDef {
    std::string id;
    std::string name;                  ///< "Strings", "Brass", "Synths".
    std::vector<std::string> sources;
    /// The stem carries its own share of the reverb and delay returns (what
    /// its tracks send there). Off prints it dry.
    bool include_effects = true;
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
    /// Plug-in delay compensation (Main Spec §12): every strip lined up
    /// with the slowest path. Saved with the project, so a mix sounds the
    /// same when it is reopened. Older files, which did not say, have it on.
    bool delay_compensation = true;
    /// The keyboard deck's key zoom (1-3), remembered with the project
    /// (docs/spec/ui, "Behavior rules" 3). A view setting: no undo step.
    double key_zoom = 1.0;
    /// The master's loudness target, LUFS (docs/spec/ui, "Master panel"):
    /// -14 for streaming by default; -16, -23 (EBU R 128) and -24 (ATSC A/85).
    double loudness_target = -14.0;
    std::vector<Track> tracks;
    std::vector<Bus> buses;
    Production production;
    std::vector<Reel> reels;
    std::vector<Scene> scenes;
    std::vector<Cue> cues;
    std::vector<PictureVersion> pictures;
    std::string active_picture_id;
    std::vector<Marker> markers;
    std::vector<StemDef> stems;
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
