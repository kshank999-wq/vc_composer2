#include <algorithm>
#include <cctype>
#include <filesystem>
#include <regex>
#include <set>

#include "vcc/core/Files.h"
#include "vcc/core/Time.h"
#include "vcc/templates/Templates.h"

namespace vcc::templates {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool contains(const std::vector<std::string>& v, const std::string& x)
{
    return std::find(v.begin(), v.end(), x) != v.end();
}

const std::vector<std::string> kCategories = {"cinematic", "regional", "era", "composer", "modern"};
const std::vector<std::string> kVariants = {"acoustic", "hybrid", "minimal", "large_orchestra", "chamber",
                                            "traditional", "fusion", "cinematic", "period", "band",
                                            "electronic", "solo"};
const std::vector<std::string> kRoles = {"melody", "countermelody", "harmony", "bass", "pad", "texture", "ostinato",
                                         "rhythm", "percussion", "drone", "fx", "solo", "continuo", "vocal"};
const std::vector<std::string> kClefs = {"treble", "bass", "alto", "tenor", "treble_8vb", "treble_8va", "bass_8vb",
                                         "percussion"};
const std::vector<std::string> kModes = {"major", "minor", "harmonic_minor", "melodic_minor", "dorian", "phrygian",
                                         "lydian", "mixolydian", "aeolian", "locrian", "phrygian_dominant",
                                         "double_harmonic", "whole_tone", "octatonic", "major_pentatonic",
                                         "minor_pentatonic", "blues", "custom"};

std::optional<json::Value> read_json(const std::string& path, std::vector<Diagnostic>& d)
{
    std::string err;
    auto text = files::read_file(path, &err);
    if (!text) {
        d.push_back({Diagnostic::Level::Error, path, err});
        return std::nullopt;
    }
    json::ParseError pe;
    auto doc = json::parse(*text, &pe);
    if (!doc) d.push_back({Diagnostic::Level::Error, path, pe.to_string()});
    return doc;
}

std::vector<std::string> json_files(const std::string& dir)
{
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::u8path(dir), ec))
        if (e.is_regular_file() && e.path().extension() == ".json") out.push_back(e.path().u8string());
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

std::string Diagnostic::to_string() const
{
    return std::string(level == Level::Error ? "error" : "warning") + " " + where + ": " + message;
}

const std::vector<std::string>& family_order()
{
    static const std::vector<std::string> order = {"strings", "woodwinds", "brass", "percussion", "keys",
                                                   "synth", "traditional", "rhythm", "vocals"};
    return order;
}

std::string family_color(const std::string& f)
{
    if (f == "strings") return "#c0392b";
    if (f == "woodwinds") return "#27ae60";
    if (f == "brass") return "#e67e22";
    if (f == "percussion") return "#7f8c8d";
    if (f == "keys") return "#2980b9";
    if (f == "synth") return "#8e44ad";
    if (f == "traditional") return "#b8860b";
    if (f == "rhythm") return "#16a085";
    if (f == "vocals") return "#d35400";
    return "#808080";
}

