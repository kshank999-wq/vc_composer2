DEVELOPMENT SPECIFICATION • VERSION 1.0 • NOVEMBER 11, 2026

Audience: coding agents, application engineers, DSP/audio engineers, product and QA teams. Status: implementation blueprint; technical decisions labeled proposed remain subject to validation.

# 1. Product Definition

VC Composer 2 is a native desktop application for macOS and Windows that treats conventional musical notation as the primary authoring language while providing a fully integrated, professional-grade MIDI sequencer, audio editor, mixer, mastering workflow, instrument hosting, picture synchronization, and collaboration/versioning. It is a standalone workstation—not a plug-in for Pro Tools or another DAW. A composition must be playable, editable, mixable, and deliverable without leaving VC Composer 2.

Core promise: write a score on staves; its linked performance appears instantly in the sequencer; select cinematic scoring templates that initialize an orchestration and quickly locate compatible samples/articulations already owned by the composer.

# 2. Product Principles

- Notation is the source of musical intent; MIDI performance is a synchronized, editable interpretation—not a disconnected export.

- One project, one consistent track list and routing model across Composer, Sequencer, and Mixer views.

- Make scoring to picture and cue management first-class, not add-ons.

- Let users retain their own sample libraries, players, effects, hardware and DAWs.

- Every automatic choice (library match, articulation, playback mapping) is visible, overridable, and savable.

- Prevent collaborator overwrites and support rapid A/B comparisons of musical alternatives.

- Keep audio processing and project editing local and functional offline; cloud collaboration is an opt-in layer.

# 3. Platform, Packaging, and Baseline Compatibility

| Area                  | Requirement                                                                                                                                                                                                                          |
|-----------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Operating systems     | Native installable desktop application for Windows 11 and recent supported macOS versions, Intel and Apple Silicon where technically feasible; publish minimum supported OS after validation.                                        |
| Distribution          | Signed installers: Windows installer/executable and notarized macOS .dmg/.pkg/app; local project storage; optional cloud workspace.                                                                                                  |
| Plugin host           | VST3 instruments and effects as primary cross-platform format. Optional AU support on macOS proposed. Legacy VST2 hosting is conditional on SDK/licensing, architecture, host testing, and rights; do not promise universal support. |
| Audio and MIDI I/O    | Windows ASIO and macOS Core Audio; MIDI device enumeration, hot-plug recovery, multi-channel outputs and external controllers. Other backends evaluated separately.                                                                  |
| Professional baseline | Configurable sample rate, buffer size, bit depth for recorded media, multicore processing, low-latency live input, project backups, crash recovery.                                                                                  |
| Video                 | Import common editorial movie files via an appropriately licensed media subsystem; verify codecs/containers per target platform.                                                                                                     |

# 4. Shared Application Shell / Navigation

Persistent left-hand Track & Library panel across Composer and Sequencer views; the same track IDs, order, labels, instrument assignment, version selection, solo/mute, routing and status controls must appear in both. Main workspace switches between score staves and a timeline/piano roll. A top toolbar provides transport, notation entry, editing tools, undo/redo, snapping, metronome, key/time signature, tempo, markers, cue navigation, and configurable shortcuts.

- Composer workspace (default): one or more linked staves per instrument, engraved notation, note entry, articulations, dynamics and score layout.

- Sequencer workspace: aligned MIDI/audio lanes, piano roll, MIDI controllers, tempo/automation lanes, waveform editing and performance refinement.

- Mixer: docked or detachable independent window/monitor; live synchronized changes while composing.

- Video/Timecode window: dock or detach to another display; display picture, timecode and cue/hit markers.

- Library browser in left panel: search, filters, audition, player open, assign, mapping and preset management.

- Flexible window layouts; multi-monitor persistence, zoom, keyboard access, accessibility labels and customizable themes.

# 5. Project Setup and Scoring Template Wizard

At project creation, request project name/location, scoring category/mood (not just genre), initial template, key and mode, time signature, tempo and tempo map, frame rate/timecode start, sample rate, and optional picture import. User can override all template defaults without rebuilding the project.

Selecting a scoring template creates named score staves AND mirrored sequencer tracks; expected orchestra sections/instruments, staff clefs and transpositions, suggested articulation slots, routing/buses, mixer groups, optional effect chains, and candidate library search queries are prepared automatically. A template may be "instrumentation only", "instrumentation + mappings", or "full production"; missing library assets never silently break the project.

