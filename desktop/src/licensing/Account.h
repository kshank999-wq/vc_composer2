// The composer's VC Composer account on this computer (the same design as
// the owner's other desktops): sign in with a code emailed by Supabase Auth,
// activate this computer against the account's licence at vc-composer.com,
// and ask every six hours how the licence stands. What the server says is
// written down here and read by vcc/licensing/Standing.h, which decides
// whether saving is on - generously wherever it is uncertain.
//
// Builds without a server configured (development, CI) and preview builds
// do not licence saving at all. A release build is configured at build time:
//   -DVCC_SUPABASE_URL=https://<project>.supabase.co
//   -DVCC_SUPABASE_ANON_KEY=<publishable anon key>
//   -DVCC_SITE_URL=https://vc-composer.com
//
// Files (user data folder, Account/): session.json (email and tokens),
// activation.json, license-standing.json, device-salt. The device
// fingerprint is a hash of a random salt and the computer's name: no
// hardware identifiers.
#pragma once

#include <optional>

#include <juce_core/juce_core.h>

#include "vcc/core/Json.h"
#include "vcc/licensing/Standing.h"

namespace vcc::desktop {

class Account {
public:
    struct Config {
        juce::String supabase_url, anon_key, site_url;
        bool configured() const { return supabase_url.isNotEmpty() && anon_key.isNotEmpty() && site_url.isNotEmpty(); }
    };
    struct Result {
        bool ok = false;
        juce::String message;
    };

    static Account& get();

    /// The build's configuration (the desktop check points it at a local server).
    void set_config(Config config);
    const Config& config() const noexcept { return config_; }
    bool configured() const { return config_.configured() && !licensing_off_; }
    /// Where the files live (default: the user data folder's Account/).
    void set_folder(const juce::File& folder);
    juce::File folder() const;

    bool signed_in() const;
    juce::String email() const;
    /// nullopt: this build does not licence saving; else whether this
    /// computer was ever activated.
    std::optional<bool> activated() const;
    licensing::SavingStanding saving() const;
    std::optional<licensing::DeskStanding> standing() const;

    // Blocking network calls (run them off the message thread).
    Result send_code(const juce::String& email);
    Result verify_code(const juce::String& email, const juce::String& code);
    /// Optional serial; without one the account's own licences are tried.
    Result activate(const juce::String& serial = {});
    /// GET /api/licenses/standing and write it down. A failure changes nothing.
    Result check_standing();
    void sign_out();

    /// A signed-in call to vc-composer.com (collaboration). Blocking.
    struct Reply {
        int status = 0;  ///< 0: the server could not be reached.
        json::Value body;
        juce::String error;  ///< The server's reason (or ours) when not 2xx.
        bool ok() const { return status >= 200 && status < 300; }
    };
    Reply api(const juce::String& method, const juce::String& path, const json::Value& body = {});
    /// Raw transfers to and from a short-lived signed storage link.
    Reply put_bytes(const juce::String& url, const juce::MemoryBlock& data);
    std::optional<juce::MemoryBlock> get_bytes(const juce::String& url, int* status = nullptr);

    juce::String device_fingerprint();
    static juce::String platform();

private:
    Account();
    juce::File file(const char* name) const;
    /// A fresh access token (refreshing it when it is about to expire), or empty.
    juce::String access_token();
    void save_session();

    mutable juce::CriticalSection lock_;
    Config config_;
    bool licensing_off_ = false;
    juce::File folder_;
    juce::String email_, access_, refresh_;
    std::int64_t access_expires_ = 0;
};

}  // namespace vcc::desktop
