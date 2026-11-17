#include "ui/TemplateBrowser.h"

#include <algorithm>
#include <map>
#include <set>

#include "AppResources.h"
#include "ui/Dialogs.h"
#include "ui/library/LibraryBrowser.h"
#include "vcc/core/Files.h"
#include "vcc/library/TemplatePackage.h"
#include "vcc/project/ArticulationMap.h"
#include "ui/Theme.h"

namespace vcc::desktop {

namespace {

constexpr int kLeftWidth = 360;

const std::vector<std::pair<std::string, juce::String>>& categories()
{
    static const std::vector<std::pair<std::string, juce::String>> c = {
        {"", "All categories"}, {"*favorites", "Favorites"}, {"*recent", "Recent"}, {"cinematic", "Cinematic"}, {"regional", "Regional"}, {"era", "Era"},
        {"composer", "Composer"}, {"modern", "Modern"},   {"custom", "Custom"}};
    return c;
}

juce::Colour category_colour(const templates::Template& t)
{
    if (!t.built_in) return colours::text_dim;
    if (t.category == "cinematic") return colours::accent;
    if (t.category == "regional") return colours::solo;
    if (t.category == "era") return colours::accent_warm;
    if (t.category == "composer") return juce::Colour(0xffb07ce8);
    if (t.category == "modern") return juce::Colour(0xff3dc6c6);
    return colours::text_dim;
}

/// "large_orchestra" -> "Large orchestra".
juce::String pretty(const std::string& id)
{
    juce::String s = juce::String(id).replaceCharacter('_', ' ').replaceCharacter('-', ' ');
    return s.substring(0, 1).toUpperCase() + s.substring(1);
}

juce::String utf8(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

juce::String key_display(const templates::KeyMode& km)
{
    if (auto k = theory::Key::parse(km.tonic + " " + km.mode))
        return utf8(k->tonic.display_name()) + " " + juce::String(theory::display_name(k->mode)).toLowerCase();
    return utf8(km.tonic) + " " + pretty(km.mode).toLowerCase();
}

juce::String years(const json::Value& from, const json::Value* to)
{
    juce::String s;
    if (from.is_number()) s << juce::String(from.as_int());
    if (to && to->is_number()) s << juce::String::fromUTF8("–") << juce::String(to->as_int());
    return s;
}

juce::String join(const std::vector<juce::String>& items, const juce::String& sep)
{
    juce::String out;
    for (const auto& i : items) {
        if (i.isEmpty()) continue;
        if (out.isNotEmpty()) out << sep;
        out << i;
    }
    return out;
}

const juce::String kDot = juce::String::fromUTF8(" · ");

juce::File shelf_file() { return user_data_directory().getChildFile("template-shelf.json"); }
juce::File user_template_folder() { return user_data_directory().getChildFile("Templates"); }

/// Saves one custom template as its own pack file (as Save as Template does).
void write_custom_template(AppContext& c, const std::string& id)
{
    auto folder = user_template_folder();
    folder.createDirectory();
    const auto pack = c.session.catalogs().templates->export_pack({id}, "user");
    files::write_file_atomic(folder.getChildFile(juce::File::createLegalFileName(juce::String(id)) + ".json").getFullPathName().toStdString(),
                             json::write(pack));
}

TemplateBrowser*& browser_slot()
{
    static TemplateBrowser* b = nullptr;
    return b;
}
TemplateBrowser* open_browser() { return browser_slot(); }

}  // namespace


TemplateBrowser* template_browser() { return open_browser(); }

templates::TemplateShelf& template_shelf()
{
    static templates::TemplateShelf shelf = [] {
        const auto text = shelf_file().loadFileAsString().toStdString();
        const auto doc = json::parse(text);
        return doc ? templates::TemplateShelf::from_json(*doc) : templates::TemplateShelf{};
    }();
    return shelf;
}

void save_template_shelf()
{
    shelf_file().getParentDirectory().createDirectory();
    files::write_file_atomic(shelf_file().getFullPathName().toStdString(), json::write(template_shelf().to_json()));
}

juce::String export_template_package_file(AppContext& c, const std::vector<std::string>& ids, const juce::File& file,
                                          bool with_patches)
{
    library::PackageOptions o;
    o.name = file.getFileNameWithoutExtension().toStdString();
    o.author = c.session.data().production.composer;
    o.created = session::Session::now_utc();
    o.include_patches = with_patches;
    const auto doc = library::export_template_package(*c.session.catalogs().templates,
                                                      c.library ? &c.library->lib() : nullptr, ids, o);
    const auto summary = library::describe_template_package(doc);
    if (!summary || summary->template_names.empty()) return "Nothing to export";
    if (!files::write_file_atomic(file.getFullPathName().toStdString(), json::write(doc)))
        return "Could not write " + file.getFullPathName();
    return "Exported " + juce::String(static_cast<int>(summary->template_names.size())) + " template(s)" +
           (summary->patches > 0 ? " and " + juce::String(summary->patches) + " sound(s)" : juce::String()) + " to " +
           file.getFileName();
}

juce::String import_template_package_file(AppContext& c, const juce::File& file)
{
    const auto doc = json::parse(file.loadFileAsString().toStdString());
    if (!doc) return file.getFileName() + " is not a template package";
    auto r = library::import_template_package(*doc, *c.session.catalogs().templates, c.library ? &c.library->lib() : nullptr);
    if (!r.ok) return "Nothing imported: " + utf8(r.error);
    for (const auto& id : r.template_ids) write_custom_template(c, id);
    if (c.library && r.patches > 0) c.library->changed();
    if (auto* b = open_browser()) {
        b->reload();
        b->select(r.template_ids.front());
    }
    juce::String text = "Imported " + juce::String(static_cast<int>(r.template_ids.size())) + " template(s)";
    if (r.replaced > 0) text << " (" << r.replaced << " replaced)";
    if (r.patches > 0) text << " and " << r.patches << " sound(s)";
    if (!r.warnings.empty()) text << "; " << static_cast<int>(r.warnings.size()) << " note(s): " << utf8(r.warnings.front());
    return text;
}

bool delete_custom_template(AppContext& c, const std::string& id)
{
    const auto* t = c.session.catalogs().templates->get(id);
    if (!t || t->built_in) return false;
    // Its file: the pack in the user folder that holds it (rewritten without
    // it when it held others too).
    for (const auto& f : user_template_folder().findChildFiles(juce::File::findFiles, false, "*.json")) {
        auto doc = json::parse(f.loadFileAsString().toStdString());
        if (!doc || !doc->get("templates") || !doc->get("templates")->is_array()) continue;
        json::Array kept;
        bool held = false;
        for (const auto& j : doc->get("templates")->as_array()) {
            if (j.get_string("template_id") == id) held = true;
            else kept.push_back(j);
        }
        if (!held) continue;
        if (kept.empty()) f.deleteFile();
        else {
            doc->set("templates", kept);
            files::write_file_atomic(f.getFullPathName().toStdString(), json::write(*doc));
        }
    }
    c.session.catalogs().templates->remove_user_template(id);
    template_shelf().forget(id);
    save_template_shelf();
    return true;
}

// --- Preview pane ---------------------------------------------------------------------------

namespace {
/// The layout last chosen, kept while the app runs.
templates::TrackLayout& last_layout()
{
    static templates::TrackLayout l = templates::TrackLayout::AsTemplate;
    return l;
}
}  // namespace

class TemplateBrowser::Preview final : public juce::Component {
public:
    explicit Preview(AppContext& c) : context_(c) {}
    templates::TrackLayout layout = templates::TrackLayout::AsTemplate;

