// Updates from vc-composer.com: is there a newer build, download it through
// the account (checked against the SHA-256 the site recorded), and install it
// over this copy, then start again.
//
//   - Check: GET /api/releases?channel=preview|stable (no sign-in), the build
//     for this platform when newer than this one (vcc/licensing/Update.h).
//   - Download: GET /api/downloads/<platform>?channel=... with the account's
//     token, for a short-lived link; the file must match the catalogue's
//     SHA-256 before anything runs it.
//   - Install: Windows runs the signed installer quietly (it closes this
//     copy, installs over it and starts the new one); macOS mounts the signed,
//     notarized disk image and a small script replaces the app once this copy
//     has quit, then opens it. Where the app's folder is not writable, the disk
//     image is opened for a drag instead.
//
// A preview build follows the preview channel, a release the stable one.
// Builds without a site configured (development, the desktop check's default)
// never look.
#pragma once

#include <functional>
#include <optional>

#include <juce_core/juce_core.h>

#include "vcc/licensing/Update.h"

namespace vcc::desktop {

class Updater {
public:
    static Updater& get();

    /// This build's label: "0.1.0-preview.17", or the project version.
    static juce::String current_version();
    /// "preview" or "stable".
    static juce::String channel();
    /// Whether this build can look for updates at all (a site is configured).
    static bool available();
    /// Whether this copy may download updates: signed in, activated with a
    /// licence, or a preview (previews update with no account at all).
    static bool may_download();

    struct Check {
        bool ok = false;  ///< False: the site could not be asked (message says why).
        std::optional<licensing::UpdateOffer> offer;
        juce::String message;
    };
    /// Blocking: run it off the message thread.
    Check check();

    struct Downloaded {
        bool ok = false;
        juce::File file;
        juce::String message;
    };
    /// Blocking. `progress` gets 0..1 (from the downloading thread).
    Downloaded download(const licensing::UpdateOffer& offer, const std::function<void(double)>& progress = {});

    /// What installing `file` would run (Windows: the installer and its
    /// switches; macOS: the replacement script's lines). For the desktop check.
    juce::StringArray install_plan(const juce::File& file) const;
    /// Starts the install. Empty on success: the caller then quits the app.
    /// Otherwise why not (and, on macOS, the disk image was opened instead).
    juce::String install(const juce::File& file);

    /// Where downloads go (default: the user data folder's Updates/).
    void set_folder(const juce::File& folder) { folder_ = folder; }
    juce::File folder() const;
    /// The site, for the desktop check; default: the account's.
    void set_site(const juce::String& site) { site_override_ = site; }

private:
    Updater() = default;
    juce::String site() const;
    juce::File folder_;
    juce::String site_override_;
};

}  // namespace vcc::desktop
