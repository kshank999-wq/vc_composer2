// Template system (Main Spec §5, §6, §11; Templates Supplement §3, §8, §12).
//
//   InstrumentRegistry  - standard instrument facts (content/instruments/*.json)
//   TemplateRegistry    - list/get/validate/search templates (content/templates/*.json)
//   InstrumentResolver  - standard instrument + articulations -> the composer's patches
//   instantiate()       - template -> tracks, folders, buses in a project, one undo step
//   save_user_template()- project -> editable custom template
//
// Nothing here knows about plug-in formats or sample libraries. The resolver
// works from a catalog of patches the composer has mapped (by hand, from a
// provider mapping pack, or from a confirmed scan), which is the honest limit
// of what a host can know about another vendor's instruments (Main Spec §10).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "vcc/core/Json.h"
#include "vcc/project/Project.h"

namespace vcc::templates {

// --- Instruments --------------------------------------------------------------

struct InstrumentDef {
    std::string id;
    std::string name;
    std::string short_name;
    std::string family;
    std::string clef = "treble";
    bool grand_staff = false;
    std::string lower_clef = "bass";
    int transposition = 0;
    int range_low = 0;
    int range_high = 127;
    int score_order = 1000;
    int gm_program = 0;
    bool unpitched = false;
    std::vector<std::string> tags;
    std::string region;
    std::string note;
    std::string pack;
};

/// The families, in the folder order Supplement §8 gives.
const std::vector<std::string>& family_order();
/// Default family colour for tracks and folders.
std::string family_color(const std::string& family);

/// The empty score an instrument's track starts with: its clef, its
/// transposition, and a grand staff for keyboards and harp.
notation::Score instrument_score(const InstrumentDef& def, const std::string& track_name);
/// Display name for a family folder: "strings" -> "Strings".
std::string family_display(const std::string& family);

struct Diagnostic {
    enum class Level { Error, Warning } level = Level::Error;
    std::string where;
    std::string message;
    std::string to_string() const;
};

class InstrumentRegistry {
public:
    /// Loads every *.json in a directory. Returns false on any error.
    bool load_directory(const std::string& dir, std::vector<Diagnostic>& diagnostics);
    bool load(const json::Value& doc, const std::string& source, std::vector<Diagnostic>& diagnostics);

    const InstrumentDef* find(const std::string& id) const noexcept;
    const std::vector<InstrumentDef>& all() const noexcept { return items_; }

private:
    std::vector<InstrumentDef> items_;
};

// --- Templates ----------------------------------------------------------------

struct TemplateTrack {
    std::string id;
    std::string name;
    std::string instrument_standard_id;
    std::string role;
    std::vector<std::string> articulation_map;
    std::vector<std::string> fallback_instruments;
    std::vector<std::string> plugin_requirements;
    std::string group_id;
    std::string output_bus_id;
    double gain_db = 0.0;
    double pan = 0.0;
    std::string color;
    std::string midi_channel_policy = "auto";
    bool optional = false;
};

struct TemplateBus {
    std::string id;
    std::string name;
    std::string kind = "group";
    std::string output = "master";
    std::vector<std::pair<std::string, double>> sends;
    std::vector<std::string> insert_suggestions;
};

struct KeyMode {
    std::string tonic;
    std::string mode;
};

struct Template {
    std::string template_id;
    int schema_version = 1;
    std::string content_version;
    std::string display_name;
    std::string category;
    std::string subcategory;
    std::vector<std::string> tags;
    std::string locale = "en";
    std::string variant;
    std::string description;
    std::string treatment;
    std::string ensemble_size;
    std::string status = "available";
    std::string authenticity_note;
    std::vector<KeyMode> recommended_key_modes;
    int tempo_low = 60;
    int tempo_high = 120;
    std::vector<std::string> meter_options;
    int default_length_bars = 32;
    std::vector<TemplateTrack> tracks;
    std::vector<TemplateBus> buses;
    std::vector<std::string> progression_ids;
    std::vector<std::string> ostinato_preset_ids;
    json::Value historical_period;
    json::Value composer;
    json::Value cultural_context;
    json::Value license_metadata;
    std::string pack;
    bool built_in = true;  ///< Shipped templates are immutable; user templates are editable.

    bool instantiable() const noexcept { return status == "available" && !tracks.empty(); }
    json::Value to_json() const;
};

struct TemplateFilter {
    std::string query;      ///< Free text: name, tags, description, region, composer.
    std::string category;   ///< "cinematic", "regional", "era", "composer", "modern".
    std::string subcategory;
    std::string variant;
    std::string ensemble_size;
    std::vector<std::string> tags;  ///< All must match.
    std::string family;     ///< Templates that use at least one instrument of this family.
    bool available_only = false;
};

class TemplateRegistry {
public:
    explicit TemplateRegistry(const InstrumentRegistry& instruments) : instruments_(instruments) {}

