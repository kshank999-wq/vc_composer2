#include "vcc/library/Library.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <system_error>

namespace vcc::library {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool alnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

/// `pattern` appears in `text` as whole words ("play" is not in "display").
bool has_words(const std::string& text, const std::string& pattern)
{
    if (pattern.empty()) return false;
    const std::string t = lower(text), p = lower(pattern);
    for (std::size_t at = t.find(p); at != std::string::npos; at = t.find(p, at + 1)) {
        const bool start = at == 0 || !alnum(t[at - 1]);
        const std::size_t end = at + p.size();
        const bool finish = end >= t.size() || !alnum(t[end]);
        if (start && finish) return true;
    }
    return false;
}

std::vector<std::string> words(const std::string& text)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : lower(text)) {
        if (alnum(c)) cur += c;
        else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

json::Value strings(const std::vector<std::string>& v)
{
    json::Value a = json::Value::make_array();
    for (const auto& s : v) a.push_back(s);
    return a;
}

json::Value plugin_json(const project::PluginRef& p, bool with_state)
{
    json::Value o = json::Value::make_object();
    o.set("format", p.format);
    o.set("identifier", p.identifier);
    o.set("name", p.name);
    o.set("vendor", p.vendor);
    if (with_state && !p.state_base64.empty()) o.set("state", p.state_base64);
    return o;
}

project::PluginRef plugin_from(const json::Value& o)
{
    project::PluginRef p;
    p.format = o.get_string("format");
    p.identifier = o.get_string("identifier");
    p.name = o.get_string("name");
    p.vendor = o.get_string("vendor");
    p.state_base64 = o.get_string("state");
    return p;
}

json::Value patch_json(const UserPatch& p, bool with_state)
{
    json::Value o = json::Value::make_object();
    o.set("id", p.id);
    o.set("display", p.display);
    o.set("provider", p.provider);
    o.set("library", p.library);
    o.set("player", p.player);
    o.set("plugin", plugin_json(p.plugin, with_state));
    o.set("instrument", p.instrument_id);
    o.set("articulations", strings(p.articulations));
    o.set("expression_map", p.expression_map);
    o.set("validation", p.validation);
    o.set("tags", strings(p.tags));
    return o;
}

UserPatch patch_from(const json::Value& o)
{
    UserPatch p;
    p.id = o.get_string("id");
    p.display = o.get_string("display");
    p.provider = o.get_string("provider");
    p.library = o.get_string("library");
    p.player = o.get_string("player");
    if (const auto* pl = o.get("plugin")) p.plugin = plugin_from(*pl);
    p.instrument_id = o.get_string("instrument");
    p.articulations = o.get_string_array("articulations");
    p.expression_map = o.get_string("expression_map", "default");
    p.validation = o.get_string("validation", "user");
    p.tags = o.get_string_array("tags");
    return p;
}

int coverage(const std::vector<std::string>& have, const std::vector<std::string>& want, std::vector<std::string>& missing)
{
    int n = 0;
    for (const auto& a : want) {
        if (std::find(have.begin(), have.end(), a) != have.end()) ++n;
        else missing.push_back(a);
    }
    return n;
}

}  // namespace

std::string status_text(const Entry& e)
{
    std::string s = e.player_installed ? "Player installed" : "Player not installed";
    if (e.kind == EntryKind::CatalogPatch)
        s += e.library_detected ? " - library detected" : " - library not detected";
    s += e.metadata_known ? " - metadata known" : " - metadata unknown";
    return s;
}

const char* to_string(Match m) noexcept
{
    switch (m) {
        case Match::Exact: return "exact";
        case Match::Fallback: return "fallback";
        case Match::Inferred: return "inferred";
    }
    return "inferred";
}

// --- Catalog ------------------------------------------------------------------------------

