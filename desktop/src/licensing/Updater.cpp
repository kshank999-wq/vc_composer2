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

Updater::Downloaded Updater::download(const licensing::UpdateOffer& offer, const std::function<void(double)>& progress)
{
    Downloaded d;
    auto& account = Account::get();
    if (!account.signed_in() && !account.device_activated()) {
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
/// from the disk image, then opens it. Falls back to opening the disk image.
juce::String mac_script(const juce::File& dmg, const juce::File& app, int pid)
{
    auto q = [](const juce::String& s) { return "'" + s.replace("'", "'\\''") + "'"; };
    juce::String s;
    s << "#!/bin/sh\n"
      << "# VC Composer update: replaces the app once it has quit.\n"
      << "while kill -0 " << pid << " 2>/dev/null; do sleep 0.5; done\n"
      << "MNT=$(mktemp -d)\n"
      << "if ! hdiutil attach -nobrowse -readonly -noautoopen -mountpoint \"$MNT\" " << q(dmg.getFullPathName()) << " >/dev/null; then\n"
      << "  open " << q(dmg.getFullPathName()) << "; exit 1\n"
      << "fi\n"
      << "SRC=\"$MNT/" << app.getFileName() << "\"\n"
      << "if [ -d \"$SRC\" ] && ditto \"$SRC\" " << q(app.getFullPathName() + ".new") << "; then\n"
      << "  rm -rf " << q(app.getFullPathName()) << " && mv " << q(app.getFullPathName() + ".new") << " " << q(app.getFullPathName()) << "\n"
      << "  hdiutil detach \"$MNT\" -quiet\n"
      << "  open " << q(app.getFullPathName()) << "\n"
      << "else\n"
      << "  rm -rf " << q(app.getFullPathName() + ".new") << "\n"
      << "  hdiutil detach \"$MNT\" -quiet\n"
      << "  open " << q(dmg.getFullPathName()) << "\n"
      << "fi\n";
    return s;
}

}  // namespace

juce::StringArray Updater::install_plan(const juce::File& file) const
{
    if (Account::platform() == "windows")
        return {file.getFullPathName(), "/SILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CLOSEAPPLICATIONS", "/UPDATE"};
    return juce::StringArray::fromLines(mac_script(file, app_bundle(), 0));
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
    if (!script.replaceWithText(mac_script(file, app, static_cast<int>(getpid())))) return "Could not prepare the update";
    juce::ChildProcess launcher;
    const juce::StringArray args{"/bin/sh", "-c", "nohup /bin/sh '" + script.getFullPathName() + "' >/dev/null 2>&1 &"};
    if (!launcher.start(args)) return "The update could not be started";
    launcher.waitForProcessToFinish(5000);
    return {};
#else
    juce::ignoreUnused(file);
    return "Updates install on Windows and macOS; on this system, install from vc-composer.com";
#endif
}

}  // namespace vcc::desktop
