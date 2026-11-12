# Decisions and licence gates

Main Spec §22 work order 25: "Record all proposed third-party dependencies and
license gates in a maintained decisions document; avoid proprietary API
assumptions."

Everything marked **verify** must be checked against the current licence text
before it ships. Nothing here is legal advice.

## Decided

| # | Decision | Why |
|---|---|---|
| D1 | Domain logic is dependency-free C++17 with CMake. | Builds and tests anywhere; framework choice stays reversible. |
| D2 | Time base: 2880 ticks per quarter. MIDI files use 960 PPQ, converted on export. | Same as the original VC Composer notation engine, so its code ports without rescaling; nested tuplets divide exactly. |
| D3 | Content is versioned JSON, validated in C++ (authoritative) and Python (authoring). | Supplement §8. New packs ship without binaries. |
| D4 | Own PRNG (xoshiro256**) and own distributions for generators. | `std::` distributions differ between standard libraries, and saved ostinato variants must sound the same on every platform (T07). |
| D5 | Roman-numeral convention: unaltered numerals take their root from the mode; written accidentals alter the major-scale degree; case gives the third. | Makes both "i–VI–III–VII" and "i–♭VII–♭VI" read as composers expect. Documented in `theory/include/vcc/theory/Chord.h`. |
| D6 | Undo stores a copy of the document per step. | Simple and obviously correct for the foundation. Note-level editing moves to the original engine's journal of inverses when ported. |
| D7 | IDs are prefixed strings from a per-project counter that undo does not roll back. | Stable IDs across views and saves (Main Spec §8, §20). |
| D8 | Instrument resolution is exact, then approved fallback, then labelled GM placeholder, and never fails. | Supplement §8 "never make missing patches fatal"; Main Spec §11 "no silent remapping". |
| D9 | Template Replace removes only orchestration tracks and the buses fed only by them. Reference/temp tracks, cues and user buses always survive. | Supplement §2, §15 acceptance 15. |
| D10 | No living-artist presets. Historical composer palettes are educational and carry authenticity notes. Regional packs carry review status and "cinematic approximation" flags. North American Indigenous material is `curation_required`. | Supplement §5, §7, §13. |
| D11 | The original VC Composer's notation, engraving, editing, composer, MIDI, audio and scheduler code is ported, not rewritten. Its JSON and atomic-write libraries are merged into `vcc_core`. | Tested, mature code for the hardest parts of Main Spec §7–§8 (see 02). One JSON and one file writer in the product. |
| D12 | The project's meter map and key are canonical and are pushed into every score after each edit. | Composer and Sequencer can never disagree about bars (Main Spec §20). |
| D13 | The desktop application is built on JUCE 8 under the **commercial JUCE 8 licence** (product-owner decision). JUCE is never vendored: `VCC_JUCE_DIR` points at a checkout, or the build fetches the pinned release (`VCC_JUCE_VERSION`, 8.0.9). | Plug-in hosting, audio/MIDI devices and the original VC Composer's JUCE notation view in one framework; resolves O1. |
| D14 | Instruments with no library assigned play through a built-in placeholder synth whose voice follows the instrument family and GM program, and every such track is labelled "Placeholder". | A template is audible on a machine with no libraries, without ever implying the real sound (Main Spec §20, Supplement §8). |
| D15 | The audio thread receives immutable playback snapshots through an atomic hand-off and returns them through the ported lock-free queue. It never allocates, locks or destroys an instrument. | The original VC Composer's real-time discipline, applied to a whole mixer. |

## Open: needs a decision from the product owner

### O1: Application framework licence (resolved: JUCE commercial, D13)

**Action for the product owner:** accept the JUCE 8 EULA and hold the JUCE
licence tier that matches the company's revenue before distributing builds.
JUCE's tiers (Starter, Indie, Pro) have revenue limits. **Verify** the current
terms at juce.com. Nothing in the build checks this.

The original analysis follows.

The spec names JUCE as a plausible candidate (Main Spec §17), and the original
VC Composer already uses JUCE for its window, notation view and plug-in wrapper.
JUCE is dual-licensed: **AGPLv3**, or a **commercial** licence (tiers by
revenue). **Verify** the current terms. A closed-source commercial product
needs the commercial licence.

Alternatives, each with a cost: Qt (LGPL/commercial) plus a separate audio
stack (RtAudio/PortAudio) and the VST3 SDK hosting classes, or a fully native
UI per platform. Recommendation: **JUCE commercial**, because of plug-in
hosting, audio devices and the existing code.

### O2: Porting the original VC Composer code (resolved)

Approved by the product owner and done. See `02-original-vc-composer-reuse-audit.md`.

## Dependency licence gates

| Component | Purpose | Licence (verify) | Gate |
|---|---|---|---|
| JUCE | UI, audio devices, plug-in hosting | AGPLv3 / commercial | O1 |
| Steinberg VST3 SDK | VST3 hosting | The original VC Composer docs record it moved to MIT in Oct 2025. **Verify** | Before Main Phase 1 |
| ASIO SDK (Windows) | Low-latency audio on Windows | Steinberg proprietary licence agreement; redistribution terms apply. **Verify** | Before the Windows audio engine |
| Core Audio / AudioUnit | macOS audio, optional AU hosting | Apple SDK terms | AU hosting optional (Main Spec §3) |
| VST2 | Legacy plug-in hosting | Steinberg no longer licenses VST2 to new developers. **Verify** | Conditional; do not promise (Main Spec §3, §21) |
| Bravura (SMuFL) | Music font | SIL Open Font License; already embedded in the original VC Composer | Bundling OK with OFL notice |
| Video decode | Picture playback | Platform frameworks (AVFoundation, Media Foundation) avoid codec licensing; FFmpeg is LGPL/GPL depending on build. **Verify** codec patents | Before Main Phase 2 video |
| Time-stretch | Warp/stretch | Rubber Band (GPL/commercial), Signalsmith Stretch (MIT). **Verify** | Before Main Phase 3 warp |
| MusicXML | Notation interchange | W3C Community Group specification | Implement natively |
| Loudness (BS.1770 / EBU R128) | Mastering meters | Public standards | Implement natively |
| AAF/OMF | DAW handoff | Conditional (Main Spec §16) | Later; BWF stems are the baseline |
| Sample libraries / player APIs | Library integration | Per vendor; no universal introspection API exists | Adapter per provider, with capability flags (Main Spec §10) |

## Proprietary API assumptions avoided

- The resolver never inspects third-party plug-ins. It works only from patches
  the composer has mapped, provider mapping packs, or user-confirmed scans.
- Shortcut profiles "inspired by" Finale and Sibelius must be authored, not
  copied wholesale (Main Spec §7). The original VC Composer's profiles should
  be reviewed with that in mind when ported.
- Living composers' works appear only as bibliographic metadata in template
  descriptions. No recordings, editions or transcriptions are bundled.