bool Library::load_catalog(const json::Value& doc, std::string* error)
{
    if (!doc.is_object() || !doc.get("players") || !doc.get("libraries")) {
        if (error) *error = "not a library catalog (players and libraries expected)";
        return false;
    }
    for (const auto& p : doc["players"].as_array()) {
        Player pl;
        pl.id = p.get_string("id");
        pl.name = p.get_string("name");
        pl.vendor = p.get_string("vendor");
        pl.plugin_names = p.get_string_array("plugin_names");
        if (!pl.id.empty()) players_.push_back(std::move(pl));
    }
    for (const auto& l : doc["libraries"].as_array()) {
        CatalogLibrary lib;
        lib.id = l.get_string("id");
        lib.name = l.get_string("name");
        lib.provider = l.get_string("provider");
        lib.player_id = l.get_string("player");
        lib.plugin_names = l.get_string_array("plugin_names");
        for (auto& f : l.get_string_array("folder_names")) lib.folder_names.push_back(lower(f));
        for (const auto& p : l["patches"].as_array()) {
            CatalogPatch cp;
            cp.name = p.get_string("name");
            cp.instrument_id = p.get_string("instrument");
            cp.articulations = p.get_string_array("articulations");
            if (instruments_->find(cp.instrument_id) == nullptr) {
                if (error) *error = lib.id + "/" + cp.name + ": unknown instrument " + cp.instrument_id;
                return false;
            }
            lib.patches.push_back(std::move(cp));
        }
        if (!lib.id.empty()) libraries_.push_back(std::move(lib));
    }
    return true;
}

// --- What is here ---------------------------------------------------------------------------

const InstalledPlugin* Library::plugin_for_player(const std::string& player_id) const
{
    const auto it = std::find_if(players_.begin(), players_.end(), [&](const Player& p) { return p.id == player_id; });
    if (it == players_.end()) return nullptr;
    for (const auto& ip : installed_)
        for (const auto& n : it->plugin_names)
            if (has_words(ip.name, n)) return &ip;
    return nullptr;
}

const CatalogLibrary* Library::library_of_plugin(const InstalledPlugin& p) const
{
    for (const auto& lib : libraries_)
        for (const auto& n : lib.plugin_names)
            if (has_words(p.name, n)) return &lib;
    return nullptr;
}

std::vector<std::string> Library::detect()
{
    std::vector<std::string> found;
    for (const auto& folder : folders_) {
        std::error_code ec;
        fs::recursive_directory_iterator it(fs::u8path(folder), fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_directory(e2)) continue;
            if (it.depth() >= 2) it.disable_recursion_pending();
            const std::string name = lower(it->path().filename().u8string());
            for (const auto& lib : libraries_)
                for (const auto& f : lib.folder_names)
                    if (!f.empty() && name.find(f) != std::string::npos && !detected_.count(lib.id)) {
                        detected_.insert(lib.id);
                        found.push_back(lib.id);
                    }
        }
    }
    return found;
}

// --- Saved patches --------------------------------------------------------------------------

std::string Library::add_patch(UserPatch patch)
{
    while (patch.id.empty() || find_patch(patch.id)) patch.id = "p" + std::to_string(next_id_++);
    if (patch.display.empty()) {
        const auto* def = instruments_->find(patch.instrument_id);
        patch.display = (def ? def->name : std::string("Patch")) + (patch.plugin.name.empty() ? "" : " (" + patch.plugin.name + ")");
    }
    patches_.push_back(std::move(patch));
    return patches_.back().id;
}

bool Library::update_patch(const UserPatch& patch)
{
    for (auto& p : patches_)
        if (p.id == patch.id) {
            p = patch;
            return true;
        }
    return false;
}

bool Library::remove_patch(const std::string& id)
{
    const auto it = std::find_if(patches_.begin(), patches_.end(), [&](const UserPatch& p) { return p.id == id; });
    if (it == patches_.end()) return false;
    favorites_.erase("patch:" + id);
    patches_.erase(it);
    return true;
}

const UserPatch* Library::find_patch(const std::string& id) const
{
    for (const auto& p : patches_)
        if (p.id == id) return &p;
    return nullptr;
}

void Library::set_favorite(const std::string& key, bool on)
{
    if (on) favorites_.insert(key);
    else favorites_.erase(key);
}

void Library::set_tags(const std::string& key, std::vector<std::string> tags)
{
    for (auto& [k, t] : tags_)
        if (k == key) {
            t = std::move(tags);
            return;
        }
    tags_.emplace_back(key, std::move(tags));
}

json::Value Library::user_json() const
{
    json::Value o = json::Value::make_object();
    o.set("schema_version", 1);
    json::Value ps = json::Value::make_array();
    for (const auto& p : patches_) ps.push_back(patch_json(p, true));
    o.set("patches", std::move(ps));
    o.set("favorites", strings(std::vector<std::string>(favorites_.begin(), favorites_.end())));
    json::Value tags = json::Value::make_object();
    for (const auto& [k, t] : tags_) tags.set(k, strings(t));
    o.set("tags", std::move(tags));
    o.set("folders", strings(folders_));
    o.set("next_id", next_id_);
    return o;
}

