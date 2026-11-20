#include "ui/ScoreExchange.h"

#include "vcc/theory/Chord.h"

#include <algorithm>

#include "AppResources.h"
#include "ui/Dialogs.h"
#include "ui/Theme.h"
#include "ui/hw/HwTheme.h"
#include "vcc/core/Files.h"
#include "vcc/engraving/PdfRenderer.h"
#include "vcc/engraving/PrintLayout.h"
#include "vcc/engraving/ScoreLayout.h"
#include "vcc/interchange/Transposition.h"
#include "vcc/interchange/MusicXml.h"
#include "vcc/ui/EmbeddedMusicFont.h"

namespace vcc::desktop {

namespace {

juce::String read_entry(juce::ZipFile& zip, int index)
{
    std::unique_ptr<juce::InputStream> in(zip.createStreamForEntry(index));
    return in ? in->readEntireStreamAsString() : juce::String();
}

std::string summary_of(const std::vector<std::string>& warnings)
{
    if (warnings.empty()) return {};
    std::string s = " (" + std::to_string(warnings.size()) + " note" + (warnings.size() == 1 ? "" : "s") + ": " +
                    warnings.front();
    if (warnings.size() > 1) s += "; ...";
    return s + ")";
}

}  // namespace

bool read_musicxml_file(const juce::File& file, std::string& xml, juce::String& error)
{
    if (!file.existsAsFile()) {
        error = "the file does not exist";
        return false;
    }
    if (!file.hasFileExtension("mxl")) {
        juce::MemoryBlock data;
        if (!file.loadFileAsData(data)) {
            error = "the file could not be read";
            return false;
        }
        xml.assign(static_cast<const char*>(data.getData()), data.getSize());
        return true;
    }
    juce::ZipFile zip(file);
    if (zip.getNumEntries() == 0) {
        error = "not a compressed MusicXML (.mxl) file";
        return false;
    }
    // The container names the root file; older writers just put one score in.
    juce::String root;
    if (const int c = zip.getIndexOfFileName("META-INF/container.xml"); c >= 0)
        if (auto doc = juce::parseXML(read_entry(zip, c)))
            if (auto* rf = doc->getChildByName("rootfiles"))
                if (auto* first = rf->getChildByName("rootfile")) root = first->getStringAttribute("full-path");
    int index = root.isNotEmpty() ? zip.getIndexOfFileName(root) : -1;
    for (int i = 0; index < 0 && i < zip.getNumEntries(); ++i) {
        const auto name = zip.getEntry(i)->filename;
        if (!name.startsWith("META-INF") && (name.endsWithIgnoreCase(".xml") || name.endsWithIgnoreCase(".musicxml")))
            index = i;
    }
    if (index < 0) {
        error = "the .mxl file holds no MusicXML score";
        return false;
    }
    xml = read_entry(zip, index).toStdString();
    if (xml.empty()) {
        error = "the score inside the .mxl file could not be read";
        return false;
    }
    return true;
}

bool import_musicxml_file(AppContext& context, const juce::File& file, bool replace, juce::String& message)
{
    std::string xml;
    juce::String error;
    if (!read_musicxml_file(file, xml, error)) {
        message = "Could not import " + file.getFileName() + ": " + error;
        return false;
    }
    const auto r = context.session.import_musicxml(xml, replace);
    if (!r.ok) {
        message = "Could not import " + file.getFileName() + ": " + juce::String(r.error);
        return false;
    }
    message = "Imported " + juce::String(static_cast<int>(r.track_ids.size())) + " part" +
              (r.track_ids.size() == 1 ? "" : "s") + " from " + file.getFileName() + juce::String(summary_of(r.warnings));
    return true;
}

bool export_musicxml_file(AppContext& context, const juce::File& file, bool concert_pitch, juce::String& message)
{
    interchange::ExportOptions opts;
    opts.concert_pitch = concert_pitch;
    opts.encoding_date = juce::Time::getCurrentTime().formatted("%Y-%m-%d").toStdString();
    std::vector<std::string> warnings;
    const std::string xml = context.session.export_musicxml(opts, &warnings);
    if (xml.empty()) {
        message = "Nothing to export: no track holds notation";
        return false;
    }
    std::string error;
    bool ok = false;
    if (file.hasFileExtension("mxl")) {
        const juce::String inner = file.getFileNameWithoutExtension() + ".musicxml";
        const std::string container =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<container>\n  <rootfiles>\n    <rootfile full-path=\"" +
            inner.toStdString() + "\" media-type=\"application/vnd.recordare.musicxml+xml\"/>\n  </rootfiles>\n</container>\n";
        juce::ZipFile::Builder zip;
        // "mimetype" first and stored, as the MusicXML spec asks.
        zip.addEntry(new juce::MemoryInputStream("application/vnd.recordare.musicxml", 35, true), 0, "mimetype",
                     juce::Time::getCurrentTime());
        zip.addEntry(new juce::MemoryInputStream(container.data(), container.size(), true), 9, "META-INF/container.xml",
                     juce::Time::getCurrentTime());
        zip.addEntry(new juce::MemoryInputStream(xml.data(), xml.size(), true), 9, inner, juce::Time::getCurrentTime());
        juce::MemoryOutputStream out;
        ok = zip.writeToStream(out, nullptr);
        if (ok) {
            const std::string bytes(static_cast<const char*>(out.getData()), out.getDataSize());
            ok = files::write_file_atomic(file.getFullPathName().toStdString(), bytes, &error);
        } else {
            error = "could not compress the score";
        }
    } else {
        ok = files::write_file_atomic(file.getFullPathName().toStdString(), xml, &error);
    }
    message = ok ? "Exported MusicXML to " + file.getFileName() + (concert_pitch ? " (concert pitch)" : "") +
                       juce::String(summary_of(warnings))
                 : "MusicXML export failed: " + juce::String(error);
    return ok;
}

int export_parts_pdf(AppContext& context, const juce::File& target, juce::String& message)
{
    const auto& fonts = music_font();
    engraving::LayoutEngine engine;
    if (fonts.loaded) engine.set_glyph_table(fonts.glyphs);
    const auto& d = context.session.data();

    std::vector<std::pair<std::string, engraving::PrintResult>> parts;
    for (const auto* t : context.session.staff_tracks()) {
        const auto* v = t->active_version();
        if (!v || t->kind != project::TrackKind::Instrument || !context.session.in_score(*t)) continue;
        engraving::PrintOptions po;
        po.subtitle = t->name;
        apply_score_info(context, po);
        po.multirests = true;  // empty runs as multi-bar rests
        // A part reads at written pitch under its written key.
        const notation::Score written = interchange::display_score(v->score, false);
        parts.emplace_back(t->name, engraving::lay_out_pages(written, engine, po));
    }
    if (parts.empty()) {
        message = "Nothing to print: no instrument track holds notation";
        return 0;
    }
    engraving::PdfOptions opts;
    opts.music_font = ui::embedded_music_font_bytes();
    opts.author = d.production.composer;
    opts.creation_date = "D:" + juce::Time::getCurrentTime().formatted("%Y%m%d%H%M%S").toStdString();
    const engraving::GlyphTable* glyphs = fonts.loaded ? &fonts.glyphs : nullptr;

    int pages = 0, files_written = 0;
    std::string error;
    auto write = [&](const juce::File& f, const std::vector<engraving::PdfDocumentPart>& docs, const std::string& title) {
        opts.title = title;
        const std::string pdf = engraving::render_pdf(docs, opts, glyphs, &error);
        if (pdf.empty() || !files::write_file_atomic(f.getFullPathName().toStdString(), pdf, &error)) return false;
        for (const auto& doc : docs) pages += static_cast<int>(doc.pages->pages.size());
        ++files_written;
        return true;
    };
    bool ok = true;
    if (target.hasFileExtension("pdf")) {
        std::vector<engraving::PdfDocumentPart> docs;
        for (const auto& [name, laid] : parts) docs.push_back({&laid, name});
        ok = write(target, docs, d.name + " - parts");
    } else {
        target.createDirectory();
        for (const auto& [name, laid] : parts) {
            const auto f = target.getChildFile(juce::File::createLegalFileName(juce::String(d.name) + " - " + name) + ".pdf");
            ok = write(f, {{&laid, name}}, d.name + " - " + name) && ok;
        }
    }
    message = ok ? "Printed " + juce::String(static_cast<int>(parts.size())) + " part" + (parts.size() == 1 ? "" : "s") +
                       " (" + juce::String(pages) + " page" + (pages == 1 ? "" : "s") + ") to " +
                       (files_written == 1 ? target.getFileName() : target.getFullPathName())
                 : "PDF export failed: " + juce::String(error);
    return ok ? pages : 0;
}

namespace {

/// Families whose parts share a bracket in a score.
std::string bracket_group(const std::string& family)
{
    for (const char* g : {"woodwinds", "brass", "percussion", "strings", "vocals", "rhythm"})
        if (family == g) return family;
    return {};
}

/// Conventional orchestral score order, top to bottom; families not listed
/// (traditional, synth, others) follow, in track order.
int score_rank(const std::string& family)
{
    static const char* kOrder[] = {"woodwinds", "brass", "percussion", "keys", "vocals", "strings", "rhythm", "synth"};
    for (int i = 0; i < 8; ++i)
        if (family == kOrder[i]) return i;
    return 8;
}

std::string abbreviate(const std::string& name)
{
    // "Violins I" -> "Vln. I" style is the registry's job; this is the
    // fallback: the first word cut to three letters, plus any numeral.
    const auto space = name.find(' ');
    std::string first = name.substr(0, space);
    std::string out = first.size() > 4 ? first.substr(0, 3) + "." : first;
    if (space != std::string::npos) out += name.substr(space);
    return out;
}

}  // namespace

FullScoreSettings saved_full_score_settings()
{
    auto& s = hw::ui_settings();
    FullScoreSettings f;
    f.concert_pitch = s.getBoolValue("score.concert_pitch", false);
    const auto paper = s.getValue("score.paper", "A3");
    f.paper = (paper == "Tabloid" || paper == "A4" || paper == "Letter") ? paper.toStdString() : std::string("A3");
    f.hide_empty_staves = s.getBoolValue("score.hide_empty", true);
    f.bars_per_system = juce::jlimit(0, 12, s.getIntValue("score.bars_per_system", 4));
    f.landscape = s.getBoolValue("score.landscape", false);
    f.score_order = s.getBoolValue("score.orchestral_order", true);
    return f;
}

void save_full_score_settings(const FullScoreSettings& f)
{
    auto& s = hw::ui_settings();
    s.setValue("score.concert_pitch", f.concert_pitch);
    s.setValue("score.paper", juce::String(f.paper));
    s.setValue("score.hide_empty", f.hide_empty_staves);
    s.setValue("score.bars_per_system", f.bars_per_system);
    s.setValue("score.landscape", f.landscape);
    s.setValue("score.orchestral_order", f.score_order);
    s.saveIfNeeded();
}

/// The project's chord symbols as text over a printed part, at the part's
/// written pitch when it transposes (a concert print passes a score whose
/// staves no longer do).
static void add_chord_symbols(const project::ProjectData& d, notation::Score& score)
{
    const int t = score.staff_count() > 0 ? score.staff(0).transposition_semitones : 0;
    // Written = sounding - t: the letter steps for that many semitones.
    static const int kSteps[12] = {0, 1, 1, 2, 2, 3, 3, 4, 5, 5, 6, 6};
    const int semis = -t;
    const int steps = semis >= 0 ? kSteps[semis % 12] + 7 * (semis / 12) : -(kSteps[(-semis) % 12] + 7 * ((-semis) / 12));
    for (const auto& ch : d.chords) {
        auto chord = theory::parse_chord_symbol(ch.text);
        if (!chord) continue;
        std::string text = ch.text;
        if (t != 0) {
            if (chord->bass || chord->inversion > 0) chord->bass = chord->bass_note().transposed(steps, semis);
            chord->inversion = 0;
            chord->root = chord->root.transposed(steps, semis);
            text = chord->name();
        }
        notation::TextMarking m;
        m.tick = ch.tick;
        m.text = text;
        score.add_text(m);
    }
}

std::unique_ptr<FullScoreLayout> lay_out_full_score(AppContext& context, const FullScoreSettings& settings,
                                                    juce::String& message)
{
    const auto& fonts = music_font();
    engraving::LayoutEngine engine;
    if (fonts.loaded) engine.set_glyph_table(fonts.glyphs);
    const auto& d = context.session.data();

    std::vector<const project::Track*> tracks;
    for (const auto* t : context.session.staff_tracks()) {
        const auto* v = t->active_version();
        if (!v || t->kind != project::TrackKind::Instrument || !context.session.in_score(*t)) continue;
        tracks.push_back(t);
    }
    if (tracks.empty()) {
        message = "Nothing to print: no instrument track holds notation";
        return nullptr;
    }
    if (settings.score_order) {
        auto family_of = [&](const project::Track* t) {
            const auto* def = context.session.instrument_for(*t);
            return def ? def->family : t->family;
        };
        std::stable_sort(tracks.begin(), tracks.end(), [&](const project::Track* a, const project::Track* b) {
            return score_rank(family_of(a)) < score_rank(family_of(b));
        });
    }
    auto out = std::make_unique<FullScoreLayout>();
    auto& scores = out->scores;  // kept alive with the layout
    scores.reserve(tracks.size());
    std::vector<engraving::ScorePart> parts;
    for (const auto* t : tracks) {
        scores.push_back(interchange::display_score(t->active_version()->score, settings.concert_pitch));
        const auto* def = context.session.instrument_for(*t);
        engraving::ScorePart part;
        part.score = &scores.back();
        part.name = t->name;
        part.abbreviation = def && !def->short_name.empty() ? def->short_name : abbreviate(t->name);
        part.group = bracket_group(def ? def->family : t->family);
        parts.push_back(part);
    }
    // Chord symbols over the top staff of the score.
    if (!d.chords.empty() && !scores.empty()) add_chord_symbols(d, scores.front());

    engraving::ScoreOptions opts;
    double w = 297, h = 420, space = 1.25;  // A3, 5 mm staff
    if (settings.paper == "Tabloid") { w = 279.4; h = 431.8; }
    else if (settings.paper == "A4") { w = 210; h = 297; space = 1.0; }
    else if (settings.paper == "Letter") { w = 215.9; h = 279.4; space = 1.0; }
    if (settings.landscape) std::swap(w, h);
    // Large orchestras get a smaller staff so a system fits the page.
    const std::size_t staves = [&] {
        std::size_t n = 0;
        for (const auto& sc : scores) n += sc.staff_count();
        return n;
    }();
    const double needed = static_cast<double>(staves) * 11.0 + 30.0;  // spaces per staff plus title room
    if (needed * space > h - 24.0) space = std::max(0.7, (h - 24.0) / needed);
    engraving::page_size_for(w, h, space, opts);
    apply_score_info(context, opts);
    if (opts.subtitle.empty()) opts.subtitle = settings.concert_pitch ? "Score in C" : "Transposed score";
    opts.hide_empty_staves = settings.hide_empty_staves;
    // "As many as fit": the layout takes fewer bars only where notes would crowd.
    opts.bars_per_system = settings.bars_per_system <= 0 ? 16 : settings.bars_per_system;
    out->laid = engraving::lay_out_score(parts, engine, opts);
    out->staff_space_mm = space;
    out->page_width_mm = w;
    out->page_height_mm = h;
    out->parts = static_cast<int>(parts.size());
    out->title = opts.title;
    return out;
}

int export_full_score_pdf(AppContext& context, const juce::File& file, const FullScoreSettings& settings,
                          juce::String& message)
{
    const auto& fonts = music_font();
    const auto& d = context.session.data();
    const auto full = lay_out_full_score(context, settings, message);
    if (!full) return 0;
    const auto& laid = full->laid;
    const double space = full->staff_space_mm;
    const auto parts_count = full->parts;
    struct {
        std::string title;
    } opts{full->title};
    engraving::PdfOptions pdf_opts;
    pdf_opts.music_font = ui::embedded_music_font_bytes();
    pdf_opts.points_per_staff_space = 72.0 / 25.4 * space;
    pdf_opts.title = opts.title + " - full score";
    pdf_opts.author = d.production.composer;
    pdf_opts.creation_date = "D:" + juce::Time::getCurrentTime().formatted("%Y%m%d%H%M%S").toStdString();
    std::string error;
    const std::string pdf = engraving::render_pages_pdf(laid, pdf_opts, fonts.loaded ? &fonts.glyphs : nullptr, &error);
    if (pdf.empty() || !files::write_file_atomic(file.getFullPathName().toStdString(), pdf, &error)) {
        message = "Full score export failed: " + juce::String(error);
        return 0;
    }
    const int pages = static_cast<int>(laid.pages.size());
    message = "Printed the full score (" + juce::String(parts_count) + " parts, " +
              juce::String(pages) + " page" + (pages == 1 ? "" : "s") + ", " +
              juce::String(settings.concert_pitch ? "concert pitch" : "transposed") + ") to " + file.getFileName();
    if (laid.crowded_system_count > 0)
        message << " - " << juce::String(static_cast<int>(laid.crowded_system_count))
                << " system(s) are crowded; try fewer bars per system";
    return pages;
}

}  // namespace vcc::desktop