# 6. Cinematic Template & Preset System

Provide an extensible curated starter catalog, targeting ~50 templates for initial content planning (actual availability dependent on assets and testing). A preset is a reusable structured configuration—not bundled copyrighted instrument samples. Each template specifies categories, recommended orchestration, optional mix routing, articulation intents, and mood-specific instrument choices.

| Template families     | Starter examples                                                                                             |
|-----------------------|--------------------------------------------------------------------------------------------------------------|
| Suspense / thriller   | Slow tension, ticking tension, creeping threat, investigative underscore, ticking clock, high-anxiety build  |
| Horror                | Dread drones, dissonant strings, jump scare, supernatural atmosphere, creature pursuit, psychological horror |
| Heroic / adventure    | Hero entrance, victorious brass, epic quest, rescue, triumph, noble theme                                    |
| Action                | Chase, combat, ticking percussion, high-impact trailer-style, tactical stealth, escalating pursuit           |
| Drama / emotion       | Grief, reflection, intimacy, melancholy, bittersweet resolution, reconciliation                              |
| Comedy / whimsy       | Light pizzicato, slapstick, quirky woodwinds, romantic comedy, mischievous tension                           |
| Mystery / crime       | Noir, procedural, dark investigation, revelation, conspiratorial tension                                     |
| Romance / warmth      | Tender piano, cinematic strings, hopeful reunion, nostalgic memory                                           |
| Sci-fi / fantasy      | Space wonder, alien threat, magical discovery, fantasy battle, otherworldly ambient                          |
| Documentary / minimal | Neutral underscore, uplifting documentary, modern pulse, sparse emotional texture                            |

Each template should supply a preview description, tags, recommended tempo range, mode/harmonic character (suggestion only), suggested instrumentation, articulations, routing and optional example score fragments owned or properly licensed by VC. Include search, favorites, personal templates, duplicate/modify, import/export template package, revision tracking and content updates.

# 7. Composer View: Professional Notation Editing

- Full score and linked instrument parts; staves/groups/brackets, multiple voices, grand staves, clefs, transposing instruments, concert vs written pitch, percussion maps and tablature as separately scoped support.

- Manual mouse/keyboard entry, step-time MIDI keyboard input, live MIDI recording with rhythm quantization and human-editable transcription; selectable note duration, rests, accidentals, ties, slurs, tuplets, grace notes, beams, enharmonic spelling, repeats and endings.

- Articulations (staccato, tenuto, accents, marcato, tremolo, pizzicato, etc.), dynamics/hairpins, expression text, tempo/metric changes, fermatas, ornaments and techniques.

- Selection/drag, cut/copy/paste, transpose, revoice, explode/implode, copy articulations, measure editing, multi-measure rest, conductor score and part extraction.

- Notation engraving and print layout: spacing, collisions, page/part layout, numbering, staff size, title/credits, print preview and PDF export.

- Shortcuts: factory profiles inspired by Finale and Sibelius workflows plus VC Default; user-editable keys, keyboard-map conflict detection and import/export. Do not assume legal ability to reproduce proprietary shortcut datasets wholesale.

- Score display linked to time-based playback; active playback cursor, audition selected passage, loop selection, count-in and click track.

# 8. Notation ↔ MIDI Synchronization Contract (Critical)

Do not maintain two independent compositions. Use a canonical event model with stable IDs, score objects and performance overrides. Each score note carries written pitch, sounding pitch, onset, notated duration, staff/voice, dynamics/articulation references, tie/slur relationships and source provenance. Rendered MIDI has note-on/off, channel, velocity, note-expression, CC/automation and optional keyswitch/program-change events.

| Edit path               | Required behavior                                                                                                                                                  |
|-------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Notation → sequence     | Entering/moving/removing notes regenerates corresponding MIDI events immediately, preserving unrelated manual performance edits.                                   |
| Articulation → playback | Translate symbols through per-instrument articulation maps into keyswitches, CC values, channel routing or player-specific controls, with latency compensation.    |
| Dynamics / hairpins     | Map notation to configurable velocity, CC1, CC11, expression and per-library curves; provide a human-readable inspector.                                           |
| MIDI → notation         | Pitch/timing edits to linked notes update notation when semantically representable; pure performance offsets/CC remain performance-layer overrides.                |
| Conflicting edits       | Display non-destructive reconciliation options when MIDI edit makes notation ambiguous (tuplets, overlaps, note spelling). Never silently destroy expressive data. |
| Versions                | Every track version references an immutable snapshot of notation and its associated MIDI/performance changes; selection swaps both views together.                 |