void Library::load_user(const json::Value& doc)
{
    patches_.clear();
    favorites_.clear();
    tags_.clear();
    for (const auto& p : doc["patches"].as_array()) patches_.push_back(patch_from(p));
    for (const auto& f : doc.get_string_array("favorites")) favorites_.insert(f);
    if (const auto* t = doc.get("tags"))
        for (const auto& k : t->keys()) tags_.emplace_back(k, (*t).get_string_array(k));
    folders_ = doc.get_string_array("folders");
    next_id_ = static_cast<int>(doc.get_int("next_id", 1));
    for (const auto& p : patches_)
        if (p.id.size() > 1 && p.id[0] == 'p') next_id_ = std::max(next_id_, std::atoi(p.id.c_str() + 1) + 1);
}

// --- Browsing ---------------------------------------------------------------------------------

std::string Library::family_of(const std::string& instrument_id) const
{
    const auto* def = instruments_->find(instrument_id);
    return def ? def->family : std::string();
}

std::string Library::infer_instrument(const std::string& plugin_name) const
{
    // A whole instrument name in the plug-in's name ("Solo Cello"), the
    // longest first; else a word of the name beginning with one ("Pianoteq").
    std::string best;
    std::size_t best_len = 0;
    for (const auto& def : instruments_->all())
        if (def.name.size() > best_len && has_words(plugin_name, def.name)) {
            best = def.id;
            best_len = def.name.size();
        }
    if (!best.empty()) return best;
    // The instrument's leading noun ("Horn" of "Horn in F"), unless it is
    // a qualifier ("English Horn", "Bass Clarinet").
    static const std::set<std::string> qualifiers{"english", "bass", "alto", "tenor", "soprano", "baritone", "electric",
                                                  "acoustic", "solo", "low", "high", "muted", "natural", "prepared",
                                                  "felt", "upright", "chamber", "double", "piccolo", "contra", "string",
                                                  "brass", "hybrid", "ambient", "sample", "cinematic", "metallic",
                                                  "ticking", "military", "suspended", "tubular", "temple", "frame",
                                                  "drum", "twelve", "resonator", "nylon", "fretless", "boys", "chant",
                                                  "backing", "tin", "wagner", "sub", "noise", "analog", "processed"};
    for (const auto& def : instruments_->all()) {
        const auto ws = words(def.name);
        if (ws.size() < 2 || ws[0].size() < 4 || qualifiers.count(ws[0])) continue;
        if (has_words(plugin_name, ws[0])) return def.id;
    }
    const auto ws = words(plugin_name);
    for (const auto& def : instruments_->all()) {
        const std::string n = lower(def.name);
        if (n.size() < 5 || n.find(' ') != std::string::npos) continue;
        for (const auto& w : ws)
            if (w.size() > n.size() && w.compare(0, n.size(), n) == 0) return def.id;
    }
    return {};
}

