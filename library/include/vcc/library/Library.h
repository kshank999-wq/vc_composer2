// Library browser and discovery (Main Spec §10, §11): what the composer can
// play the score with, found only through supported means.
//
//   - Players and libraries come from a curated catalog
//     (content/library/providers.json): which plug-in loads a library and
//     which instruments and techniques it covers. It holds no samples and no
//     preset files.
//   - What is installed comes from the plug-in scan (each instrument
//     plug-in's name, vendor, format and identifier) and from library folders
//     the user points at (their folder names are matched).
//   - What the composer has confirmed is a saved patch: a plug-in with its
//     state (the sound as set up in the player), the instrument it plays and
//     the techniques its articulation map covers. Saved patches feed the
//     template resolver, so templates build with the composer's own sounds.
//
// Every entry says which of three things is true - player installed,
// library detected, metadata known - so "Kontakt is here" is never mistaken
// for "this library is here", nor "this library is here" for "we know what
// is in it". Ranking for a template track says exact, fallback or inferred
// (needs verification); nothing is substituted silently.
#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "vcc/core/Json.h"
#include "vcc/project/Project.h"
#include "vcc/templates/Templates.h"

namespace vcc::library {

struct Player {
    std::string id, name, vendor;
    std::vector<std::string> plugin_names;  ///< Lower-case parts of the plug-in's name.
};

struct CatalogPatch {
    std::string name;
    std::string instrument_id;
    std::vector<std::string> articulations;
};

struct CatalogLibrary {
    std::string id, name, provider, player_id;
    std::vector<std::string> plugin_names;  ///< Libraries with a plug-in of their own.
    std::vector<std::string> folder_names;  ///< Lower case, matched against folders on disk.
    std::vector<CatalogPatch> patches;
};

/// An instrument plug-in the scan found.
struct InstalledPlugin {
    std::string name, vendor, format, identifier;
};

/// A sound the composer saved: the plug-in with its state, and what it is.
struct UserPatch {
    std::string id;
    std::string display;  ///< "Horns a4 - Marcato (BBC SO)".
    std::string provider, library, player;
    project::PluginRef plugin;  ///< With the state as set up.
    std::string instrument_id;
    std::vector<std::string> articulations;
    std::string expression_map = "default";
    /// "verified" (the composer confirmed it), "user" (saved, not checked)
    /// or "inferred" (proposed by VC Composer).
    std::string validation = "user";
    std::vector<std::string> tags;
};

enum class EntryKind { UserPatch, CatalogPatch, Plugin };

struct Entry {
    EntryKind kind = EntryKind::Plugin;
    std::string key;  ///< "patch:<id>", "catalog:<library>/<patch>", "plugin:<identifier>".
    std::string display, provider, player, library;
    std::string instrument_id;  ///< Empty when unknown.
    std::string family;
    std::vector<std::string> articulations, tags;
    bool player_installed = false;
    bool library_detected = false;
    bool metadata_known = false;
    bool favorite = false;
    /// The plug-in that loads it (empty when the player is not installed).
    std::string plugin_identifier;
    std::string validation;  ///< As UserPatch, or "inferred" / "unknown".
};

/// "Player installed - library detected - metadata known".
std::string status_text(const Entry& e);

struct Filter {
    std::string query;  ///< Words in name, provider, player, library, instrument, tags.
    std::string provider, player, family, instrument_id, technique, tag;
    bool installed_only = false;  ///< Something here can load it now.
    bool favorites_only = false;
};

enum class Match { Exact, Fallback, Inferred };
const char* to_string(Match m) noexcept;

struct Candidate {
    Entry entry;
    Match match = Match::Inferred;
    int score = 0;
    std::string reason;
    std::vector<std::string> missing_articulations;
};

class Library {
public:
    explicit Library(const templates::InstrumentRegistry& instruments) : instruments_(&instruments) {}

    // --- Catalog (shipped) ----------------------------------------------------------
    bool load_catalog(const json::Value& doc, std::string* error = nullptr);
    const std::vector<Player>& players() const noexcept { return players_; }
    const std::vector<CatalogLibrary>& libraries() const noexcept { return libraries_; }

    // --- What is here ---------------------------------------------------------------
    void set_installed(std::vector<InstalledPlugin> plugins) { installed_ = std::move(plugins); }
    const std::vector<InstalledPlugin>& installed() const noexcept { return installed_; }
    /// The plug-in that loads a player, if installed.
    const InstalledPlugin* plugin_for_player(const std::string& player_id) const;
    std::vector<std::string>& library_folders() noexcept { return folders_; }
    const std::vector<std::string>& library_folders() const noexcept { return folders_; }
    /// Looks through the library folders (three levels deep) for the
    /// catalog's folder names. Returns the libraries detected.
    std::vector<std::string> detect();
    void set_detected(std::set<std::string> library_ids) { detected_ = std::move(library_ids); }
    bool detected(const std::string& library_id) const { return detected_.count(library_id) > 0; }

    // --- Saved patches, favorites, tags (the user's) --------------------------------------
    std::string add_patch(UserPatch patch);
    bool update_patch(const UserPatch& patch);
    bool remove_patch(const std::string& id);
    const UserPatch* find_patch(const std::string& id) const;
    const std::vector<UserPatch>& patches() const noexcept { return patches_; }
    void set_favorite(const std::string& key, bool on);
    bool favorite(const std::string& key) const { return favorites_.count(key) > 0; }
    void set_tags(const std::string& key, std::vector<std::string> tags);

    json::Value user_json() const;
    void load_user(const json::Value& doc);

    // --- Browsing ---------------------------------------------------------------------
    std::vector<Entry> entries() const;
    std::vector<Entry> search(const Filter& f) const;
    std::optional<Entry> find(const std::string& key) const;

    /// Candidates for a track (its instrument, techniques and approved
    /// fallbacks), best first: saved patches for the instrument, then
    /// catalog patches a player here can load, then fallbacks, then plug-ins
    /// whose name suggests the instrument (inferred: please verify).
    std::vector<Candidate> candidates(const std::string& instrument_id, const std::vector<std::string>& articulations,
                                      const std::vector<std::string>& fallbacks, std::size_t limit = 20) const;

    /// Saved patches as the template resolver takes them.
    std::vector<templates::InstalledPatch> installed_patches() const;

    // --- Mapping packs (§10.8): saved patches to share and reload -----------------------------
    /// The patches (all when `ids` is empty) as a pack. Plug-in states are
    /// left out unless `with_states` (they can be large, and are the
    /// composer's settings).
    json::Value export_pack(const std::string& name, const std::vector<std::string>& ids = {},
                            bool with_states = false) const;
    /// Adds a pack's patches (new ids; a patch already here with the same
    /// plug-in, instrument and name is updated). Returns how many.
    int import_pack(const json::Value& doc, std::vector<std::string>* warnings = nullptr);

private:
    std::string family_of(const std::string& instrument_id) const;
    std::string infer_instrument(const std::string& plugin_name) const;
    const CatalogLibrary* library_of_plugin(const InstalledPlugin& p) const;

    const templates::InstrumentRegistry* instruments_;
    std::vector<Player> players_;
    std::vector<CatalogLibrary> libraries_;
    std::vector<InstalledPlugin> installed_;
    std::vector<std::string> folders_;
    std::set<std::string> detected_;
    std::vector<UserPatch> patches_;
    std::set<std::string> favorites_;
    std::vector<std::pair<std::string, std::vector<std::string>>> tags_;
    int next_id_ = 1;
};

}  // namespace vcc::library
