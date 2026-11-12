#include "ui/console/ConsoleStyle.h"

#include <array>
#include <map>
#include <tuple>
#include <cmath>
#include <utility>

namespace vcc::desktop::console {

namespace {

// (dB, proportion) breakpoints of the printed fader scale.
constexpr std::array<std::pair<double, double>, 11> kFaderLaw{{{-96.0, 0.0},
                                                               {-60.0, 0.05},
                                                               {-50.0, 0.10},
                                                               {-40.0, 0.17},
                                                               {-30.0, 0.27},
                                                               {-20.0, 0.40},
                                                               {-10.0, 0.57},
                                                               {-5.0, 0.67},
                                                               {0.0, 0.77},
                                                               {6.0, 0.885},
                                                               {12.0, 1.0}}};

constexpr std::array<std::pair<double, double>, 11> kMeterLaw{{{-60.0, 0.0},
                                                               {-50.0, 0.09},
                                                               {-40.0, 0.19},
                                                               {-30.0, 0.31},
                                                               {-24.0, 0.40},
                                                               {-18.0, 0.50},
                                                               {-12.0, 0.62},
                                                               {-9.0, 0.69},
                                                               {-6.0, 0.77},
                                                               {-3.0, 0.87},
                                                               {0.0, 1.0}}};

template <std::size_t N>
double interpolate(const std::array<std::pair<double, double>, N>& law, double x, bool forward)
{
    auto key = [&](std::size_t i) { return forward ? law[i].first : law[i].second; };
    auto val = [&](std::size_t i) { return forward ? law[i].second : law[i].first; };
    if (x <= key(0)) return val(0);
    for (std::size_t i = 1; i < N; ++i)
        if (x <= key(i)) {
            const double t = (x - key(i - 1)) / (key(i) - key(i - 1));
            return val(i - 1) + t * (val(i) - val(i - 1));
        }
    return val(N - 1);
}

juce::Image make_brush_texture()
{
    constexpr int w = 256, h = 256;
    juce::Image img(juce::Image::ARGB, w, h, true);
    juce::Random rng(0x5eed);
    juce::Image::BitmapData bits(img, juce::Image::BitmapData::writeOnly);
    std::array<float, w> column{};
    for (auto& c : column) c = rng.nextFloat();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            // Vertical brushing: strong variation across, weak along.
            const float v = column[static_cast<std::size_t>(x)] * 0.75f + rng.nextFloat() * 0.25f;
            const bool light = v > 0.5f;
            const auto a = static_cast<juce::uint8>(std::abs(v - 0.5f) * 2.0f * 34.0f);
            bits.setPixelColour(x, y, light ? juce::Colour(juce::uint8(255), juce::uint8(255), juce::uint8(255), a) : juce::Colour(juce::uint8(0), juce::uint8(0), juce::uint8(0), a));
        }
    return img;
}

const juce::Image& brush_texture()
{
    static const juce::Image texture = make_brush_texture();
    return texture;
}

int gesture_counter = 0;

}  // namespace

double fader_proportion(double db) { return interpolate(kFaderLaw, db, true); }
double fader_db(double proportion) { return interpolate(kFaderLaw, proportion, false); }
double meter_proportion(double dbfs) { return interpolate(kMeterLaw, dbfs, true); }

juce::String db_text(double db, bool unit)
{
    juce::String s;
    if (db <= kMinDb + 0.05) s = juce::String(juce::CharPointer_UTF8("-\xe2\x88\x9e"));
    else if (std::abs(db) < 0.05) s = "0.0";
    else s = (db > 0 ? "+" : "") + juce::String(db, 1);
    return unit ? s + " dB" : s;
}

juce::String freq_text(double hz)
{
    if (hz >= 1000.0) return juce::String(hz / 1000.0, hz >= 10000.0 ? 1 : 2) + " kHz";
    return juce::String(juce::roundToInt(hz)) + " Hz";
}

juce::String pan_text(double pan)
{
    const int v = juce::roundToInt(pan * 100.0);
    if (v == 0) return "C";
    return (v < 0 ? "L" : "R") + juce::String(std::abs(v));
}

juce::String ms_text(double ms)
{
    if (ms >= 1000.0) return juce::String(ms / 1000.0, 2) + " s";
    if (ms < 10.0) return juce::String(ms, 1) + " ms";
    return juce::String(juce::roundToInt(ms)) + " ms";
}

juce::Colour automation_colour(const std::string& mode)
{
    if (mode == "read") return paint::led_green;
    if (mode == "touch") return paint::led_yellow;
    if (mode == "latch") return juce::Colour(0xffff8a1e);
    if (mode == "write") return paint::led_red;
    return juce::Colours::transparentBlack;
}

