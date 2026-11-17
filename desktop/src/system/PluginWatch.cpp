#include "system/PluginWatch.h"

#include "audio/PluginHost.h"
#include "ui/hw/HwTheme.h"

namespace vcc::desktop {

namespace {
PluginWatch*& watch_slot()
{
    static PluginWatch* w = nullptr;
    return w;
}

juce::String label(const juce::PluginDescription& d)
{
    return d.name + (d.manufacturerName.isNotEmpty() ? " (" + d.manufacturerName + ")" : juce::String());
}

juce::String plural(int n, const char* one, const char* many) { return juce::String(n) + " " + (n == 1 ? one : many); }
}  // namespace

PluginWatch* plugin_watch() { return watch_slot(); }
void set_plugin_watch(PluginWatch* watch) { watch_slot() = watch; }

juce::String PluginReport::summary() const
{
    juce::StringArray parts;
    if (!added.isEmpty()) parts.add(juce::String(added.size()) + " new");
    if (!updated.isEmpty()) parts.add(juce::String(updated.size()) + " updated");
    if (!removed.isEmpty()) parts.add(juce::String(removed.size()) + " removed");
    if (!attention.isEmpty()) parts.add(plural(attention.size(), "needs attention", "need attention"));
    if (parts.isEmpty()) return "Plug-ins up to date (" + plural(looked_at, "plug-in file", "plug-in files") + " looked at)";
    return "Plug-ins: " + parts.joinIntoString(", ");
}

juce::String PluginReport::details() const
{
    juce::String s;
    s << (finished ? "Checked " : "Stopped while checking ") << when.toString(true, true, false) << ": "
      << plural(looked_at, "plug-in file", "plug-in files") << " in the plug-in folders.\n";
    auto section = [&s](const char* title, const juce::StringArray& lines) {
        if (lines.isEmpty()) return;
        s << "\n" << title << "\n";
        for (const auto& l : lines) s << "  " << l << "\n";
    };
    section("New", added);
    section("Updated", updated);
    section("Removed", removed);
    section("Needs attention (did not load: crashed, hung, or waited for a licence or authorisation)", attention);
    if (!any_change() && attention.isEmpty()) s << "\nNothing new, updated or removed.\n";
    return s;
}

PluginWatch::PluginWatch(PluginHost& host) : juce::Thread("VCC plug-in check"), host_(host) {}

PluginWatch::~PluginWatch()
{
    signalThreadShouldExit();
    stopThread(70000);  // one plug-in's scan at most; it watches threadShouldExit
    if (watch_slot() == this) watch_slot() = nullptr;
}

bool PluginWatch::at_startup() { return hw::ui_settings().getBoolValue("plugin_check_at_startup", true); }

void PluginWatch::set_at_startup(bool on)
{
    hw::ui_settings().setValue("plugin_check_at_startup", on);
    hw::ui_settings().saveIfNeeded();
}

juce::FileSearchPath PluginWatch::folders(juce::AudioPluginFormat& format) const
{
    if (auto* props = host_.properties()) return juce::PluginListComponent::getLastSearchPath(*props, format);
    return format.getDefaultLocationsToSearch();
}

void PluginWatch::start(std::function<void(const juce::String&)> progress, std::function<void(const PluginReport&)> done)
{
    if (isThreadRunning()) return;
    progress_ = std::move(progress);
    done_ = std::move(done);
    startThread(juce::Thread::Priority::low);
}

PluginReport PluginWatch::run_now()
{
    last_ = look({});
    if (last_.any_change()) host_.save_list();
    return last_;
}

void PluginWatch::run()
{
    auto report = look([this](int at, int of) {
        if (!progress_) return;
        auto cb = progress_;
        juce::MessageManager::callAsync([cb, at, of] { cb("Checking plug-ins: " + juce::String(at) + " of " + juce::String(of)); });
    });
    auto cb = done_;
    juce::MessageManager::callAsync([this, report, cb] {
        last_ = report;
        if (report.any_change()) host_.save_list();
        if (cb) cb(report);
    });
}

PluginReport PluginWatch::look(const std::function<void(int, int)>& progress)
{
    PluginReport r;
    auto& known = host_.known();
    // Out of process only: a plug-in loaded here to be looked at could take
    // the app down with it (the Plug-ins window scans in process when it must).
    const bool safe = host_.scans_out_of_process();

    struct Item {
        juce::AudioPluginFormat* format;
        juce::String id;
    };
    std::vector<Item> items;
    for (auto* format : host_.formats().getFormats()) {
        if (!format->canScanForPlugins()) continue;
        for (const auto& id : format->searchPathsForPlugins(folders(*format), true, true)) items.push_back({format, id});
    }
    r.looked_at = static_cast<int>(items.size());
    const auto failures = host_.scan_failures();
    const auto blacklisted = known.getBlacklistedFiles();

    int at = 0;
    for (const auto& item : items) {
        if (threadShouldExit()) {
            r.when = juce::Time::getCurrentTime();
            return r;
        }
        ++at;
        if (progress && (at % 5 == 0 || at == r.looked_at)) progress(at, r.looked_at);
        if (blacklisted.contains(item.id)) continue;
        if (const auto f = failures.find(item.id); f != failures.end()) {
            // Failed before and not changed since: still needs attention.
            r.attention.add(juce::File::createFileWithoutCheckingPath(item.id).getFileNameWithoutExtension() + ": " + f->second);
            continue;
        }
        if (known.isListingUpToDate(item.id, *item.format)) continue;
        // New, or changed on disk since it was listed (an update was installed).
        juce::Array<juce::PluginDescription> before;
        for (const auto& t : known.getTypesForFormat(*item.format))
            if (t.fileOrIdentifier == item.id) before.add(t);
        if (!safe) {
            r.attention.add(item.id + ": not looked at (the scanner tool is missing - use Studio > Plug-ins to scan)");
            continue;
        }
        juce::OwnedArray<juce::PluginDescription> found;
        const bool ok = host_.scan_file(*item.format, item.id, found, [this] { return threadShouldExit(); });
        if (threadShouldExit()) continue;
        if (!ok || found.isEmpty()) {
            const auto now = host_.scan_failures();
            const auto why = now.find(item.id);
            r.attention.add(juce::File::createFileWithoutCheckingPath(item.id).getFileNameWithoutExtension() + ": " +
                            (why != now.end() ? why->second : juce::String("no plug-in found in it")));
            continue;
        }
        for (const auto& old : before) known.removeType(old);
        for (auto* d : found) {
            known.addType(*d);
            juce::String old_version;
            for (const auto& b : before)
                if (b.name == d->name) old_version = b.version;
            if (before.isEmpty()) r.added.add(label(*d) + (d->version.isNotEmpty() ? " " + d->version : juce::String()));
            else
                r.updated.add(label(*d) + (old_version.isNotEmpty() || d->version.isNotEmpty()
                                               ? " " + (old_version.isEmpty() ? juce::String("?") : old_version) + " -> " +
                                                     (d->version.isEmpty() ? juce::String("?") : d->version)
                                               : juce::String()));
        }
    }
    // Uninstalled since: off the list.
    for (auto* format : host_.formats().getFormats())
        for (const auto& t : known.getTypesForFormat(*format)) {
            bool listed = false;
            for (const auto& item : items) listed = listed || (item.format == format && item.id == t.fileOrIdentifier);
            if (!listed && !format->doesPluginStillExist(t)) {
                r.removed.add(label(t));
                known.removeType(t);
            }
        }
    r.finished = true;
    r.when = juce::Time::getCurrentTime();
    return r;
}

}  // namespace vcc::desktop