Implementation detail: event stores must separate score intent, performance modifiers and rendered playback events. Cache rendering by dirty time range; do not continuously regenerate an entire score. Include notation-to-MIDI regression tests for tempo changes, tuplets, tied notes, nested repeats, transpositions and mixed meter.

# 9. Sequencer / Editing Workspace

- Mirror each score instrument in timeline tracks; permit additional audio tracks, AUX/bus tracks, MIDI-only tracks, reference/temp tracks and video tracks.

- Piano roll, velocity lanes, channel aftertouch/pitch bend, CC curves, expression maps, quantize/swing/groove, grid/snap, automation and MIDI chase.

- Record MIDI live; comp/loop takes, overdub, step recording, punch, retrospective record where feasible, MIDI input filtering and merge/replacement modes.

- Standard DAW timeline operations: edit/crop/split/slip/stretch, duplicate, crossfade, clip gain, reverse, warp/time stretch (algorithm/license evaluation), freeze/bounce and region groups.

- Video markers, cue boundaries and tempo changes are visible on shared time ruler; sample/beat/timecode display switch.

# 10. Library Browser, Discovery & Native Player Integration

This is a flagship feature. The system indexes available instrument identities/articulation metadata only through supported means: user-specified directories and permitted parsable manifests, VST3 plugin metadata, a curated provider catalog, user-confirmed scan results and explicit player integrations when their APIs/SDKs permit. A VST host generally cannot introspect arbitrary presets or instruments inside another company’s plugin; do not rely on fictional universal APIs.

1.  Scan configured VST3 directories and enumerate valid instruments/effects through safe sandboxed discovery; quarantine crashing plugins and show diagnostic status.

2.  Detect configured sample-library locations and installed players when feasible; distinguish "player installed" from "library detected" from "instrument metadata known".

3.  Offer filters for provider, player, family, instrument, technique, dynamics/length, mood, installed status, favorites and user tags.

4.  For scoring template track (e.g., French Horns \> marcato), rank mapped candidate patches and display exact match vs inferred/requires user verification.

5.  Audition candidate sounds through their native plugin/player, with a temporary preview channel and safe loading limits; do not claim direct sample decoding if library is encrypted/proprietary.

6.  Assign selected plugin state/patch to track; persist plugin state via supported state chunks, preset URI/ID or explicit user-confirmed restore workflow.

7.  Create an articulation map automatically when reliable metadata exists; otherwise teach it via guided assignment of keyswitches, MIDI CC, program change, channel switching and velocity layers.

8.  Save per-provider mapping packs, per-instrument presets and full scoring templates; users can edit, share and reload them.

Examples of supported ecosystems to investigate and test: EastWest PLAY/OPUS (current player support must be verified), Native Instruments Kontakt, Spitfire Audio, Vienna Synchron, Orchestral Tools/SINE, and general VST3 instruments. Treat each as an adapter with capability flags; integration depends on provider permissions and supported formats. Where introspection is unavailable, the native plugin GUI is the fallback and user-created mappings remain fully supported.

# 11. Instrument and Articulation Mapping Model

Fields: provider_id, plugin_id, plugin_format, player_version, library_id, patch_identifier, articulation_id, display_name, technique/category, activation_method, MIDI_channel, keyswitch_note, keyswitch_timing_offset, CC_number/value/range, program_change, velocity_range, round_robin notes, dynamic_controller, expression_controller, tuning, audio_outputs, plugin_state_reference and validation_status. Articulation changes must be chaseable on playback from any timeline location.

Every track may have multiple articulation maps and patches, but a template maps to intent (e.g., "short accented horns") rather than hard-coded sample filenames. The resolved instrument is a project-level user choice. Include missing-asset substitution prompts, no silent remapping, and a mapping health indicator.

# 12. Pro DAW Engine / Mixing & Mastering

- Sample-accurate audio engine, transport and latency-aware scheduler with plugin delay compensation, automation, bus routing and deterministic offline bounce.

- Audio input and output assignments per track, mono/stereo/multichannel channels, configurable hardware I/O and sends/returns; ASIO and Core Audio initial backends.

