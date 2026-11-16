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
#include "vcc/project/MidiTools.h"
#include "vcc/project/Freeze.h"
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

/// What File > New Project... asks (Main Spec §5): every template default
/// can be overridden without rebuilding the project.
struct ProjectSetup {
    std::string name = "Untitled";
    /// Where it is saved: <folder>/<name>/<name>.vcc2 (a folder of its own,
    /// for its audio). Empty: not saved yet.
    std::string folder;
    std::string template_id;  ///< Empty: an empty project.
    /// Nullopt keeps the template's recommendation (or the default).
    std::optional<std::string> key;      ///< "D minor".
    std::optional<TimeSignature> meter;
    std::optional<double> tempo;
    FrameRate frame_rate = FrameRate::Fps24;
    std::string start_timecode = "01:00:00:00";
    int sample_rate = 48000;
};

struct SetupResult {
    bool ok = false;
    std::string error;
    std::string project_file;  ///< Where it was saved, if it was.
    std::vector<std::string> warnings;  ///< From the template (placeholders, fallbacks).
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
    /// Counts documents: a new, opened or template-made project is a new one,
    /// even where its track and version ids repeat the last one's.
    std::uint64_t document_generation() const noexcept { return generation_; }
    /// Counts every change told to listeners (caches of derived values key on it).
    std::uint64_t change_count() const noexcept { return changes_; }

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
    // --- Score tools (Main Spec §7) ------------------------------------------------
    /// "Hal-le-lu-jah, sing" -> "Hal-", "le-", "lu-", "jah,", "sing".
    static std::vector<std::string> lyric_syllables(const std::string& text);
    /// Sings `text` on these notes, a syllable per onset in time order (a
    /// chord takes one); empty text clears their lyrics. One undo step.
    /// Returns the syllables placed.
    int set_lyrics(const std::string& track_id, const std::vector<notation::EventId>& notes, const std::string& text);
    /// Explode: the chords of [from, to) on the track's first staff split
    /// into `parts` new tracks of the same instrument, top note to the
    /// first; a chord with fewer notes doubles its lowest. One undo step.
    std::vector<std::string> explode(const std::string& track_id, Ticks from, Ticks to, int parts);
    /// Implode: the notes of [from, to) on the sources' first staves become
    /// chords on the target's (what the target had there gives way).
    bool implode(const std::vector<std::string>& sources, const std::string& target, Ticks from, Ticks to);
    /// Repeats and endings (printed and exchanged). One undo step "Repeats".
    bool set_repeats(std::vector<notation::RepeatSection> repeats);
    /// The bars in the order a player plays them.
    static std::vector<std::int32_t> play_order(const std::vector<notation::RepeatSection>& repeats, std::int32_t last_bar);
    /// Writes the repeats out (Edit > Unroll Repeats): every track's notes,
    /// markings and the chord symbols copied into the bars they repeat,
    /// the meter following; the repeat signs go. Audio clips, MIDI regions
    /// and markers stay where they are. One undo step; returns bars added.
    int unroll_repeats();