    /// The sound tracks a part opens with in the chosen layout.
    std::vector<std::string> sounds_of(const templates::TemplateTrack& tr) const
    {
        if (layout == templates::TrackLayout::PerInstrument) return {};
        if (layout == templates::TrackLayout::PerArticulation) return templates::articulation_tracks_for(tr);
        return tr.articulation_tracks;
    }

    void set(const templates::Template* t)
    {
        template_ = t;
        repaint();
    }

    int measure(int width)
    {
        return render(nullptr, width);
    }

    void paint(juce::Graphics& g) override { render(&g, getWidth()); }

private:
    struct Pen {
        juce::Graphics* g;
        int x, width, y;
    };

    static int text(Pen& p, const juce::AttributedString& as, int indent = 0)
    {
        juce::TextLayout layout;
        layout.createLayout(as, static_cast<float>(p.width - indent));
        const int h = static_cast<int>(std::ceil(layout.getHeight()));
        if (p.g) layout.draw(*p.g, juce::Rectangle<float>(static_cast<float>(p.x + indent), static_cast<float>(p.y),
                                                           static_cast<float>(p.width - indent), static_cast<float>(h)));
        return h;
    }

    static juce::AttributedString plain(const juce::String& s, float size, juce::Colour colour, bool bold = false)
    {
        juce::AttributedString as;
        as.setWordWrap(juce::AttributedString::byWord);
        as.append(s, juce::FontOptions(size, bold ? juce::Font::bold : juce::Font::plain), colour);
        return as;
    }

    static void para(Pen& p, const juce::String& s, float size, juce::Colour colour, bool bold = false, int after = 8)
    {
        if (s.isEmpty()) return;
        p.y += text(p, plain(s, size, colour, bold)) + after;
    }

    static void heading(Pen& p, const juce::String& s)
    {
        p.y += 6;
        if (p.g) {
            p.g->setColour(colours::text_dim);
            p.g->setFont(juce::FontOptions(11.5f, juce::Font::bold));
            p.g->drawText(s.toUpperCase(), p.x, p.y, p.width, 16, juce::Justification::centredLeft);
            p.g->setColour(colours::outline);
            p.g->drawHorizontalLine(p.y + 18, static_cast<float>(p.x), static_cast<float>(p.x + p.width));
        }
        p.y += 26;
    }

    static void fact(Pen& p, const juce::String& label, const juce::String& value)
    {
        if (value.isEmpty()) return;
        constexpr int kLabel = 118;
        if (p.g) {
            p.g->setColour(colours::text_dim);
            p.g->setFont(juce::FontOptions(13.0f));
            p.g->drawText(label, p.x, p.y, kLabel - 8, 18, juce::Justification::topLeft);
        }
        p.y += std::max(18, text(p, plain(value, 13.0f, colours::text), kLabel)) + 4;
    }

