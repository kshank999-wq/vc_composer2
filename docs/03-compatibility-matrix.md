# Compatibility matrix (release-gated)

Main Spec §22 work order 26: "create a release-gated compatibility matrix by
OS, plugin, driver, video format and provider". A row moves to **Supported**
only with a recorded test run (date, build, machine). Until then it is
**Untested** or **Planned**, and the app UI must say so (Main Spec §22).

## Operating systems

| OS | Arch | Domain libs (CI) | Desktop app | Notes |
|---|---|---|---|---|
| macOS (recent supported) | Apple Silicon | CI build + tests | CI job configured, first run pending | Not yet run by a person; minimum version decided after validation (Main Spec §3). Unsigned until notarisation is set up |
| macOS (recent supported) | Intel | CI build + tests | Untested | "where technically feasible" |
| Windows 11 | x64 | CI build + tests (MSVC) | CI job configured, first run pending | Not yet run by a person; unsigned |
| Windows 11 | ARM64 | Untested | Not planned for v1 | |
| Linux | x64 | Dev/CI | Built and checked headlessly (Xvfb) | Development platform, not a product target |

## Audio drivers

| Driver | Status | Gate |
|---|---|---|
| Core Audio (macOS) | Enabled through JUCE; untested on hardware | |
| ASIO (Windows) | Not enabled | ASIO SDK licence (01) |
| WASAPI / DirectSound (Windows) | Enabled through JUCE; untested on hardware | |
| ALSA (Linux) | Enabled through JUCE | Development only |

## Plug-in formats

| Format | Instruments | Effects | Status |
|---|---|---|---|
| VST3 | **Hosted**: scan with crash blacklist, load, play, save/restore state (tested with the built-in test instrument) | Not yet (bus inserts are suggestions) | Main Spec §20 still needs real plug-ins from multiple vendors |
| AU (macOS) | Enabled in the macOS build, untested | Not yet | Conditional |
| VST2 | Not promised | Not promised | Conditional on licensing (01) |

## Library / player providers

Integration is per adapter, with capability flags. Without introspection, the
native plug-in GUI and user mappings are the supported path (Main Spec §10).

| Provider / player | Detect installed | Metadata | Patch state restore | Status |
|---|---|---|---|---|
| Generic VST3 instrument | Scanned list | Plug-in name and vendor | VST3 state chunk (tested) | Hosted |
| Native Instruments Kontakt | Investigate | Investigate | VST3 state chunk | Untested |
| Spitfire Audio | Investigate | Investigate | VST3 state chunk | Untested |
| Vienna Synchron | Investigate | Investigate | VST3 state chunk | Untested |
| Orchestral Tools SINE | Investigate | Investigate | VST3 state chunk | Untested |
| EastWest Opus/PLAY | Investigate | Investigate | VST3 state chunk | Untested |
| Built-in placeholder synth | n/a | n/a | n/a | **Shipping**: family-aware voices, drum kit, labelled "Placeholder" |

## Video

Decoding is through FFmpeg (D16), so the codec list depends on the FFmpeg
build that ships. Frame-accurate decoding is tested on Linux with a
generated PNG-in-MOV movie (`test_video_decoder`), and the desktop check
imports an MPEG-4 movie with source timecode.

| Container / codec | Linux (dev) | macOS | Windows | Status |
|---|---|---|---|---|
| MOV / PNG (test) | Tested | CI pending | Not built (no FFmpeg in Windows CI yet) | Frame-exact |
| MOV / MPEG-4 Part 2 | Tested in desktop check | CI pending | Not built | |
| MOV / ProRes | Untested | Untested | Untested | Depends on FFmpeg build |
| MP4 / H.264 | Untested | Untested | Untested | Patent licence gate (01) |
| MXF / DNxHD/HR | Untested | Untested | Untested | |

## Frame rates (engine)

23.976, 24, 25, 29.97 NDF, 29.97 DF, 30, 47.952, 48, 50, 59.94 NDF, 59.94 DF
and 60 are implemented and unit-tested in `vcc_core`. A/V sync tolerances are
still to be defined in the test plan (Main Spec §20).