    /// File > New Project...: a new document from the setup, with no undo
    /// history, saved when a folder is given (the folder is created; an
    /// existing project file there is never overwritten).
    SetupResult create_project(const ProjectSetup& setup);
    /// The composer's saved library patches (Main Spec §10): templates
    /// resolve their tracks to these, and load their sounds, unless the
    /// options name others.
    void set_installed_patches(std::vector<templates::InstalledPatch> patches) { installed_patches_ = std::move(patches); }
    const std::vector<templates::InstalledPatch>& installed_patches() const noexcept { return installed_patches_; }
    /// Adds a template to the open project (merge, never overwriting).
    templates::InstantiateResult add_template(const std::string& template_id,
                                              const templates::InstantiateOptions& options = {});
    bool open(const std::string& path, std::string* error = nullptr);
    bool save(std::string* error = nullptr);
    bool save_as(const std::string& path, std::string* error = nullptr);
    /// Crash protection (Main Spec §12): writes the document beside its file
    /// (or into `fallback_dir` when untitled) without changing path or dirty.
    bool write_autosave(const std::string& fallback_dir, std::string* error = nullptr) const;
    /// An autosave in two halves: the copy, taken now (about a millisecond
    /// even for a large project), and the write, which may run on another
    /// thread - serializing a large project is the slow part, and it must
    /// not stall the window every minute.
    struct AutosaveJob {
        project::Project copy;
        std::string path;
    };
    AutosaveJob autosave_job(const std::string& fallback_dir) const;
    static bool write_autosave_job(const AutosaveJob& job, std::string* error = nullptr);
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
    /// What an instrument or MIDI track plays: "midi", "audio" (its audio
    /// lane) or "both". One undo step; false when unchanged or not one of those.
    bool set_playback(const std::string& track_id, const std::string& playback);
    bool set_active_version(const std::string& track_id, const std::string& version_id);
    std::optional<std::string> duplicate_version(const std::string& track_id);
    bool set_instrument_plugin(const std::string& track_id, const project::PluginRef& plugin);
    /// Assigns a sound from the library browser (Main Spec §10.6): the
    /// plug-in (with its state), how the track's request was met (shown in
    /// the inspector; nothing is resolved silently) and, when given, the
    /// articulation map. One undo step, "Assign <patch>".
    /// Gives a track its own articulation map (Main Spec §11), or - with no
    /// map - plays it through a built-in one by name. One undo step,
    /// "Articulation map".
    /// A drum kit's percussion map ("gm_kit"; empty for none). One undo step.
    bool set_percussion_map(const std::string& track_id, const std::string& map);
    bool set_articulation_map(const std::string& track_id, const std::optional<notation::ExpressionMap>& map,
                              const std::string& builtin = "default");
    bool assign_sound(const std::string& track_id, const project::PluginRef& plugin,
                      const project::InstrumentAssignment& how, const std::string& expression_map = {});
    /// Stores a plug-in's state without an undo step (it changes whenever the
    /// composer touches the plug-in's own window).
    void store_plugin_state(const std::string& track_id, const std::string& state_base64);
    /// The same for an effect plug-in in an insert slot of a track or bus.
    void store_insert_plugin_state(const std::string& owner_id, int slot, const std::string& state_base64);
    bool set_tempo(double bpm);
    bool set_meter(const TimeSignature& signature);
    bool set_key(const std::string& key);
    /// Plug-in delay compensation on or off, saved with the project. One
    /// undo step; false when it is already so.
    bool set_delay_compensation(bool on);
    /// Apply to Score: notes kept as Producer overrides are written as they
    /// play (one undo step); how many were written, and in `left` how many
    /// could not be.
    int apply_overrides_to_score(const std::string& track_id, const std::vector<notation::EventId>& notes, int* left = nullptr);
    /// "ask", "apply" or "keep": what a notation-changing Producer edit does.
    bool set_producer_edits(const std::string& policy);
    /// The keyboard deck's key zoom, saved with the project without an undo step.
    void set_key_zoom(double zoom);
    /// The master's loudness target (LUFS, -36..-6). One undo step.
    bool set_loudness_target(double lufs);

    /// Replaces the active version's score with an edited copy (the Composer
    /// view edits a working copy and commits each change here).
    bool commit_score(const std::string& track_id, const notation::Score& score, const std::string& label);
    /// The same, also setting the chord symbol at `tick` (Chord Mode writes
    /// the chord it entered), as one undo step. An empty symbol sets none.
    bool commit_score_with_chord(const std::string& track_id, const notation::Score& score, const std::string& label,
                                 Ticks tick, const std::string& symbol);

    project::InsertResult insert_notes(const std::string& track_id, Ticks start, Ticks end,
                                       std::vector<NoteEvent> notes, project::CollisionPolicy policy,
                                       const std::string& source);
    /// Chord Explorer "Send to Track" (Supplement §9.1): one undo step.
    project::InsertResult send_progression(const std::string& track_id, const theory::ProgressionRender& render,
                                           const std::string& progression_id, project::CollisionPolicy policy);
    /// One chord (or single note) to write into a score: written pitches,
    /// spelled as they are to appear, from `start` for `duration` ticks.
    struct WrittenEvent {
        Ticks start = 0;
        Ticks duration = 0;
        std::vector<notation::Pitch> pitches;
    };
    /// Writes `events` into the track's score (first staff; on a grand staff
    /// what is below middle C goes on the bass staff), each split at
    /// the barlines into note values (dotted where they fit) and tied, with
    /// `symbols` over them: one undo step. Ask: needs a decision when voice
    /// 1 has notes in the span; Replace: they go; Merge or NewLane: the
    /// events go in voice 2 instead.
    project::InsertResult write_events(const std::string& track_id, const std::vector<WrittenEvent>& events,
                                       const std::vector<project::ChordMark>& symbols, project::CollisionPolicy policy,
                                       const std::string& label);
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