    int render(juce::Graphics* g, int width)
    {
        Pen p{g, 20, std::max(200, width - 40), 18};
        if (g) g->fillAll(colours::background);
        const auto* t = template_;
        if (!t) {
            para(p, "Choose a template on the left to see its tracks, notes and suggestions.", 14.0f, colours::text_dim);
            return p.y + 20;
        }
        const auto& cat = context_.session.catalogs();

        para(p, utf8(t->display_name), 22.0f, colours::text, true, 4);
        para(p, join({t->built_in ? pretty(t->category) : juce::String("Custom template"), pretty(t->subcategory),
                      pretty(t->variant), t->ensemble_size.empty() ? juce::String() : pretty(t->ensemble_size) + " ensemble"},
                     kDot),
             13.0f, category_colour(*t), false, 10);
        if (!t->instantiable()) {
            const juce::String note = t->status == "curation_required"
                                          ? "Curation required: this template is waiting for expert review and cannot be created yet."
                                          : "This template is not available yet.";
            Pen measure_pen{nullptr, p.x, p.width - 12, p.y};
            const int h = text(measure_pen, plain(note, 13.0f, colours::missing), 12) + 12;
            if (g) {
                g->setColour(colours::missing.withAlpha(0.12f));
                g->fillRoundedRectangle(static_cast<float>(p.x), static_cast<float>(p.y - 6), static_cast<float>(p.width),
                                        static_cast<float>(h), 5.0f);
                Pen draw_pen{g, p.x, p.width - 12, p.y};
                text(draw_pen, plain(note, 13.0f, colours::missing), 12);
            }
            p.y += h + 6;
        }
        para(p, utf8(t->description), 14.5f, colours::text, false, 8);
        if (!t->treatment.empty()) {
            juce::AttributedString as;
            as.setWordWrap(juce::AttributedString::byWord);
            as.append("Treatment  ", juce::FontOptions(13.0f, juce::Font::bold), colours::text_dim);
            as.append(utf8(t->treatment), juce::FontOptions(13.0f, juce::Font::italic), colours::text.withAlpha(0.85f));
            p.y += text(p, as) + 8;
        }

        heading(p, "At a glance");
        fact(p, "Tempo", juce::String(t->tempo_low) + juce::String::fromUTF8("–") + juce::String(t->tempo_high) + " BPM");
        {
            std::vector<juce::String> m;
            for (const auto& s : t->meter_options) m.push_back(s);
            fact(p, "Meters", join(m, ", "));
        }
        {
            std::vector<juce::String> k;
            for (const auto& km : t->recommended_key_modes) k.push_back(key_display(km));
            fact(p, "Keys", join(k, ", "));
        }
        fact(p, "Length", juce::String(t->default_length_bars) + " bars");
        int optional = 0, sound_tracks = 0;
        for (const auto& tr : t->tracks) {
            optional += tr.optional ? 1 : 0;
            sound_tracks += static_cast<int>(sounds_of(tr).size());
        }
        fact(p, "Tracks", juce::String(static_cast<int>(t->tracks.size())) +
                              (optional ? " (" + juce::String(optional) + " optional)" : juce::String()) +
                              (sound_tracks ? " + " + juce::String(sound_tracks) + " articulation tracks" : juce::String()) +
                              (t->buses.empty() ? juce::String() : ", " + juce::String(static_cast<int>(t->buses.size())) + " buses"));

        // Composer, period and culture.
        const bool has_context = t->composer.is_object() || t->historical_period.is_object() || t->cultural_context.is_object();
        if (has_context) {
            heading(p, t->composer.is_object() ? "Composer and period" : t->cultural_context.is_object() ? "Cultural context" : "Period");
            if (t->composer.is_object()) {
                const auto& c = t->composer;
                juce::String y = c.get("born") ? years(*c.get("born"), c.get("died")) : juce::String();
                fact(p, "Composer", utf8(c.get_string("name")) + (y.isNotEmpty() ? " (" + y + ")" : juce::String()));
                std::vector<juce::String> genres;
                if (const auto* gs = c.get("genres"))
                    for (const auto& v : gs->as_array()) genres.push_back(utf8(v.as_string()));
                fact(p, "Genres", join(genres, ", "));
                std::vector<juce::String> works;
                if (const auto* ws = c.get("example_works"))
                    for (const auto& v : ws->as_array()) works.push_back(utf8(v.as_string()));
                fact(p, "Listen to", join(works, "; "));
            }
            if (t->historical_period.is_object()) {
                const auto& h = t->historical_period;
                juce::String y = h.get("start_year") ? years(*h.get("start_year"), h.get("end_year")) : juce::String();
                fact(p, "Period", utf8(h.get_string("label")) + (y.isNotEmpty() ? " (" + y + ")" : juce::String()));
            }
            if (t->cultural_context.is_object()) {
                const auto& c = t->cultural_context;
                fact(p, "Region", join({utf8(c.get_string("region")), utf8(c.get_string("subregion"))}, ", "));
                fact(p, "Tradition", utf8(c.get_string("tradition")));
                fact(p, "Instruments", utf8(c.get_string("instrument_provenance")));
                fact(p, "Tuning", utf8(c.get_string("tuning_assumptions")));
                if (c.get("cinematic_approximation") && c.get("cinematic_approximation")->as_bool())
                    fact(p, "Approach", "A cinematic approximation, not a traditional ensemble");
                fact(p, "Review", pretty(c.get_string("review_status")));
            }
        }

        if (!t->authenticity_note.empty()) {
            p.y += 8;
            auto as = plain(utf8(t->authenticity_note), 13.0f, colours::text.withAlpha(0.9f));
            Pen inner{nullptr, p.x + 16, p.width - 28, p.y + 30};
            const int body = text(inner, as);
            const int h = body + 42;
            if (g) {
                auto box = juce::Rectangle<float>(static_cast<float>(p.x), static_cast<float>(p.y),
                                                  static_cast<float>(p.width), static_cast<float>(h));
                g->setColour(colours::accent_warm.withAlpha(0.10f));
                g->fillRoundedRectangle(box, 6.0f);
                g->setColour(colours::accent_warm.withAlpha(0.45f));
                g->drawRoundedRectangle(box.reduced(0.5f), 6.0f, 1.0f);
                g->setColour(colours::accent_warm);
                g->fillRoundedRectangle(box.withWidth(4.0f), 2.0f);
                g->setFont(juce::FontOptions(12.0f, juce::Font::bold));
                g->drawText("AUTHENTICITY NOTE", p.x + 16, p.y + 9, p.width - 30, 16, juce::Justification::centredLeft);
                inner.g = g;
                text(inner, as);
            }
            p.y += h + 8;
        }

        // Tracks grouped by family, in the folder order the project will use.
        heading(p, "Tracks");
        std::map<std::string, std::vector<const templates::TemplateTrack*>> by_family;
        for (const auto& tr : t->tracks) {
            const auto* def = cat.instruments.find(tr.instrument_standard_id);
            by_family[def ? def->family : std::string("other")].push_back(&tr);
        }
        std::vector<std::string> order = templates::family_order();
        for (const auto& [f, list] : by_family)
            if (std::find(order.begin(), order.end(), f) == order.end()) order.push_back(f);
        for (const auto& family : order) {
            auto it = by_family.find(family);
            if (it == by_family.end()) continue;
            const auto fam_colour = colour_from_hex(templates::family_color(family));
            if (g) {
                g->setColour(fam_colour);
                g->fillRoundedRectangle(static_cast<float>(p.x), static_cast<float>(p.y + 4), 10.0f, 10.0f, 2.0f);
                g->setFont(juce::FontOptions(13.5f, juce::Font::bold));
                g->setColour(colours::text);
                g->drawText(utf8(templates::family_display(family)), p.x + 18, p.y, p.width - 18, 18,
                            juce::Justification::centredLeft);
            }
            p.y += 22;
            for (const auto* tr : it->second) {
                const auto* def = cat.instruments.find(tr->instrument_standard_id);
                juce::AttributedString as;
                as.setWordWrap(juce::AttributedString::byWord);
                as.append(utf8(tr->name), juce::FontOptions(13.0f), colours::text);
                if (def && def->name != tr->name)
                    as.append(juce::String::fromUTF8("  —  ") + utf8(def->name), juce::FontOptions(12.5f), colours::text_dim);
                if (!tr->role.empty()) as.append(kDot + pretty(tr->role).toLowerCase(), juce::FontOptions(12.5f), colours::text_dim);
                if (!tr->articulation_map.empty()) {
                    std::vector<juce::String> arts;
                    for (const auto& a : tr->articulation_map) arts.push_back(pretty(a).toLowerCase());
                    as.append("   [" + join(arts, ", ") + "]", juce::FontOptions(12.0f), fam_colour.withAlpha(0.85f));
                }
                if (tr->optional) as.append("   optional", juce::FontOptions(12.0f, juce::Font::italic), colours::accent_warm);
                if (const auto sounds = sounds_of(*tr); !sounds.empty()) {
                    std::vector<juce::String> names;
                    for (const auto& g2 : sounds) names.push_back(utf8(project::articulation_group_name(g2)));
                    as.append(juce::String::fromUTF8("\n      → tracks: ") + join(names, ", "), juce::FontOptions(12.0f),
                              colours::text_dim);
                }
                p.y += text(p, as, 18) + 3;
            }
            p.y += 6;
        }

        // Suggestions.
        std::vector<juce::String> progs, osts;
        for (const auto& id : t->progression_ids)
            if (const auto* pr = cat.progressions.find(id))
                progs.push_back(utf8(pr->name) + "  (" + utf8(pr->roman_text()) + ")");
        for (const auto& id : t->ostinato_preset_ids)
            if (const auto* o = cat.ostinato_presets.find(id)) osts.push_back(utf8(o->get_string("name", id)));
        if (!progs.empty() || !osts.empty()) {
            heading(p, "Suggested material");
            fact(p, "Progressions", join(progs, "\n"));
            fact(p, "Ostinati", join(osts, "\n"));
        }
        return p.y + 16;
    }

