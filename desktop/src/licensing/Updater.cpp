#include "licensing/Updater.h"

#include "AppResources.h"
#include "Preview.h"
#include "licensing/Account.h"
#include "vcc/core/Hash.h"
#include "vcc/core/Json.h"

#if JUCE_MAC
#include <unistd.h>
#endif

#ifndef VCC_BUILD_LABEL
#define VCC_BUILD_LABEL ""
#endif

namespace vcc::desktop {

Updater& Updater::get()
{
    static Updater u;
    return u;
}

juce::String Updater::current_version()
{
    const juce::String label(VCC_BUILD_LABEL);
    return label.isNotEmpty() ? label : juce::String(JUCE_APPLICATION_VERSION_STRING);
}

juce::String Updater::channel() { return preview::is_preview() ? "preview" : "stable"; }

bool Updater::available()
{
#if JUCE_WINDOWS || JUCE_MAC
    return get().site().isNotEmpty();
#else
    return false;  // Linux builds are for development: nothing to install
#endif
}

juce::String Updater::site() const
{
    return (site_override_.isNotEmpty() ? site_override_ : Account::get().config().site_url).trimCharactersAtEnd("/");
}

juce::File Updater::folder() const
{
    return folder_ != juce::File() ? folder_ : user_data_directory().getChildFile("Updates");
}

Updater::Check Updater::check()
{
    Check c;
    if (site().isEmpty()) {
        c.message = "This build is not connected to vc-composer.com";
        return c;
    }
    juce::WebInputStream stream(juce::URL(site() + "/api/releases?channel=" + channel()), false);
    stream.withExtraHeaders("Accept: application/json\r\n");
    stream.withConnectionTimeout(15000);
    if (!stream.connect(nullptr) || stream.getStatusCode() != 200) {
        c.message = stream.getStatusCode() == 0 ? juce::String("Could not reach vc-composer.com. Check the internet connection.")
                                                : "vc-composer.com could not say (" + juce::String(stream.getStatusCode()) + ")";
        return c;
    }
    const auto body = json::parse(stream.readEntireStreamAsString().toStdString());
    if (!body) {
        c.message = "vc-composer.com's answer was not understood";
        return c;
    }
    c.ok = true;
    c.offer = licensing::pick_update(*body, Account::platform().toStdString(), current_version().toStdString());
    c.message = c.offer ? "VC Composer " + juce::String(c.offer->version) + " is available (you have " + current_version() + ")"
                        : "VC Composer is up to date (" + current_version() + ")";
    return c;
}

bool Updater::may_download()
{
    auto& account = Account::get();
    return channel() == "preview" || account.signed_in() || account.device_activated();
}

Updater::Downloaded Updater::download(const licensing::UpdateOffer& offer, const std::function<void(double)>& progress)
{
    Downloaded d;
    auto& account = Account::get();
    if (!may_download()) {
        d.message = "Sign in first (Help > Account and Licence...): updates download through your account";
        return d;
    }
    // A short-lived link; the site checks who is asking - the account, or a
    // computer activated with a licence (no new sign-in for updates).
    const auto link = account.device_get("/api/downloads/" + Account::platform() + "?channel=" + channel());
    if (!link.ok()) {
        d.message = link.error;
        return d;
    }
    const auto url = juce::String(link.body.get_string("url"));
    const auto version = juce::String(link.body.get_string("version"));
    if (version.toStdString() != offer.version) {
        d.message = "vc-composer.com now offers " + version + ": check again";
        return d;
    }
    const auto expected = offer.sha256.empty() ? link.body.get_string("sha256") : offer.sha256;
    if (expected.empty()) {
        d.message = "The update has no recorded checksum, so it will not be installed";
        return d;
    }

    folder().createDirectory();
    const auto file = folder().getChildFile("VC-Composer-" + juce::File::createLegalFileName(version) +
                                            (Account::platform() == "windows" ? "-Setup.exe" : ".dmg"));
    const auto part = file.getSiblingFile(file.getFileName() + ".part");
    part.deleteFile();
    {
        juce::WebInputStream stream(juce::URL(url), false);
        stream.withConnectionTimeout(30000);
        if (!stream.connect(nullptr) || stream.getStatusCode() < 200 || stream.getStatusCode() >= 300) {
            d.message = "The download was refused (" + juce::String(stream.getStatusCode()) + ")";
            return d;
        }
        const auto total = stream.getTotalLength();
        juce::FileOutputStream out(part);
        if (!out.openedOk()) {
            d.message = "Could not write " + part.getFullPathName();
            return d;
        }
        juce::HeapBlock<char> buffer(1 << 16);
        juce::int64 got = 0;
        while (!stream.isExhausted()) {
            const int n = stream.read(buffer.getData(), 1 << 16);
            if (n < 0) break;
            if (n == 0 && stream.isExhausted()) break;
            if (!out.write(buffer.getData(), static_cast<size_t>(n))) {
                d.message = "Could not write the update (is the disk full?)";
                return d;
            }
            got += n;
            if (progress && total > 0) progress(static_cast<double>(got) / static_cast<double>(total));
        }
        out.flush();
        if (total > 0 && got != total) {
            d.message = "The download was cut off: try again";
            return d;
        }
    }
    // Nothing runs unless it is byte for byte what the site recorded.
    if (hash::sha256_file(part.getFullPathName().toStdString()) != expected) {
        part.deleteFile();
        d.message = "The update did not match its checksum and was deleted: try again";
        return d;
    }
    file.deleteFile();
    if (!part.moveFileTo(file)) {
        d.message = "Could not put the update in place";
        return d;
    }
    d.ok = true;
    d.file = file;
    d.message = "Downloaded VC Composer " + version;
    return d;
}

namespace {

/// The running app bundle on macOS (".../VC Composer.app").
juce::File app_bundle() { return juce::File::getSpecialLocation(juce::File::currentApplicationFile); }

/// The macOS replacement script: waits for this copy to quit, swaps the app
/// for the one on the disk image, then opens it. Every step goes to
/// install-update.log beside the download; if any step fails, the old app is
/// put back and the disk image is opened in the Finder, so there is always a
/// way on (drag VC Composer to Applications).
juce::String mac_script(const juce::File& dmg, const juce::File& app, int pid, const juce::File& log)
{
    auto q = [](const juce::String& s) { return "'" + s.replace("'", "'\\''") + "'"; };
    juce::String s;
    s << "#!/bin/sh\n"
      << "# VC Composer update: replaces the app once it has quit.\n"
      << "APP=" << q(app.getFullPathName()) << "\n"
      << "DMG=" << q(dmg.getFullPathName()) << "\n"
      << "exec >>" << q(log.getFullPathName()) << " 2>&1 </dev/null\n"
      << "echo \"=== $(date) installing $DMG over $APP\"\n"
      << "MNT=\"\"; NEW=\"$APP.new\"; OLD=\"$APP.old\"\n"
      << "fail() {\n"
      << "  echo \"FAILED: $1\"\n"
      << "  [ -d \"$OLD\" ] && [ ! -d \"$APP\" ] && mv \"$OLD\" \"$APP\"\n"
      << "  [ -n \"$MNT\" ] && hdiutil detach \"$MNT\" -quiet -force\n"
      << "  rm -rf \"$NEW\"\n"
      << "  open \"$DMG\"\n"
      << "  exit 1\n"
      << "}\n"
      << "i=0\n"
      << "while kill -0 " << pid << " 2>/dev/null; do\n"
      << "  sleep 0.5; i=$((i+1))\n"
      << "  [ $i -gt 240 ] && fail \"VC Composer did not quit\"\n"
      << "done\n"
      << "echo \"VC Composer has quit\"\n"
      << "MNT=$(mktemp -d /tmp/vcc-update.XXXXXX) || fail \"no temporary folder\"\n"
      << "hdiutil attach -nobrowse -readonly -noautoopen -mountpoint \"$MNT\" \"$DMG\" || fail \"the disk image would not open\"\n"
      << "SRC=\"\"\n"
      << "for a in \"$MNT\"/*.app; do [ -d \"$a\" ] && SRC=\"$a\" && break; done\n"
      << "[ -n \"$SRC\" ] || fail \"no app on the disk image\"\n"
      << "rm -rf \"$NEW\" \"$OLD\"\n"
      << "ditto \"$SRC\" \"$NEW\" || fail \"could not copy the new version\"\n"
      << "xattr -dr com.apple.quarantine \"$NEW\" 2>/dev/null\n"
      << "mv \"$APP\" \"$OLD\" || fail \"could not move the old version aside\"\n"
      << "mv \"$NEW\" \"$APP\" || fail \"could not put the new version in place\"\n"
      << "rm -rf \"$OLD\"\n"
      << "hdiutil detach \"$MNT\" -quiet || hdiutil detach \"$MNT\" -quiet -force\n"
      << "rmdir \"$MNT\" 2>/dev/null\n"
      << "echo \"installed; opening the new version\"\n"
      << "open \"$APP\" || fail \"the new version would not open\"\n"
      << "echo \"done\"\n";
    return s;
}

}  // namespace

bool Updater::write_script(const juce::File& file, const juce::String& text)
{
    // Unix line ends: juce's replaceWithText writes "\r\n" by default, and a
    // shell script with them stops at its first line (so no update went in).
    return file.replaceWithText(text, false, false, "\n") && file.setExecutePermission(true);
}

juce::File Updater::install_log() const { return folder().getChildFile("install-update.log"); }

juce::StringArray Updater::install_plan(const juce::File& file) const
{
    if (Account::platform() == "windows")
        return {file.getFullPathName(), "/SILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CLOSEAPPLICATIONS", "/UPDATE"};
    return juce::StringArray::fromLines(mac_script(file, app_bundle(), 0, install_log()));
}

juce::String Updater::install(const juce::File& file)
{
    if (!file.existsAsFile()) return "The update file is missing: download it again";
#if JUCE_WINDOWS
    // The signed installer, quietly: it closes this copy, installs over it,
    // and starts the new one (its /UPDATE switch).
    if (!file.startAsProcess("/SILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /UPDATE"))
        return "The installer could not be started";
    return {};
#elif JUCE_MAC
    const auto app = app_bundle();
    if (!app.getParentDirectory().hasWriteAccess()) {
        file.startAsProcess();  // the disk image, for a drag into Applications
        return "VC Composer's folder cannot be written here, so the disk image is open: drag VC Composer to Applications and replace it";
    }
    const auto script = folder().getChildFile("install-update.sh");
    if (!write_script(script, mac_script(file, app, static_cast<int>(getpid()), install_log())))
        return "Could not prepare the update";
    // Detached from this process (its own session, its parent gone at once),
    // so it lives on after VC Composer quits.
    juce::ChildProcess launcher;
    const juce::StringArray args{"/bin/sh", "-c",
                                 "( trap '' HUP; exec /bin/sh \"$0\" ) </dev/null >/dev/null 2>&1 &", script.getFullPathName()};
    if (!launcher.start(args, 0)) return "The update could not be started";
    launcher.waitForProcessToFinish(5000);
    return {};
#else
    juce::ignoreUnused(file);
    return "Updates install on Windows and macOS; on this system, install from vc-composer.com";
#endif
}

}  // namespace vcc::desktop
