# Compatibility matrix (release-gated)

Main Spec §22 work order 26: "create a release-gated compatibility matrix by
OS, plugin, driver, video format and provider". A row moves to **Supported**
only with a recorded test run (date, build, machine). Until then it is
**Untested** or **Planned**, and the app UI must say so (Main Spec §22).

## Operating systems

| OS | Arch | Domain libs (CI) | Desktop app | Notes |
|---|---|---|---|---|
| macOS (recent supported) | Apple Silicon | CI build + tests | Planned | Minimum version decided after validation (Main Spec §3) |
| macOS (recent supported) | Intel | CI build + tests | Planned | "where technically feasible" |
| Windows 11 | x64 | CI build + tests (MSVC) | Planned | |
| Windows 11 | ARM64 | Untested | Not planned for v1 | |
| Linux | x64 | Dev/CI only | Not a product target | Used for headless tests |

## Audio drivers

| Driver | Status | Gate |
|---|---|---|
| Core Audio (macOS) | Planned | Phase 1 |
| ASIO (Windows) | Planned | ASIO SDK licence (01) |
| WASAPI (Windows) | Planned (fallback) | Phase 1 |

## Plug-in formats

| Format | Instruments | Effects | Status |
|---|---|---|---|
| VST3 | Planned | Planned | Phase 1; at least one tested plug-in from multiple vendors (Main Spec §20) |
| AU (macOS) | Optional | Optional | Conditional |
| VST2 | Not promised | Not promised | Conditional on licensing (01) |

## Library / player providers

Integration is per adapter, with capability flags. Without introspection, the
native plug-in GUI and user mappings are the supported path (Main Spec §10).

| Provider / player | Detect installed | Metadata | Patch state restore | Status |
|---|---|---|---|---|
| Generic VST3 instrument | Planned | Plug-in name only | VST3 state chunk | Phase 1 |
| Native Instruments Kontakt | Investigate | Investigate | VST3 state chunk | Untested |
| Spitfire Audio | Investigate | Investigate | VST3 state chunk | Untested |
| Vienna Synchron | Investigate | Investigate | VST3 state chunk | Untested |
| Orchestral Tools SINE | Investigate | Investigate | VST3 state chunk | Untested |
| EastWest Opus/PLAY | Investigate | Investigate | VST3 state chunk | Untested |
| Built-in GM placeholder | n/a | n/a | n/a | Engine resolves to it today; synth pending |

## Video

| Container / codec | macOS | Windows | Status |
|---|---|---|---|
| QuickTime ProRes | Planned (AVFoundation) | Investigate | Phase 2 |
| MP4 H.264 | Planned | Planned (Media Foundation) | Phase 2 |
| DNxHD/HR (MXF/MOV) | Investigate | Investigate | Phase 2 |

## Frame rates (engine)

23.976, 24, 25, 29.97 NDF, 29.97 DF, 30, 47.952, 48, 50, 59.94 NDF, 59.94 DF
and 60 are implemented and unit-tested in `vcc_core`. A/V sync tolerances are
still to be defined in the test plan (Main Spec §20).
