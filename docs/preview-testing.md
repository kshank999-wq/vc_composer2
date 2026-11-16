# Testing a preview build with your own plug-ins

A preview build is the app as it will ship, time-limited (it says
"Preview - N days left" in the title bar) and not signed unless the signing
secrets are set. It needs no account. These steps check it against the
instruments and effects licensed on your computer, which no automated test
can reach.

## 1. Get it

- **From a run:** GitHub > vc_composer2 > Actions > *Preview builds* > the
  newest green run > Artifacts > `windows` (or `macos`). Unzip it.
- **From a release:** pushing a tag such as `preview-0.1.0` also publishes
  the files as a GitHub pre-release (and, once the website is live, they are
  uploaded to it for signed-in users).

Windows: run `VC-Composer-2-Preview-<version>-Windows-Setup.exe`. Until the
Azure signing secrets are in place, SmartScreen says "Windows protected your
PC": choose *More info* > *Run anyway*. A portable zip is there too.

macOS: open the `.dmg` and drag the app to Applications. Unsigned builds
need right-click > *Open* the first time.

## 2. What to try

Work through these in order; each line says what should happen.

1. **Scan** - Tools > Plug-ins... > Scan. Every VST3 instrument and effect
   you own is listed. A plug-in that crashes or hangs during the scan is
   marked as such and does not take the app down (the scan runs in a
   separate process).
2. **Library** - the Library tab, *All sounds*. Your players (Kontakt,
   Spitfire, Opus...) say "Player installed"; libraries in your sample
   folders (add them under *Library folders*) say "Library detected".
3. **Audition and assign** - select a track (e.g. Violins I), *For the
   selected track*, pick a sound, *Audition* (it plays through the
   plug-in), then *Assign*. The track now plays that plug-in.
4. **Open the plug-in's window** - Track > Open Instrument Window. Change a
   preset, close it, save the project, reopen it: the setting is back.
5. **Articulations** - Track > Articulation Map. Use *Learn* on a row and
   press the keyswitch on a MIDI keyboard. Write staccato and legato notes;
   playback switches articulations.
6. **Effects** - add an EQ/reverb insert on a mixer strip and a send to a
   bus; play; automate a parameter (Write mode) and play it back.
7. **Record** - arm a MIDI track, record a phrase from your keyboard; arm an
   audio track with your interface, record a take.
8. **Latency** - play a busy passage at a small buffer (Tools > Audio
   Settings). Note any crackles, with the buffer size.
9. **Crash isolation** - if a plug-in crashes while playing, the app keeps
   running and offers Tools > Reload Crashed Plug-ins.
10. **Exports** - File > Export Stems (WAV), Export Parts PDF, Export MIDI,
    MusicXML; with a movie imported, File > Export Picture with Score.
11. **Save and reopen** - everything above survives Save, quit, reopen.

## 3. Report back

For each plug-in that misbehaves: its name, vendor and version, the step,
what happened. Attach, from `%APPDATA%\VC Composer 2` (Windows) or
`~/Library/Application Support/VC Composer 2` (macOS), the files ending in
`.log` and the plug-in scan list, plus the project if it is involved.
