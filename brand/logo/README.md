# VC Composer logos

| File | Size | Use |
| --- | --- | --- |
| `VC-Composer-Square.png` | 1254 × 1254, black background | App icon, store listings, social avatars |
| `VC-Composer-Stacked-Transparent.png` | 1254 × 1254, transparent | Splash screens, the about box, anywhere square on a light or dark page |
| `VC-Composer-Horizontal-Transparent.png` | 1983 × 793, transparent | Website header, title bars, banners |

Art Deco gold on black, with the deep red accent, matching VC Writer's
brand (`kshank999-wq/VCWriter`, `brand/`).

These were saved from copies of the artwork shared in chat. Replace them with
the original full-resolution PNGs, under the same names, if those are sharper,
then run `python3 brand/make-assets.py` to cut everything below again.

## Where the logos appear (`brand/make-assets.py`)

| Where | Logo | File |
| --- | --- | --- |
| Desktop app, top-left corner (above Composer / Producer) | Horizontal | `resources/brand/logo-horizontal.png`, embedded |
| Desktop app, Help > About | Stacked | `resources/brand/logo-stacked.png`, embedded |
| Website header and footer | Horizontal | `web/public/brand/logo-horizontal.png` |
| Website home page, beside the headline | Stacked | `web/public/brand/logo-stacked.png` |
| Link previews (Open Graph, X/Twitter) | Horizontal on black, 1200 × 630 | `web/src/app/opengraph-image.png`, `twitter-image.png` |
| Emails (header; its alt text reads "VC COMPOSER" when images are blocked) | Horizontal | `web/public/brand/logo-email.png` |
| Windows installer wizard | Stacked (side), monogram (corner) | `packaging/windows/wizard-*.bmp` |
| iPad app header | Horizontal | `ipad/VCComposerPad/Assets.xcassets/Logo.imageset` |
| README | Horizontal | this folder |

The website, emails and the app's About box use the logo's palette: gold
`#d6a743` (bright `#ebc46a`) on warm black `#0f0e0c`, with the red `#b52a2a`
used sparingly.

## App icons (`brand/icons`)

Cut from `VC-Composer-Square.png`: the whole logo at 1024, 512, 256 and 128;
the VC monogram alone at 64, 48, 32 and 16, where the name cannot be read.

| Where | What it uses |
| --- | --- |
| Desktop app (macOS `.icns`, Windows `.ico`) | `juce_add_gui_app` `ICON_BIG` (1024) and `ICON_SMALL` (64) in `desktop/CMakeLists.txt` |
| Windows installer | `VC-Composer.ico` (16–256), `SetupIconFile` in `packaging/windows/VCComposer2.iss` |
| iPad app | `ipad/VCComposerPad/Assets.xcassets/AppIcon.appiconset` (one 1024 image) |
| Website | `web/src/app/icon.png` (512) and `apple-icon.png` (180) |