    // --- Review comments (Main Spec §15) -------------------------------------------------
    /// Pins a comment at `seconds` (project time), on a track or (empty) the
    /// whole project. `created` defaults to now (UTC). One undo step; the id.
    std::string add_comment(double seconds, const std::string& text, const std::string& author,
                            const std::string& track_id = {}, const std::string& created = {});
    bool edit_comment(const std::string& comment_id, const std::string& text);
    bool move_comment(const std::string& comment_id, double seconds);
    bool set_comment_resolved(const std::string& comment_id, bool resolved);
    bool reply_to_comment(const std::string& comment_id, const std::string& author, const std::string& text,
                          const std::string& created = {});
    bool remove_comment(const std::string& comment_id);
    const project::Comment* find_comment(const std::string& comment_id) const;
    /// "2026-11-15T18:42:10Z".
    static std::string now_utc();
    bool remove_marker(const std::string& marker_id);
    /// Hit-point tool: refits the tempo between `from_tick` and `target_tick`
    /// so that `target_tick` sounds exactly on the marker (one undo step).
    bool fit_tempo_to_marker(const std::string& marker_id, Ticks target_tick, Ticks from_tick,
                             std::string* error = nullptr);
    /// Tempo finder result: a steady `bpm` from `from_tick` to `until_tick`
    /// (changes between them removed; the tempo in force after `until_tick`
    /// kept), or to the end when `until_tick` <= `from_tick`. One undo step.
    bool set_steady_tempo(Ticks from_tick, Ticks until_tick, double bpm);

    // --- Chord symbols (the ruler's Chords row; MusicXML <harmony>; printed) ---------------
    /// Sets the chord symbol at `tick` ("Chord symbol"); empty text removes
    /// it. False when the text is not a chord symbol or nothing changes.
    bool set_chord_symbol(Ticks tick, const std::string& text);
    bool remove_chord_symbol(Ticks tick);
    /// Replaces the symbols in [from, to) with these (ticks inside the span),
    /// as one step labelled `label`. An empty list clears the span.
    bool replace_chord_symbols(Ticks from, Ticks to, const std::vector<project::ChordMark>& chords,
                               const std::string& label = "Chord symbols");
    /// The symbol sounding at `tick` (the last one at or before it), or null.
    const project::ChordMark* chord_symbol_at(Ticks tick) const;

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
    /// A MIDI track: notes, sent to an external MIDI device (set_midi_output)
    /// or played by its instrument. Its audio input is the hardware's return.
    std::string add_midi_track(const std::string& name);
    /// A MIDI track's external MIDI output device by name (empty: none). One undo step.
    bool set_midi_output(const std::string& track_id, const std::string& device);
    /// Record-arms a track. Not an undo step and does not dirty the project.
    void set_record_armed(const std::string& track_id, bool armed);

    // --- VCAs and mix groups (Main Spec §12) ------------------------------------
    /// A VCA over `members` (track ids). Returns its id.
    std::string add_vca(const std::string& name, const std::vector<std::string>& members);
    /// Its fader: one coalesced undo step per gesture, like a track fader.
    bool set_vca_gain(const std::string& vca_id, double gain_db);
    /// Rename, mute, members... (one undo step named `label`).
    bool update_vca(const std::string& vca_id, const std::string& label, const std::function<void(project::Vca&)>& change);
    bool remove_vca(const std::string& vca_id);
    /// A mix group over `members`: fader, mute and solo linked (arm off).
    std::string add_mix_group(const std::string& name, const std::vector<std::string>& members);
    bool update_mix_group(const std::string& group_id, const std::string& label,
                          const std::function<void(project::MixGroup&)>& change);
    bool remove_mix_group(const std::string& group_id);
    /// Places clips (one recording pass, or an import) as one undo step. With
    /// `as_takes`, clips already on lane 0 that a new clip overlaps move to
    /// the next free take lane, so nothing recorded is ever lost. Ids are
    /// assigned here and returned in order.
    std::vector<std::string> add_audio_clips(const std::vector<std::pair<std::string, project::AudioClip>>& clips,
                                             const std::string& label, bool as_takes = true);
    /// Print to audio's result, as one undo step: each clip on its track's
    /// own audio lane (an earlier print it overlaps moves to a take lane);
    /// with `play_audio` each track then plays its audio lane, and
    /// `printed_fx` says whether the prints carry the strip and inserts.
    /// Ids are returned in order; empty when nothing could be placed.
    std::vector<std::string> place_prints(const std::vector<std::pair<std::string, project::AudioClip>>& prints,
                                          bool printed_fx, bool play_audio);
    // --- Freeze (Memory Management spec §5) ---------------------------------------------
    /// Live, Frozen (the print plays and matches its source) or Stale.
    project::FreezeState freeze_state(const std::string& track_id) const;
    /// Closes (true) or reopens (false) a frozen track's instrument plug-in;
    /// its settings stay in the track. Unloading needs a print playing.
    bool set_instrument_unloaded(const std::string& track_id, bool unloaded);
    /// Suspend / Resume: the instrument stays loaded but is not played.
    bool set_instrument_suspended(const std::string& track_id, bool suspended);
    /// Back to the live instrument: the MIDI plays, the plug-in is loaded;
    /// the printed audio stays on the track's audio lane.
    bool unfreeze(const std::string& track_id);