    AppContext& context_;
    const templates::Template* template_ = nullptr;
};

// --- Browser --------------------------------------------------------------------------------

TemplateBrowser::TemplateBrowser(AppContext& c, bool for_new_project) : context_(c), for_new_project_(for_new_project)
{
    addAndMakeVisible(search_);
    search_.setTitle("Search templates");
    search_.setIndents(8, 8);
    search_.setTextToShowWhenEmpty("Search name, mood, region, composer, era...", colours::text_dim);
    search_.onTextChange = [this] { filter(); };
    search_.onReturnKey = [this] { list_.grabKeyboardFocus(); };

    category_.setTitle("Category");
    for (std::size_t i = 0; i < categories().size(); ++i) category_.addItem(categories()[i].second, static_cast<int>(i) + 1);
    category_.setSelectedId(1, juce::dontSendNotification);
    std::set<std::string> variants;
    for (const auto& t : context_.session.catalogs().templates->all())
        if (!t.variant.empty()) variants.insert(t.variant);
    variants_.assign(variants.begin(), variants.end());
    variant_.setTitle("Variant");
    variant_.addItem("Any variant", 1);
    for (std::size_t i = 0; i < variants_.size(); ++i) variant_.addItem(pretty(variants_[i]), static_cast<int>(i) + 2);
    variant_.setSelectedId(1, juce::dontSendNotification);
    size_.setTitle("Ensemble size");
    size_.addItem("Any ensemble size", 1);
    size_.addItem("Small", 2);
    size_.addItem("Medium", 3);
    size_.addItem("Large", 4);
    size_.setSelectedId(1, juce::dontSendNotification);
    for (auto* cb : {&category_, &variant_, &size_}) {
        addAndMakeVisible(*cb);
        cb->onChange = [this] { filter(); };
    }
    category_.setTooltip("Category");
    variant_.setTooltip("Ensemble variant");
    size_.setTooltip("Ensemble size");

    addAndMakeVisible(count_);
    count_.setFont(juce::FontOptions(11.5f));
    count_.setColour(juce::Label::textColourId, colours::text_dim);

    addAndMakeVisible(list_);
    list_.setModel(this);
    list_.setRowHeight(46);
    list_.setColour(juce::ListBox::backgroundColourId, colours::panel);
    list_.setColour(juce::ListBox::outlineColourId, colours::outline);
    list_.setOutlineThickness(1);

    preview_ = std::make_unique<Preview>(c);
    layout_label_.setText("Tracks:", juce::dontSendNotification);
    layout_label_.setColour(juce::Label::textColourId, colours::text_dim);
    addAndMakeVisible(layout_label_);
    layout_.setTitle("Track layout");
    layout_.setComponentID("template_layout");
    layout_.addItem("As the template lays them out", 1);
    layout_.addItem("One track per instrument (articulations inside it)", 2);
    layout_.addItem("A track per articulation (Violins - Pizzicato, - Tremolo...)", 3);
    layout_.setTooltip("Per articulation: each instrument's sounds get tracks of their own, played from the "
                       "instrument's staff; Track > Consolidate folds them back later");
    layout_.onChange = [this] {
        last_layout() = layout();
        show_selected();
    };
    addAndMakeVisible(layout_);
    layout_.setSelectedId(static_cast<int>(last_layout()) + 1, juce::dontSendNotification);
    preview_->layout = last_layout();
    preview_view_.setViewedComponent(preview_.get(), false);
    preview_view_.setScrollBarsShown(true, false);
    addAndMakeVisible(preview_view_);

    browser_slot() = this;
    setComponentID("template_browser");
    for (auto* b : {&audition_, &stop_, &create_, &add_, &favorite_, &share_}) addAndMakeVisible(*b);
    favorite_.setTooltip("Star this template: Favorites lists the starred ones");
    favorite_.onClick = [this] { toggle_favorite(); };
    share_.setTooltip("Export templates as a package to share, import one, or delete a custom template");
    share_.onClick = [this] { share_menu(); };
    audition_.setTooltip("Plays a short MIDI sketch of the template's suggested harmony (nothing is written)");
    stop_.setTooltip("Stop the audition");
    create_.setTooltip("Start a new project from this template");
    add_.setTooltip("Add this template's tracks to the open project (nothing existing is changed)");
    audition_.onClick = [this] { audition(); };
    stop_.onClick = [this] { context_.engine.stop_preview(); };
    create_.onClick = [this] { create(); };
    add_.onClick = [this] { add(); };
    set_for_new_project(for_new_project);

    filter();
    if (!shown_.empty()) list_.selectRow(0);
    setSize(1040, 660);
}

TemplateBrowser::~TemplateBrowser()
{
    if (browser_slot() == this) browser_slot() = nullptr;
    list_.setModel(nullptr);
}

void TemplateBrowser::reload() { filter(); }

bool TemplateBrowser::select(const std::string& id)
{
    for (std::size_t i = 0; i < shown_.size(); ++i)
        if (shown_[i]->template_id == id) {
            list_.selectRow(static_cast<int>(i));
            return true;
        }
    return false;
}

void TemplateBrowser::show_category(const juce::String& name)
{
    for (std::size_t i = 0; i < categories().size(); ++i)
        if (categories()[i].second == name) category_.setSelectedId(static_cast<int>(i) + 1, juce::sendNotificationSync);
}

void TemplateBrowser::toggle_favorite()
{
    const auto* t = chosen();
    if (!t) return;
    auto& shelf = template_shelf();
    const bool on = !shelf.is_favorite(t->template_id);
    shelf.set_favorite(t->template_id, on);
    save_template_shelf();
    context_.status((on ? "Starred " : "Unstarred ") + utf8(t->display_name));
    const int cat = category_.getSelectedId() - 1;
    if (cat >= 0 && categories()[static_cast<std::size_t>(cat)].first == "*favorites") filter();
    list_.repaint();
    update_buttons();
}

void TemplateBrowser::share_menu()
{
    const auto* t = chosen();
    std::vector<std::string> custom;
    for (const auto& x : context_.session.catalogs().templates->all())
        if (!x.built_in) custom.push_back(x.template_id);
    juce::PopupMenu m;
    const std::string id = t ? t->template_id : std::string();
    auto save_as = [this](std::vector<std::string> ids, const juce::String& suggested) {
        auto chooser = std::make_shared<juce::FileChooser>(
            "Export a template package",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile(juce::File::createLegalFileName(suggested) + ".vcctemplates"),
            "*.vcctemplates");
        chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                             [this, ids, chooser](const juce::FileChooser& fc) {
                                 auto file = fc.getResult();
                                 if (file == juce::File()) return;
                                 file = file.withFileExtension("vcctemplates");
                                 context_.status(export_template_package_file(context_, ids, file, true));
                             });
    };
    m.addItem("Export \"" + (t ? utf8(t->display_name) : juce::String("-")) + "\" as a Package...", t != nullptr,
              false, [save_as, id, name = t ? utf8(t->display_name) : juce::String()] { save_as({id}, name); });
    m.addItem("Export All Custom Templates...", !custom.empty(), false, [save_as, custom] { save_as(custom, "My templates"); });
    m.addItem("Import Package...", [this] {
        auto chooser = std::make_shared<juce::FileChooser>("Import a template package",
                                                           juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
                                                           "*.vcctemplates;*.json");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [this, chooser](const juce::FileChooser& fc) {
                                 if (fc.getResult().existsAsFile())
                                     context_.status(import_template_package_file(context_, fc.getResult()));
                             });
    });
    m.addSeparator();
    m.addItem("Delete Custom Template", t != nullptr && !t->built_in, false, [this, id] {
        if (delete_custom_template(context_, id)) {
            context_.status("Deleted the custom template");
            filter();
        }
    });
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&share_));
}

