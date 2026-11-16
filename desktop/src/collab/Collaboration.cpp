#include "collab/Collaboration.h"

#include <juce_cryptography/juce_cryptography.h>

#include "AppResources.h"
#include "licensing/Account.h"
#include "vcc/core/Files.h"
#include "vcc/core/Hash.h"
#include "vcc/session/Relink.h"

namespace vcc::desktop::collab {

namespace shared = session::shared;

namespace {

juce::String str(const json::Value& v, const char* key) { return juce::String(v.get_string(key)); }

/// A person's name as the server gives it: the profile's display name, else the email.
juce::String person(const json::Value& row, const char* profile_key, const juce::String& fallback)
{
    const auto name = str(row[profile_key], "display_name");
    return name.isNotEmpty() ? name : fallback;
}

/// Runs `work` off the message thread and `then` on it, if the controller is still there.
template <typename T>
void background(std::shared_ptr<bool> alive, std::function<T()> work, std::function<void(T)> then)
{
    juce::Thread::launch([alive, work = std::move(work), then = std::move(then)]() mutable {
        auto result = std::make_shared<T>(work());
        juce::MessageManager::callAsync([alive, result, then = std::move(then)] {
            if (*alive) then(std::move(*result));
        });
    });
}

juce::String sha256(const std::string& text) { return juce::SHA256(text.data(), text.size()).toHexString(); }

/// Downloads a revision and checks it is the file the server recorded.
struct Fetched {
    bool ok = false;
    juce::String error;
    std::string text;
    shared::Manifest media;
};

Fetched fetch_revision(const juce::String& project_id, int number)
{
    Fetched f;
    auto& a = Account::get();
    const auto link = a.api("GET", "/api/collab/projects/" + project_id + "/revisions/" + juce::String(number));
    if (!link.ok()) {
        f.error = link.error;
        return f;
    }
    int status = 0;
    const auto data = a.get_bytes(str(link.body, "url"), &status);
    if (!data) {
        f.error = "Revision " + juce::String(number) + " could not be downloaded (" + juce::String(status) + ")";
        return f;
    }
    f.text = data->toString().toStdString();
    f.media = shared::manifest_from_json(link.body["media"]);
    const auto expected = str(link.body, "sha256");
    if (expected.isNotEmpty() && sha256(f.text) != expected) {
        f.error = "Revision " + juce::String(number) + " arrived damaged; try again";
        return f;
    }
    f.ok = true;
    return f;
}

struct Pushed {
    bool accepted = false;
    int revision = 0;
    int head = -1;  ///< On a refusal because somebody pushed since: their head.
    juce::String error;
};

/// Uploads the document and pushes it as the revision after `base`.
Pushed push_revision(const juce::String& project_id, int base, const std::string& text, const juce::String& message,
                     std::uint32_t site, const shared::Manifest& media)
{
    Pushed p;
    auto& a = Account::get();
    const auto slot = a.api("POST", "/api/collab/projects/" + project_id + "/upload");
    if (!slot.ok()) {
        p.error = slot.error;
        return p;
    }
    const auto up = a.put_bytes(str(slot.body, "url"), juce::MemoryBlock(text.data(), text.size()));
    if (!up.ok()) {
        p.error = up.error;
        return p;
    }
    const auto r = a.api("POST", "/api/collab/projects/" + project_id + "/revisions",
                         json::Object{{"base", static_cast<long long>(base)},
                                      {"message", message.substring(0, 500).toStdString()},
                                      {"sizeBytes", static_cast<long long>(text.size())},
                                      {"sha256", sha256(text).toStdString()},
                                      {"site", static_cast<long long>(site)},
                                      {"path", slot.body.get_string("path")},
                                      {"media", shared::manifest_to_json(media)}});
    if (r.status == 409) {
        p.head = static_cast<int>(r.body.get_int("head", -1));
        p.error = r.error;
        return p;
    }
    if (!r.ok()) {
        p.error = r.error;
        return p;
    }
    p.accepted = true;
    p.revision = static_cast<int>(r.body.get_int("revision"));
    return p;
}

juce::String plural(int n, const char* one, const char* many) { return juce::String(n) + " " + (n == 1 ? one : many); }

/// Uploads the files of `manifest` the server does not have yet. Empty on success, else why not.
juce::String upload_media(const juce::String& project_id, const shared::Manifest& manifest, const std::string& project_file,
                          const std::function<void(const juce::String&)>& progress)
{
    auto& a = Account::get();
    for (std::size_t from = 0; from < manifest.size(); from += 1000) {
        json::Array files;
        std::map<std::string, const shared::MediaEntry*> by_sha;
        for (std::size_t i = from; i < std::min(manifest.size(), from + 1000); ++i) {
            files.push_back(json::Object{{"sha256", manifest[i].sha256}, {"sizeBytes", static_cast<long long>(manifest[i].size)}});
            by_sha[manifest[i].sha256] = &manifest[i];
        }
        const auto r = a.api("POST", "/api/collab/projects/" + project_id + "/media", json::Object{{"files", json::Value(std::move(files))}});
        if (!r.ok()) return r.error;
        const auto& missing = r.body["missing"].as_array();
        int n = 0;
        for (const auto& m : missing) {
            const auto it = by_sha.find(m.get_string("sha256"));
            if (it == by_sha.end()) continue;
            const auto& e = *it->second;
            progress("Uploading " + juce::String(++n) + " of " + juce::String(static_cast<int>(missing.size())) + ": " + juce::String(e.path));
            juce::MemoryBlock data;
            if (!juce::File(shared::media_file(project_file, e.path)).loadFileAsData(data) || data.getSize() != e.size)
                return juce::String(e.path) + " could not be read (or changed while syncing)";
            if (juce::SHA256(data.getData(), data.getSize()).toHexString().toStdString() != e.sha256)
                return juce::String(e.path) + " changed while syncing: sync again";
            const auto up = a.put_bytes(str(m, "url"), data);
            if (!up.ok()) return juce::String(e.path) + ": " + up.error;
        }
    }
    return {};
}

/// Downloads these media files into the project folder, each checked against
/// its SHA-256 before it replaces anything. Returns the failures.
juce::StringArray download_media(const juce::String& project_id, const shared::Manifest& entries, const std::string& project_file,
                                 shared::HashCache& cache, const std::function<void(const juce::String&)>& progress)
{
    juce::StringArray failed;
    auto& a = Account::get();
    int n = 0;
    for (const auto& e : entries) {
        progress("Downloading " + juce::String(++n) + " of " + juce::String(static_cast<int>(entries.size())) + ": " + juce::String(e.path));
        const auto link = a.api("GET", "/api/collab/projects/" + project_id + "/media/" + juce::String(e.sha256));
        if (!link.ok()) {
            failed.add(juce::String(e.path) + ": " + link.error);
            continue;
        }
        const juce::File target(shared::media_file(project_file, e.path));
        const auto part = target.getSiblingFile(target.getFileName() + ".part");
        juce::String error;
        if (!a.download_to_file(str(link.body, "url"), part, &error)) {
            failed.add(juce::String(e.path) + ": " + error);
            continue;
        }
        if (hash::sha256_file(part.getFullPathName().toStdString()) != e.sha256) {
            part.deleteFile();
            failed.add(juce::String(e.path) + ": arrived damaged");
            continue;
        }
        if (!part.moveFileTo(target)) {
            part.deleteFile();
            failed.add(juce::String(e.path) + ": could not be put in place (is it open elsewhere?)");
            continue;
        }
        cache.note(e.path, e.sha256);
    }
    cache.save();
    return failed;
}

/// "2 media files are missing here" and the like, for the summary.
juce::String media_notes(const shared::Listed& l)
{
    juce::String s;
    if (!l.missing.empty()) s << " " << plural(static_cast<int>(l.missing.size()), "media file is", "media files are") << " missing here and not shared.";
    if (!l.too_large.empty()) s << " " << plural(static_cast<int>(l.too_large.size()), "file is", "files are") << " over 2 GB and not shared.";
    return s;
}

struct DetailReply {
    std::optional<Detail> detail;
    juce::String error;
};

DetailReply fetch_detail(const juce::String& id)
{
    DetailReply d;
    const auto r = Account::get().api("GET", "/api/collab/projects/" + id);
    if (!r.ok()) d.error = r.error;
    else if (!(d.detail = parse_detail(r.body))) d.error = "The server's answer was not understood";
    return d;
}


}  // namespace

// --- Parsing -----------------------------------------------------------------------------------

std::optional<Detail> parse_detail(const json::Value& body)
{
    if (!body.is_object() || !body["project"].is_object()) return std::nullopt;
    Detail d;
    d.role = str(body, "role");
    d.me = str(body, "me");
    d.id = str(body["project"], "id");
    d.name = str(body["project"], "name");
    d.head = static_cast<int>(body["project"].get_int("head_revision"));
    for (const auto& m : body["members"].as_array())
        d.members.push_back({person(m, "profile", str(m, "invited_email")), str(m, "invited_email"), str(m, "role"),
                             !m["accepted_at"].is_null() && m.get("accepted_at") != nullptr});
    for (const auto& r : body["revisions"].as_array())
        d.revisions.push_back({static_cast<int>(r.get_int("number")), str(r, "message"), person(r, "author", "Someone"),
                               str(r, "created_at").substring(0, 16).replace("T", " ")});
    for (const auto& l : body["locks"].as_array())
        d.locks.push_back({str(l, "track_id"), person(l, "holder", "Another composer"), str(l, "user_id"),
                           str(l, "expires_at").substring(11, 16)});
    return d;
}

std::vector<ProjectSummary> parse_list(const json::Value& body)
{
    std::vector<ProjectSummary> out;
    for (const auto& p : body["projects"].as_array())
        out.push_back({str(p, "id"), str(p, "name"), str(p, "role"), str(p, "ownerName"), static_cast<int>(p.get_int("head"))});
    return out;
}

juce::String describe_value(const std::optional<json::Value>& v)
{
    if (!v) return "deleted";
    if (v->is_string()) return "\"" + juce::String(v->as_string()) + "\"";
    if (v->is_bool()) return v->as_bool() ? "on" : "off";
    if (v->is_number()) return juce::String(v->as_double());
    if (v->is_array()) return plural(static_cast<int>(v->as_array().size()), "item", "items");
    if (v->is_object()) {
        if (v->get_bool("rest")) return "a rest";
        if (v->has("step") && v->has("octave")) {
            juce::String s = juce::String(v->get_string("step")) ;
            const auto alt = v->get_int("alteration");
            for (int i = 0; i < std::abs(static_cast<int>(alt)); ++i) s << (alt > 0 ? "#" : "b");
            return s + juce::String(v->get_int("octave"));
        }
        if (v->has("name") && (*v)["name"].is_string()) return "\"" + str(*v, "name") + "\"";
    }
    auto text = juce::String(json::write(*v));
    return text.length() > 60 ? text.substring(0, 57) + "..." : text;
}

// --- The controller -------------------------------------------------------------------------------

Controller& Controller::get()
{
    static Controller c;
    return c;
}

std::uint32_t Controller::site()
{
    static std::uint32_t cached = 0;
    if (cached != 0) return cached;
    const auto f = user_data_directory().getChildFile("collaboration-site.txt");
    auto value = static_cast<std::uint32_t>(f.loadFileAsString().trim().getLargeIntValue());
    if (value == 0 || value > 0xffffffu) {
        value = shared::site_from(static_cast<std::uint64_t>(juce::Random::getSystemRandom().nextInt64()));
        f.getParentDirectory().createDirectory();
        f.replaceWithText(juce::String(value));
    }
    return cached = value;
}

void Controller::attach(AppContext& context)
{
    detach();
    context_ = &context;
    listener_ = context.session.add_listener([this](session::Change c) {
        if (c != session::Change::File || !context_) return;
        if (context_->session.document_generation() != generation_ || context_->session.path() != path_) opened();
    });
    opened();
}

void Controller::detach()
{
    if (context_ && listener_) context_->session.remove_listener(listener_);
    context_ = nullptr;
    listener_ = 0;
}

std::optional<shared::Link> Controller::link() const
{
    if (!context_ || context_->session.path().empty()) return std::nullopt;
    return shared::read_link(context_->session.path());
}

void Controller::opened()
{
    generation_ = context_->session.document_generation();
    path_ = context_->session.path();
    detail_ = {};
    if (link()) {
        context_->session.set_site(site());
        if (Account::get().signed_in()) refresh();
    }
    if (on_change) on_change();
}

juce::String Controller::summary() const
{
    const auto l = link();
    if (!l) return context_ && !context_->session.path().empty() ? "This project is not shared." : "Save the project to share it.";
    juce::String s = "\"" + juce::String(l->name) + "\" - you are " + juce::String(shared::role_label(l->role)) + ". ";
    s << "Synced with revision " << l->base;
    if (detail_.head > l->base) s << "; revision " << detail_.head << " is waiting: Sync to bring it in";
    else if (detail_.head > 0) s << " (the latest)";
    s << ".";
    return s;
}

void Controller::apply_locks()
{
    if (!context_) return;
    std::map<std::string, std::string> others;
    for (const auto& l : detail_.locks)
        if (l.user_id != detail_.me) others[l.track_id.toStdString()] = l.holder.toStdString();
    context_->session.set_track_locks(std::move(others));
}

void Controller::finish(Done& done, Outcome outcome)
{
    busy_ = false;
    if (context_ && outcome.message.isNotEmpty()) context_->status(outcome.message);
    if (on_change) on_change();
    if (done) done(outcome);
}

void Controller::list(std::function<void(std::vector<ProjectSummary>, Outcome)> done)
{
    background<Account::Reply>(
        alive_, [] { return Account::get().api("GET", "/api/collab/projects"); },
        [done = std::move(done)](Account::Reply r) {
            if (!r.ok()) return done({}, {false, r.error});
            auto projects = parse_list(r.body);
            done(projects, {true, plural(static_cast<int>(projects.size()), "shared project", "shared projects")});
        });
}

void Controller::refresh(Done done)
{
    const auto l = link();
    if (!l) {
        if (done) done({false, "This project is not shared"});
        return;
    }
    const juce::String id(l->project_id);
    background<DetailReply>(alive_, [id] { return fetch_detail(id); },
                            [this, done = std::move(done), path = path_](DetailReply d) mutable {
                                if (!context_ || context_->session.path() != path) return;
                                if (!d.detail) {
                                    if (done) done({false, d.error});
                                    return;
                                }
                                detail_ = std::move(*d.detail);
                                apply_locks();
                                if (on_change) on_change();
                                if (done) done({true, {}});
                            });
}

void Controller::progress(const juce::String& text)
{
    juce::MessageManager::callAsync([this, alive = alive_, text] {
        if (*alive && context_) context_->status(text);
    });
}

void Controller::share(const juce::String& name, Done done)
{
    if (busy_ || !context_) return done({false, "Busy: another shared-project operation is running"});
    const auto path = context_->session.path();
    if (path.empty()) return done({false, "Save the project first: a shared project lives in a folder of its own"});
    if (link()) return done({false, "This project is already shared"});
    if (!Account::get().signed_in()) return done({false, "Sign in first (Help > Account and Licence...)"});
    busy_ = true;
    context_->session.set_site(site());
    background<Account::Reply>(
        alive_, [name] { return Account::get().api("POST", "/api/collab/projects", json::Object{{"name", name.toStdString()}}); },
        [this, done = std::move(done), path, name](Account::Reply r) mutable {
            if (!r.ok()) return finish(done, {false, "Not shared: " + r.error});
            // Revision 0 here; the first sync gathers the media and pushes revision 1.
            shared::Link l{str(r.body, "id").toStdString(), name.toStdString(), "owner", 0, site(), {}};
            std::string error;
            if (!shared::write_link(path, l, &error))
                return finish(done, {false, "Shared, but the sharing file could not be written: " + juce::String(error)});
            step_pull(
                [this, done = std::move(done), name](Outcome o) mutable {
                    if (o.ok) o.message = "Shared \"" + name + "\". " + o.message + " Invite people from My shared projects on vc-composer.com";
                    else o.message = "Shared, but not uploaded yet: " + o.message + ". Sync again to finish";
                    if (done) done(o);
                },
                0);
        });
}

void Controller::open_shared(const ProjectSummary& project, const juce::File& folder,
                             std::function<bool(const juce::File&)> open, Done done)
{
    if (busy_) return done({false, "Busy: another shared-project operation is running"});
    if (project.head < 1) return done({false, "Nothing has been pushed to \"" + project.name + "\" yet"});
    const auto safe = juce::File::createLegalFileName(project.name).trim();
    const auto dir = folder.getChildFile(safe.isEmpty() ? juce::String("Shared Project") : safe);
    const auto file = dir.getChildFile(dir.getFileName() + ".vcc2");
    if (file.existsAsFile() && !shared::read_link(file.getFullPathName().toStdString()))
        return done({false, file.getFullPathName() + " already exists and is not this shared project: choose another folder"});
    busy_ = true;
    struct Opened {
        juce::String error;
        juce::StringArray failed;
        int fetched = 0;
    };
    const auto p = file.getFullPathName().toStdString();
    background<Opened>(
        alive_,
        [this, project, dir, p] {
            Opened o;
            const auto f = fetch_revision(project.id, project.head);
            if (!f.ok) {
                o.error = f.error;
                return o;
            }
            dir.createDirectory();
            std::string error;
            shared::Link l{project.id.toStdString(), project.name.toStdString(), project.role.toStdString(), project.head, site(), f.media};
            if (!files::write_file_atomic(p, f.text, &error) || !shared::write_base(p, f.text, &error) ||
                !shared::write_link(p, l, &error)) {
                o.error = "Could not write " + juce::String(p) + ": " + juce::String(error);
                return o;
            }
            // Its audio and picture, before it opens.
            const auto doc = project::Project::from_json(*json::parse(f.text));
            if (doc) {
                shared::HashCache cache(p);
                const auto fetch = shared::to_fetch(doc->data(), f.media, p, cache);
                o.fetched = static_cast<int>(fetch.size());
                o.failed = download_media(project.id, fetch, p, cache, [this](const juce::String& t) { progress(t); });
            }
            return o;
        },
        [this, project, file, open = std::move(open), done = std::move(done)](Opened o) mutable {
            if (o.error.isNotEmpty()) return finish(done, {false, o.error});
            busy_ = false;
            if (!open(file)) return finish(done, {false, "Downloaded to " + file.getFullPathName() + "; not opened"});
            juce::String m = "Opened \"" + project.name + "\" (revision " + juce::String(project.head) + ") from " +
                             file.getParentDirectory().getFullPathName();
            if (o.fetched > 0) m << " with " << plural(o.fetched - o.failed.size(), "media file", "media files");
            if (!o.failed.isEmpty()) m << "; " << o.failed.size() << " could not be downloaded (Sync tries again): " << o.failed[0];
            finish(done, {o.failed.isEmpty(), m});
        });
}

void Controller::sync(Done done)
{
    if (busy_ || !context_) return done({false, "Busy: another shared-project operation is running"});
    if (!link()) return done({false, "This project is not shared: File > Share Project... first"});
    busy_ = true;
    step_pull(std::move(done), 0);
}

/// Pull: the server's head, if newer, merged in. First, this copy's media is
/// gathered into the project folder, and a file of mine that clashes with
/// theirs (same path, other content) is kept apart under a new name.
void Controller::step_pull(Done done, int attempt)
{
    const auto l = *link();
    const auto path = path_;
    const juce::String id(l.project_id);
    auto data = std::make_shared<project::ProjectData>(context_->session.data());
    struct Pulled {
        DetailReply detail;
        std::optional<Fetched> head;
        std::map<std::string, std::string> relinks;  ///< Gathered and kept apart: stored -> file.
        juce::String error;
    };
    background<Pulled>(
        alive_,
        [id, l, path, data] {
            Pulled p;
            p.detail = fetch_detail(id);
            if (!p.detail.detail) return p;
            if (p.detail.detail->head > l.base) p.head = fetch_revision(id, p.detail.detail->head);
            // Gather what lies outside the project folder.
            const auto copies = shared::plan_gather(*data, path);
            std::string error;
            if (!shared::run_copies(copies, &error)) {
                p.error = juce::String(error);
                return p;
            }
            p.relinks = shared::relinks_of(copies);
            if (!p.relinks.empty()) session::relink::apply(*data, p.relinks, path);
            if (p.head && p.head->ok) {
                shared::HashCache cache(path);
                const auto ours = shared::list_media(*data, path, cache, l.base_media);
                cache.save();
                const auto apart = shared::separate_clashes(ours.manifest, l.base_media, p.head->media, path, l.site, &error);
                if (!error.empty()) p.error = juce::String(error);
                for (const auto& [stored, file] : apart) p.relinks[stored] = file;
            }
            return p;
        },
        [this, done = std::move(done), attempt, path](Pulled p) mutable {
            if (!context_ || context_->session.path() != path) return finish(done, {false, "Sync stopped: another project was opened"});
            if (!p.detail.detail) return finish(done, {false, "Not synced: " + p.detail.error});
            if (p.error.isNotEmpty()) return finish(done, {false, "Not synced: " + p.error});
            detail_ = std::move(*p.detail.detail);
            apply_locks();
            if (!p.relinks.empty())
                context_->session.edit("Gather media for sharing", [&](project::ProjectData& d) {
                    return session::relink::apply(d, p.relinks, path) > 0;
                });
            auto current = *link();
            current.role = detail_.role.toStdString();
            if (!p.head) {
                shared::write_link(path, current);
                return step_fetch(std::move(done), attempt, {});
            }
            if (!p.head->ok) return finish(done, {false, "Not synced: " + p.head->error});
            const auto theirs = json::parse(p.head->text);
            if (!theirs) return finish(done, {false, "Not synced: revision " + juce::String(detail_.head) + " is not a project file"});
            auto result = std::make_shared<project::MergeResult>(
                shared::pull(shared::read_base(path), context_->session.project().to_json(), *theirs));
            auto adopt = [this, result, l = current, text = p.head->text, media = p.head->media, head = detail_.head, done, attempt,
                          path](std::optional<std::vector<bool>> choice) mutable {
                if (!choice) return finish(done, {false, "Sync cancelled: nothing changed"});
                if (!context_ || context_->session.path() != path) return finish(done, {false, "Sync stopped: another project was opened"});
                const auto doc = result->clean() ? result->merged : result->resolve(*choice);
                std::string error;
                if (!context_->session.adopt_document(doc, "Sync with " + l.name, &error))
                    return finish(done, {false, "Not synced: " + juce::String(error)});
                auto link = l;
                link.base = head;
                link.base_media = media;
                // The new base and the project that includes it are written together.
                if (!shared::write_base(path, text, &error) || !shared::write_link(path, link, &error) ||
                    !context_->session.save(&error))
                    return finish(done, {false, "Synced, but not saved: " + juce::String(error)});
                const int asked = static_cast<int>(result->conflicts.size());
                step_fetch(std::move(done), attempt,
                           "Brought in revision " + juce::String(head) +
                               (asked > 0 ? " (" + plural(asked, "conflict", "conflicts") + " decided)" : juce::String()));
            };
            if (result->clean()) return adopt(std::vector<bool>{});
            if (!ask_conflicts) return adopt(std::vector<bool>(result->conflicts.size(), false));
            ask_conflicts(*result, std::move(adopt));
        });
}

/// Fetch: the media the project uses that this computer lacks (theirs, new
/// or changed; or an earlier download that failed).
void Controller::step_fetch(Done done, int attempt, juce::String pulled)
{
    const auto l = *link();
    const auto path = path_;
    auto data = std::make_shared<project::ProjectData>(context_->session.data());
    struct Got {
        int wanted = 0;
        juce::StringArray failed;
    };
    background<Got>(
        alive_,
        [this, l, path, data] {
            Got g;
            shared::HashCache cache(path);
            const auto fetch = shared::to_fetch(*data, l.base_media, path, cache);
            g.wanted = static_cast<int>(fetch.size());
            g.failed = fetch.empty() ? juce::StringArray() : download_media(juce::String(l.project_id), fetch, path, cache,
                                                                            [this](const juce::String& t) { progress(t); });
            cache.save();
            return g;
        },
        [this, done = std::move(done), attempt, pulled, path](Got g) mutable {
            if (!context_ || context_->session.path() != path) return finish(done, {false, "Sync stopped: another project was opened"});
            if (g.wanted > 0) {
                pulled << (pulled.isEmpty() ? "" : "; ") << plural(g.wanted - g.failed.size(), "media file", "media files") << " downloaded";
                if (!g.failed.isEmpty()) pulled << ", " << g.failed.size() << " not (Sync tries again): " << g.failed[0];
            }
            if (pulled.isNotEmpty()) context_->status(pulled);
            step_push(std::move(done), attempt, pulled);
        });
}

/// Push: this copy's project and media, if anything changed since the base
/// and this role writes. Refused because somebody pushed meanwhile: pull again.
void Controller::step_push(Done done, int attempt, juce::String pulled)
{
    const auto l = *link();
    const auto path = path_;
    const auto doc = context_->session.project().to_json();
    const auto text = shared::document_text(doc);
    const auto base_doc = shared::read_base(path);
    const bool doc_changed = !base_doc || shared::changed_since(*base_doc, doc);
    const auto message = juce::String(context_->session.project().undo_label());
    auto data = std::make_shared<project::ProjectData>(context_->session.data());
    struct Sent {
        bool tried = false;
        Pushed pushed;
        shared::Manifest manifest;
        juce::String notes, error;
    };
    background<Sent>(
        alive_,
        [this, l, path, text, doc_changed, message, data] {
            Sent s;
            shared::HashCache cache(path);
            const auto listed = shared::list_media(*data, path, cache, l.base_media,
                                                   [](std::uint64_t) { return true; });
            cache.save();
            s.manifest = listed.manifest;
            s.notes = media_notes(listed);
            if (!doc_changed && shared::same_media(s.manifest, l.base_media)) return s;
            if (!shared::can_push(l.role)) return s;
            s.tried = true;
            s.error = upload_media(juce::String(l.project_id), s.manifest, path, [this](const juce::String& t) { progress(t); });
            if (s.error.isNotEmpty()) return s;
            s.pushed = push_revision(juce::String(l.project_id), l.base, text, message, site(), s.manifest);
            return s;
        },
        [this, l, text, done = std::move(done), attempt, pulled, path](Sent s) mutable {
            if (!context_ || context_->session.path() != path) return finish(done, {false, "Sync stopped: another project was opened"});
            const auto lead = pulled.isEmpty() ? juce::String() : pulled + ". ";
            if (!s.tried) {
                if (!shared::can_push(l.role) && (l.base == 0 || shared::changed_since(*shared::read_base(path), context_->session.project().to_json())))
                    return finish(done, {true, lead + "Your changes stay on this computer: a " +
                                                   juce::String(shared::role_label(l.role)).toLowerCase() + " does not push." + s.notes});
                return finish(done, {true, lead + "In sync with revision " + juce::String(l.base) + " of \"" + juce::String(l.name) + "\"." + s.notes});
            }
            if (s.error.isNotEmpty()) return finish(done, {false, lead + "Not pushed: " + s.error});
            if (!s.pushed.accepted && s.pushed.head >= 0) {
                // Somebody pushed while this was merging: pull their work too.
                if (attempt >= 3) return finish(done, {false, "Not pushed: others keep pushing; Sync again in a moment"});
                return step_pull(std::move(done), attempt + 1);
            }
            if (!s.pushed.accepted) return finish(done, {false, lead + "Not pushed: " + s.pushed.error});
            auto link = l;
            link.base = s.pushed.revision;
            link.base_media = s.manifest;
            std::string error;
            if (!shared::write_base(path, text, &error) || !shared::write_link(path, link, &error))
                return finish(done, {false, "Pushed, but the sharing file could not be written: " + juce::String(error)});
            if (context_->session.dirty()) context_->session.save(&error);
            detail_.head = s.pushed.revision;
            refresh();
            finish(done, {true, lead + "Synced: your changes are revision " + juce::String(s.pushed.revision) + " of \"" +
                                    juce::String(l.name) + "\" (" + plural(static_cast<int>(s.manifest.size()), "media file", "media files") +
                                    ")." + s.notes});
        });
}

void Controller::set_lock(const std::string& track_id, bool lock, Done done)
{
    const auto l = link();
    if (!l) return done({false, "This project is not shared"});
    if (!shared::can_push(l->role)) return done({false, "A " + juce::String(shared::role_label(l->role)).toLowerCase() + " does not lock parts"});
    const juce::String id(l->project_id);
    const juce::String track(track_id);
    background<Account::Reply>(
        alive_,
        [id, track, lock] {
            return Account::get().api(lock ? "POST" : "DELETE", "/api/collab/projects/" + id + "/locks",
                                      json::Object{{"trackId", track.toStdString()}});
        },
        [this, done = std::move(done), lock](Account::Reply r) mutable {
            if (!r.ok()) {
                if (done) done({false, (lock ? "Not locked: " : "Not unlocked: ") + r.error});
                return;
            }
            refresh([done = std::move(done), lock](Outcome) {
                if (done) done({true, lock ? "Locked for writing: nobody else can change this part until you unlock it (or for two hours)"
                                           : "Unlocked: others can write this part again"});
            });
        });
}

}  // namespace vcc::desktop::collab
