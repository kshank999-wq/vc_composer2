#!/usr/bin/env python3
"""Assembles the sound pack VC Composer downloads on first launch.

    python3 -I make_pack.py <piano.sf3> <MuseScore_General.sf3> <MuseScore_General_License.md> <out dir>

Writes into <out dir>:
  Salamander-Grand-Piano-VCC.sf3   the piano (from build_piano.py)
  MuseScore_General.sf3            the General MIDI bank, unchanged
  CREDITS.txt                      who made them and their licences
  manifest.json                    {"version", "files": [{name, role, size, sha256}]}

The app (desktop/src/audio/SoundPack.cpp) reads manifest.json, downloads
each file and uses it only if its SHA-256 matches.
"""
from __future__ import annotations

import hashlib
import json
import shutil
import sys
from pathlib import Path

PACK_VERSION = "1"

CREDITS = """VC Composer built-in sounds
===========================

Salamander Grand Piano (V3)
---------------------------
By Alexander Holm. Licensed under the Creative Commons Attribution 3.0
licence: https://creativecommons.org/licenses/by/3.0/
SFZ+FLAC edition and retuning by the FreePats project:
https://freepats.zenvoid.org/Piano/acoustic-grand-piano.html

Changed for VC Composer: four of its sixteen velocity layers, each note
shortened with a fade (about ten seconds in the bass to three at the top),
stereo kept as two panned samples, compressed with Ogg Vorbis into a
SoundFont 3 file (Salamander-Grand-Piano-VCC.sf3). Its author does not
endorse VC Composer or this edition.

MuseScore General (0.2)
-----------------------
By S. Christian Collins, under the MIT licence; unchanged
(MuseScore_General.sf3). Its licence and notices follow in full.

"""


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def main() -> None:
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    piano, gm, gm_licence, out = (Path(a) for a in sys.argv[1:])
    out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(piano, out / "Salamander-Grand-Piano-VCC.sf3")
    shutil.copyfile(gm, out / "MuseScore_General.sf3")
    (out / "CREDITS.txt").write_text(CREDITS + gm_licence.read_text(encoding="utf-8", errors="replace"), encoding="utf-8")
    files = []
    for name, role in (("Salamander-Grand-Piano-VCC.sf3", "piano"), ("MuseScore_General.sf3", "gm"), ("CREDITS.txt", "text")):
        p = out / name
        files.append({"name": name, "role": role, "size": p.stat().st_size, "sha256": sha256(p)})
    (out / "manifest.json").write_text(json.dumps({"version": PACK_VERSION, "files": files}, indent=2) + "\n")
    total = sum(f["size"] for f in files)
    print(f"{out}: {len(files)} files, {total / 1e6:.1f} MB")
    for f in files:
        print(f"  {f['name']}  {f['size']}  {f['sha256']}")


if __name__ == "__main__":
    main()
