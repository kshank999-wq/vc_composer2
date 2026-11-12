#include "AppResources.h"

#include "VccContentInstruments.h"
#include "VccContentRoot.h"
#include "VccContentTemplates.h"
#include "vcc/ui/EmbeddedMusicFont.h"

namespace vcc::desktop {

namespace {

void add_all(std::map<std::string, std::string>& docs, const char* folder, int count, const char* const* names,
             const char* const* original, const char* (*get)(const char*, int&))
{
    for (int i = 0; i < count; ++i) {
        int size = 0;
        const char* data = get(names[i], size);
        if (data == nullptr) continue;
        docs[std::string(folder) + original[i]] = std::string(data, static_cast<std::size_t>(size));
    }
}

}  // namespace

juce::File user_data_directory()
{
    auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("VC Composer 2");
    dir.createDirectory();
    return dir;
}

std::unique_ptr<session::Catalogs> load_embedded_catalogs()
{
    std::map<std::string, std::string> docs;
    add_all(docs, "", vcc_content_root::namedResourceListSize, vcc_content_root::namedResourceList,
                  vcc_content_root::originalFilenames, vcc_content_root::getNamedResource);
    add_all(docs, "instruments/", vcc_content_instruments::namedResourceListSize,
                  vcc_content_instruments::namedResourceList, vcc_content_instruments::originalFilenames,
                  vcc_content_instruments::getNamedResource);
    add_all(docs, "templates/", vcc_content_templates::namedResourceListSize,
                  vcc_content_templates::namedResourceList, vcc_content_templates::originalFilenames,
                  vcc_content_templates::getNamedResource);
    // The composer's own templates (Supplement §3 "Save As New Custom Template").
    auto user = user_data_directory().getChildFile("Templates");
    for (const auto& f : user.findChildFiles(juce::File::findFiles, false, "*.json"))
        docs["templates/user/" + f.getFileName().toStdString()] = f.loadFileAsString().toStdString();
    return session::Catalogs::load_from_documents(docs);
}

MusicFontResources& music_font()
{
    static MusicFontResources r = [] {
        MusicFontResources m;
        m.loaded = ui::load_embedded_music_font(m.font, m.glyphs).usable();
        return m;
    }();
    return r;
}

}  // namespace vcc::desktop
