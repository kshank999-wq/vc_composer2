#include "vcc/ui/MusicFontSearch.h"

#include "vcc/ui/EmbeddedMusicFont.h"

namespace vcc::ui {

const char* to_string(MusicFontSource source) noexcept
{
    switch (source) {
        case MusicFontSource::None:                  return "none";
        case MusicFontSource::NamedDirectory:        return "named directory";
        case MusicFontSource::Embedded:              return "built in";
        case MusicFontSource::ConventionalDirectory: return "SMuFL directory";
        case MusicFontSource::InstalledFont:         return "installed font";
    }
    return "none";
}

juce::File music_font_directory_from_environment()
{
    const juce::String value = juce::SystemStats::getEnvironmentVariable("VCC_MUSIC_FONT_DIR", {});
    if (value.isEmpty())
        return {};

    // A relative path here would resolve against whatever directory the host
    // happened to start the plug-in in, which is not something a composer
    // can reason about.
    if (!juce::File::isAbsolutePath(value))
        return {};
    return juce::File(value);
}

juce::Array<juce::File> conventional_music_font_directories()
{
    juce::Array<juce::File> directories;

   #if JUCE_MAC
    directories.add(juce::File("/Library/Application Support/SMuFL/Fonts"));
    directories.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                        .getChildFile("Library/Application Support/SMuFL/Fonts"));
   #elif JUCE_WINDOWS
    const juce::String common =
        juce::SystemStats::getEnvironmentVariable("CommonProgramFiles", {});
    if (common.isNotEmpty())
        directories.add(juce::File(common).getChildFile("SMuFL\\Fonts"));
   #elif JUCE_LINUX || JUCE_BSD
    directories.add(juce::File("/usr/share/SMuFL/Fonts"));
    directories.add(juce::File("/usr/local/share/SMuFL/Fonts"));
    directories.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                        .getChildFile(".local/share/SMuFL/Fonts"));
   #endif

    return directories;
}

namespace {

/// Tries one directory, and every immediate subdirectory of it.
///
/// A SMuFL font directory holds one folder per font - `Bravura/`,
/// `Petaluma/` - rather than the font files themselves, so looking only at
/// the directory named would find nothing in the place the convention puts
/// things.
MusicFontLoad try_directory(MusicFont& font, engraving::GlyphTable& table,
                            const juce::File& directory, juce::String& from)
{
    MusicFontLoad result = font.load_from_directory(directory, table);
    if (result.usable()) {
        from = directory.getFullPathName();
        return result;
    }

    juce::Array<juce::File> children;
    directory.findChildFiles(children, juce::File::findDirectories, false);
    for (const juce::File& child : children) {
        MusicFontLoad inner = font.load_from_directory(child, table);
        if (inner.usable()) {
            from = child.getFullPathName();
            return inner;
        }
    }
    return result;
}

}  // namespace

MusicFontSearchResult find_music_font(MusicFont& font, engraving::GlyphTable& table,
                                      const juce::File& named_directory)
{
    MusicFontSearchResult result;

    const auto attempt = [&result](const juce::String& where, const MusicFontLoad& load) {
        result.attempts.add(where + ": " + load.message);
    };

    // 1. What the composer named, if anything.
    const juce::File named =
        named_directory != juce::File() ? named_directory : music_font_directory_from_environment();
    if (named != juce::File()) {
        const MusicFontLoad load = try_directory(font, table, named, result.from);
        attempt(named.getFullPathName(), load);
        if (load.usable()) {
            result.source = MusicFontSource::NamedDirectory;
            result.load = load;
            return result;
        }
    }

    // 2. The font built into this binary. Always there, so this is what a
    // composer sees unless they named something else.
    {
        const MusicFontLoad load = load_embedded_music_font(font, table);
        attempt(embedded_music_font_name(), load);
        if (load.usable()) {
            result.source = MusicFontSource::Embedded;
            result.from = embedded_music_font_name();
            result.load = load;
            return result;
        }
    }

    // 3. Where the convention puts them.
    for (const juce::File& directory : conventional_music_font_directories()) {
        const MusicFontLoad load = try_directory(font, table, directory, result.from);
        attempt(directory.getFullPathName(), load);
        if (load.usable()) {
            result.source = MusicFontSource::ConventionalDirectory;
            result.load = load;
            return result;
        }
    }

    // 4. Whatever is installed. Last, because it brings no metadata - a
    // font with its metadata is worth more than a font without, and this is
    // the one that gives up the metrics.
    const juce::String family = MusicFont::first_installed_font_family();
    if (family.isNotEmpty()) {
        const MusicFontLoad load = font.load_installed(family, table);
        attempt(family, load);
        if (load.usable()) {
            result.source = MusicFontSource::InstalledFont;
            result.from = family;
            result.load = load;
            return result;
        }
    } else {
        result.attempts.add("installed fonts: none of "
                            + MusicFont::known_font_families().joinIntoString(", ")
                            + " is installed");
    }

    result.load.message = "No music font found; notation will be drawn with stand-in shapes";
    return result;
}

}  // namespace vcc::ui