- Mixer with faders, pan, meters, gain staging, mute/solo, groups/VCAs, inserts, pre/post-fader sends, AUX buses, subgroup buses, sidechains, master and monitor paths.

- Resizable mixer in independent detachable window with per-project monitor layouts; edits reflect instantly in track inspector and playback.

- Host VST3 effects/instruments with scanning, preset state, GUI hosting, bypass, automation, oversampling if provided by plugin, latency handling, bridging strategy only if viable.

- Audio recording, monitoring, comping, editing, crossfades, clip gain, fades, automation and bounce in place.

- Mastering chain (EQ, compression, limiting via bundled or external plugins), loudness/peak analysis, true peak and export/dither options as supported by engine.

- Automatic backups, save-on-transport-stop option, crash-resilient journaling, missing media relink, plugin crash isolation wherever feasible.

# 13. Video Scoring & Timecode

- Dedicated video track, movie import and picture preview with independent resize/detach/fullscreen on secondary display.

- Project timecode, selectable film/TV frame rates (including fractional rates and applicable drop/non-drop semantics), start TC, bars/beats, feet+frames as optional extension.

- Synchronized transport and visible timecode overlays; add cue markers, hit points, streamers/punches and sync markers.

- Tempo map and hit-point tools: anchor musical beats to picture events without accidental sync drift; recalculate event timing consistently.

- Picture revisions: import replacement cut, preserve prior version, apply user-approved conform/offsets and flag changed regions; full automated conform is later phase.

- Temp score/reference audio lives in dedicated reference track(s) with visible toggle, mute/solo and level control; excluded from final exports by default unless explicitly enabled.

- Video export option: picture + stereo score draft for director review, preserving original or licensed/transcoded codec rules.

# 14. Cue Management for Film & Episodic Work

Hierarchy: Production → Reel/Episode → Scene → Cue. Cue IDs, titles, descriptions, start/end TC, status, revision, deadline, director notes, tempo/meter, palette/template, assigned composer and deliverables. Link cue to score/timeline range; support overlaps and transitions. Dashboard for unsent/awaiting feedback/approved/locked cues with search, filters and sequence ordering.

Cue sheet exports, spotting notes, usage/timecode reports and deliverable status; allow one score per cue or master project with shared libraries and reusable track setup. Provide render-by-cue, bounce naming presets and batch export.

# 15. Collaboration, Track Versions & Director Audition

Non-destructive per-track versions are a core feature, similar to screenplay drafts: Version 1/2/3 (renamable) with independent notes, MIDI edits, articulations, player state references and optional mix settings. Users select the active version from a drop-down in the persistent left track panel; duplicate version instantly, add notes, compare and restore previous versions. Also support full-cue and project snapshots so a director can audition coherent orchestration choices without manually swapping dozens of parts.

- Collaboration roles: owner, editor/composer, arranger/orchestrator, reviewer/director and view-only; permissions by project and optionally cue/track.

- Prevent overwrites using version IDs, optimistic concurrency and track/cue-level locks or branches; display merge/conflict tools for overlapping changes.

- Shared reviews: timecode comments, annotations, review links or local review packages, A/B/C playlists and approved-version pinning.

- Do not sync third-party sample content/plugins illegally. Share project metadata, composition and authorized media; indicate missing plugins/libraries on another workstation and allow substitutes.

- Offline edits must reconcile safely on reconnect; maintain audit history of who changed what and when.

- Export isolated director review mixes for each cue or selected variation; never overwrite approved deliverables without explicit confirmation.

# 16. Import / Export / Interoperability

| Output / input      | Scope                                                                                                                                                                                                                                       |
|---------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Notation            | MusicXML import/export for staff-level interchange; PDF full scores and parts; printing; native project files.                                                                                                                              |
| MIDI                | Standard MIDI File Type 0/1 import/export; include tempo/meter and relevant CC / articulation events; warn on nonportable mappings.                                                                                                         |
| Audio               | WAV, AIFF and common review format where supported; mixdown, stems, individual tracks, cue batches, mono/stereo/multichannel targets, sample-rate and bit-depth controls.                                                                   |
| DAW handoff         | AAF/OMF only if implemented/licensed and tested; otherwise multitrack Broadcast WAV stems with shared origin and MIDI files as dependable baseline. Do not claim full session interchange with Pro Tools/Logic/Cubase/Studio One/FL Studio. |
| Video               | Picture+reference audio output with TC and watermark/overlay options; deliver compatible movie files subject to encoder availability.                                                                                                       |
| Reports             | Cue sheets, spotting notes, cue status, track lists, version comparison, recording/mix notes and collaborator tasks as PDF/CSV where suitable.                                                                                              |
| Project portability | Collect/copy authorized audio assets, inventory missing plugins/library dependencies, export project package; do not bundle proprietary sample-library content by default.                                                                  |