// --- Score Info ---------------------------------------------------------------------------

namespace vcc::desktop {

namespace {

std::string upper(std::string s)
{
    for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

template <typename Options>
void fill_head(const AppContext& c, Options& o)
{
    const auto si = c.session.score_info();
    auto shown = [](bool on, const std::string& text) { return on ? text : std::string(); };
    o.title = shown(si.show_title, si.title_caps ? upper(si.title) : si.title);
    if (!si.show_subtitle) o.subtitle.clear();
    else if (!si.subtitle.empty() && o.subtitle.empty()) o.subtitle = si.subtitle;
    o.composer = shown(si.show_composer, si.composer);
    o.title_size *= std::clamp(si.title_scale, 0.5, 3.0);
    o.head.title_bold = si.title_bold;
    o.head.title_italic = si.title_italic;
    o.head.lyricist = shown(si.show_lyricist, si.lyricist);
    o.head.arranger = shown(si.show_arranger, si.arranger);
    std::string info = shown(si.show_date, si.date);
    if (si.show_revision && !si.revision.empty()) info += (info.empty() ? "" : "  -  ") + si.revision;
    o.head.info_line = info;
    o.head.copyright = shown(si.show_copyright, si.copyright);
    o.head.tempo_text = shown(si.show_tempo_text, si.tempo_text);
    o.head.tempo_bpm = si.show_tempo ? c.session.data().tempo.bpm_at(0) : 0.0;
}

}  // namespace

void apply_score_info(const AppContext& context, engraving::PrintOptions& options) { fill_head(context, options); }
void apply_score_info(const AppContext& context, engraving::ScoreOptions& options)
{
    fill_head(context, options);
    // The full score's own subtitle is the score info's when there is one.
    if (const auto si = context.session.score_info(); !si.subtitle.empty() && si.show_subtitle) options.subtitle = si.subtitle;
}

namespace {

class ScoreInfoPanel final : public juce::Component {
public:
    explicit ScoreInfoPanel(AppContext& c) : context_(c)
    {
        setComponentID("score_info");
        const auto si = c.session.score_info();
        auto field = [this](juce::TextEditor& e, const juce::String& label, const std::string& value, const juce::String& hint,
                            bool shown) {
            // The switch: does this line print?
            auto sw = std::make_unique<juce::ToggleButton>();
            sw->setTitle("Show " + label);
            sw->setTooltip("Show the " + label.toLowerCase() + " on the score");
            sw->setComponentID("sheet_show_" + label.toLowerCase().replaceCharacter(' ', '_'));
            sw->setToggleState(shown, juce::dontSendNotification);
            sw->onClick = [this] { refresh_shown(); };
            addAndMakeVisible(*sw);
            shows_.push_back(std::move(sw));
            auto l = std::make_unique<juce::Label>(juce::String(), label);
            l->setColour(juce::Label::textColourId, colours::text_dim);
            l->setJustificationType(juce::Justification::centredRight);
            addAndMakeVisible(*l);
            labels_.push_back(std::move(l));
            e.setTitle(label);
            e.setText(juce::String::fromUTF8(value.c_str()), false);
            e.setTextToShowWhenEmpty(hint, colours::text_dim);
            e.onTextChange = [this] { repaint(); };
            addAndMakeVisible(e);
            fields_.push_back(&e);
        };
        // A date to start from: today, as a score would carry it.
        const std::string date = si.date.empty() ? juce::Time::getCurrentTime().formatted("%d %B %Y").toStdString() : si.date;
        field(title_, "Title", si.title, "The score's title", si.show_title);
        field(subtitle_, "Subtitle", si.subtitle, "for orchestra / from the film ...", si.show_subtitle);
        field(composer_, "Composer", si.composer, "Music by ...", si.show_composer);
        field(arranger_, "Arranger", si.arranger, "Orch. / Arr. ...", si.show_arranger);
        field(lyricist_, "Words", si.lyricist, "Words by ...", si.show_lyricist);
        field(date_, "Date", date, "19 November 2026", si.show_date);
        field(revision_, "Version", si.revision, "Version 3 / Rev. 2", si.show_revision);
        field(copyright_, "Copyright", si.copyright, "(c) 2026 ...", si.show_copyright);
        field(tempo_text_, "Tempo text", si.tempo_text, "Allegro / Slowly, with tension", si.show_tempo_text);
        refresh_shown();
        show_tempo_.setButtonText("Metronome mark from the music");
        show_tempo_.setToggleState(si.show_tempo, juce::dontSendNotification);
        muted_.setButtonText("Muted instruments leave the score");
        muted_.setComponentID("score_info_muted");
        muted_.setToggleState(si.muted_leave_score, juce::dontSendNotification);
        addAndMakeVisible(muted_);
        muted_.setColour(juce::ToggleButton::textColourId, colours::text);
        bold_.setButtonText("Bold");
        italic_.setButtonText("Italic");
        caps_.setButtonText("ALL CAPS");
        bold_.setToggleState(si.title_bold, juce::dontSendNotification);
        italic_.setToggleState(si.title_italic, juce::dontSendNotification);
        caps_.setToggleState(si.title_caps, juce::dontSendNotification);
        size_.setRange(0.5, 3.0, 0.05);
        size_.setValue(si.title_scale, juce::dontSendNotification);
        size_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
        size_.setTitle("Title size");
        for (juce::Component* comp : std::initializer_list<juce::Component*>{&show_tempo_, &bold_, &italic_, &caps_, &size_, &ok_, &cancel_})
            addAndMakeVisible(comp);
        for (auto* b : {&bold_, &italic_, &caps_}) b->onClick = [this] { repaint(); };
        size_.onValueChange = [this] { repaint(); };
        for (auto* b : {&show_tempo_, &bold_, &italic_, &caps_})
            b->setColour(juce::ToggleButton::textColourId, colours::text);
        ok_.onClick = [this] { apply(); };
        cancel_.onClick = [this] { close(); };
        // What the music says, put on the score by itself.
        const auto& d = c.session.data();
        auto_info_ = "From the music: tempo " + juce::String(d.tempo.bpm_at(0), 0) + " BPM  -  key " + juce::String(d.key) +
                     "  -  meter " + juce::String(d.meter.at_bar(1).to_string());
        setSize(560, 600);
    }