    /// Optional cross-reference catalogs; when set, unknown ids are errors.
    void set_known_articulations(std::vector<std::string> ids) { articulations_ = std::move(ids); }
    void set_known_progressions(std::vector<std::string> ids) { progressions_ = std::move(ids); }
    void set_known_ostinato_presets(std::vector<std::string> ids) { ostinatos_ = std::move(ids); }

    bool load_directory(const std::string& dir, std::vector<Diagnostic>& diagnostics);
    /// Loads one pack document. Templates with errors are skipped and reported;
    /// valid ones in the same pack still load.
    bool load_pack(const json::Value& doc, const std::string& source, std::vector<Diagnostic>& diagnostics,
                   bool built_in = true);

    /// Validates a single template against the schema and catalogs.
    std::vector<Diagnostic> validate(const json::Value& tpl) const;

    const Template* get(const std::string& template_id) const noexcept;
    std::vector<const Template*> list(const TemplateFilter& filter = {}) const;
    const std::vector<Template>& all() const noexcept { return items_; }

    /// Adds or replaces a user (non built-in) template. Built-ins cannot be replaced.
    bool add_user_template(const Template& t, std::string* error = nullptr);

    /// Exports templates as a pack document, and imports one with validation.
    json::Value export_pack(const std::vector<std::string>& template_ids, const std::string& pack_name) const;
    bool import_pack(const json::Value& doc, std::vector<Diagnostic>& diagnostics);

private:
    std::optional<Template> parse(const json::Value& tpl, const std::string& pack, std::vector<Diagnostic>& d) const;

    const InstrumentRegistry& instruments_;
    std::vector<std::string> articulations_, progressions_, ostinatos_;
    std::vector<Template> items_;
};

// --- Resolver -----------------------------------------------------------------

/// A patch the composer owns and has mapped (Main Spec §11 mapping model,
/// reduced to what resolution needs).
struct InstalledPatch {
    std::string patch_id;
    std::string display;                ///< "Library X - Horns a4 Marcato"
    std::string instrument_standard_id; ///< What it plays.
    std::vector<std::string> articulations;  ///< Intents its articulation map supports.
    bool favorite = false;
    std::string validation_status = "verified";  ///< "verified", "inferred", "user".
};

struct Resolution {
    project::ResolutionStatus status = project::ResolutionStatus::Unresolved;
    std::string resolved_instrument_id;
    const InstalledPatch* patch = nullptr;
    int placeholder_program = 0;
    std::vector<std::string> unsupported_articulations;
    std::string message;
};

class InstrumentResolver {
public:
    InstrumentResolver(const InstrumentRegistry& instruments, std::vector<InstalledPatch> installed)
        : instruments_(instruments), installed_(std::move(installed)) {}

    /// Exact patch for the instrument, else a patch for the first approved
    /// fallback, else a labelled General MIDI placeholder. Among several
    /// patches it prefers favorites, then verified mappings, then the most
    /// requested articulations covered. Never fails, never stays silent.
    Resolution resolve(const std::string& instrument_id, const std::vector<std::string>& articulations,
                       const std::vector<std::string>& fallbacks) const;

private:
    const InstalledPatch* best_patch(const std::string& instrument_id,
                                     const std::vector<std::string>& articulations) const;
    const InstrumentRegistry& instruments_;
    std::vector<InstalledPatch> installed_;
};

// --- Instantiation --------------------------------------------------------------

enum class InstantiateMode {
    Create,  ///< Into an empty project; applies tempo/meter/key defaults.
    Merge,   ///< "Add to Existing Project": nothing existing is altered.
    Replace, ///< Clears instrument tracks first; needs replace_confirmed.
};

struct InstantiateOptions {
    InstantiateMode mode = InstantiateMode::Create;
    bool replace_confirmed = false;
    bool include_optional = true;
    bool apply_project_defaults = true;  ///< Create only: tempo, meter, key from the template.
    std::vector<InstalledPatch> installed;
};

struct InstantiateResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> track_ids;   ///< Instrument tracks created, in order.
    std::vector<std::string> folder_ids;
    std::vector<std::string> bus_ids;
    std::vector<std::string> warnings;    ///< Placeholders, fallbacks, missing articulations.
};

/// Instantiates a template as one undoable step ("Insert template ...").
/// Missing libraries never fail it: every track is created and labelled with
/// how it was resolved (tests T01, T02, acceptance criterion 7).
InstantiateResult instantiate(project::Project& project, const Template& tpl, const InstrumentRegistry& instruments,
                              const InstantiateOptions& options);

/// Captures a project's instrument tracks and buses as a custom template
/// (Supplement §3 "Save As New Custom Template").
Template save_user_template(const project::Project& project, const std::string& template_id,
                            const std::string& display_name, const std::string& description);

}  // namespace vcc::templates