void TemplateBrowser::set_for_new_project(bool for_new_project)
{
    for_new_project_ = for_new_project;
    auto& primary = for_new_project_ ? create_ : add_;
    auto& secondary = for_new_project_ ? add_ : create_;
    primary.setColour(juce::TextButton::buttonColourId, colours::accent);
    secondary.removeColour(juce::TextButton::buttonColourId);
    primary.addShortcut(juce::KeyPress(juce::KeyPress::returnKey));
    secondary.clearShortcuts();
    repaint();
}

void TemplateBrowser::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
    g.setColour(colours::panel);
    g.fillRect(0, 0, kLeftWidth, getHeight());
    g.setColour(colours::outline);
    g.drawVerticalLine(kLeftWidth, 0.0f, static_cast<float>(getHeight()));
    g.drawHorizontalLine(getHeight() - 52, static_cast<float>(kLeftWidth), static_cast<float>(getWidth()));
}

void TemplateBrowser::resized()
{
    auto r = getLocalBounds();
    auto left = r.removeFromLeft(kLeftWidth).reduced(12);
    search_.setBounds(left.removeFromTop(30));
    left.removeFromTop(8);
    category_.setBounds(left.removeFromTop(26));
    left.removeFromTop(6);
    auto row = left.removeFromTop(26);
    variant_.setBounds(row.removeFromLeft(row.getWidth() / 2 - 3));
    row.removeFromLeft(6);
    size_.setBounds(row);
    left.removeFromTop(4);
    count_.setBounds(left.removeFromTop(20));
    list_.setBounds(left);

    r.removeFromLeft(1);
    auto bar = r.removeFromBottom(52).reduced(14, 11);
    {
        auto row2 = r.removeFromBottom(36).reduced(14, 4);
        layout_label_.setBounds(row2.removeFromLeft(56));
        layout_.setBounds(row2.removeFromLeft(std::min(440, row2.getWidth())));
    }
    audition_.setBounds(bar.removeFromLeft(88));
    bar.removeFromLeft(6);
    stop_.setBounds(bar.removeFromLeft(58));
    bar.removeFromLeft(6);
    favorite_.setBounds(bar.removeFromLeft(96));
    bar.removeFromLeft(6);
    share_.setBounds(bar.removeFromLeft(76));
    create_.setBounds(bar.removeFromRight(136));
    bar.removeFromRight(8);
    add_.setBounds(bar.removeFromRight(136));
    preview_view_.setBounds(r);
    show_selected();
}