    // --- Articulation tracks (Memory Management spec §4) ---------------------------------
    struct ArticulationSplit {
        std::string group;  ///< "staccato", "natural"...
        std::string name;   ///< "Staccato".
        int notes = 0;      ///< Of the part's notes, how many play this sound.
    };
    /// What Expand would make: one track per sound the part's notes play
    /// (as playback selects them); for a part with no notes yet, one per
    /// sound its articulation map has, natural first.
    std::vector<ArticulationSplit> preview_articulation_tracks(const std::string& track_id) const;
    /// Expand Articulations to Tracks: a track per sound, after the part,
    /// with its instrument, map, routing and mix; the part then plays
    /// through them. One undo step; the new ids.
    std::vector<std::string> expand_articulations(const std::string& track_id,
                                                  const std::vector<std::string>& groups = {});
    /// Consolidate to One Track: the articulation tracks go and the part
    /// plays itself again - the same notes. One undo step.
    bool consolidate_articulations(const std::string& track_id);
    /// A part's articulation tracks, in order.
    std::vector<const project::Track*> articulation_tracks(const std::string& track_id) const;
    /// MIDI played in (a recording pass), as one undo step: each region on
    /// its track's main lane; with `as_takes` the main-lane regions it
    /// overlaps move to the next free take lane. Ids are returned in order.
    std::vector<std::string> add_recorded_regions(const std::vector<std::pair<std::string, project::MidiRegion>>& regions,
                                                  const std::string& label, bool as_takes = true);
    /// Comping: the part of a take (an audio clip on a take lane) between
    /// `from` and `to` seconds becomes the main lane's audio there. Main-lane
    /// clips it covers are trimmed or split around it. One undo step.
    bool comp_take(const std::string& track_id, const std::string& take_clip_id, double from, double to);
    /// What an armed instrument or MIDI track records: "midi" or "audio". One undo step.
    bool set_record_source(const std::string& track_id, const std::string& source);

    // --- MIDI tools on regions (one undo step each) ---------------------------------------
    /// A note in a region, as the piano roll selects it.
    struct RegionNote {
        std::string region;
        Ticks start = 0;
        int pitch = 0;
    };
    /// Quantizes `notes`, or with none every note of the track's main-lane
    /// regions ("Quantize"). Regions grow, to whole bars, to keep what moves
    /// past their edges. Returns how many notes moved (0: nothing changed).
    int quantize_regions(const std::string& track_id, const std::vector<RegionNote>& notes,
                         const project::QuantizeSettings& q);
    /// Changes velocities of `notes`, or with none of every note in the
    /// track's main-lane regions ("Velocity"). Returns how many changed.
    int change_region_velocities(const std::string& track_id, const std::vector<RegionNote>& notes,
                                 const project::VelocityChange& change);
    /// Replaces controller `cc`'s events in [from, to) of a region with
    /// `points` (their controller number and channel are set here) ("Draw
    /// CC"). The region grows, to whole bars, to hold them.
    bool draw_region_controller(const std::string& track_id, const std::string& region_id, int cc, Ticks from, Ticks to,
                                std::vector<ControllerEvent> points);
    /// MIDI comping: the notes (by where they start) and controllers of a
    /// take region (lane > 0) between `from` and `to` replace the main
    /// lane's there ("Comp take"). They join the main-lane region at `from`,
    /// or a new one of whole bars. Returns false when nothing changed.
    bool comp_region_take(const std::string& track_id, const std::string& take_region_id, Ticks from, Ticks to);
    /// Swaps a MIDI take onto the main lane; main-lane regions it overlaps
    /// go to its lane ("Promote take").
    bool promote_region_take(const std::string& track_id, const std::string& take_region_id);
    /// Sidechain: keys a track's or bus's strip compressor and gate (slot
    /// -1) or a built-in compressor/gate insert (slot >= 0) from another
    /// track or bus (`source` empty: its own signal again). One undo step
    /// ("Sidechain"); false for an unknown owner or source, or the owner itself.
    bool set_sidechain(const std::string& owner_id, int slot, const std::string& source_id);
    /// Input quantize for MIDI recorded onto the track (0: off). One undo step.
    bool set_input_quantize(const std::string& track_id, Ticks grid);

