#include "ui/hw/HwTheme.h"

#include "VccUiFontData.h"

namespace vcc::desktop::hw {

// --- Runtime theme ------------------------------------------------------------------------------

Theme& Theme::get()
{
    static Theme theme;
    return theme;
}

const std::array<Theme::Choice, 4>& Theme::led_choices()
{
    static const std::array<Choice, 4> c{{{"Amber", juce::Colour(0xffffb547)},
                                          {"Red", juce::Colour(0xffff5a4e)},
                                          {"Cyan", juce::Colour(0xff4fd1ff)},
                                          {"Green", juce::Colour(0xff9bf07a)}}};
    return c;
}

const std::array<Theme::Choice, 4>& Theme::lcd_choices()
{
    static const std::array<Choice, 4> c{{{"Cyan", juce::Colour(0xff6ee7d2)},
                                          {"Green", juce::Colour(0xff9bf07a)},
                                          {"Amber", juce::Colour(0xffffc266)},
                                          {"Blue", juce::Colour(0xff9cc2ff)}}};
    return c;
}

juce::Colour Theme::staff_paper() const noexcept { return paper_ ? juce::Colour(0xfff1ece1) : juce::Colour(0xff14171b); }
juce::Colour Theme::ink() const noexcept { return paper_ ? juce::Colour(0xff1b1a18) : juce::Colour(0xffe7ebf0); }
juce::Colour Theme::staff_line() const noexcept { return paper_ ? juce::Colour(0xff8c8579) : juce::Colour(0xff525b66); }
juce::Colour Theme::grid() const noexcept
{
    return paper_ ? juce::Colours::black.withAlpha(0.07f) : juce::Colours::white.withAlpha(0.04f);
}

void Theme::set_led(juce::Colour c)
{
    if (c == led_) return;
    led_ = c;
    changed();
}

void Theme::set_lcd(juce::Colour c)
{
    if (c == lcd_) return;
    lcd_ = c;
    changed();
}

void Theme::set_paper(bool paper)
{
    if (paper == paper_) return;
    paper_ = paper;
    changed();
}

void Theme::attach(juce::PropertiesFile* settings)
{
    settings_ = settings;
    if (!settings_) return;
    auto colour = [&](const char* key, juce::Colour fallback) {
        const auto text = settings_->getValue(key);
        return text.isEmpty() ? fallback : juce::Colour::fromString(text);
    };
    led_ = colour("led_colour", led_);
    lcd_ = colour("lcd_colour", lcd_);
    paper_ = settings_->getBoolValue("staff_paper", paper_);
    sendSynchronousChangeMessage();
}

void Theme::changed()
{
    if (settings_) {
        settings_->setValue("led_colour", led_.toString());
        settings_->setValue("lcd_colour", lcd_.toString());
        settings_->setValue("staff_paper", paper_);
        settings_->saveIfNeeded();
    }
    // Everything reads the theme when it paints: repainting is enough.
    for (int i = juce::TopLevelWindow::getNumTopLevelWindows(); --i >= 0;)
        if (auto* w = juce::TopLevelWindow::getTopLevelWindow(i)) w->repaint();
    sendSynchronousChangeMessage();
}

juce::PropertiesFile& ui_settings()
{
    static juce::PropertiesFile file([] {
        juce::PropertiesFile::Options o;
        o.applicationName = "VC Composer 2 Interface";
        o.filenameSuffix = ".settings";
        o.folderName = "VC Composer 2";
        // The headless check keeps its settings in a throwaway folder.
        if (const auto dir = juce::SystemStats::getEnvironmentVariable("VCC_SETTINGS_DIR", {}); dir.isNotEmpty())
            o.folderName = dir;
        o.osxLibrarySubFolder = "Application Support";
        o.storageFormat = juce::PropertiesFile::storeAsXML;
        return o;
    }());
    return file;
}

// --- Fonts --------------------------------------------------------------------------------------

namespace fonts {
namespace {
struct Faces {
    juce::Typeface::Ptr silk, silk_bold, lcd, ui, ui_semibold;
};

const Faces& faces()
{
    static const Faces f = [] {
        namespace d = vcc_ui_font_data;
        auto load = [](const char* data, int size) {
            return juce::Typeface::createSystemTypefaceFor(data, static_cast<size_t>(size));
        };
        Faces x;
        x.silk = load(d::BarlowCondensedSemiBold_ttf, d::BarlowCondensedSemiBold_ttfSize);
        x.silk_bold = load(d::BarlowCondensedBold_ttf, d::BarlowCondensedBold_ttfSize);
        x.lcd = load(d::ShareTechMonoRegular_ttf, d::ShareTechMonoRegular_ttfSize);
        x.ui = load(d::InterRegular_ttf, d::InterRegular_ttfSize);
        x.ui_semibold = load(d::InterSemiBold_ttf, d::InterSemiBold_ttfSize);
        return x;
    }();
    return f;
}

juce::Font from(const juce::Typeface::Ptr& face, float size, juce::Font fallback)
{
    if (face == nullptr) return fallback;
    return juce::Font(juce::FontOptions(face).withHeight(size));
}
}  // namespace

juce::Font silk(float size, float tracking, bool bold)
{
    auto f = from(bold ? faces().silk_bold : faces().silk, size, juce::Font(juce::FontOptions(size, juce::Font::bold)));
    return f.withExtraKerningFactor(tracking);
}

juce::Font lcd(float size)
{
    return from(faces().lcd, size, juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), size, 0)));
}

juce::Font ui(float size, bool semibold)
{
    return from(semibold ? faces().ui_semibold : faces().ui, size,
                juce::Font(juce::FontOptions(size, semibold ? juce::Font::bold : juce::Font::plain)));
}

juce::Font dynamics(float size)
{
    return juce::Font(juce::FontOptions("Georgia", size, juce::Font::bold | juce::Font::italic));
}

bool loaded()
{
    const auto& f = faces();
    return f.silk && f.silk_bold && f.lcd && f.ui && f.ui_semibold;
}
}  // namespace fonts

}  // namespace vcc::desktop::hw