int TemplateBrowser::getNumRows() { return static_cast<int>(shown_.size()); }

void TemplateBrowser::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (row < 0 || row >= getNumRows()) return;
    const auto* t = shown_[static_cast<std::size_t>(row)];
    const bool ok = t->instantiable();
    g.fillAll(selected ? colours::accent.withAlpha(0.28f) : (row % 2 ? colours::panel : colours::panel.brighter(0.025f)));
    g.setColour(category_colour(*t).withAlpha(ok ? 1.0f : 0.4f));
    g.fillRect(0, 6, 3, h - 12);
    const bool star = template_shelf().is_favorite(t->template_id);
    if (star) {
        g.setColour(colours::accent_warm);
        g.setFont(juce::FontOptions(15.0f));
        g.drawText(juce::String::fromUTF8("\xe2\x98\x85"), w - 26, 3, 20, h / 2, juce::Justification::bottomRight, false);
    }
    g.setColour(ok ? colours::text : colours::text_dim.withAlpha(0.6f));
    g.setFont(juce::FontOptions(14.0f));
    g.drawText(utf8(t->display_name), 12, 3, w - (star ? 40 : 18), h / 2, juce::Justification::bottomLeft, true);
    g.setFont(juce::FontOptions(11.5f));
    if (!ok) {
        g.setColour(colours::missing.withAlpha(0.8f));
        g.drawText("Curation required - not yet available", 12, h / 2 + 3, w - 18, h / 2 - 4,
                   juce::Justification::topLeft, true);
    } else {
        g.setColour(colours::text_dim);
        g.drawText(join({pretty(t->subcategory), pretty(t->variant), juce::String(static_cast<int>(t->tracks.size())) + " tracks"}, kDot),
                   12, h / 2 + 3, w - 18, h / 2 - 4, juce::Justification::topLeft, true);
    }
    g.setColour(colours::outline.withAlpha(0.5f));
    g.drawHorizontalLine(h - 1, 0.0f, static_cast<float>(w));
}