    void apply()
    {
        auto si = context_.session.score_info();
        si.title = title_.getText().toStdString();
        si.subtitle = subtitle_.getText().toStdString();
        si.composer = composer_.getText().toStdString();
        si.arranger = arranger_.getText().toStdString();
        si.lyricist = lyricist_.getText().toStdString();
        si.date = date_.getText().toStdString();
        si.revision = revision_.getText().toStdString();
        si.copyright = copyright_.getText().toStdString();
        si.tempo_text = tempo_text_.getText().toStdString();
        si.show_tempo = show_tempo_.getToggleState();
        si.title_bold = bold_.getToggleState();
        si.title_italic = italic_.getToggleState();
        si.title_caps = caps_.getToggleState();
        si.title_scale = size_.getValue();
        si.muted_leave_score = muted_.getToggleState();
        bool* flags[] = {&si.show_title, &si.show_subtitle, &si.show_composer, &si.show_arranger, &si.show_lyricist,
                         &si.show_date, &si.show_revision, &si.show_copyright, &si.show_tempo_text};
        for (std::size_t i = 0; i < shows_.size() && i < std::size(flags); ++i) *flags[i] = shows_[i]->getToggleState();
        context_.session.set_score_info(si);
        context_.status("Sheet info set: " + title_.getText());
        close();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(colours::background);
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(10.0f));
        g.drawText("Show", show_header_, juce::Justification::centred);
        // The title as it will print.
        auto r = preview_area_.toFloat();
        g.setColour(juce::Colours::white);
        g.fillRoundedRectangle(r, 4.0f);
        g.setColour(juce::Colours::black);
        int style = juce::Font::plain;
        if (bold_.getToggleState()) style |= juce::Font::bold;
        if (italic_.getToggleState()) style |= juce::Font::italic;
        auto text = shown(0) ? title_.getText() : juce::String();
        if (caps_.getToggleState()) text = text.toUpperCase();
        g.setFont(juce::FontOptions(static_cast<float>(16.0 * size_.getValue()), style));
        g.drawFittedText(text, preview_area_.reduced(8, 4).removeFromTop(preview_area_.getHeight() * 2 / 3),
                         juce::Justification::centred, 1);
        g.setFont(juce::FontOptions(11.0f));
        if (shown(2)) g.drawText(composer_.getText(), preview_area_.reduced(8, 4), juce::Justification::bottomRight);
        juce::String info = shown(5) ? date_.getText() : juce::String();
        if (shown(6) && revision_.getText().isNotEmpty()) info += (info.isEmpty() ? "" : "  -  ") + revision_.getText();
        g.drawText(info, preview_area_.reduced(8, 4), juce::Justification::topLeft);
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(auto_info_, auto_area_, juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(14);
        preview_area_ = r.removeFromTop(80);
        show_header_ = r.removeFromTop(14).removeFromLeft(36);
        for (std::size_t i = 0; i < fields_.size(); ++i) {
            auto row = r.removeFromTop(28);
            shows_[i]->setBounds(row.removeFromLeft(36).reduced(4, 2));
            labels_[i]->setBounds(row.removeFromLeft(80));
            row.removeFromLeft(6);
            fields_[i]->setBounds(row.reduced(0, 2));
            r.removeFromTop(2);
        }
        auto style = r.removeFromTop(28);
        style.removeFromLeft(96);
        bold_.setBounds(style.removeFromLeft(70));
        italic_.setBounds(style.removeFromLeft(70));
        caps_.setBounds(style.removeFromLeft(96));
        size_.setBounds(style);
        show_tempo_.setBounds(r.removeFromTop(26).withTrimmedLeft(96));
        muted_.setBounds(r.removeFromTop(26).withTrimmedLeft(96));
        auto_area_ = r.removeFromTop(22).withTrimmedLeft(96);
        auto buttons = r.removeFromBottom(30);
        cancel_.setBounds(buttons.removeFromRight(100));
        buttons.removeFromRight(8);
        ok_.setBounds(buttons.removeFromRight(100));
    }

