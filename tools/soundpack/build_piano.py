#!/usr/bin/env python3
"""Builds the sound pack's piano: a compact .sf3 from the Salamander Grand
Piano (Alexander Holm, CC-BY 3.0; FreePats SFZ+FLAC edition).

    python3 -I build_piano.py <folder with the .sfz and samples/> <out.sf3>

What it keeps, so the pack stays a small download and a modest amount of
memory, while still sounding like a real grand:

  - four of the sixteen velocity layers (soft, medium, loud, very loud);
  - every sampled pitch (minor thirds across the keyboard), with the
    Retuned edition's per-sample tuning (equal temperament);
  - each note shortened from about ten seconds in the bass to three at the
    top, with a gentle fade (the piano has mostly died away by then);
  - stereo, as two linked mono samples panned left and right;
  - Ogg Vorbis compression per sample (SoundFont 3), which VC Composer's
    player (TinySoundFont with stb_vorbis) decodes when it loads.

The release-noise and string-resonance samples are left out.

Needs numpy and soundfile (libsndfile with Ogg Vorbis).
"""
from __future__ import annotations

import io
import re
import struct
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

# (Salamander layer, lowest velocity, highest velocity)
LAYERS = [(4, 1, 48), (8, 49, 74), (11, 75, 100), (15, 101, 127)]
LONGEST_S, SHORTEST_S, FADE_S = 10.0, 3.0, 0.6
QUALITY = 0.3  # libsndfile compression level for Vorbis: 0 = best quality, 1 = smallest


def note_seconds(key: int) -> float:
    t = (key - 21) / (108 - 21)
    return LONGEST_S + (SHORTEST_S - LONGEST_S) * min(1.0, max(0.0, t))


def parse_regions(sfz: Path):
    """The plain note regions: {(layer, keycenter): (sample, lokey, hikey, tune cents)}."""
    out = {}
    for line in sfz.read_text(errors="replace").splitlines():
        m = re.match(r"<region>\s+sample=(samples/([A-G]#?\d)v(\d+)\.flac)\s+(.*)", line.strip())
        if not m:
            continue
        ops = dict(re.findall(r"(\w+)=(\S+)", m.group(4)))
        if "trigger" in ops:
            continue
        layer = int(m.group(3))
        # The C4 regions leave pitch_keycenter out (SFZ's default is 60): use the name.
        name = m.group(2)
        named = 12 * (int(name[-1]) + 1) + "C D EF G A B".index(name[0]) + (1 if "#" in name else 0)
        out[(layer, int(ops.get("pitch_keycenter", named)))] = (m.group(1), int(ops["lokey"]), int(ops["hikey"]),
                                                                  int(ops.get("tune", 0)))
    return out


def vorbis(mono: np.ndarray, rate: int) -> bytes:
    buf = io.BytesIO()
    sf.write(buf, mono, rate, format="OGG", subtype="VORBIS", compression_level=QUALITY)
    return buf.getvalue()


def chunk(cid: bytes, data: bytes) -> bytes:
    pad = b"\0" if len(data) % 2 else b""
    return cid + struct.pack("<I", len(data)) + data + pad


def list_chunk(kind: bytes, body: bytes) -> bytes:
    return chunk(b"LIST", kind + body)


def zstr(s: str, n: int) -> bytes:
    b = s.encode("ascii", "replace")[: n - 1]
    return b + b"\0" * (n - len(b))


# SoundFont generator numbers
PAN, RELEASE_VOL, INSTRUMENT, KEY_RANGE, VEL_RANGE, FINE_TUNE, SAMPLE_ID, ROOT_KEY = 17, 38, 41, 43, 44, 52, 53, 58


def gen(op: int, amount: int) -> bytes:
    return struct.pack("<Hh", op, amount)


def gen_range(op: int, lo: int, hi: int) -> bytes:
    return struct.pack("<HBB", op, lo, hi)