juce::String automation_label(const std::string& mode) { return juce::String(mode.empty() ? "off" : mode).toUpperCase(); }

juce::Font print_font(float size, bool bold)
{
    return juce::Font(juce::FontOptions(size, bold ? juce::Font::bold : juce::Font::plain)).withHorizontalScale(0.92f);
}

void fill_panel(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour base)
{
    juce::ColourGradient grad(base.brighter(0.10f), area.getX(), area.getY(), base.darker(0.18f), area.getX(),
                              area.getBottom(), false);
    g.setGradientFill(grad);
    g.fillRect(area);
    // A soft highlight down the middle, like light on satin paint.
    juce::ColourGradient sheen(juce::Colours::white.withAlpha(0.0f), area.getX(), 0.0f,
                               juce::Colours::white.withAlpha(0.0f), area.getRight(), 0.0f, false);
    sheen.addColour(0.5, juce::Colours::white.withAlpha(0.035f));
    g.setGradientFill(sheen);
    g.fillRect(area);
    g.setTiledImageFill(brush_texture(), 0, 0, 0.55f);
    g.fillRect(area);
}

float pixel_scale(juce::Graphics& g)
{
    return juce::jlimit(1.0f, 4.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
}

void draw_scaled(juce::Graphics& g, const juce::Image& image, juce::Point<float> top_left, float scale)
{
    if (scale <= 1.0001f) {
        g.drawImageAt(image, juce::roundToInt(top_left.x), juce::roundToInt(top_left.y));
        return;
    }
    g.drawImageTransformed(image, juce::AffineTransform::scale(1.0f / scale).translated(top_left.x, top_left.y));
}

void fill_panel_cached(juce::Graphics& g, juce::Rectangle<int> area, juce::Colour base)
{
    using Key = std::tuple<juce::uint32, int, int, int>;
    static std::map<Key, juce::Image> cache;
    const float scale = pixel_scale(g);
    const Key key{base.getARGB(), area.getWidth(), area.getHeight(), juce::roundToInt(scale * 100.0f)};
    auto it = cache.find(key);
    if (it == cache.end()) {
        if (cache.size() > 24) cache.clear();
        const int w = juce::jmax(1, juce::roundToInt(static_cast<float>(area.getWidth()) * scale));
        const int h = juce::jmax(1, juce::roundToInt(static_cast<float>(area.getHeight()) * scale));
        juce::Image img(juce::Image::RGB, w, h, false);
        juce::Graphics ig(img);
        ig.addTransform(juce::AffineTransform::scale(scale));
        fill_panel(ig, area.withZeroOrigin().toFloat(), base);
        it = cache.emplace(key, img).first;
    }
    draw_scaled(g, it->second, area.getTopLeft().toFloat(), scale);
}

void print_text(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, float size,
                juce::Justification just, juce::Colour colour)
{
    g.setFont(print_font(size));
    g.setColour(colour);
    g.drawText(text, area, just, false);
}

void print_heading(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, juce::Colour colour)
{
    const auto font = print_font(8.0f);
    g.setFont(font);
    const float tw = juce::GlyphArrangement::getStringWidth(font, text) + 8.0f;
    const float cy = area.getCentreY();
    g.setColour(colour.withAlpha(0.35f));
    const float side = (area.getWidth() - tw) * 0.5f;
    if (side > 4.0f) {
        g.fillRect(area.getX() + 3.0f, cy, side - 3.0f, 1.0f);
        g.fillRect(area.getRight() - side, cy, side - 3.0f, 1.0f);
    }
    g.setColour(colour);
    g.drawText(text, area, juce::Justification::centred, false);
}

void draw_well(juce::Graphics& g, juce::Rectangle<float> area, float corner)
{
    g.setColour(juce::Colours::white.withAlpha(0.07f));
    g.drawRoundedRectangle(area.translated(0.0f, 0.8f).reduced(-0.5f), corner + 0.5f, 1.0f);
    g.setColour(paint::well);
    g.fillRoundedRectangle(area, corner);
    g.setColour(juce::Colours::black.withAlpha(0.8f));
    g.drawRoundedRectangle(area, corner, 1.0f);
}

void begin_gesture() { ++gesture_counter; }
std::string gesture_suffix() { return (gesture_counter & 1) != 0 ? " " : ""; }

void show_tip(juce::Component& source, const juce::String& text)
{
    if (auto* host = source.findParentComponentOfClass<TipHost>()) host->show_tip(source, text);
}

void hide_tip(juce::Component& source)
{
    if (auto* host = source.findParentComponentOfClass<TipHost>()) host->hide_tip();
}

}  // namespace vcc::desktop::console