void TemplateBrowser::selectedRowsChanged(int) { show_selected(); }

void TemplateBrowser::listBoxItemDoubleClicked(int, const juce::MouseEvent&)
{
    for_new_project_ ? create() : add();
}

void TemplateBrowser::returnKeyPressed(int) { for_new_project_ ? create() : add(); }

void TemplateBrowser::filter()
{
    const auto* previous = chosen();
    templates::TemplateFilter f;
    f.query = search_.getText().trim().toStdString();
    const int cat = category_.getSelectedId() - 1;
    const std::string key = cat >= 0 ? categories()[static_cast<std::size_t>(cat)].first : std::string();
    const bool custom = key == "custom";
    const bool favorites = key == "*favorites", recent = key == "*recent";
    if (cat > 0 && !custom && !favorites && !recent) f.category = key;
    const int v = variant_.getSelectedId() - 2;
    if (v >= 0 && v < static_cast<int>(variants_.size())) f.variant = variants_[static_cast<std::size_t>(v)];
    static const char* sizes[] = {"", "", "small", "medium", "large"};
    if (size_.getSelectedId() >= 2) f.ensemble_size = sizes[size_.getSelectedId()];

    shown_.clear();
    const auto listed = context_.session.catalogs().templates->list(f);
    if (recent) {
        // In the order used, newest first.
        for (const auto& id : template_shelf().recent())
            for (const auto* t : listed)
                if (t->template_id == id) shown_.push_back(t);
    } else {
        for (const auto* t : listed)
            if ((!custom || !t->built_in) && (!favorites || template_shelf().is_favorite(t->template_id))) shown_.push_back(t);
    }
    count_.setText(shown_.empty() ? juce::String(favorites ? "No favorites yet: select a template and press Favorite"
                                                 : recent  ? "No templates used yet"
                                                           : "No templates match")
                                  : juce::String(static_cast<int>(shown_.size())) + (shown_.size() == 1 ? " template" : " templates"),
                   juce::dontSendNotification);
    list_.updateContent();
    auto it = std::find(shown_.begin(), shown_.end(), previous);
    if (it != shown_.end()) list_.selectRow(static_cast<int>(it - shown_.begin()));
    else if (!shown_.empty()) list_.selectRow(0);
    else list_.deselectAllRows();
    list_.repaint();
    show_selected();
}

const templates::Template* TemplateBrowser::chosen() const
{
    const int row = list_.getSelectedRow();
    return row >= 0 && row < static_cast<int>(shown_.size()) ? shown_[static_cast<std::size_t>(row)] : nullptr;
}

void TemplateBrowser::show_selected()
{
    const auto* t = chosen();
    preview_->layout = layout();
    preview_->set(t);
    preview_->repaint();
    const int w = std::max(200, preview_view_.getWidth() - preview_view_.getScrollBarThickness());
    preview_->setSize(w, std::max(preview_->measure(w), preview_view_.getHeight()));
    preview_view_.setViewPosition(0, 0);
    update_buttons();
}

void TemplateBrowser::update_buttons()
{
    const auto* t = chosen();
    const bool ok = t && t->instantiable();
    create_.setEnabled(ok);
    add_.setEnabled(ok);
    audition_.setEnabled(t != nullptr);
    favorite_.setEnabled(t != nullptr);
    const bool starred = t && template_shelf().is_favorite(t->template_id);
    favorite_.setButtonText(juce::String::fromUTF8(starred ? "\xe2\x98\x85 Favorite" : "\xe2\x98\x86 Favorite"));
    favorite_.setTitle(starred ? "Remove from favorites" : "Add to favorites");
}

