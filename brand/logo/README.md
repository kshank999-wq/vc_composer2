# VC Composer logos

| File | Size | Use |
| --- | --- | --- |
| `VC-Composer-Square.png` | 1254 × 1254, black background | App icon, store listings, social avatars |
| `VC-Composer-Stacked-Transparent.png` | 1254 × 1254, transparent | Splash screens, the about box, anywhere square on a light or dark page |
| `VC-Composer-Horizontal-Transparent.png` | 1983 × 793, transparent | Website header, title bars, banners |

Art Deco gold on black, with the deep red accent, matching VC Writer's
brand (`kshank999-wq/VCWriter`, `brand/`).

These were saved from copies of the artwork shared in chat. Replace them with
the original full-resolution PNGs, under the same names, if those are sharper.

## App icons (`brand/icons`)

Cut from `VC-Composer-Square.png`: the whole logo at 1024, 512, 256 and 128;
the VC monogram alone at 64, 48, 32 and 16, where the name cannot be read.

| Where | What it uses |
| --- | --- |
| Desktop app (macOS `.icns`, Windows `.ico`) | `juce_add_gui_app` `ICON_BIG` (1024) and `ICON_SMALL` (64) in `desktop/CMakeLists.txt` |
| Windows installer | `VC-Composer.ico` (16–256), `SetupIconFile` in `packaging/windows/VCComposer2.iss` |
| iPad app | `ipad/VCComposerPad/Assets.xcassets/AppIcon.appiconset` (one 1024 image) |
| Website | `web/src/app/icon.png` (512) and `apple-icon.png` (180) |