std::string family_display(const std::string& f)
{
    if (f.empty()) return "Other";
    std::string s = f;
    s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

// --- Instruments ----------------------------------------------------------------

bool InstrumentRegistry::load(const json::Value& doc, const std::string& source, std::vector<Diagnostic>& d)
{
    bool ok = true;
    auto err = [&](const std::string& where, const std::string& msg) {
        d.push_back({Diagnostic::Level::Error, source + ":" + where, msg});
        ok = false;
    };
    if (doc.get_int("schema_version") != 1) err("", "schema_version must be 1");
    std::string pack = doc.get_string("pack");
    for (const auto& j : doc.get("instruments") ? doc.get("instruments")->as_array() : json::Array{}) {
        InstrumentDef i;
        i.id = j.get_string("id");
        if (i.id.empty()) { err("?", "instrument without id"); continue; }
        if (find(i.id)) { err(i.id, "duplicate instrument id"); continue; }
        i.name = j.get_string("name");
        i.short_name = j.get_string("short_name");
        i.family = j.get_string("family");
        i.clef = j.get_string("clef", "treble");
        i.grand_staff = j.get_bool("grand_staff");
        i.lower_clef = j.get_string("lower_clef", "bass");
        i.transposition = static_cast<int>(j.get_int("transposition"));
        i.score_order = static_cast<int>(j.get_int("score_order", 1000));
        i.gm_program = static_cast<int>(j.get_int("gm_program", 0));
        i.unpitched = j.get_bool("unpitched");
        i.tags = j.get_string_array("tags");
        i.region = j.get_string("region");
        i.note = j.get_string("note");
        i.pack = pack;
        const auto* r = j.get("range");
        if (!r || r->as_array().size() != 2) { err(i.id, "range must be [low, high]"); continue; }
        i.range_low = static_cast<int>(r->as_array()[0].as_int(-1));
        i.range_high = static_cast<int>(r->as_array()[1].as_int(-1));
        if (i.range_low < 0 || i.range_high > 127 || i.range_low > i.range_high) { err(i.id, "bad range"); continue; }
        if (i.name.empty() || i.short_name.empty()) { err(i.id, "missing name/short_name"); continue; }
        if (!contains(family_order(), i.family)) { err(i.id, "unknown family '" + i.family + "'"); continue; }
        if (!contains(kClefs, i.clef) || !contains(kClefs, i.lower_clef)) { err(i.id, "unknown clef"); continue; }
        if (i.gm_program < -1 || i.gm_program > 127 || (i.gm_program == -1 && !i.unpitched)) {
            err(i.id, "bad gm_program");
            continue;
        }
        items_.push_back(std::move(i));
    }
    return ok;
}

bool InstrumentRegistry::load_directory(const std::string& dir, std::vector<Diagnostic>& d)
{
    bool ok = true;
    auto files = json_files(dir);
    if (files.empty()) {
        d.push_back({Diagnostic::Level::Error, dir, "no instrument files"});
        return false;
    }
    for (const auto& f : files) {
        auto doc = read_json(f, d);
        ok = doc && load(*doc, fs::u8path(f).filename().u8string(), d) && ok;
    }
    return ok;
}

const InstrumentDef* InstrumentRegistry::find(const std::string& id) const noexcept
{
    for (const auto& i : items_)
        if (i.id == id) return &i;
    return nullptr;
}

// --- Template parsing -----------------------------------------------------------

std::vector<Diagnostic> TemplateRegistry::validate(const json::Value& tpl) const
{
    std::vector<Diagnostic> d;
    parse(tpl, "", d);
    return d;
}

std::optional<Template> TemplateRegistry::parse(const json::Value& j, const std::string& pack,
                                                std::vector<Diagnostic>& d) const
{
    Template t;
    t.template_id = j.get_string("template_id");
    const std::string where = (pack.empty() ? "" : pack + ":") + (t.template_id.empty() ? "?" : t.template_id);
    std::size_t errors_before = std::count_if(d.begin(), d.end(), [](const Diagnostic& x) {
        return x.level == Diagnostic::Level::Error;
    });
    auto err = [&](const std::string& m) { d.push_back({Diagnostic::Level::Error, where, m}); };
    auto warn = [&](const std::string& m) { d.push_back({Diagnostic::Level::Warning, where, m}); };

    static const std::regex id_re("^[a-z][a-z0-9_]*(\\.[a-z0-9_]+)+$");
    static const std::regex track_re("^[a-z][a-z0-9_]*$");
    if (!std::regex_match(t.template_id, id_re)) err("template_id must be dotted lower-case");
    if (!j.is_object()) { err("template must be an object"); return std::nullopt; }

    for (const char* f : {"schema_version", "content_version", "display_name", "category", "subcategory", "tags",
                          "variant", "description", "tempo_range_bpm", "meter_options", "license_metadata"})
        if (!j.get(f)) err(std::string("missing ") + f);

    t.schema_version = static_cast<int>(j.get_int("schema_version"));
    if (t.schema_version != 1) err("unsupported schema_version " + std::to_string(t.schema_version));
    t.content_version = j.get_string("content_version");
    t.display_name = j.get_string("display_name");
    t.category = j.get_string("category");
    if (!contains(kCategories, t.category)) err("unknown category '" + t.category + "'");
    t.subcategory = j.get_string("subcategory");
    t.tags = j.get_string_array("tags");
    t.locale = j.get_string("locale", "en");
    t.variant = j.get_string("variant");
    if (!contains(kVariants, t.variant)) err("unknown variant '" + t.variant + "'");
    t.description = j.get_string("description");
    t.treatment = j.get_string("treatment");
    t.status = j.get_string("status", "available");
    if (t.status != "available" && t.status != "curation_required") err("unknown status '" + t.status + "'");
    t.authenticity_note = j.get_string("authenticity_note");
    if (const auto* v = j.get("historical_period")) t.historical_period = *v;
    if (const auto* v = j.get("composer")) t.composer = *v;
    if (const auto* v = j.get("cultural_context")) t.cultural_context = *v;
    if (const auto* v = j.get("license_metadata")) t.license_metadata = *v;
    if ((t.cultural_context.is_object() || t.composer.is_object()) && t.authenticity_note.empty())
        err("authenticity_note is required with cultural_context or composer");

    if (const auto* tr = j.get("tempo_range_bpm"); tr && tr->as_array().size() == 2) {
        t.tempo_low = static_cast<int>(tr->as_array()[0].as_int());
        t.tempo_high = static_cast<int>(tr->as_array()[1].as_int());
        if (t.tempo_low < 20 || t.tempo_high > 320 || t.tempo_low > t.tempo_high) err("bad tempo_range_bpm");
    } else if (j.get("tempo_range_bpm")) {
        err("tempo_range_bpm must be [low, high]");
    }
    t.meter_options = j.get_string_array("meter_options");
    if (t.meter_options.empty()) err("meter_options is empty");
    for (const auto& m : t.meter_options)
        if (!TimeSignature::parse(m)) err("bad meter '" + m + "'");
    t.default_length_bars = static_cast<int>(j.get_int("default_length_bars", 32));
    if (t.default_length_bars < 1) err("default_length_bars must be positive");

    if (const auto* km = j.get("recommended_key_modes")) {
        static const std::regex tonic_re("^[A-G](#|b)?$");
        for (const auto& k : km->as_array()) {
            KeyMode m{k.get_string("tonic"), k.get_string("mode")};
            if (!std::regex_match(m.tonic, tonic_re)) err("bad tonic '" + m.tonic + "'");
            if (!contains(kModes, m.mode)) err("bad mode '" + m.mode + "'");
            t.recommended_key_modes.push_back(m);
        }
    }

    std::set<std::string> bus_ids;
    for (const auto& b : j.get("buses") ? j.get("buses")->as_array() : json::Array{}) {
        TemplateBus bus;
        bus.id = b.get_string("id");
        bus.name = b.get_string("name", bus.id);
        bus.kind = b.get_string("kind", "group");
        bus.output = b.get_string("output", "master");
        if (bus.id.empty() || !bus_ids.insert(bus.id).second) err("missing or duplicate bus id '" + bus.id + "'");
        if (bus.kind != "group" && bus.kind != "fx" && bus.kind != "master") err("bad bus kind '" + bus.kind + "'");
        for (const auto& s : b.get("sends") ? b.get("sends")->as_array() : json::Array{})
            bus.sends.emplace_back(s.get_string("to"), s.get_double("level_db", -12.0));
        bus.insert_suggestions = b.get_string_array("insert_suggestions");
        t.buses.push_back(bus);
    }
    for (const auto& b : t.buses) {
        if (b.output != "master" && !b.output.empty() && !bus_ids.count(b.output)) err("bus " + b.id + " outputs to unknown bus");
        for (const auto& s : b.sends)
            if (!bus_ids.count(s.first)) err("bus " + b.id + " sends to unknown bus " + s.first);
    }

    std::set<std::string> track_ids;
    for (const auto& k : j.get("tracks") ? j.get("tracks")->as_array() : json::Array{}) {
        TemplateTrack tt;
        tt.id = k.get_string("id");
        tt.name = k.get_string("name");
        tt.instrument_standard_id = k.get_string("instrument_standard_id");
        tt.role = k.get_string("role");
        const std::string tw = "track " + tt.id + ": ";
        if (!std::regex_match(tt.id, track_re)) err(tw + "id must be snake_case");
        if (!track_ids.insert(tt.id).second) err(tw + "duplicate track id");
        if (tt.name.empty()) err(tw + "missing name");
        if (!instruments_.find(tt.instrument_standard_id))
            err(tw + "unknown instrument '" + tt.instrument_standard_id + "'");
        if (!contains(kRoles, tt.role)) err(tw + "unknown role '" + tt.role + "'");
        tt.articulation_map = k.get_string_array("articulation_map");
        if (!articulations_.empty())
            for (const auto& a : tt.articulation_map)
                if (!contains(articulations_, a)) warn(tw + "articulation '" + a + "' not in vocabulary");
        tt.fallback_instruments = k.get_string_array("fallback_instruments");
        for (const auto& f : tt.fallback_instruments)
            if (!instruments_.find(f)) err(tw + "unknown fallback instrument '" + f + "'");
        tt.plugin_requirements = k.get_string_array("plugin_requirements");
        tt.group_id = k.get_string("group_id");
        tt.output_bus_id = k.get_string("output_bus_id");
        if (!tt.output_bus_id.empty() && !bus_ids.count(tt.output_bus_id))
            err(tw + "output_bus_id '" + tt.output_bus_id + "' is not declared");
        if (const auto* gp = k.get("default_gain_pan")) {
            tt.gain_db = gp->get_double("gain_db");
            tt.pan = gp->get_double("pan");
            if (tt.pan < -1.0 || tt.pan > 1.0) err(tw + "pan out of range");
            if (tt.gain_db < -96.0 || tt.gain_db > 24.0) err(tw + "gain out of range");
        }
        tt.color = k.get_string("color");
        tt.midi_channel_policy = k.get_string("midi_channel_policy", "auto");
        static const std::regex ch_re("^(auto|drum|fixed:([1-9]|1[0-6]))$");
        if (!std::regex_match(tt.midi_channel_policy, ch_re)) err(tw + "bad midi_channel_policy");
        tt.optional = k.get_bool("optional");
        t.tracks.push_back(tt);
    }
    if (t.status == "available" && t.tracks.empty()) err("an available template needs at least one track");

    t.progression_ids = j.get_string_array("progression_ids");
    t.ostinato_preset_ids = j.get_string_array("ostinato_preset_ids");
    if (!progressions_.empty())
        for (const auto& p : t.progression_ids)
            if (!contains(progressions_, p)) err("unknown progression '" + p + "'");
    if (!ostinatos_.empty())
        for (const auto& p : t.ostinato_preset_ids)
            if (!contains(ostinatos_, p)) err("unknown ostinato preset '" + p + "'");

    t.ensemble_size = j.get_string("ensemble_size");
    if (t.ensemble_size.empty()) {
        std::size_t n = t.tracks.size();
        t.ensemble_size = n <= 1 ? "solo" : n <= 6 ? "small" : n <= 12 ? "medium" : "large";
    }
    t.pack = pack;

    std::size_t errors_after = std::count_if(d.begin(), d.end(), [](const Diagnostic& x) {
        return x.level == Diagnostic::Level::Error;
    });
    if (errors_after != errors_before) return std::nullopt;
    return t;
}

bool TemplateRegistry::load_pack(const json::Value& doc, const std::string& source, std::vector<Diagnostic>& d,
                                 bool built_in)
{
    bool ok = true;
    if (doc.get_int("schema_version") != 1) {
        d.push_back({Diagnostic::Level::Error, source, "pack schema_version must be 1"});
        return false;
    }
    std::string pack = doc.get_string("pack", source);
    for (const auto& j : doc.get("templates") ? doc.get("templates")->as_array() : json::Array{}) {
        auto t = parse(j, pack, d);
        if (!t) { ok = false; continue; }
        if (get(t->template_id)) {
            d.push_back({Diagnostic::Level::Error, pack + ":" + t->template_id, "duplicate template_id"});
            ok = false;
            continue;
        }
        t->built_in = built_in;
        items_.push_back(std::move(*t));
    }
    return ok;
}

bool TemplateRegistry::load_directory(const std::string& dir, std::vector<Diagnostic>& d)
{
    bool ok = true;
    for (const auto& f : json_files(dir)) {
        auto doc = read_json(f, d);
        ok = doc && load_pack(*doc, fs::u8path(f).filename().u8string(), d) && ok;
    }
    return ok;
}

const Template* TemplateRegistry::get(const std::string& id) const noexcept
{
    for (const auto& t : items_)
        if (t.template_id == id) return &t;
    return nullptr;
}

std::vector<const Template*> TemplateRegistry::list(const TemplateFilter& f) const
{
    std::vector<std::string> tokens;
    {
        std::string q = lower(f.query), cur;
        for (char c : q) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!cur.empty()) tokens.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) tokens.push_back(cur);
    }
    std::vector<const Template*> out;
    for (const auto& t : items_) {
        if (!f.category.empty() && t.category != f.category) continue;
        if (!f.subcategory.empty() && t.subcategory != f.subcategory) continue;
        if (!f.variant.empty() && t.variant != f.variant) continue;
        if (!f.ensemble_size.empty() && t.ensemble_size != f.ensemble_size) continue;
        if (f.available_only && !t.instantiable()) continue;
        bool tags_ok = std::all_of(f.tags.begin(), f.tags.end(), [&](const std::string& tag) {
            return std::any_of(t.tags.begin(), t.tags.end(), [&](const std::string& x) { return lower(x) == lower(tag); });
        });
        if (!tags_ok) continue;
        if (!f.family.empty()) {
            bool uses = std::any_of(t.tracks.begin(), t.tracks.end(), [&](const TemplateTrack& tt) {
                const InstrumentDef* i = instruments_.find(tt.instrument_standard_id);
                return i && i->family == f.family;
            });
            if (!uses) continue;
        }
        if (!tokens.empty()) {
            std::string hay = lower(t.template_id + " " + t.display_name + " " + t.category + " " + t.subcategory +
                                    " " + t.variant + " " + t.description + " " + t.treatment);
            for (const auto& tag : t.tags) hay += " " + lower(tag);
            hay += " " + lower(t.cultural_context.get_string("region")) + " " +
                   lower(t.cultural_context.get_string("tradition")) + " " + lower(t.composer.get_string("name")) +
                   " " + lower(t.historical_period.get_string("label"));
            bool all = std::all_of(tokens.begin(), tokens.end(),
                                   [&](const std::string& tok) { return hay.find(tok) != std::string::npos; });
            if (!all) continue;
        }
        out.push_back(&t);
    }
    std::stable_sort(out.begin(), out.end(), [](const Template* a, const Template* b) {
        auto ca = std::find(kCategories.begin(), kCategories.end(), a->category) - kCategories.begin();
        auto cb = std::find(kCategories.begin(), kCategories.end(), b->category) - kCategories.begin();
        if (ca != cb) return ca < cb;
        return a->display_name < b->display_name;
    });
    return out;
}

