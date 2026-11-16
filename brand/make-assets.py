#!/usr/bin/env python3
"""Makes every logo asset the products use from the three originals in
brand/logo/. Replace an original (same name) and run this again:

    python3 brand/make-assets.py

Needs Pillow. Writes:
  resources/brand/          the desktop app's (embedded): corner logo, about box
  web/public/brand/         the website's: header, hero, footer, emails
  web/src/app/              opengraph-image.png and twitter-image.png (link previews)
  packaging/windows/        the installer wizard's side and corner images (BMP)
  ipad/.../Logo.imageset    the iPad app's header logo
The app icons (brand/icons) are made separately; see brand/logo/README.md.
"""
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
LOGO = ROOT / "brand" / "logo"
BLACK = (10, 9, 8)  # the logos' own near-black


def trimmed(name: str) -> Image.Image:
    """The logo cut to its visible art (soft glow kept), RGBA."""
    im = Image.open(LOGO / name).convert("RGBA")
    alpha = im.getchannel("A").point(lambda v: 255 if v > 8 else 0)
    box = alpha.getbbox()
    pad = 6
    box = (max(0, box[0] - pad), max(0, box[1] - pad), min(im.width, box[2] + pad), min(im.height, box[3] + pad))
    return im.crop(box)


def width(im: Image.Image, w: int) -> Image.Image:
    return im.resize((w, round(im.height * w / im.width)), Image.LANCZOS)


def fit(im: Image.Image, w: int, h: int) -> Image.Image:
    scale = min(w / im.width, h / im.height)
    return im.resize((max(1, round(im.width * scale)), max(1, round(im.height * scale))), Image.LANCZOS)


def on_black(im: Image.Image, size: tuple[int, int], art: tuple[int, int]) -> Image.Image:
    """`im` fitted into `art` and centred on a black canvas of `size`."""
    canvas = Image.new("RGBA", size, BLACK + (255,))
    art_im = fit(im, *art)
    canvas.alpha_composite(art_im, ((size[0] - art_im.width) // 2, (size[1] - art_im.height) // 2))
    return canvas


def save(im: Image.Image, path: Path, **kw) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    im.save(path, optimize=True, **kw)
    print(f"{path.relative_to(ROOT)}  {im.width}x{im.height}")


def main() -> None:
    horizontal = trimmed("VC-Composer-Horizontal-Transparent.png")
    stacked = trimmed("VC-Composer-Stacked-Transparent.png")

    # Desktop app: the corner logo (drawn about 150 px wide, sharp at 2x and
    # beyond) and the about box.
    save(width(horizontal, 640), ROOT / "resources" / "brand" / "logo-horizontal.png")
    save(width(stacked, 560), ROOT / "resources" / "brand" / "logo-stacked.png")

    # Website.
    web = ROOT / "web" / "public" / "brand"
    save(width(horizontal, 1400), web / "logo-horizontal.png")
    save(width(stacked, 1000), web / "logo-stacked.png")
    save(width(horizontal, 600), web / "logo-email.png")
    preview = on_black(horizontal, (1200, 630), (1080, 520))
    save(preview.convert("RGB"), ROOT / "web" / "src" / "app" / "opengraph-image.png")
    save(preview.convert("RGB"), ROOT / "web" / "src" / "app" / "twitter-image.png")

    # Windows installer (Inno Setup, modern wizard): the side image at 100%
    # and 250% scale, and the corner image. 24-bit BMP, which every Inno 6 reads.
    win = ROOT / "packaging" / "windows"
    for w, h in ((164, 314), (410, 797)):
        side = on_black(stacked, (w, h), (round(w * 0.9), round(h * 0.6)))
        save(side.convert("RGB"), win / f"wizard-side-{w}x{h}.bmp")
    icons = ROOT / "brand" / "icons"
    for size, source in ((55, "VC-Composer-64.png"), (138, "VC-Composer-256.png")):
        small = Image.open(icons / source).convert("RGBA").resize((size, size), Image.LANCZOS)
        save(on_black(small, (size, size), (size, size)).convert("RGB"), win / f"wizard-corner-{size}.bmp")

    # iPad: the header logo (one universal image).
    ipad = ROOT / "ipad" / "VCComposerPad" / "Assets.xcassets" / "Logo.imageset"
    save(width(horizontal, 600), ipad / "Logo.png")
    (ipad / "Contents.json").write_text(
        '{\n  "images" : [\n    {\n      "filename" : "Logo.png",\n      "idiom" : "universal"\n    }\n  ],\n'
        '  "info" : {\n    "author" : "xcode",\n    "version" : 1\n  }\n}\n'
    )


if __name__ == "__main__":
    main()
