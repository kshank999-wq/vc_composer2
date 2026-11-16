# VC Composer for iPad

The iPad companion (Add-On Features spec §6): a native SwiftUI app for Apple
Pencil and touch, sharing the desktop's C++ core. It opens and saves the
desktop's own project files (`.vcc2`), enters notes with the desktop's score
editor and engraves with the desktop's layout engine and Bravura, so a score
moves between iPad and desktop with nothing converted.

It does not host VST/AU plug-ins or mix: playback is a lightweight audition
on Apple's built-in sampler. The desktop stays the place for sample
libraries, mixing and delivery.

## Layout

- `../mobile/` - `vcc::mobile::Document`, the C++ facade the app calls.
  Standard library only in its header; built and unit-tested with the rest
  of the core on every platform (`tests/unit/test_mobile.cpp`).
- `VCComposerPad/Bridge/` - the Objective-C++ bridge Swift sees, which also
  draws the engraving's display list with CoreGraphics and CoreText.
- `VCComposerPad/App/` - the SwiftUI app: document (`.vcc2`), score canvas
  (fingers pan and zoom, the Pencil writes; a shadow note previews the pitch
  before it is placed), floating keypad, part picker, playback.
- `project.yml` - the Xcode project, generated with XcodeGen.

## Build

On a Mac with Xcode 16 and XcodeGen (`brew install xcodegen`):

```sh
ipad/build-core.sh          # the core for device + simulator -> ipad/build/VCCCore.xcframework
cd ipad && xcodegen generate
open VCComposerPad.xcodeproj
```

CI (`.github/workflows/ipad.yml`) does the same on a macOS runner and runs
the unit and UI tests on an iPad simulator.

## Signing and TestFlight (later)

Needs, as GitHub secrets: the Apple team ID, an App Store Connect API key
(issuer ID, key ID, the .p8 key) and an Apple Distribution certificate.
Not wired yet.
