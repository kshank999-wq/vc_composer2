// Licence standing on this computer (the same rules as the owner's other
// products' desktops): vc-composer.com decides entitlement; the app writes
// down what the server last said and reads it here, generously wherever it
// is uncertain.
//
//   - Never activated (a configured release build): saving is off until the
//     composer signs in and activates; projects still open, play, print and
//     export.
//   - Activated, nothing heard yet, or the licence is live: saving is on.
//   - The last answer is over 30 days old: saving stays on (being unable to
//     ask is not a lapse), with a warning.
//   - Lapsed: saving stays on for 7 days from when this computer first saw
//     it, with a warning that says the date; then it is off.
//
// Pure and JUCE-free, so the rules are tested here and the desktop only
// stores and asks.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "vcc/core/Json.h"

namespace vcc::licensing {

/// Seconds since 1970 of an ISO 8601 UTC time ("2026-11-16T10:00:00Z",
/// "...00.123+00:00", or a bare "2026-11-16"); nullopt when it is not one.
std::optional<std::int64_t> parse_iso(const std::string& text);
/// "2026-11-16T10:00:00.000Z".
std::string format_iso(std::int64_t seconds);

/// What the server says about the account's liveliest licence.
struct Claim {
    std::string status = "active";  ///< active, suspended, revoked, expired.
    std::string expires_at;         ///< ISO; empty: never ends.
    std::string grant;              ///< "", "purchase", "academic", "master".
};
bool licence_live(const std::string& status, const std::string& expires_at, std::int64_t now);
/// From GET /api/licenses/standing's body: {"standing": {...} | null}.
/// nullopt for null (an account with no licence: not a lapse) or a body that
/// is not one.
std::optional<Claim> claim_from_response(const json::Value& body);

/// What this computer has written down.
struct DeskStanding {
    std::string status;
    std::string expires_at;      ///< Empty: never ends.
    std::string checked_at;      ///< When this computer was last told.
    std::string seen_lapsed_at;  ///< When it first saw the lapse (the grace counts from it); empty while live.
    std::string grant;
    json::Value to_json() const;
    static std::optional<DeskStanding> from_json(const json::Value& doc);
};
/// Writes down a new answer, carrying forward when the lapse was first seen.
DeskStanding note_standing(const std::optional<DeskStanding>& previous, const Claim& claim, std::int64_t now);

constexpr int kGraceDays = 7;
constexpr int kStandingGoodForDays = 30;

struct SavingStanding {
    bool can_save = true;
    std::string notice;            ///< Empty: nothing to say.
    bool warning = false;          ///< A warning of something coming (dismissable), not a refusal.
    bool needs_activation = false; ///< Offer Activate, not Renew.
};
/// `activated`: false = never activated; nullopt = a build that does not
/// licence saving (development, preview, no server configured).
SavingStanding saving_standing(const std::optional<DeskStanding>& standing, std::int64_t now,
                               std::optional<bool> activated);

}  // namespace vcc::licensing