    juce::TextEditor title_, subtitle_, composer_, arranger_, lyricist_, date_, revision_, copyright_, tempo_text_;
    /// The show switches, one per field (title first).
    std::vector<std::unique_ptr<juce::ToggleButton>> shows_;

private:
    bool shown(std::size_t i) const { return i < shows_.size() && shows_[i]->getToggleState(); }
    /// A hidden line's field is dimmed (its text is kept).
    void refresh_shown()
    {
        for (std::size_t i = 0; i < shows_.size() && i < fields_.size(); ++i) fields_[i]->setAlpha(shown(i) ? 1.0f : 0.45f);
        repaint();
    }
    void close()
    {
        if (auto* w = findParentComponentOfClass<juce::DocumentWindow>()) w->closeButtonPressed();
    }
    AppContext& context_;
    std::vector<std::unique_ptr<juce::Label>> labels_;
    std::vector<juce::TextEditor*> fields_;
    juce::ToggleButton show_tempo_, bold_, italic_, caps_, muted_;
    juce::Slider size_;
    juce::TextButton ok_{"Set"}, cancel_{"Cancel"};
    juce::Rectangle<int> preview_area_, auto_area_, show_header_;
    juce::String auto_info_;
};

juce::Component::SafePointer<dialogs::ToolWindow>& score_info_window()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}

}  // namespace

void show_score_info(AppContext& context)
{
    dialogs::show_tool(score_info_window(), "Sheet Info", 560, 590, [&context] { return new ScoreInfoPanel(context); });
}

}  // namespace vcc::desktop