# 17. Suggested Technical Architecture (Implementation Proposal)

Build a native application with a separation of UI, domain model, music notation, scheduling/audio DSP, plugin hosting, media decoding and collaboration services. C++ with JUCE (subject to current license evaluation) is a plausible audio/plugin-hosting candidate; notation engraving may require a custom engine or legally compatible third-party technology after licensing diligence. A web-based editor is not adequate as the sole low-latency audio engine.

| Subsystem                  | Responsibilities                                                                                                        |
|----------------------------|-------------------------------------------------------------------------------------------------------------------------|
| Project/document model     | Stable IDs, score graph, note/measure/track/cue entities, transactional undo/redo, snapshots and migration.             |
| Notation editor / engraver | Staff layout, glyph rendering, semantic editing, part extraction, MusicXML.                                             |
| Playback interpreter       | Score intent → articulations/CC/notes; deterministic render + performance overrides.                                    |
| Realtime audio engine      | Audio graph, device drivers, scheduling, mixing, latency/PDC, offline bounce; no blocking operations in audio callback. |
| Plugin services            | VST3 host, scan isolation, instance lifecycle, state persistence, GUIs, mapping adapters.                               |
| Library catalog            | Indexed metadata, search relevance, provider adapters, scan telemetry and user mappings.                                |
| Video / timecode           | Decode/preload, transport sync, visible review and cue markers.                                                         |
| Collaboration layer        | Local-first versioned operations, merge/conflict resolution, permissions, optional hosted sync.                         |
| Presentation layer         | Composer, Sequencer, Mixer, Library, Cue, Video and Settings views sharing state.                                       |

Suggested persisted entities: Project, Production, Cue, Track, Staff, Voice, Measure, NoteEvent, NotationMark, PerformanceOverride, MIDIEventCache, PluginInstance, InstrumentAssignment, ArticulationMap, LibraryCatalogEntry, Template, TrackVersion, CueSnapshot, MediaAsset, TimecodeMarker, AutomationLane, RoutingNode, Collaborator, Comment and AuditEvent. Use schema versioning and atomic saves. Prefer content-addressed or reference-based assets and portable relative paths.

# 18. Example User Workflow / End-to-End Acceptance Journey

9.  Composer selects New Film Score → "Psychological Suspense"; enters D minor, 4/4, initial tempo and 23.976 fps picture settings.

10. Wizard generates labeled strings, woodwinds, brass, low percussion, synth texture and keyboard staves; sequencer tracks, buses and mixer channels appear automatically.

11. Library browser searches installed providers for suitable "sul ponticello strings", "low brass swells", "short pulses"; candidate results show verified vs inferred status.

12. Composer auditions sounds using native player UI, chooses a library patch, confirms an articulation map and saves this combination as a personal preset.

13. Composer enters phrases on staff via computer/MIDI keyboard; articulations and dynamics create appropriate MIDI/CC while Sequencer view mirrors the notes.

14. Composer switches to Sequencer view to adjust a note’s onset and expression curve; notation remains semantically stable and performance adjustments persist.

15. Video opens on second monitor; musician uses timecode markers and toggles the dedicated temp track to compare timing.

16. Mixer opens on a third or second screen; composer uses inserts, sends and buses while notation remains active.

17. Composer duplicates French Horn track Version 1 to Version 2 and tries a different voicing; director can A/B selected track or whole-cue snapshots.

18. Export full score/parts PDF, MIDI, stereo preview picture, individual stems and cue report with consistent naming and timecode.

# 19. Milestones / Dependency-Aware Build Plan