    // --- Importing a Standard MIDI File --------------------------------------------------
    struct MidiImport {
        std::vector<std::string> track_ids;  ///< Tracks that received a region (new or the target).
        int parts = 0;                       ///< Regions placed.
        int notes = 0;
        bool new_tracks = false;
        bool took_tempo = false;             ///< The empty project took the file's tempo and meter.
        std::string error;                   ///< Why nothing was imported.
    };
    /// Imports a .mid file as one undo step ("Import <file>"). Each part (a
    /// track of the file, or each channel of a single-track file) becomes a
    /// region of whole bars, with its notes and controllers. With
    /// `target_track` (an instrument or MIDI track) and a one-part file, it
    /// lands there; otherwise each part gets a new instrument track named
    /// after it, its sound chosen from the part's program change (channel 10:
    /// percussion). Placed from the bar at `at`. A project with nothing in
    /// it yet takes the file's tempo and meter.
    MidiImport import_midi_file(const std::string& path, const std::string& target_track, Ticks at);
    bool update_audio_clip(const std::string& track_id, const std::string& clip_id, const std::string& label,
                           const std::function<void(project::AudioClip&)>& change);
    bool remove_audio_clip(const std::string& track_id, const std::string& clip_id);
    /// A clip on a track: (track id, clip id).
    using ClipRef = std::pair<std::string, std::string>;
    /// Several clips at once, as one undo step ("Delete clips").
    bool remove_audio_clips(const std::vector<ClipRef>& clips);
    /// The same change to several clips, as one undo step.
    bool update_audio_clips(const std::vector<ClipRef>& clips, const std::string& label,
                            const std::function<void(project::AudioClip&)>& change);
    /// Groups clips and MIDI regions, (track id, clip or region id), so a
    /// move or nudge of one moves them all (Main Spec §9 "region groups").
    /// One undo step "Group"; returns the group id (empty for fewer than 2).
    std::string group_items(const std::vector<ClipRef>& items);
    /// Dissolves the groups these items belong to ("Ungroup").
    bool ungroup_items(const std::vector<ClipRef>& items);

    // --- Clip editing (one undo step each) ---------------------------------------------
    /// Splits the clips of these tracks that `at` (project seconds) falls
    /// inside, every lane. Each left part keeps its id and fade in, each
    /// right part its fade out. Returns the right parts' ids ("Split clip").
    std::vector<std::string> split_audio_clips(const std::vector<std::string>& track_ids, double at);
    /// Splits the MIDI regions of these tracks that `at` falls inside, every
    /// lane ("Split region"). Notes stay with the region they start in (a
    /// note across the cut keeps its length, so nothing re-attacks);
    /// controllers go by their tick. Returns the right parts' ids.
    std::vector<std::string> split_regions(const std::vector<std::string>& track_ids, Ticks at);
    /// A crossfade of `seconds` between two clips on one lane where the first
    /// ends at or after the second starts: an overlap is faded across;
    /// clips that only meet have the second one's start pulled earlier into
    /// its file (as far as it has audio before its start, up to `seconds`)
    /// so the two fade across each other ("Crossfade"). False when neither
    /// is possible.
    bool crossfade_audio_clips(const std::string& track_id, const std::string& first_id, const std::string& second_id,
                               double seconds);
    // --- Shuffle edit mode: the clips on a lane stay butted together -----------------------
    /// Deletes clips and closes the gap each leaves: the later clips on its
    /// lane move earlier by its length ("Delete clip(s)").
    bool remove_audio_clips_shuffle(const std::vector<ClipRef>& clips);
    /// Changes one clip (a trim) and moves the later clips on its lane by
    /// however much its end moved. Coalesces under `label`, as a drag does.
    bool update_audio_clip_shuffle(const std::string& track_id, const std::string& clip_id, const std::string& label,
                                   const std::function<void(project::AudioClip&)>& change);
    /// Moves a clip within its lane: the gap it leaves closes, and it goes in
    /// at the clip boundary (a start or an end, or 0) nearest `target_seconds`,
    /// pushing the clips from there on later by its length ("Move clip").
    bool shuffle_move_audio_clip(const std::string& track_id, const std::string& clip_id, double target_seconds);
    /// Deletes [from, to) from the main-lane clips of these tracks: clips
    /// inside go, clips across an edge are trimmed or split. With `shuffle`
    /// the later clips move earlier to close the gap ("Delete range").
    bool delete_audio_range(const std::vector<std::string>& track_ids, double from, double to, bool shuffle);

