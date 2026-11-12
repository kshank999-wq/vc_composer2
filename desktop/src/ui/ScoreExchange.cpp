#include "ui/ScoreExchange.h"

#include "AppResources.h"
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
    for (const auto* t : context.session.music_tracks()) {
        const auto* v = t->active_version();
        if (!v || t->kind != project::TrackKind::Instrument || !t->include_in_export) continue;
        engraving::PrintOptions po;
        po.title = d.production.title.empty() ? d.name : d.production.title;
        po.subtitle = t->name;
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

int export_full_score_pdf(AppContext& context, const juce::File& file, const FullScoreSettings& settings,
                          juce::String& message)
{
    const auto& fonts = music_font();
    engraving::LayoutEngine engine;
    if (fonts.loaded) engine.set_glyph_table(fonts.glyphs);
    const auto& d = context.session.data();

    std::vector<notation::Score> scores;  // the printed views, kept alive for the layout
    std::vector<const project::Track*> tracks;
    for (const auto* t : context.session.music_tracks()) {
        const auto* v = t->active_version();
        if (!v || t->kind != project::TrackKind::Instrument || !t->include_in_export) continue;
        tracks.push_back(t);
    }
    if (tracks.empty()) {
        message = "Nothing to print: no instrument track holds notation";
        return 0;
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

    engraving::ScoreOptions opts;
    double w = 297, h = 420, space = 1.25;  // A3, 5 mm staff
    if (settings.paper == "Tabloid") { w = 279.4; h = 431.8; }
    else if (settings.paper == "A4") { w = 210; h = 297; space = 1.0; }
    else if (settings.paper == "Letter") { w = 215.9; h = 279.4; space = 1.0; }
    // Large orchestras get a smaller staff so a system fits the page.
    const std::size_t staves = [&] {
        std::size_t n = 0;
        for (const auto& sc : scores) n += sc.staff_count();
        return n;
    }();
    const double needed = static_cast<double>(staves) * 11.0 + 30.0;  // spaces per staff plus title room
    if (needed * space > h - 24.0) space = std::max(0.7, (h - 24.0) / needed);
    engraving::page_size_for(w, h, space, opts);
    opts.title = d.production.title.empty() ? d.name : d.production.title;
    opts.subtitle = settings.concert_pitch ? "Score in C" : "Transposed score";
    opts.composer = d.production.composer;
    opts.hide_empty_staves = settings.hide_empty_staves;
    opts.bars_per_system = std::max(1, settings.bars_per_system);
    const auto laid = engraving::lay_out_score(parts, engine, opts);

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
    message = "Printed the full score (" + juce::String(static_cast<int>(parts.size())) + " parts, " +
              juce::String(pages) + " page" + (pages == 1 ? "" : "s") + ", " +
              juce::String(settings.concert_pitch ? "concert pitch" : "transposed") + ") to " + file.getFileName();
    if (laid.crowded_system_count > 0)
        message << " - " << juce::String(static_cast<int>(laid.crowded_system_count))
                << " system(s) are crowded; try fewer bars per system";
    return pages;
}

}  // namespace vcc::desktop
