# TinySoundFont + stb_vorbis

- `tsf.h`: TinySoundFont by Bernhard Schelling, MIT licence
  (https://github.com/schellingb/TinySoundFont), one change: the loaded
  samples are kept as 16-bit integers instead of 32-bit floats, so a General
  MIDI bank takes half the memory. Marked "VC Composer" in the source.
- `stb_vorbis.c`: Sean Barrett's Ogg Vorbis decoder, public domain (or MIT),
  unchanged; TinySoundFont uses it to read compressed .sf3 SoundFonts.

Used by `desktop/src/audio/SoundFontInstrument` to play the sound pack
(`desktop/src/audio/SoundPack`). Credits: Help > Credits.