    /// Moves clips by `seconds` (negative: earlier, never before 0) ("Nudge clip").
    bool nudge_audio_clips(const std::string& track_id, const std::vector<std::string>& clip_ids, double seconds);
    /// Replaces clips with one (a consolidated file, made by the caller) as
    /// one step ("Consolidate"). Returns the new clip's id, empty on failure.
    std::string replace_audio_clips(const std::string& track_id, const std::vector<std::string>& clip_ids,
                                    project::AudioClip replacement, const std::string& label);
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

    // --- Phrase blocks (docs/spec/ui, "Phrase blocks") --------------------------------
    // Named spans of a track's score on its active version: the Composer
    // view's clips. `why`, when given, says why an edit was refused.
    /// A new block over [from, to). Returns its id, or empty.
    std::string add_phrase(const std::string& track_id, const std::string& name, Ticks from, Ticks to);
    bool rename_phrase(const std::string& track_id, const std::string& phrase_id, const std::string& name);
    /// Changes the span only (trimming); same-label calls coalesce (a drag).
    bool set_phrase_span(const std::string& track_id, const std::string& phrase_id, Ticks from, Ticks to,
                         const std::string& label);
    /// Moves the block and everything written in it (notes, dynamics,
    /// hairpins, slurs) by `delta`. Refused when it would land on notes
    /// written outside the block.
    bool move_phrase(const std::string& track_id, const std::string& phrase_id, Ticks delta,
                     std::string* why = nullptr);
    /// Splits the block in two at `at`; both keep their notes.
    bool split_phrase(const std::string& track_id, const std::string& phrase_id, Ticks at);
    /// A copy of the block and its notes right after it. Returns the new id.
    std::string duplicate_phrase(const std::string& track_id, const std::string& phrase_id, std::string* why = nullptr);
    /// Removes the block; its notes stay.
    bool remove_phrase(const std::string& track_id, const std::string& phrase_id);

    /// Transposes score notes (and the notes tied on from them) by
    /// semitones, spelled in the staff's written key. One undo step.
    bool transpose_notes(const std::string& track_id, const std::vector<notation::EventId>& notes, int semitones);

    // --- Stems (Main Spec §16) ---------------------------------------------------------
    /// The project's stems, or sensible defaults when it has none (group
    /// buses, then instrument families).
    std::vector<project::StemDef> stems() const;
    /// Replaces the stem definitions (one undo step). Stems without an id get one.
    bool set_stems(std::vector<project::StemDef> stems);

    // --- Selection ----------------------------------------------------------------
    /// The one selection every view and panel shares (docs/spec/ui,
    /// "Interaction and state"): a track, a time range and score notes.
    /// Selecting notes in the Composer view selects them in the piano roll
    /// and the inspector, and the reverse.
    struct Selection {
        std::string track;
        Ticks from = 0, to = 0;                ///< Empty when to <= from.
        std::vector<notation::EventId> notes;  ///< Score notes on `track`.
        bool has_range() const noexcept { return to > from; }
        bool operator==(const Selection& o) const
        {
            return track == o.track && from == o.from && to == o.to && notes == o.notes;
        }
        bool operator!=(const Selection& o) const { return !(*this == o); }
    };
    const Selection& selection() const noexcept { return selection_; }
    const std::string& selected_track() const noexcept { return selection_.track; }
    /// A new track clears the notes (they belong to the old one) and keeps the range.
    void select_track(const std::string& track_id);
    /// Replaces the whole selection. Notes that are not on the track's score
    /// are dropped; one notification, none when nothing changed.
    void set_selection(Selection selection);
    /// The range alone (the ruler, a drag across bars).
    void set_selection_range(Ticks from, Ticks to);
    /// Selects score notes on a track; the range becomes the notes' span.
    void select_notes(const std::string& track_id, std::vector<notation::EventId> notes);