std::vector<Entry> Library::entries() const
{
    std::vector<Entry> out;
    auto tags_of = [&](const std::string& key) {
        for (const auto& [k, t] : tags_)
            if (k == key) return t;
        return std::vector<std::string>{};
    };
    auto installed_by = [&](const project::PluginRef& ref) -> const InstalledPlugin* {
        for (const auto& ip : installed_)
            if (ip.identifier == ref.identifier) return &ip;
        for (const auto& ip : installed_)
            if (!ref.name.empty() && ip.name == ref.name && ip.format == ref.format) return &ip;
        return nullptr;
    };

    for (const auto& p : patches_) {
        Entry e;
        e.kind = EntryKind::UserPatch;
        e.key = "patch:" + p.id;
        e.display = p.display;
        e.provider = p.provider.empty() ? p.plugin.vendor : p.provider;
        e.player = p.player.empty() ? p.plugin.name : p.player;
        e.library = p.library;
        e.instrument_id = p.instrument_id;
        e.family = family_of(p.instrument_id);
        e.articulations = p.articulations;
        e.tags = p.tags;
        for (auto& t : tags_of(e.key)) e.tags.push_back(t);
        const auto* ip = installed_by(p.plugin);
        e.player_installed = ip != nullptr;
        e.library_detected = true;
        e.metadata_known = true;
        e.plugin_identifier = ip ? ip->identifier : std::string();
        e.validation = p.validation;
        e.favorite = favorite(e.key);
        out.push_back(std::move(e));
    }

    std::set<std::string> library_plugins;
    for (const auto& lib : libraries_) {
        const InstalledPlugin* loader = nullptr;
        for (const auto& ip : installed_)
            for (const auto& n : lib.plugin_names)
                if (!loader && has_words(ip.name, n)) loader = &ip;
        if (!loader) loader = plugin_for_player(lib.player_id);
        if (loader) library_plugins.insert(loader->identifier);
        const bool own_plugin = !lib.plugin_names.empty() && loader &&
                                std::any_of(lib.plugin_names.begin(), lib.plugin_names.end(),
                                            [&](const std::string& n) { return has_words(loader->name, n); });
        const auto player = std::find_if(players_.begin(), players_.end(), [&](const Player& p) { return p.id == lib.player_id; });
        for (const auto& cp : lib.patches) {
            Entry e;
            e.kind = EntryKind::CatalogPatch;
            e.key = "catalog:" + lib.id + "/" + cp.name;
            e.display = cp.name + " (" + lib.name + ")";
            e.provider = lib.provider;
            e.player = player != players_.end() ? player->name : lib.player_id;
            e.library = lib.name;
            e.instrument_id = cp.instrument_id;
            e.family = family_of(cp.instrument_id);
            e.articulations = cp.articulations;
            e.tags = tags_of(e.key);
            e.player_installed = loader != nullptr;
            e.library_detected = own_plugin || detected(lib.id);
            e.metadata_known = true;
            e.plugin_identifier = loader ? loader->identifier : std::string();
            e.validation = "inferred";
            e.favorite = favorite(e.key);
            out.push_back(std::move(e));
        }
    }

    for (const auto& ip : installed_) {
        Entry e;
        e.kind = EntryKind::Plugin;
        e.key = "plugin:" + ip.identifier;
        e.display = ip.name;
        e.provider = ip.vendor;
        e.player = ip.name;
        if (const auto* lib = library_of_plugin(ip)) e.library = lib->name;
        e.instrument_id = library_plugins.count(ip.identifier) ? std::string() : infer_instrument(ip.name);
        e.family = family_of(e.instrument_id);
        e.tags = tags_of(e.key);
        e.player_installed = true;
        e.library_detected = false;
        e.metadata_known = false;
        e.plugin_identifier = ip.identifier;
        e.validation = e.instrument_id.empty() ? "unknown" : "inferred";
        e.favorite = favorite(e.key);
        out.push_back(std::move(e));
    }
    return out;
}

std::optional<Entry> Library::find(const std::string& key) const
{
    for (auto& e : entries())
        if (e.key == key) return e;
    return std::nullopt;
}

std::vector<Entry> Library::search(const Filter& f) const
{
    std::vector<Entry> out;
    const auto query = words(f.query);
    for (auto& e : entries()) {
        if (!f.provider.empty() && lower(e.provider) != lower(f.provider)) continue;
        if (!f.player.empty() && lower(e.player) != lower(f.player)) continue;
        if (!f.family.empty() && e.family != f.family) continue;
        if (!f.instrument_id.empty() && e.instrument_id != f.instrument_id) continue;
        if (!f.technique.empty() &&
            std::find(e.articulations.begin(), e.articulations.end(), f.technique) == e.articulations.end())
            continue;
        if (!f.tag.empty() && std::find(e.tags.begin(), e.tags.end(), f.tag) == e.tags.end()) continue;
        if (f.installed_only && !e.player_installed) continue;
        if (f.favorites_only && !e.favorite) continue;
        if (!query.empty()) {
            std::string hay = e.display + " " + e.provider + " " + e.player + " " + e.library + " " + e.instrument_id +
                              " " + e.family;
            if (const auto* def = instruments_->find(e.instrument_id)) hay += " " + def->name;
            for (const auto& t : e.tags) hay += " " + t;
            for (const auto& a : e.articulations) hay += " " + a;
            hay = lower(hay);
            if (!std::all_of(query.begin(), query.end(), [&](const std::string& w) { return hay.find(w) != std::string::npos; }))
                continue;
        }
        out.push_back(std::move(e));
    }
    std::stable_sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        if (a.favorite != b.favorite) return a.favorite;
        if (a.player_installed != b.player_installed) return a.player_installed;
        return static_cast<int>(a.kind) < static_cast<int>(b.kind);
    });
    return out;
}

