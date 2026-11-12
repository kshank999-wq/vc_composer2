#include "ui/ScoreExchange.h"

#include "AppResources.h"
#include "vcc/core/Files.h"
#include "vcc/engraving/PdfRenderer.h"
#include "vcc/engraving/PrintLayout.h"
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
        parts.emplace_back(t->name, engraving::lay_out_pages(v->score, engine, po));
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

}  // namespace vcc::desktop
