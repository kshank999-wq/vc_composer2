#include "licensing/Account.h"

#include <ctime>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include <juce_cryptography/juce_cryptography.h>

#include "AppResources.h"
#include "Preview.h"
#include "vcc/core/Files.h"
#include "vcc/core/Json.h"

#ifndef VCC_SUPABASE_URL
#define VCC_SUPABASE_URL ""
#endif
#ifndef VCC_SUPABASE_ANON_KEY
#define VCC_SUPABASE_ANON_KEY ""
#endif
#ifndef VCC_SITE_URL
#define VCC_SITE_URL ""
#endif

namespace vcc::desktop {

namespace {

std::int64_t now_seconds() { return static_cast<std::int64_t>(std::time(nullptr)); }

struct Http {
    int status = 0;
    json::Value body;
    bool reached = false;
};

/// One JSON request. Status 0 when the server could not be reached.
Http request(const juce::String& url, const juce::String& method, const juce::String& headers, const juce::String& body = {})
{
    Http out;
    // Taken as written: parsed, a query (?grant_type=password) would be moved
    // into a POST's body and the JSON would no longer be JSON.
    juce::URL u = juce::URL::createWithoutParsing(url);
    if (body.isNotEmpty()) u = u.withPOSTData(body);
    juce::WebInputStream stream(u, method == "POST");
    stream.withExtraHeaders("Accept: application/json\r\n" + headers +
                            (body.isNotEmpty() ? "Content-Type: application/json\r\n" : ""));
    stream.withConnectionTimeout(15000);
    if (method != "GET" && method != "POST") stream.withCustomRequestCommand(method);
    stream.connect(nullptr);
    out.status = stream.getStatusCode();
    out.reached = out.status != 0;
    const auto text = stream.readEntireStreamAsString();
    if (auto parsed = json::parse(text.toStdString())) out.body = *parsed;
    return out;
}

juce::String error_of(const Http& r, const juce::String& fallback)
{
    if (!r.reached) return "Could not reach the server. Check the internet connection and try again.";
    for (const char* key : {"error", "msg", "error_description", "message"}) {
        const auto& v = r.body[key];
        if (v.is_string() && !v.as_string().empty()) return juce::String(v.as_string());
    }
    return fallback + " (" + juce::String(r.status) + ")";
}

json::Value read_json(const juce::File& f)
{
    if (!f.existsAsFile()) return {};
    return json::parse(f.loadFileAsString().toStdString()).value_or(json::Value());
}

void write_json(const juce::File& f, const json::Value& v)
{
    f.getParentDirectory().createDirectory();
    files::write_file_atomic(f.getFullPathName().toStdString(), json::write(v));
}

}  // namespace

Account& Account::get()
{
    static Account a;
    return a;
}

Account::Account()
{
    config_ = {VCC_SUPABASE_URL, VCC_SUPABASE_ANON_KEY, VCC_SITE_URL};
    licensing_off_ = preview::is_preview();  // previews are time-limited instead
    set_folder(user_data_directory().getChildFile("Account"));
}

void Account::set_config(Config c)
{
    const juce::ScopedLock sl(lock_);
    config_ = std::move(c);
    licensing_off_ = false;
}

void Account::set_folder(const juce::File& f)
{
    const juce::ScopedLock sl(lock_);
    folder_ = f;
    const auto s = read_json(file("session.json"));
    email_ = juce::String(s.get_string("email"));
    access_ = juce::String(s.get_string("access_token"));
    refresh_ = juce::String(s.get_string("refresh_token"));
    access_expires_ = s.get_int("expires_at");
}

juce::File Account::folder() const
{
    const juce::ScopedLock sl(lock_);
    return folder_;
}

juce::File Account::file(const char* name) const { return folder_.getChildFile(name); }

bool Account::signed_in() const
{
    const juce::ScopedLock sl(lock_);
    return refresh_.isNotEmpty();
}

juce::String Account::email() const
{
    const juce::ScopedLock sl(lock_);
    return email_;
}

std::optional<bool> Account::activated() const
{
    const juce::ScopedLock sl(lock_);
    if (!configured()) return std::nullopt;
    return read_json(file("activation.json")).get_string("activatedAt").empty() ? false : true;
}

std::optional<licensing::DeskStanding> Account::standing() const
{
    const juce::ScopedLock sl(lock_);
    return licensing::DeskStanding::from_json(read_json(file("license-standing.json")));
}

licensing::SavingStanding Account::saving() const
{
    return licensing::saving_standing(standing(), now_seconds(), activated());
}

void Account::save_session()
{
    write_json(file("session.json"), json::Object{{"email", email_.toStdString()},
                                                  {"access_token", access_.toStdString()},
                                                  {"refresh_token", refresh_.toStdString()},
                                                  {"expires_at", static_cast<long long>(access_expires_)}});
#if !JUCE_WINDOWS
    // The tokens are the composer's: readable by them only.
    ::chmod(file("session.json").getFullPathName().toRawUTF8(), 0600);
#endif
}

Account::Result Account::send_code(const juce::String& address)
{
    const auto e = address.trim().toLowerCase();
    if (!e.containsChar('@')) return {false, "Enter the email address of your VC Composer account"};
    Config c;
    {
        const juce::ScopedLock sl(lock_);
        c = config_;
    }
    if (!c.configured()) return {false, "This build is not connected to vc-composer.com"};
    const auto r = request(c.supabase_url + "/auth/v1/otp", "POST", "apikey: " + c.anon_key + "\r\n",
                           juce::String(json::write(json::Object{{"email", e.toStdString()}, {"create_user", false}})));
    if (r.status < 200 || r.status >= 300) return {false, error_of(r, "The code could not be sent")};
    return {true, "A sign-in code is on its way to " + e};
}

Account::Result Account::verify_code(const juce::String& address, const juce::String& code)
{
    const auto e = address.trim().toLowerCase();
    Config c;
    {
        const juce::ScopedLock sl(lock_);
        c = config_;
    }
    const auto r = request(c.supabase_url + "/auth/v1/verify", "POST", "apikey: " + c.anon_key + "\r\n",
                           juce::String(json::write(json::Object{
                               {"type", "email"}, {"email", e.toStdString()}, {"token", code.trim().toStdString()}})));
    if (r.status < 200 || r.status >= 300 || r.body.get_string("access_token").empty())
        return {false, r.reached && r.status >= 400 && r.status < 500
                           ? error_of(r, "That code did not work") +
                                 " - use the code from the newest email, within the hour, or sign in with your password"
                           : error_of(r, "That code did not work")};
    const juce::ScopedLock sl(lock_);
    email_ = e;
    access_ = juce::String(r.body.get_string("access_token"));
    refresh_ = juce::String(r.body.get_string("refresh_token"));
    access_expires_ = now_seconds() + r.body.get_int("expires_in", 3600);
    save_session();
    return {true, "Signed in as " + e};
}

Account::Result Account::sign_in_password(const juce::String& address, const juce::String& password)
{
    const auto e = address.trim().toLowerCase();
    if (!e.containsChar('@')) return {false, "Enter the email address of your VC Composer account"};
    if (password.isEmpty()) return {false, "Enter your password (the one you sign in to vc-composer.com with)"};
    Config c;
    {
        const juce::ScopedLock sl(lock_);
        c = config_;
    }
    if (!c.configured()) return {false, "This build is not connected to vc-composer.com"};
    const auto r = request(c.supabase_url + "/auth/v1/token?grant_type=password", "POST", "apikey: " + c.anon_key + "\r\n",
                           juce::String(json::write(json::Object{{"email", e.toStdString()}, {"password", password.toStdString()}})));
    if (r.status < 200 || r.status >= 300 || r.body.get_string("access_token").empty()) {
        if (r.reached && r.status >= 400 && r.status < 500)
            return {false, "That email and password do not match. Never set a password? Press Email Me a Code, or set one "
                           "on your account page (Your Account Online)."};
        return {false, error_of(r, "Could not sign in")};
    }
    const juce::ScopedLock sl(lock_);
    email_ = e;
    access_ = juce::String(r.body.get_string("access_token"));
    refresh_ = juce::String(r.body.get_string("refresh_token"));
    access_expires_ = now_seconds() + r.body.get_int("expires_in", 3600);
    save_session();
    return {true, "Signed in as " + e};
}

juce::String Account::access_token()
{
    const juce::ScopedLock sl(lock_);
    if (refresh_.isEmpty()) return {};
    if (access_.isNotEmpty() && now_seconds() < access_expires_ - 60) return access_;
    const auto r = request(config_.supabase_url + "/auth/v1/token?grant_type=refresh_token", "POST",
                           "apikey: " + config_.anon_key + "\r\n",
                           juce::String(json::write(json::Object{{"refresh_token", refresh_.toStdString()}})));
    if (r.status < 200 || r.status >= 300 || r.body.get_string("access_token").empty()) {
        if (r.reached && r.status >= 400 && r.status < 500) {  // the session is gone: sign in again
            access_ = refresh_ = {};
            save_session();
        }
        return {};
    }
    access_ = juce::String(r.body.get_string("access_token"));
    refresh_ = juce::String(r.body.get_string("refresh_token"));
    access_expires_ = now_seconds() + r.body.get_int("expires_in", 3600);
    save_session();
    return access_;
}

juce::String Account::platform()
{
#if JUCE_WINDOWS
    return "windows";
#else
    return "macos";
#endif
}

juce::String Account::device_fingerprint()
{
    const juce::ScopedLock sl(lock_);
    auto salt_file = file("device-salt");
    auto salt = salt_file.loadFileAsString().trim();
    if (salt.length() < 32) {
        juce::Random r(juce::Time::getHighResolutionTicks() ^ juce::Time::currentTimeMillis());
        juce::MemoryBlock bytes(32);
        r.fillBitsRandomly(bytes.getData(), bytes.getSize());
        salt = juce::String::toHexString(bytes.getData(), static_cast<int>(bytes.getSize()), 0);
        salt_file.getParentDirectory().createDirectory();
        salt_file.replaceWithText(salt);
    }
    const auto text = salt + ":" + juce::SystemStats::getComputerName();
    return juce::SHA256(text.toUTF8()).toHexString();
}

Account::Result Account::activate(const juce::String& serial)
{
    const auto token = access_token();
    if (token.isEmpty()) return {false, "Sign in first"};
    json::Object body{{"deviceFingerprint", device_fingerprint().toStdString()},
                      {"deviceName", juce::SystemStats::getComputerName().substring(0, 120).toStdString()},
                      {"platform", platform().toStdString()},
                      {"appVersion", JUCE_APPLICATION_VERSION_STRING}};
    if (serial.trim().isNotEmpty()) body.set("serial", serial.trim().toUpperCase().toStdString());
    const auto r = request(config().site_url + "/api/licenses/activate", "POST", "Authorization: Bearer " + token + "\r\n",
                           juce::String(json::write(body)));
    if (r.status != 200 || !r.body["activated"].as_bool()) return {false, error_of(r, "This computer could not be activated")};
    {
        const juce::ScopedLock sl(lock_);
        write_json(file("activation.json"), json::Object{{"activatedAt", licensing::format_iso(now_seconds())}});
    }
    check_standing();
    return {true, "This computer is activated"};
}

Account::Result Account::check_standing()
{
    const auto token = access_token();
    if (token.isEmpty()) return {false, "Not signed in"};
    const auto r = request(config().site_url + "/api/licenses/standing", "GET", "Authorization: Bearer " + token + "\r\n");
    if (r.status != 200) return {false, error_of(r, "The licence could not be checked")};
    const juce::ScopedLock sl(lock_);
    const auto claim = licensing::claim_from_response(r.body);
    if (!claim) {
        // An account with no licence: nothing to write down (not a lapse).
        file("license-standing.json").deleteFile();
        return {true, "This account has no licence"};
    }
    const auto previous = licensing::DeskStanding::from_json(read_json(file("license-standing.json")));
    write_json(file("license-standing.json"), licensing::note_standing(previous, *claim, now_seconds()).to_json());
    return {true, "Licence " + juce::String(claim->status)};
}

Account::Reply Account::api(const juce::String& method, const juce::String& path, const json::Value& body)
{
    Reply out;
    const auto token = access_token();
    if (token.isEmpty()) {
        out.status = 401;
        out.error = "Sign in first (Help > Account and Licence...)";
        return out;
    }
    const auto r = request(config().site_url + path, method, "Authorization: Bearer " + token + "\r\n",
                           body.is_null() ? juce::String() : juce::String(json::write(body)));
    out.status = r.status;
    out.body = r.body;
    if (!out.ok()) out.error = error_of(r, "The server refused");
    return out;
}

Account::Reply Account::put_bytes(const juce::String& url, const juce::MemoryBlock& data)
{
    Reply out;
    juce::URL u = juce::URL(url).withPOSTData(data);
    juce::WebInputStream stream(u, true);
    stream.withExtraHeaders("Content-Type: application/octet-stream\r\nx-upsert: false\r\n");
    stream.withCustomRequestCommand("PUT");
    stream.withConnectionTimeout(30000);
    stream.connect(nullptr);
    out.status = stream.getStatusCode();
    const auto text = stream.readEntireStreamAsString();
    if (auto parsed = json::parse(text.toStdString())) out.body = *parsed;
    if (!out.ok())
        out.error = out.status == 0 ? juce::String("Could not reach the server. Check the internet connection and try again.")
                                    : "The upload was refused (" + juce::String(out.status) + ")";
    return out;
}

std::optional<juce::MemoryBlock> Account::get_bytes(const juce::String& url, int* status)
{
    juce::WebInputStream stream(juce::URL(url), false);
    stream.withConnectionTimeout(30000);
    const bool connected = stream.connect(nullptr);
    if (status) *status = stream.getStatusCode();
    if (!connected || stream.getStatusCode() < 200 || stream.getStatusCode() >= 300) return std::nullopt;
    juce::MemoryBlock data;
    juce::MemoryOutputStream out(data, false);
    out.writeFromInputStream(stream, -1);
    out.flush();
    return data;
}

bool Account::download_to_file(const juce::String& url, const juce::File& file, juce::String* error)
{
    juce::WebInputStream stream(juce::URL(url), false);
    stream.withConnectionTimeout(30000);
    if (!stream.connect(nullptr) || stream.getStatusCode() < 200 || stream.getStatusCode() >= 300) {
        if (error) *error = "the download was refused (" + juce::String(stream.getStatusCode()) + ")";
        return false;
    }
    file.getParentDirectory().createDirectory();
    juce::TemporaryFile temp(file);
    {
        juce::FileOutputStream out(temp.getFile());
        if (!out.openedOk()) {
            if (error) *error = "could not write " + file.getFullPathName();
            return false;
        }
        const auto expected = stream.getTotalLength();
        const auto written = out.writeFromInputStream(stream, -1);
        out.flush();
        if (out.getStatus().failed() || (expected > 0 && written != expected)) {
            if (error) *error = "the download was cut off";
            return false;
        }
    }
    if (!temp.overwriteTargetFileWithTemporary()) {
        if (error) *error = "could not replace " + file.getFullPathName() + " (is it open elsewhere?)";
        return false;
    }
    return true;
}

void Account::sign_out()
{
    const juce::ScopedLock sl(lock_);
    email_ = access_ = refresh_ = {};
    access_expires_ = 0;
    file("session.json").deleteFile();
}

}  // namespace vcc::desktop