| Phase                        | Deliverable / exit gate                                                                                                                                                       |
|------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| 0 – Discovery                | Review original VC Composer source/spec if supplied; prototype notation renderer, one VST3 host, sample-accurate transport and player mapping. Validate licensing/OS targets. |
| 1 – Core scoring MVP         | Desktop project shell, notation entry, shared track panel, MIDI mirror, basic playback, keyboard shortcuts, save/reload and basic VST3 hosting.                               |
| 2 – Scoring workflow         | Template wizard, starter preset catalog, library search/mapping UI, articulation engine, media/video+timecode, temp tracks and cue model.                                     |
| 3 – DAW production           | Audio recording/editing, mixer/buses/inserts/sends, automation, device I/O, latency compensation, stem/score export and stabilization.                                        |
| 4 – Collaborative production | Track versions, cue snapshots, director A/B review, optimistic sync with conflict handling, roles, comments and project packaging.                                            |
| 5 – Professional hardening   | Cross-platform performance, plugin compatibility matrix, video sync testing, recovery, accessibility, importer/exporter validation, signed installers.                        |

Release sizing guidance: a true full-scale DAW is a multiyear engineering program. Phases are capability gates, not arbitrary short calendar commitments. VST2, deep proprietary library indexing, AAF/OMF, advanced video conform and AU hosting are conditional rather than foundational release blockers.

# 20. Engineering Acceptance Criteria & QA

- Any added notation instrument creates exactly one configured logical track (plus explicitly requested audio/plugin outputs) visible in both screens with the same ID and label.

- Switching Composer ↔ Sequencer never duplicates, drops or changes notes; articulations and expressive offsets survive ten edit/save/reopen cycles.

- Opening the Mixer on another monitor preserves playback and reflects fader/routing changes immediately.

- At least one tested VST3 plugin from multiple vendors loads, saves and restores state; failed scan cannot crash the whole project.

- Template creation with an absent instrument library yields a recoverable "unresolved instrument" slot and selectable alternatives rather than missing sound claims.

- Track Version 1 and 2 can be alternated repeatedly without overwriting one another; cue snapshots recall coherent combined versions.

- Temp audio mute/unmute is sample-safe and temp tracks are not exported into deliverables unless explicitly requested.

- Video and audio remain within an agreed measurable sync budget during seek, playback and export; define actual frame/sample tolerances in test plan.

- Project export delivers a readable score PDF, MIDI timing/tempo, aligned WAV stems and a cue report.

- Force-quit/crash recovery opens the last committed project state without corruption; collaboration conflicts never cause silent data loss.

# 21. Decisions, Constraints & Known Risks

- Original VC Composer: requested as a functional baseline, but its source/spec was not available to this drafting session. Conduct a feature-by-feature parity audit before implementation and integrate findings into this spec.

- Legacy VST2 hosting may be legally or technically constrained; implement only where distribution, SDK access, platform and compatibility are verified.

- Native sample player patch search is provider-dependent; design honest fallback workflows and per-provider integration partnerships rather than assuming universal metadata access.

- Licensing must be confirmed for notation glyph fonts/engraving components, DSP/codec libraries, sample content, plugin APIs and redistributable presets.

- Third-party plugins are not necessarily available on each collaborator’s machine or OS; offer freeze/render/relink and substitution support.

- Deterministic score↔MIDI editing and expressive note playback are high-complexity components requiring dedicated prototypes and automated regression suites.

- Low-latency plugin audio, high-resolution notation, video decode and collaboration must not block one another; isolate processes and threads appropriately.

# 22. Coding Agent Instructions / Immediate Work Orders

19. Create a repository plan and module boundaries; propose build system, test infrastructure, code signing/distribution workflow and supported architecture targets.

20. Write a typed schema for canonical notation, performance overrides, track versions, player/articulation mappings, cue/timecode objects and mixer routing.

21. Implement a vertical slice: one stave, three notes, staccato/dynamics, a VST3 instrument, a synchronized piano roll, a bus and a detachable mixer.

22. Prove bidirectional edit preservation, plugin state restore and crash-safe save before adding broad UI features.

23. Add template creation with missing-library graceful fallback; implement first local user-mapping adapter and search index.

24. Add automated tests at domain, MIDI scheduler, audio graph, media timecode and project-migration layers.

25. Record all proposed third-party dependencies and license gates in a maintained decisions document; avoid proprietary API assumptions.

26. Document limitations explicitly in the UI and create a release-gated compatibility matrix by OS, plugin, driver, video format and provider.

Definition of done: a composer can create a score from a cinematic template, enter real notation, hear expressive instrument-library playback, edit the mirrored MIDI, mix through professional routing, score to timecode picture, compare track/cue versions, and export professional deliverables from one installed desktop application.