def build(src: Path, out: Path) -> None:
    # The Retuned edition: each sample's tuning corrected to equal temperament.
    sfz = next(p for p in src.glob("*.sfz") if "Retuned" in p.name)
    regions = parse_regions(sfz)
    smpl = bytearray()
    shdrs: list[bytes] = []
    zones: list[bytes] = []  # each zone's generator list
    total_frames = 0

    def add_sample(name: str, data: bytes, frames: int, rate: int, key: int, link: int, kind: int) -> int:
        nonlocal smpl
        start = len(smpl)
        smpl += data
        end = len(smpl)
        # SoundFont 3: start/end are byte offsets of the Ogg data; the loop
        # points are relative (no loop here); sampleType | 0x10 = Vorbis.
        shdrs.append(zstr(name, 20) + struct.pack("<IIIIIBbHH", start, end, 0, 0, rate, key, 0, link, kind | 0x10))
        return len(shdrs) - 1

    for layer, lovel, hivel in LAYERS:
        for (lay, key), (sample, lokey, hikey, tune) in sorted(regions.items()):
            if lay != layer:
                continue
            audio, rate = sf.read(src / sample, dtype="float32", always_2d=True)
            n = min(len(audio), int(note_seconds(key) * rate))
            audio = audio[:n].copy()
            fade = min(n, int(FADE_S * rate))
            audio[n - fade:] *= (0.5 + 0.5 * np.cos(np.linspace(0, np.pi, fade)))[:, None]
            total_frames += n
            stem = Path(sample).stem
            left = add_sample(f"{stem}L", vorbis(audio[:, 0], rate), n, rate, key, len(shdrs) + 1, 4)
            right = add_sample(f"{stem}R", vorbis(audio[:, 1], rate), n, rate, key, left, 2)
            # Edge layers cover the whole keyboard (A0 down to 21, C8 up to 108).
            lo = 21 if lokey <= 22 else lokey
            hi = 108 if hikey >= 106 else hikey
            for sid, pan in ((left, -500), (right, 500)):
                zones.append(
                    gen_range(KEY_RANGE, lo, hi) + gen_range(VEL_RANGE, lovel, hivel) + gen(PAN, pan)
                    + gen(RELEASE_VOL, 0) + gen(FINE_TUNE, tune) + gen(ROOT_KEY, key) + gen(SAMPLE_ID, sid)
                )
        print(f"layer {layer}: done ({len(shdrs)} samples so far, {len(smpl) / 1e6:.1f} MB)", flush=True)

    shdrs.append(zstr("EOS", 20) + b"\0" * 26)

    # Instrument: one zone per sample.
    igen = bytearray()
    ibag = bytearray()
    for z in zones:
        ibag += struct.pack("<HH", len(igen) // 4, 0)
        igen += z
    ibag += struct.pack("<HH", len(igen) // 4, 0)
    igen += b"\0\0\0\0"
    inst = zstr("Salamander Grand", 20) + struct.pack("<H", 0) + zstr("EOI", 20) + struct.pack("<H", len(zones))
    imod = b"\0" * 10

    # Preset 0:0 -> the instrument.
    pgen = gen(INSTRUMENT, 0) + b"\0\0\0\0"
    pbag = struct.pack("<HH", 0, 0) + struct.pack("<HH", 1, 0)
    phdr = (zstr("Salamander Grand Piano", 20) + struct.pack("<HHHIII", 0, 0, 0, 0, 0, 0)
            + zstr("EOP", 20) + struct.pack("<HHHIII", 0, 0, 1, 0, 0, 0))
    pmod = b"\0" * 10

    info = (chunk(b"ifil", struct.pack("<HH", 3, 1)) + chunk(b"isng", b"EMU8000\0")
            + chunk(b"INAM", b"Salamander Grand Piano (VC Composer edition)\0")
            + chunk(b"IENG", b"Alexander Holm; FreePats; VC Composer\0")
            + chunk(b"ICOP", b"Salamander Grand Piano by Alexander Holm, CC-BY 3.0\0")
            + chunk(b"ICMT", b"Reduced from the FreePats SFZ+FLAC edition: four velocity layers, shortened notes, Ogg Vorbis.\0"))
    sdta = chunk(b"smpl", bytes(smpl))
    pdta = (chunk(b"phdr", phdr) + chunk(b"pbag", pbag) + chunk(b"pmod", pmod) + chunk(b"pgen", pgen)
            + chunk(b"inst", inst) + chunk(b"ibag", bytes(ibag)) + chunk(b"imod", imod) + chunk(b"igen", bytes(igen))
            + chunk(b"shdr", b"".join(shdrs)))
    body = b"sfbk" + list_chunk(b"INFO", info) + list_chunk(b"sdta", sdta) + list_chunk(b"pdta", pdta)
    out.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)
    print(f"{out}: {out.stat().st_size / 1e6:.1f} MB, {len(zones)} zones, "
          f"{total_frames * 2 * 2 / 1e6:.0f} MB once loaded (16-bit)")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    build(Path(sys.argv[1]), Path(sys.argv[2]))