    // --- Queries for views ----------------------------------------------------------
    /// Instrument tracks in display order (folders and video excluded).
    std::vector<const project::Track*> music_tracks() const;
    /// The tracks that are staves: music tracks but expanded articulation
    /// tracks (those belong to Producer; their notes are their parent's).
    std::vector<const project::Track*> staff_tracks() const;
    /// Whether a track is audible given mute and solo across the project.
    bool audible(const project::Track& track) const;
    /// Total length worth showing: the last note or region end, at least 32 bars.
    Ticks content_end() const;
    /// Instrument definition for a track, if it names one in the registry.
    const templates::InstrumentDef* instrument_for(const project::Track& track) const;
    /// Sounding range for a track (instrument range, or full piano).
    std::pair<int, int> range_for(const project::Track& track) const;

    // --- Routing (the Visual Routing canvas, Add-On Features spec §5) ------------------
    /// Points a track's or bus's output at `bus_id` ("Route output"). False,
    /// with the reason in `why`, for the master, an unknown bus or a loop.
    bool route_output(const std::string& source_id, const std::string& bus_id, std::string* why = nullptr);
    /// A new send from a track or bus to a bus ("Add send"); false with why
    /// for a loop, a send that is already there, or the master.
    bool add_send(const std::string& source_id, const std::string& bus_id, bool pre_fader, double level_db,
                  std::string* why = nullptr);
    bool remove_send(const std::string& source_id, int index);
    /// Changes one send (level, pre/post, mute, pan); coalesces under `label`.
    bool update_send(const std::string& source_id, int index, const std::string& label,
                     const std::function<void(project::Send&)>& change);
    /// Moves an insert within its chain ("Move insert"): the order the engine runs.
    bool move_insert(const std::string& owner_id, int from, int to);

    // --- Review: snapshots, A/B auditions, approval (Main Spec §15) ----------------------

    /// Records every track's active version under a name ("Create snapshot").
    std::string create_snapshot(const std::string& name);
    bool rename_version(const std::string& track_id, const std::string& version_id, const std::string& name);
    /// The project's name (the package and export names follow it). One undo step.
    bool rename_project(const std::string& name);
    /// Points missing media at files found (stored path -> file found; see
    /// relink::search), every clip and picture cut that uses them, as one
    /// undo step "Relink media". Files inside the project's folder are
    /// stored relative. Returns the references changed (0: nothing to do).
    int relink_media(const std::map<std::string, std::string>& relinks);
    /// Makes a snapshot's versions active, one undo step.
    bool recall_snapshot(const std::string& snapshot_id);
    bool rename_snapshot(const std::string& snapshot_id, const std::string& name);
    bool remove_snapshot(const std::string& snapshot_id);
    /// Track id -> active version id, for every track with versions.
    std::map<std::string, std::string> active_versions() const;
    /// Plays other versions with no undo step (an A/B audition: switching
    /// back and forth is listening, not editing). Unknown ids are skipped.
    bool audition_versions(const std::map<std::string, std::string>& versions);
    /// Ends an audition on `chosen` as one undo step from `from` (the
    /// versions before the audition began); nothing when they are the same.
    bool choose_versions(const std::string& label, const std::map<std::string, std::string>& from,
                         const std::map<std::string, std::string>& chosen);
    /// Approves (locks) or releases a version; one undo step.
    bool set_version_approved(const std::string& track_id, const std::string& version_id, bool approved);
    bool set_snapshot_approved(const std::string& snapshot_id, bool approved);
    /// Why the last edit was refused because of an approved version, or empty.
    const std::string& last_refusal() const noexcept { return project_.last_refusal(); }

