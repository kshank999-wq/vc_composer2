// The plug-in check at start-up (Studio > Plug-ins): a little after the app
// opens, the plug-in folders are looked through in the background -
// the system's standard ones (VST3 on both systems, Audio Units on macOS)
// and any added in the Plug-ins window - and the list is brought up to date:
//
//   - new:      installed since the last look, scanned and added;
//   - updated:  a newer file (an installer ran), scanned again; the version
//               it now reports is shown beside the old one;
//   - removed:  uninstalled, taken off the list (projects that use one show
//               it missing, as before);
//   - attention: did not load - crashed, hung, or waited for a licence or
//               authorisation dialog (often a plug-in not yet authorised on
//               this computer). Scanning runs in its own process, so none of
//               this can take the app down.
//
// Whether a plug-in is licensed is the maker's business: a plug-in that is
// not authorised usually refuses to load or waits for its own dialog, which
// is what "attention" reports; it cannot be asked directly. Nothing here goes
// online.
#pragma once

#include <atomic>
#include <functional>

#include <juce_audio_processors/juce_audio_processors.h>

namespace vcc::desktop {

class PluginHost;

struct PluginReport {
    juce::StringArray added;      ///< "Pigments (Arturia)"
    juce::StringArray updated;    ///< "Kontakt 7 (Native Instruments) 7.8.1 -> 7.10.0"
    juce::StringArray removed;
    juce::StringArray attention;  ///< "Omnisphere: did not finish loading within 60 s (...)"
    int looked_at = 0;            ///< Plug-in files and identifiers found in the folders.
    bool finished = false;        ///< False: stopped before the end (the app quit).
    juce::Time when;

    bool any_change() const { return !added.isEmpty() || !updated.isEmpty() || !removed.isEmpty(); }
    /// "3 new, 1 updated, 1 needs attention", or "Plug-ins up to date (142)".
    juce::String summary() const;
    /// Every line, for the Plug-ins window.
    juce::String details() const;
};

class PluginWatch final : private juce::Thread {
public:
    explicit PluginWatch(PluginHost& host);
    ~PluginWatch() override;

    /// Starts a look through the folders; `progress` (message thread) gets
    /// "Checking plug-ins: 12 of 140", `done` (message thread) the report.
    void start(std::function<void(const juce::String&)> progress, std::function<void(const PluginReport&)> done);
    /// Blocking, on the calling thread (the checks).
    PluginReport run_now();
    bool running() const { return isThreadRunning(); }
    /// The last report (empty before the first look).
    const PluginReport& last() const noexcept { return last_; }

    /// The folders looked in for a format: the ones set in the Plug-ins
    /// window, else the system's standard ones.
    juce::FileSearchPath folders(juce::AudioPluginFormat& format) const;

    /// Whether the app looks at start-up (on by default; remembered).
    static bool at_startup();
    static void set_at_startup(bool on);

private:
    void run() override;
    PluginReport look(const std::function<void(int, int)>& progress);

    PluginHost& host_;
    std::function<void(const juce::String&)> progress_;
    std::function<void(const PluginReport&)> done_;
    PluginReport last_;
};

/// The app's watch (made by the main window).
PluginWatch* plugin_watch();
void set_plugin_watch(PluginWatch* watch);

}  // namespace vcc::desktop
