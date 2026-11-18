#include "audio/SoundPack.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <map>
#include <mutex>

#include "AppResources.h"
#include "tsf.h"
#include "vcc/core/Hash.h"
#include "vcc/core/Json.h"

#ifndef VCC_SUPABASE_URL
#define VCC_SUPABASE_URL ""
#endif

namespace vcc::desktop::sounds {

Bank::~Bank()
{
    if (font != nullptr) tsf_close(font);
}

namespace {

struct Shared {
    std::mutex mutex;  // guards status_ and the listener map (message thread mostly)
    std::atomic<State> state{State::Missing};
    std::atomic<double> progress{0.0};
    std::atomic<int> generation{0};
    std::atomic<bool> busy{false};
    juce::String status = "Not installed";
    std::shared_ptr<const Bank> piano, gm;
    std::map<int, std::function<void()>> listeners;
    int next_id = 1;
};

Shared& shared()
{
    static Shared s;
    return s;
}

void notify()
{
    auto run = [] {
        std::map<int, std::function<void()>> copy;
        {
            std::lock_guard<std::mutex> lock(shared().mutex);
            copy = shared().listeners;
        }
        for (auto& [id, fn] : copy)
            if (fn) fn();
    };
    if (juce::MessageManager::getInstanceWithoutCreating() != nullptr &&
        juce::MessageManager::getInstance()->isThisTheMessageThread())
        run();
    else
        juce::MessageManager::callAsync(run);
}

void set_status(State st, const juce::String& text)
{
    {
        std::lock_guard<std::mutex> lock(shared().mutex);
        shared().status = text;
    }
    shared().state = st;
    notify();
}

juce::String megabytes(std::int64_t bytes) { return juce::String(static_cast<double>(bytes) / 1e6, 0) + " MB"; }

/// Loads one SoundFont into a Bank; null (and why) if it cannot.
std::shared_ptr<Bank> load_bank(const juce::File& file, const juce::String& name, juce::String& error)
{
    tsf* f = tsf_load_filename(file.getFullPathName().toRawUTF8());
    if (f == nullptr) {
        error = "Could not read " + file.getFileName();
        return {};
    }
    return std::make_shared<Bank>(f, name);
}

struct Loaded {
    std::shared_ptr<Bank> piano, gm;
    juce::String error;
};

Loaded load_files()
{
    Loaded out;
    const auto manifest = folder().getChildFile("manifest.json");
    for (const auto& f : parse_manifest(manifest.loadFileAsString())) {
        const auto file = folder().getChildFile(f.name);
        if (f.role == "piano")
            out.piano = load_bank(file, "Salamander Grand Piano", out.error);
        else if (f.role == "gm")
            out.gm = load_bank(file, "MuseScore General", out.error);
    }
    if (out.piano == nullptr && out.gm == nullptr && out.error.isEmpty()) out.error = "The sound pack has no banks";
    return out;
}

void publish(Loaded loaded)
{
    auto& s = shared();
    s.piano = std::move(loaded.piano);
    s.gm = std::move(loaded.gm);
    ++s.generation;
    if (s.piano == nullptr && s.gm == nullptr) {
        set_status(State::Failed, loaded.error);
        return;
    }
    juce::StringArray names;
    if (s.piano) names.add(s.piano->name);
    if (s.gm) names.add(s.gm->name);
    set_status(State::Ready, "Ready: " + names.joinIntoString(" and ") +
                                 (loaded.error.isNotEmpty() ? " (" + loaded.error + ")" : juce::String()));
}

/// Copies or downloads `name` from the source into `dest`; empty or why not.
juce::String fetch(const juce::String& base, const juce::String& name, const juce::File& dest,
                   std::int64_t expected_size, std::int64_t done_before, std::int64_t total_all)
{
    dest.deleteFile();
    // A local folder (file:// or a path): the checks, or an offline install.
    if (base.startsWith("file:") || juce::File::isAbsolutePath(base)) {
        const juce::File src = base.startsWith("file:") ? juce::URL(base).getLocalFile() : juce::File(base);
        if (!src.getChildFile(name).copyFileTo(dest)) return "Could not copy " + name;
        shared().progress = total_all > 0 ? static_cast<double>(done_before + expected_size) / static_cast<double>(total_all) : 1.0;
        notify();
        return {};
    }
    juce::WebInputStream stream(juce::URL(base + "/" + name), false);
    stream.withConnectionTimeout(30000);
    if (!stream.connect(nullptr) || stream.getStatusCode() < 200 || stream.getStatusCode() >= 300)
        return name + " could not be downloaded (" + juce::String(stream.getStatusCode()) + ")";
    juce::FileOutputStream out(dest);
    if (!out.openedOk()) return "Could not write " + dest.getFullPathName();
    juce::HeapBlock<char> buffer(1 << 16);
    std::int64_t got = 0;
    int ticks = 0;
    while (!stream.isExhausted()) {
        const int n = stream.read(buffer.getData(), 1 << 16);
        if (n < 0) break;
        if (n == 0 && stream.isExhausted()) break;
        if (!out.write(buffer.getData(), static_cast<size_t>(n))) return "Could not write the sounds (is the disk full?)";
        got += n;
        if (total_all > 0) shared().progress = static_cast<double>(done_before + got) / static_cast<double>(total_all);
        if (++ticks % 32 == 0) notify();
    }
    out.flush();
    if (expected_size > 0 && got != expected_size) return "The download of " + name + " was cut off: try again";
    return {};
}

void download_job(bool force)
{
    auto& s = shared();
    const auto base = source_url();
    if (base.isEmpty()) {
        set_status(State::Missing, "Not installed (this build has nowhere to download the sounds from)");
        s.busy = false;
        return;
    }
    if (!force && installed()) {
        set_status(State::Loading, "Loading the sounds...");
        auto loaded = load_files();
        juce::MessageManager::callAsync([l = std::move(loaded)]() mutable {
            publish(std::move(l));
            shared().busy = false;
        });
        return;
    }
    s.progress = 0.0;
    set_status(State::Downloading, "Downloading the sounds...");
    const auto dir = folder();
    dir.createDirectory();
    const auto staging = dir.getChildFile("download");
    staging.deleteRecursively();
    staging.createDirectory();

    auto fail = [&](const juce::String& why) {
        staging.deleteRecursively();
        set_status(installed() ? State::Ready : State::Failed, why);
        s.busy = false;
    };

    const auto manifest_file = staging.getChildFile("manifest.json");
    if (const auto err = fetch(base, "manifest.json", manifest_file, 0, 0, 0); err.isNotEmpty()) return fail(err);
    juce::String version;
    const auto text = manifest_file.loadFileAsString();
    const auto files = parse_manifest(text, &version);
    if (files.empty()) return fail("The sound pack's list of files could not be read");
    std::int64_t total = 0;
    for (const auto& f : files) total += f.size;
    set_status(State::Downloading, "Downloading the sounds (" + megabytes(total) + ")...");
    std::int64_t done = 0;
    for (const auto& f : files) {
        if (f.name.find_first_of("/\\") != std::string::npos || f.name.empty() || f.name[0] == '.')
            return fail("The sound pack lists an unexpected file name");
        const auto dest = staging.getChildFile(juce::String(f.name));
        if (const auto err = fetch(base, juce::String(f.name), dest, f.size, done, total); err.isNotEmpty()) return fail(err);
        // Nothing is used unless it is byte for byte what the manifest records.
        if (hash::sha256_file(dest.getFullPathName().toStdString()) != f.sha256)
            return fail(juce::String(f.name) + " did not match its checksum: try again");
        done += f.size;
    }
    // Swap the new files in (the loaded banks hold their own copies in memory).
    for (const auto& f : files) {
        const auto dest = dir.getChildFile(juce::String(f.name));
        dest.deleteFile();
        staging.getChildFile(juce::String(f.name)).moveFileTo(dest);
    }
    dir.getChildFile("manifest.json").deleteFile();
    manifest_file.moveFileTo(dir.getChildFile("manifest.json"));
    staging.deleteRecursively();
    s.progress = 1.0;
    set_status(State::Loading, "Loading the sounds...");
    auto loaded = load_files();
    juce::MessageManager::callAsync([l = std::move(loaded)]() mutable {
        publish(std::move(l));
        shared().busy = false;
    });
}

}  // namespace

juce::File folder()
{
    const auto env = juce::SystemStats::getEnvironmentVariable("VCC_SOUNDS_DIR", {});
    if (env.isNotEmpty()) return juce::File(env);
    return user_data_directory().getChildFile("Sounds");
}

juce::String source_url()
{
    const auto env = juce::SystemStats::getEnvironmentVariable("VCC_SOUNDS_URL", {});
    if (env.isNotEmpty()) return env.trimCharactersAtEnd("/");
    const juce::String supabase(VCC_SUPABASE_URL);
    if (supabase.isEmpty()) return {};
    return supabase.trimCharactersAtEnd("/") + "/storage/v1/object/public/sounds/v1";
}

std::vector<PackFile> parse_manifest(const juce::String& text, juce::String* version)
{
    std::vector<PackFile> out;
    const auto doc = json::parse(text.toStdString());
    if (!doc || !doc->is_object()) return out;
    if (version != nullptr) *version = juce::String(doc->get_string("version"));
    if (const auto* files = doc->get("files"))
        for (const auto& f : files->as_array()) {
            PackFile p{f.get_string("name"), f.get_string("role"), f.get_string("sha256"), f.get_int("size")};
            if (!p.name.empty() && !p.sha256.empty() && p.size > 0) out.push_back(std::move(p));
        }
    return out;
}

bool installed()
{
    const auto files = parse_manifest(folder().getChildFile("manifest.json").loadFileAsString());
    if (files.empty()) return false;
    for (const auto& f : files) {
        const auto file = folder().getChildFile(juce::String(f.name));
        if (!file.existsAsFile() || file.getSize() != f.size) return false;
    }
    return true;
}

State state() { return shared().state.load(); }

juce::String status()
{
    std::lock_guard<std::mutex> lock(shared().mutex);
    if (shared().state == State::Downloading)
        return shared().status + " " + juce::String(juce::roundToInt(shared().progress.load() * 100.0)) + "%";
    return shared().status;
}

double progress() { return shared().progress.load(); }
int generation() { return shared().generation.load(); }
std::shared_ptr<const Bank> piano() { return shared().piano; }
std::shared_ptr<const Bank> gm() { return shared().gm; }

void load_async()
{
    if (shared().state == State::Ready || shared().busy.exchange(true)) return;
    juce::Thread::launch([] {
        if (!installed()) {
            set_status(State::Missing, "Not installed");
            shared().busy = false;
            return;
        }
        set_status(State::Loading, "Loading the sounds...");
        auto loaded = load_files();
        juce::MessageManager::callAsync([l = std::move(loaded)]() mutable {
            publish(std::move(l));
            shared().busy = false;
        });
    });
}

void download_async(bool force)
{
    if (shared().busy.exchange(true)) return;
    juce::Thread::launch([force] { download_job(force); });
}

void start()
{
    if (installed())
        load_async();
    else if (source_url().isNotEmpty())
        download_async(false);
    else
        set_status(State::Missing, "Not installed");
}

bool load_now()
{
    if (!installed()) return false;
    publish(load_files());
    return shared().state == State::Ready;
}

void unload()
{
    shared().piano.reset();
    shared().gm.reset();
    ++shared().generation;
    set_status(State::Missing, "Not loaded");
}

int add_listener(std::function<void()> fn)
{
    std::lock_guard<std::mutex> lock(shared().mutex);
    const int id = shared().next_id++;
    shared().listeners[id] = std::move(fn);
    return id;
}

void remove_listener(int id)
{
    std::lock_guard<std::mutex> lock(shared().mutex);
    shared().listeners.erase(id);
}

}  // namespace vcc::desktop::sounds