bool TemplateRegistry::add_user_template(const Template& t, std::string* error)
{
    std::vector<Diagnostic> d;
    auto parsed = parse(t.to_json(), "user", d);
    if (!parsed) {
        if (error) *error = d.empty() ? "invalid template" : d.front().to_string();
        return false;
    }
    parsed->built_in = false;
    for (auto& existing : items_) {
        if (existing.template_id != t.template_id) continue;
        if (existing.built_in) {
            if (error) *error = "built-in templates are immutable; save a copy under a new id";
            return false;
        }
        existing = *parsed;
        return true;
    }
    items_.push_back(*parsed);
    return true;
}

json::Value TemplateRegistry::export_pack(const std::vector<std::string>& ids, const std::string& name) const
{
    json::Array arr;
    for (const auto& id : ids)
        if (const Template* t = get(id)) arr.push_back(t->to_json());
    return json::Object{{"schema_version", 1}, {"pack", name}, {"content_version", "1.0.0"}, {"templates", arr}};
}

bool TemplateRegistry::import_pack(const json::Value& doc, std::vector<Diagnostic>& d)
{
    return load_pack(doc, doc.get_string("pack", "import"), d, /*built_in=*/false);
}

json::Value Template::to_json() const
{
    json::Object o{{"template_id", template_id}, {"schema_version", schema_version},
                   {"content_version", content_version}, {"display_name", display_name},
                   {"category", category}, {"subcategory", subcategory}};
    json::Array tg;
    for (const auto& x : tags) tg.emplace_back(x);
    o.set("tags", tg);
    o.set("locale", locale);
    o.set("variant", variant);
    o.set("description", description);
    if (!treatment.empty()) o.set("treatment", treatment);
    o.set("ensemble_size", ensemble_size);
    if (status != "available") o.set("status", status);
    if (historical_period.is_object()) o.set("historical_period", historical_period);
    if (composer.is_object()) o.set("composer", composer);
    if (cultural_context.is_object()) o.set("cultural_context", cultural_context);
    if (!authenticity_note.empty()) o.set("authenticity_note", authenticity_note);
    json::Array km;
    for (const auto& k : recommended_key_modes) km.emplace_back(json::Object{{"tonic", k.tonic}, {"mode", k.mode}});
    o.set("recommended_key_modes", km);
    o.set("tempo_range_bpm", json::Array{tempo_low, tempo_high});
    json::Array mo;
    for (const auto& m : meter_options) mo.emplace_back(m);
    o.set("meter_options", mo);
    o.set("default_length_bars", default_length_bars);
    json::Array tr;
    for (const auto& t : tracks) {
        auto arr = [](const std::vector<std::string>& v) {
            json::Array a;
            for (const auto& s : v) a.emplace_back(s);
            return a;
        };
        json::Object to{{"id", t.id}, {"name", t.name}, {"instrument_standard_id", t.instrument_standard_id},
                        {"role", t.role}, {"articulation_map", arr(t.articulation_map)},
                        {"fallback_instruments", arr(t.fallback_instruments)},
                        {"plugin_requirements", arr(t.plugin_requirements)}};
        if (!t.group_id.empty()) to.set("group_id", t.group_id);
        if (!t.output_bus_id.empty()) to.set("output_bus_id", t.output_bus_id);
        to.set("default_gain_pan", json::Object{{"gain_db", t.gain_db}, {"pan", t.pan}});
        if (!t.color.empty()) to.set("color", t.color);
        to.set("midi_channel_policy", t.midi_channel_policy);
        if (t.optional) to.set("optional", true);
        tr.emplace_back(to);
    }
    o.set("tracks", tr);
    if (!buses.empty()) {
        json::Array bs;
        for (const auto& b : buses) {
            json::Array sends, ins;
            for (const auto& s : b.sends) sends.emplace_back(json::Object{{"to", s.first}, {"level_db", s.second}});
            for (const auto& i : b.insert_suggestions) ins.emplace_back(i);
            bs.emplace_back(json::Object{{"id", b.id}, {"name", b.name}, {"kind", b.kind}, {"output", b.output},
                                         {"sends", sends}, {"insert_suggestions", ins}});
        }
        o.set("buses", bs);
    }
    json::Array pr, os;
    for (const auto& p : progression_ids) pr.emplace_back(p);
    for (const auto& p : ostinato_preset_ids) os.emplace_back(p);
    o.set("progression_ids", pr);
    o.set("ostinato_preset_ids", os);
    o.set("license_metadata", license_metadata.is_object() ? license_metadata
                                                           : json::Value(json::Object{{"content_license", "user"}}));
    return o;
}

}  // namespace vcc::templates