    /// Where two versions of a track differ, by bar (notes as played -
    /// performance included - articulation messages, controllers and audio).
    struct VersionDiff {
        std::vector<std::pair<int, int>> bars;  ///< Inclusive bar ranges that differ.
        std::size_t notes_a = 0, notes_b = 0;
        bool same() const noexcept { return bars.empty(); }
        /// "Same music", "bars 5-8, 12".
        std::string describe() const;
    };
    VersionDiff compare_versions(const std::string& track_id, const std::string& version_a,
                                 const std::string& version_b) const;

    // --- Track Trans (Add-On Features spec §10) ----------------------------------------
    /// Whether a track can be transposed: pitched notes only (not audio,
    /// buses, unpitched percussion or the drum channel). False with why.
    bool can_transpose(const project::Track& track, std::string* why = nullptr) const;
    /// Sets the shift in semitones (-48..48). Coalesces as "Transpose <name>",
    /// so a run of +1 presses is one undo step.
    bool set_track_transpose(const std::string& track_id, int semitones);
    /// The same shift on several tracks, explicitly chosen, as one step.
    bool set_tracks_transpose(const std::vector<std::string>& track_ids, int semitones);
    /// "notation" (heard and shown) or "playback" (heard only).
    bool set_transpose_mode(const std::string& track_id, const std::string& mode);
    /// The A/B switch: true plays and shows the track untransposed.
    bool set_transpose_bypass(const std::string& track_id, bool bypass);
    /// Writes the shift into the notes - score notes respelled in the key,
    /// played-in MIDI notes moved - and sets the shift back to 0, as one
    /// undo step ("Apply transpose to notes"). False when there is none.
    bool commit_track_transpose(const std::string& track_id);
    /// `score` with every pitched note moved `semitones`, respelled as the
    /// written key at that point spells it; note ids are kept (the Composer
    /// shows a transposed track this way, and Apply to notes writes it).
    static notation::Score transposed_score(const notation::Score& score, int semitones);
    /// The interval a shift names: "major 2nd up", "octave down".
    static std::string interval_name(int semitones);

    // --- Instrument range guardrails (Add-On Features spec §4) ------------------------
    enum class RangePolicy { Strict, Warn, Extended, Off };
    struct RangeLimits {
        RangePolicy policy = RangePolicy::Off;
        bool policy_is_default = true;
        int low = 0, high = 127;  ///< What is enforced or flagged, sounding (the extended range under Extended).
        int playable_low = 0, playable_high = 127;
        int comfortable_low = 0, comfortable_high = 127;
        bool has_extended = false;
        std::string instrument;   ///< Its name, for messages.
    };
    /// The limits and policy of a track. The default policy is Strict for an
    /// acoustic instrument, Off for synths, percussion, unpitched and tracks
    /// with no instrument profile.
    RangeLimits range_limits(const project::Track& track) const;
    static const char* range_policy_name(RangePolicy policy) noexcept;
    /// "strict", "warn", "extended", "off", or empty for the default ("Range policy").
    bool set_range_policy(const std::string& track_id, const std::string& policy);
    /// A note outside the limits: where, and which way.
    struct RangeIssue {
        notation::EventId id = notation::kInvalidEventId;
        std::size_t staff = 0;
        Ticks tick = 0;
        int sounding = 0;  ///< MIDI note number as it sounds.
        bool above = false;
    };
    /// The notes of `score` (the track's when omitted) outside the track's
    /// limits, in time order. None under the Off policy.
    std::vector<RangeIssue> range_issues(const project::Track& track, const notation::Score& score) const;
    std::vector<RangeIssue> range_issues(const project::Track& track) const;
    /// Moves notes by whole octaves into the limits (those that cannot fit
    /// are left), in `score`. Returns how many moved.
    static int fit_into_range(notation::Score& score, const std::vector<RangeIssue>& issues, const RangeLimits& limits);
    /// The same on the track's score, as one undo step ("Move into range").
    int move_into_range(const std::string& track_id);

private:
    void notify(Change change);
    void document_changed();
    void reset(project::Project project, const std::string& path);
    std::uint64_t generation_ = 0;
    std::uint64_t changes_ = 0;

    const Catalogs& catalogs_;
    project::Project project_;
    std::string path_;
    std::vector<templates::InstalledPatch> installed_patches_;
    bool dirty_ = false;
    Selection selection_;
    std::map<int, Listener> listeners_;
    int next_listener_ = 1;
};

}  // namespace vcc::session