void TemplateBrowser::audition()
{
    const auto* t = chosen();
    if (!t) return;
    const auto& cat = context_.session.catalogs();

    // Harmony: the template's first suggested progression, or I-IV-V-I.
    theory::Progression basic;
    basic.id = "basic_cadence";
    basic.name = "I-IV-V-I";
    basic.bars = 4;
    basic.allowed_bar_lengths = {4, 8};
    basic.steps = {{"I", 1.0}, {"IV", 1.0}, {"V", 1.0}, {"I", 1.0}};
    const theory::Progression* prog = &basic;
    for (const auto& id : t->progression_ids)
        if (const auto* p = cat.progressions.find(id)) {
            prog = p;
            break;
        }

    // Key: the first recommended one, else a neutral default for the tonality.
    const bool minor = theory::is_minor_family(prog->default_mode);
    const theory::Key fallback = *theory::Key::parse(minor ? "D minor" : "C major");
    theory::Key key = fallback;
    if (!t->recommended_key_modes.empty())
        if (auto k = theory::Key::parse(t->recommended_key_modes.front().tonic + " " + t->recommended_key_modes.front().mode))
            key = *k;

    TimeSignature sig;
    if (!t->meter_options.empty()) {
        const auto parts = juce::StringArray::fromTokens(juce::String(t->meter_options.front()), "/", "");
        if (parts.size() == 2) {
            TimeSignature s{parts[0].getIntValue(), parts[1].getIntValue()};
            if (s.is_valid()) sig = s;
        }
    }
    const MeterMap meter(sig);

    theory::ProgressionRenderOptions o;
    o.key = key;
    o.start_bar = 1;
    o.length_bars = 4;
    o.voicing.style = theory::VoicingStyle::Spread;
    o.voicing.center = 62;
    o.voicing.low = 36;
    o.voicing.high = 84;
    o.voicing.voice_lead = true;
    o.rhythm = theory::ChordRhythm::Block;
    o.velocity = 72;
    std::string error;
    auto render = theory::render_progression(*prog, meter, o, &error);
    if (!render) {
        o.length_bars = prog->bars;  // fixed forms (12-bar blues) play one chorus
        render = theory::render_progression(*prog, meter, o, &error);
    }
    if (!render) {
        o.key = fallback;
        render = theory::render_progression(*prog, meter, o, &error);
    }
    if (!render) {
        context_.status("Could not audition this template: " + utf8(error));
        return;
    }

    // Voice: the first harmony, pad or melody instrument.
    std::string family = "keys";
    int program = 0;
    for (const auto& role : {"harmony", "pad", "melody"}) {
        auto it = std::find_if(t->tracks.begin(), t->tracks.end(), [&](const templates::TemplateTrack& tr) {
            const auto* def = cat.instruments.find(tr.instrument_standard_id);
            return tr.role == role && def && !def->unpitched;
        });
        if (it != t->tracks.end()) {
            const auto* def = cat.instruments.find(it->instrument_standard_id);
            family = def->family;
            program = def->gm_program;
            break;
        }
    }
    const double bpm = juce::jlimit(50.0, 160.0, (t->tempo_low + t->tempo_high) / 2.0);
    context_.engine.preview(dialogs::shifted(render->notes, render->start), family, program, bpm, false,
                            render->end - render->start);
    context_.status("Auditioning " + utf8(t->display_name) + ": " + utf8(prog->name) + " in " +
                    utf8(key.tonic.display_name()) + " " + juce::String(theory::display_name(key.mode)).toLowerCase() +
                    " (a MIDI sketch on the placeholder voice)");
}

void TemplateBrowser::set_layout(templates::TrackLayout l)
{
    layout_.setSelectedId(static_cast<int>(l) + 1, juce::sendNotificationSync);
}

templates::TrackLayout TemplateBrowser::layout() const
{
    return static_cast<templates::TrackLayout>(std::max(0, layout_.getSelectedId() - 1));
}

void TemplateBrowser::create()
{
    const auto* t = chosen();
    if (!t || !t->instantiable()) return;
    context_.engine.stop_preview();
    const std::string id = t->template_id;
    const juce::String name = utf8(t->display_name);
    templates::InstantiateOptions opt;
    opt.layout = layout();
    auto r = context_.session.new_from_template(id, opt);
    if (r.ok) {
        template_shelf().used(id);
        save_template_shelf();
    }
    if (!r.ok) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Create Project",
                                               "Could not create a project from " + name + ":\n" + utf8(r.error));
        return;
    }
    context_.status("New project from " + name + ": " + juce::String(static_cast<int>(r.track_ids.size())) + " tracks" +
                    (r.warnings.empty() ? juce::String()
                                        : ", " + juce::String(static_cast<int>(r.warnings.size())) +
                                              " warnings (placeholders or fallbacks - see each track's status)"));
    if (auto* w = findParentComponentOfClass<dialogs::ToolWindow>()) w->closeButtonPressed();
}

void TemplateBrowser::add()
{
    const auto* t = chosen();
    if (!t || !t->instantiable()) return;
    const juce::String name = utf8(t->display_name);
    const std::string id = t->template_id;
    templates::InstantiateOptions opt;
    opt.layout = layout();
    auto r = context_.session.add_template(id, opt);
    if (r.ok) {
        template_shelf().used(id);
        save_template_shelf();
    }
    if (!r.ok) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Add to Project",
                                               "Could not add " + name + ":\n" + utf8(r.error));
        return;
    }
    context_.status("Added " + name + ": " + juce::String(static_cast<int>(r.track_ids.size())) + " tracks" +
                    (r.warnings.empty() ? juce::String()
                                        : ", " + juce::String(static_cast<int>(r.warnings.size())) + " warnings"));
}

}  // namespace vcc::desktop
