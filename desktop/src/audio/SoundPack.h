// The built-in sounds (Studio > Built-in Sounds...): a real piano and a full
// General MIDI orchestra, so a template, a chord or an ostinato sounds like
// the instrument it is written for before any library is installed.
//
//   - Piano: the Salamander Grand Piano (Alexander Holm, CC-BY 3.0), reduced
//     to four velocity layers (tools/soundpack/build_piano.py).
//   - Everything else: MuseScore General (S. Christian Collins, MIT), the
//     128 General MIDI programs and the drum kits.
//
// Both are SoundFont 3 files (Ogg Vorbis compressed), downloaded once from
// vc-composer.com's storage on first launch into the user data folder's
// Sounds/, each checked against the manifest's SHA-256 before it is used.
// Until then (or offline) the placeholder synth plays, as it always has.
//
// Loading takes a few seconds, on a background thread; when the banks are
// ready the engine swaps the placeholder sound for them (generation()).
#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>
#include <string>

struct tsf;

namespace vcc::desktop::sounds {

/// One loaded SoundFont, shared read-only by every instrument that plays it.
/// Created and released on the message thread.
struct Bank {
    explicit Bank(tsf* f, juce::String n) : font(f), name(std::move(n)) {}
    ~Bank();
    Bank(const Bank&) = delete;
    Bank& operator=(const Bank&) = delete;
    tsf* font = nullptr;
    juce::String name;
};

enum class State { Missing, Downloading, Loading, Ready, Failed };

/// The files of the pack, as its manifest lists them.
struct PackFile {
    std::string name, role, sha256;  ///< role: "piano", "gm" or "text"
    std::int64_t size = 0;
};

/// Where the pack lives (VCC_SOUNDS_DIR, else <user data>/Sounds).
juce::File folder();
/// Where it is downloaded from: VCC_SOUNDS_URL, else the release build's
/// Supabase storage (public bucket "sounds", folder v1). Empty: nowhere.
juce::String source_url();
/// Reads a manifest ({"version", "files": [{name, role, size, sha256}]}).
std::vector<PackFile> parse_manifest(const juce::String& json, juce::String* version = nullptr);
/// Whether every file of the installed manifest is present at its size.
bool installed();

State state();
/// "Ready: Salamander Grand Piano, MuseScore General (370 MB)", "Downloading 42%"...
juce::String status();
/// 0..1 while downloading.
double progress();
/// Bumped each time the loaded banks change (the engine rebuilds then).
int generation();

/// The banks (null until loaded).
std::shared_ptr<const Bank> piano();
std::shared_ptr<const Bank> gm();

/// Loads an installed pack in the background (no-op if loaded or busy).
void load_async();
/// Downloads the pack (then loads it) in the background. `force` downloads
/// again even if it is installed.
void download_async(bool force = false);
/// First launch: loads the pack if installed, otherwise downloads it (unless
/// turned off in the settings or there is nowhere to get it from).
void start();
/// Blocking load (the checks; the message thread).
bool load_now();
/// Drops the loaded banks (the checks).
void unload();

/// Called on the message thread whenever state or progress changes.
int add_listener(std::function<void()> fn);
void remove_listener(int id);

}  // namespace vcc::desktop::sounds
