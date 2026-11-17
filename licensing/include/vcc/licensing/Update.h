// Updates: which build is newer, and whether vc-composer.com offers one.
//
// Builds are labelled "<major>.<minor>.<patch>" (a release) or
// "<major>.<minor>.<patch>-preview.<n>" (the n-th preview of that version,
// numbered by the build that made it). A release outranks every preview of
// the same version; previews of one version are ordered by n.
//
// The site answers GET /api/releases?channel=preview|stable with
// {"builds":[{"platform","version","releaseNotes","sha256",...}]}; the app
// offers the build for its own platform when it is newer than itself.
#pragma once

#include <optional>
#include <string>

#include "vcc/core/Json.h"

namespace vcc::licensing {

struct BuildVersion {
    int major = 0, minor = 0, patch = 0;
    int preview = -1;  ///< -1: a release.
    bool ok = false;   ///< False: not a version this app understands.
};

/// "0.1.0", "v0.1.0", "0.1.0-preview.16".
BuildVersion parse_version(const std::string& text);
/// Whether `a` is a later build than `b` (false if either does not parse).
bool is_newer(const BuildVersion& a, const BuildVersion& b);
bool is_newer(const std::string& a, const std::string& b);

struct UpdateOffer {
    std::string version;
    std::string notes;
    std::string sha256;
    std::string minimum_os;
};

/// The build the site offers for `platform` ("windows", "macos"), if it is
/// newer than `current`.
std::optional<UpdateOffer> pick_update(const json::Value& releases, const std::string& platform, const std::string& current);

}  // namespace vcc::licensing