std::vector<Candidate> Library::candidates(const std::string& instrument_id, const std::vector<std::string>& artics,
                                           const std::vector<std::string>& fallbacks, std::size_t limit) const
{
    std::vector<Candidate> out;
    const auto all = entries();
    const auto* def = instruments_->find(instrument_id);
    const std::string name = def ? def->name : instrument_id;
    auto consider = [&](const std::string& want, bool fallback) {
        for (const auto& e : all) {
            if (e.instrument_id != want || !e.player_installed) continue;
            Candidate c;
            c.entry = e;
            const int covered = coverage(e.articulations, artics, c.missing_articulations);
            int score = covered * 10 + (e.favorite ? 500 : 0);
            if (e.kind == EntryKind::UserPatch) {
                score += 1000 + (e.validation == "verified" ? 200 : 0);
                c.match = fallback ? Match::Fallback : Match::Exact;
                c.reason = std::string(fallback ? "Saved patch for approved fallback " : "Saved patch for ") +
                           (instruments_->find(want) ? instruments_->find(want)->name : want) +
                           (e.validation == "verified" ? "" : " (not verified yet)");
            } else if (e.kind == EntryKind::CatalogPatch) {
                score += e.library_detected ? 600 : 300;
                c.match = fallback ? Match::Fallback : Match::Inferred;
                c.reason = e.library + (e.library_detected ? " is detected" : " may be installed (its player is)") +
                           "; the catalog lists this section - load it in " + e.player + " and verify";
            } else {
                score += 200;
                c.match = Match::Inferred;
                c.reason = "The plug-in's name suggests " + name + ": audition it to verify";
            }
            if (fallback) score -= 300;
            if (artics.empty() || c.missing_articulations.empty()) score += 5;
            c.score = score;
            out.push_back(std::move(c));
        }
    };
    consider(instrument_id, false);
    for (const auto& fb : fallbacks)
        if (fb != instrument_id) consider(fb, true);
    std::stable_sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    if (out.size() > limit) out.resize(limit);
    return out;
}

std::vector<templates::InstalledPatch> Library::installed_patches() const
{
    std::vector<templates::InstalledPatch> out;
    for (const auto& e : entries()) {
        if (e.kind != EntryKind::UserPatch || !e.player_installed) continue;
        templates::InstalledPatch p;
        p.patch_id = e.key.substr(6);
        p.display = e.display;
        p.instrument_standard_id = e.instrument_id;
        p.articulations = e.articulations;
        p.favorite = e.favorite;
        p.validation_status = e.validation;
        if (const auto* up = find_patch(p.patch_id)) {
            p.plugin = up->plugin;
            p.expression_map = up->expression_map;
        }
        out.push_back(std::move(p));
    }
    return out;
}

// --- Mapping packs ------------------------------------------------------------------------------

json::Value Library::export_pack(const std::string& name, const std::vector<std::string>& ids, bool with_states) const
{
    json::Value o = json::Value::make_object();
    o.set("vcc_mapping_pack", 1);
    o.set("name", name);
    json::Value ps = json::Value::make_array();
    for (const auto& p : patches_)
        if (ids.empty() || std::find(ids.begin(), ids.end(), p.id) != ids.end()) ps.push_back(patch_json(p, with_states));
    o.set("patches", std::move(ps));
    return o;
}

int Library::import_pack(const json::Value& doc, std::vector<std::string>* warnings)
{
    if (doc.get_int("vcc_mapping_pack", 0) < 1) {
        if (warnings) warnings->push_back("not a VC Composer mapping pack");
        return 0;
    }
    int n = 0;
    for (const auto& j : doc["patches"].as_array()) {
        UserPatch p = patch_from(j);
        if (!p.instrument_id.empty() && instruments_->find(p.instrument_id) == nullptr) {
            if (warnings) warnings->push_back(p.display + ": unknown instrument " + p.instrument_id + " (skipped)");
            continue;
        }
        auto same = std::find_if(patches_.begin(), patches_.end(), [&](const UserPatch& q) {
            return q.plugin.identifier == p.plugin.identifier && q.instrument_id == p.instrument_id && q.display == p.display;
        });
        if (same != patches_.end()) {
            p.id = same->id;
            if (p.plugin.state_base64.empty()) p.plugin.state_base64 = same->plugin.state_base64;
            *same = std::move(p);
        } else {
            p.id.clear();
            add_patch(std::move(p));
        }
        ++n;
    }
    return n;
}

}  // namespace vcc::library
