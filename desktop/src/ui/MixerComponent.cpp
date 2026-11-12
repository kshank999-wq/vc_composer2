#include "ui/MixerComponent.h"

#include <cmath>

#include "ui/Theme.h"
#include "ui/TrackPanel.h"

namespace vcc::desktop {

namespace {

// --- Geometry and scales ------------------------------------------------------------------

constexpr int kStripWidth = 84;
constexpr int kMasterWidth = 100;
constexpr int kSectionGap = 12;
constexpr int kHeadHeight = 146;    // everything above the fader, the same for every strip
constexpr int kFootHeight = 44;     // dB readout and the bottom row
constexpr double kMinDb = -60.0;
constexpr double kMaxDb = 12.0;
constexpr double kUnityPos = 0.75;  // 0 dB sits three quarters of the way up
constexpr double kCurve = 0.7;

const juce::Colour kStripTop{0xff2b3037};
const juce::Colour kStripBottom{0xff23272d};
const juce::Colour kWell{0xff15171b};
const juce::Colour kGroupColour{0xff5b8fd6};
const juce::Colour kFxColour{0xffa07ad6};
const juce::Colour kMasterColour{0xffd0d4da};

/// Fader law: -60..0 dB on a gentle curve over the lower 75 %, 0..+12 dB linear above.
double db_to_proportion(double db)
{
    db = juce::jlimit(kMinDb, kMaxDb, db);
    if (db >= 0.0) return kUnityPos + (1.0 - kUnityPos) * db / kMaxDb;
    return kUnityPos * (1.0 - std::pow(-db / -kMinDb, kCurve));
}

double proportion_to_db(double p)
{
    p = juce::jlimit(0.0, 1.0, p);
    if (p >= kUnityPos) return (p - kUnityPos) / (1.0 - kUnityPos) * kMaxDb;
    return kMinDb * std::pow(1.0 - p / kUnityPos, 1.0 / kCurve);
}

juce::NormalisableRange<double> fader_range()
{
    juce::NormalisableRange<double> r(
        kMinDb, kMaxDb, [](double, double, double p) { return proportion_to_db(p); },
        [](double, double, double v) { return db_to_proportion(v); },
        [](double, double, double v) { return juce::jlimit(kMinDb, kMaxDb, std::round(v * 10.0) / 10.0); });
    return r;
}

juce::String db_text(double db)
{
    if (db <= kMinDb + 0.05) return "-inf";
    if (std::abs(db) < 0.05) return "0.0";
    return (db > 0 ? "+" : "") + juce::String(db, 1);
}

juce::String pan_text(double pan)
{
    const int v = juce::roundToInt(pan * 100.0);
    if (v == 0) return "C";
    return (v < 0 ? "L" : "R") + juce::String(std::abs(v));
}

juce::String short_db(double db)
{
    const int v = juce::roundToInt(db);
    return (v > 0 ? "+" : "") + juce::String(v);
}

// --- Look ------------------------------------------------------------------------------------

class MixerLook final : public juce::LookAndFeel_V4 {
public:
    MixerLook()
    {
        using namespace colours;
        auto scheme = juce::LookAndFeel_V4::getDarkColourScheme();
        scheme.setUIColour(ColourScheme::windowBackground, background);
        scheme.setUIColour(ColourScheme::widgetBackground, panel_raised);
        scheme.setUIColour(ColourScheme::menuBackground, panel);
        scheme.setUIColour(ColourScheme::outline, outline);
        scheme.setUIColour(ColourScheme::defaultText, text);
        scheme.setUIColour(ColourScheme::defaultFill, accent);
        scheme.setUIColour(ColourScheme::highlightedText, juce::Colours::white);
        scheme.setUIColour(ColourScheme::highlightedFill, accent);
        scheme.setUIColour(ColourScheme::menuText, text);
        setColourScheme(scheme);
        setColour(juce::TextButton::buttonColourId, panel_raised);
        setColour(juce::TextButton::textColourOffId, text);
        setColour(juce::PopupMenu::backgroundColourId, panel);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, accent);
        setColour(juce::ScrollBar::thumbColourId, outline.brighter(0.2f));
        setColour(juce::ComboBox::backgroundColourId, kWell);
        setColour(juce::ComboBox::outlineColourId, colours::outline);
        setColour(juce::ComboBox::textColourId, colours::text);
        setColour(juce::ComboBox::arrowColourId, colours::text_dim);
        setColour(juce::Label::textColourId, colours::text);
        setColour(juce::Label::backgroundWhenEditingColourId, kWell);
        setColour(juce::Label::textWhenEditingColourId, colours::text);
        setColour(juce::Label::outlineWhenEditingColourId, colours::accent);
        setColour(juce::TextEditor::highlightColourId, colours::selection);
        setColour(juce::TooltipWindow::backgroundColourId, colours::panel_raised);
    }

    juce::Font getComboBoxFont(juce::ComboBox&) override { return juce::Font(juce::FontOptions(11.0f)); }

    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds(1, 1, box.getWidth() - 14, box.getHeight() - 2);
        label.setFont(getComboBoxFont(box));
        label.setBorderSize({0, 4, 0, 0});
    }

    void drawComboBox(juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<float>(0, 0, static_cast<float>(w), static_cast<float>(h)).reduced(0.5f);
        g.setColour(box.findColour(juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle(r, 3.0f);
        g.setColour(box.hasKeyboardFocus(true) ? colours::accent : colours::outline);
        g.drawRoundedRectangle(r, 3.0f, 1.0f);
        juce::Path arrow;
        const float cx = static_cast<float>(w) - 8.0f, cy = static_cast<float>(h) * 0.5f;
        arrow.addTriangle(cx - 3.0f, cy - 1.5f, cx + 3.0f, cy - 1.5f, cx, cy + 2.5f);
        g.setColour(box.isEnabled() ? colours::text_dim : colours::outline);
        g.fillPath(arrow);
    }

    void drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour& base, bool highlighted,
                              bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced(0.5f);
        auto c = base;
        if (down) c = c.brighter(0.15f);
        else if (highlighted) c = c.brighter(0.08f);
        g.setColour(c);
        g.fillRoundedRectangle(r, 3.0f);
        g.setColour(b.getToggleState() ? c.darker(0.4f) : colours::outline);
        g.drawRoundedRectangle(r, 3.0f, 1.0f);
    }

    juce::Font getTextButtonFont(juce::TextButton&, int height) override
    {
        return juce::Font(juce::FontOptions(juce::jmin(12.0f, static_cast<float>(height) * 0.6f), juce::Font::bold));
    }

    int getSliderThumbRadius(juce::Slider& s) override
    {
        return s.isVertical() ? 10 : LookAndFeel_V4::getSliderThumbRadius(s);
    }

    void drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                          juce::Slider::SliderStyle, juce::Slider& s) override
    {
        const float cx = static_cast<float>(x) + static_cast<float>(w) * 0.5f;
        // Groove
        const float top = static_cast<float>(y), bottom = static_cast<float>(y + h);
        juce::Rectangle<float> groove(cx - 2.5f, top - 2.0f, 5.0f, bottom - top + 4.0f);
        g.setColour(kWell);
        g.fillRoundedRectangle(groove, 2.5f);
        g.setColour(juce::Colours::black.withAlpha(0.5f));
        g.drawRoundedRectangle(groove, 2.5f, 1.0f);
        // Level fill from the bottom up to the cap, in the strip colour
        const auto tint = s.findColour(juce::Slider::trackColourId);
        g.setColour(tint.withAlpha(0.55f));
        g.fillRoundedRectangle(groove.withTop(pos).reduced(1.0f, 0.0f), 1.5f);

        // Cap
        juce::Rectangle<float> cap(cx - 13.0f, pos - 9.0f, 26.0f, 18.0f);
        g.setColour(juce::Colours::black.withAlpha(0.45f));
        g.fillRoundedRectangle(cap.translated(0.0f, 2.0f), 3.0f);
        juce::ColourGradient grad(juce::Colour(0xff8a929c), cap.getX(), cap.getY(), juce::Colour(0xff4c535c),
                                  cap.getX(), cap.getBottom(), false);
        grad.addColour(0.48, juce::Colour(0xff6c747e));
        grad.addColour(0.52, juce::Colour(0xff5a616a));
        g.setGradientFill(grad);
        g.fillRoundedRectangle(cap, 3.0f);
        g.setColour(juce::Colours::black.withAlpha(0.6f));
        g.drawRoundedRectangle(cap, 3.0f, 1.0f);
        g.setColour(s.isMouseButtonDown() ? colours::accent : juce::Colours::white.withAlpha(0.9f));
        g.fillRect(cap.getX() + 3.0f, pos - 0.75f, cap.getWidth() - 6.0f, 1.5f);
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float start, float end,
                          juce::Slider& s) override
    {
        const auto bounds = juce::Rectangle<int>(x, y, w, h).toFloat().reduced(2.0f);
        const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto c = bounds.getCentre();
        const float angle = start + pos * (end - start);
        const float centre = (start + end) * 0.5f;

        // Track arc
        juce::Path track;
        track.addCentredArc(c.x, c.y, radius - 1.5f, radius - 1.5f, 0.0f, start, end, true);
        g.setColour(kWell);
        g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        // Value arc from centre
        if (std::abs(angle - centre) > 0.01f) {
            juce::Path value;
            value.addCentredArc(c.x, c.y, radius - 1.5f, radius - 1.5f, 0.0f, juce::jmin(angle, centre),
                                juce::jmax(angle, centre), true);
            g.setColour(s.findColour(juce::Slider::rotarySliderFillColourId));
            g.strokePath(value, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        // Knob body
        const float kr = radius - 5.0f;
        juce::ColourGradient grad(juce::Colour(0xff5a616b), c.x, c.y - kr, juce::Colour(0xff30353c), c.x, c.y + kr, false);
        g.setGradientFill(grad);
        g.fillEllipse(c.x - kr, c.y - kr, kr * 2.0f, kr * 2.0f);
        g.setColour(juce::Colours::black.withAlpha(0.6f));
        g.drawEllipse(c.x - kr, c.y - kr, kr * 2.0f, kr * 2.0f, 1.0f);
        // Pointer
        juce::Path pointer;
        pointer.addRoundedRectangle(-1.0f, -kr + 1.5f, 2.0f, kr * 0.6f, 1.0f);
        g.setColour(colours::text);
        g.fillPath(pointer, juce::AffineTransform::rotation(angle).translated(c.x, c.y));
    }
};

// --- Meter -----------------------------------------------------------------------------------

/// Peak meter on the fader's dB law, with peak hold, decay and a clip light.
class LevelMeter final : public juce::Component, public juce::SettableTooltipClient {
public:
    void set_enabled_meter(bool on)
    {
        active_ = on;
        repaint();
    }

    /// One 30 Hz tick with the peak since the last tick (linear, 0..1+).
    void push(float peak)
    {
        const double in = peak > 0.0f ? 20.0 * std::log10(static_cast<double>(peak)) : -200.0;
        const double before_level = level_, before_hold = hold_;
        const bool before_clip = clip_ > 0;
        level_ = in > level_ ? in : level_ - kFallPerTick;
        if (in >= hold_) {
            hold_ = in;
            hold_ticks_ = 45;
        } else if (hold_ticks_ > 0) {
            --hold_ticks_;
        } else {
            hold_ -= kFallPerTick;
        }
        if (in > 0.0) clip_ = 90;
        else if (clip_ > 0) --clip_;
        level_ = juce::jmax(level_, -200.0);
        hold_ = juce::jmax(hold_, -200.0);
        const bool visible = level_ > kMinDb || before_level > kMinDb || hold_ > kMinDb || before_hold > kMinDb;
        if ((visible && (std::abs(level_ - before_level) > 1e-6 || std::abs(hold_ - before_hold) > 1e-6)) || before_clip != (clip_ > 0)) repaint();
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        clip_ = 0;
        hold_ = -200.0;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        auto light = r.removeFromTop(5.0f);
        r.removeFromTop(5.0f);
        g.setColour(clip_ > 0 ? juce::Colour(0xffff4a3d) : kWell);
        g.fillRoundedRectangle(light, 1.5f);
        g.setColour(kWell);
        g.fillRoundedRectangle(r.expanded(0.0f, 1.0f), 1.5f);
        g.setColour(juce::Colours::black.withAlpha(0.6f));
        g.drawRoundedRectangle(r.expanded(0.0f, 1.0f), 1.5f, 1.0f);
        if (!active_) {
            // No metering on this strip yet: hatched, so it does not read as silence.
            g.setColour(colours::outline.withAlpha(0.5f));
            for (float yy = r.getY() + 2.0f; yy < r.getBottom() - 2.0f; yy += 6.0f)
                g.drawLine(r.getX() + 1.0f, yy + 3.0f, r.getRight() - 1.0f, yy, 1.0f);
            return;
        }
        const auto y_of = [&](double db) {
            return r.getBottom() - static_cast<float>(db_to_proportion(db)) * r.getHeight();
        };
        auto bar = r.reduced(1.0f, 0.0f);
        if (level_ > kMinDb) {
            const float y = y_of(level_);
            const float y0 = y_of(0.0), y6 = y_of(-6.0);
            juce::Rectangle<float> lit(bar.getX(), y, bar.getWidth(), bar.getBottom() - y);
            g.setColour(juce::Colour(0xff3fbf6a));
            g.fillRect(lit.withTop(juce::jmax(y, y6)));
            if (y < y6) {
                g.setColour(juce::Colour(0xffe0c341));
                g.fillRect(lit.withTop(juce::jmax(y, y0)).withBottom(y6));
            }
            if (y < y0) {
                g.setColour(juce::Colour(0xffff4a3d));
                g.fillRect(lit.withBottom(y0));
            }
            // Segment lines for an LED look.
            g.setColour(kWell.withAlpha(0.55f));
            for (float yy = bar.getBottom() - 3.0f; yy > y; yy -= 3.0f) g.drawHorizontalLine(juce::roundToInt(yy), bar.getX(), bar.getRight());
        }
        if (hold_ > kMinDb) {
            const float y = y_of(hold_);
            g.setColour(hold_ > 0.0 ? juce::Colour(0xffff4a3d) : colours::text);
            g.fillRect(bar.getX(), y - 1.0f, bar.getWidth(), 2.0f);
        }
    }

private:
    static constexpr double kFallPerTick = 24.0 / 30.0;  // 24 dB per second
    bool active_ = true;
    double level_ = -200.0, hold_ = -200.0;
    int hold_ticks_ = 0, clip_ = 0;
};

// --- Strips ----------------------------------------------------------------------------------

class Strip : public juce::Component {
public:
    explicit Strip(AppContext& c) : context_(c)
    {
        fader_.setSliderStyle(juce::Slider::LinearVertical);
        fader_.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        fader_.setNormalisableRange(fader_range());
        fader_.setDoubleClickReturnValue(true, 0.0);
        fader_.setMouseDragSensitivity(300);
        fader_.setVelocityModeParameters(0.4, 1, 0.0, false, juce::ModifierKeys::shiftModifier);
        fader_.onValueChange = [this] {
            write_gain(fader_.getValue());
            readout_.setText(db_text(fader_.getValue()), juce::dontSendNotification);
        };
        addAndMakeVisible(fader_);
        addAndMakeVisible(meter_);

        readout_.setJustificationType(juce::Justification::centred);
        readout_.setFont(juce::FontOptions(11.0f));
        readout_.setEditable(false, true, false);
        readout_.setColour(juce::Label::backgroundColourId, kWell);
        readout_.setColour(juce::Label::outlineColourId, colours::outline);
        readout_.setTooltip("Gain in dB - double-click to type a value");
        readout_.onTextChange = [this] {
            auto t = readout_.getText().trim().toLowerCase();
            const double v = t.startsWith("-inf") ? kMinDb : t.retainCharacters("+-.0123456789").getDoubleValue();
            fader_.setValue(juce::jlimit(kMinDb, kMaxDb, v), juce::sendNotificationSync);
            readout_.setText(db_text(fader_.getValue()), juce::dontSendNotification);
        };
        addAndMakeVisible(readout_);
    }

    virtual void refresh() = 0;
    virtual void tick(bool) {}
    virtual juce::String key() const = 0;
    void set_number(int n) { number_ = n; }

    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced(1.0f, 0.0f);
        juce::ColourGradient grad(kStripTop, 0.0f, 0.0f, kStripBottom, 0.0f, r.getBottom(), false);
        g.setGradientFill(grad);
        g.fillRoundedRectangle(r, 3.0f);
        g.setColour(selected() ? colours::accent.withAlpha(0.8f) : juce::Colours::black.withAlpha(0.55f));
        g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, selected() ? 1.5f : 1.0f);
        g.setColour(colour());
        g.fillRoundedRectangle(r.withHeight(4.0f).reduced(2.0f, 0.0f).translated(0.0f, 1.0f), 1.5f);

        // Name: two lines, centred, ellipsised.
        g.setColour(name_colour());
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.drawFittedText(name(), juce::Rectangle<int>(5, 7, getWidth() - 10, 28), juce::Justification::centred, 2, 0.85f);

        paint_scale(g);

        // Bottom: channel number / caption.
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(10.0f));
        const auto foot = juce::Rectangle<int>(4, getHeight() - 20, getWidth() - 8, 16);
        if (!caption().isEmpty()) g.drawText(caption(), foot, juce::Justification::centred, true);
        paint_extra(g);
    }

    void resized() override
    {
        auto fz = fader_zone();
        const int total = 28 + 20 + 10;
        auto row = fz.withSizeKeepingCentre(total, fz.getHeight());
        fader_.setBounds(row.removeFromLeft(28));
        scale_ = row.removeFromLeft(20);
        // The meter shares the fader's dB law and spans exactly the fader's travel.
        const int top = fader_.getY() + juce::roundToInt(fader_.getPositionOfValue(kMaxDb));
        const int bottom = fader_.getY() + juce::roundToInt(fader_.getPositionOfValue(kMinDb));
        meter_.setBounds(row.removeFromLeft(10).withY(top - 10).withHeight(bottom - top + 10));
        readout_.setBounds(getLocalBounds().reduced(10, 0).withTop(getHeight() - kFootHeight + 2).withHeight(17));
        layout_head();
    }

protected:
    virtual void write_gain(double db) = 0;
    virtual juce::String name() const = 0;
    virtual juce::Colour colour() const = 0;
    virtual juce::Colour name_colour() const { return colours::text; }
    virtual juce::String caption() const { return number_ > 0 ? juce::String(number_) : juce::String(); }
    virtual bool selected() const { return false; }
    virtual void layout_head() {}
    virtual void paint_extra(juce::Graphics&) {}

    juce::Rectangle<int> fader_zone() const
    {
        return {0, kHeadHeight + 4, getWidth(), juce::jmax(40, getHeight() - kHeadHeight - kFootHeight - 4)};
    }

    void set_gain_display(double db)
    {
        fader_.setColour(juce::Slider::trackColourId, colour());
        if (!fader_.isMouseButtonDown()) fader_.setValue(db, juce::dontSendNotification);
        if (!readout_.isBeingEdited()) readout_.setText(db_text(fader_.getValue()), juce::dontSendNotification);
    }

    void paint_scale(juce::Graphics& g)
    {
        if (scale_.isEmpty()) return;
        g.setFont(juce::FontOptions(9.0f));
        for (int db : {12, 6, 0, -6, -12, -20, -30, -40, -60}) {
            const float y = static_cast<float>(fader_.getY()) + fader_.getPositionOfValue(db);
            g.setColour(db == 0 ? colours::text : colours::text_dim.withAlpha(0.8f));
            g.drawText(db == -60 ? juce::String("inf") : juce::String(std::abs(db)),
                       juce::Rectangle<float>(static_cast<float>(scale_.getX()), y - 5.0f, static_cast<float>(scale_.getWidth()), 10.0f),
                       juce::Justification::centred, false);
            g.setColour(colours::outline);
            g.drawHorizontalLine(juce::roundToInt(y), static_cast<float>(fader_.getRight() - 6), static_cast<float>(fader_.getRight() - 2));
        }
    }

    void style_button(juce::TextButton& b, juce::Colour on, const juce::String& tip)
    {
        b.setClickingTogglesState(true);
        b.setColour(juce::TextButton::buttonColourId, kWell);
        b.setColour(juce::TextButton::buttonOnColourId, on);
        b.setColour(juce::TextButton::textColourOffId, colours::text_dim);
        b.setColour(juce::TextButton::textColourOnId, juce::Colour(0xff15171b));
        b.setTooltip(tip);
        addAndMakeVisible(b);
    }

    AppContext& context_;
    juce::Slider fader_;
    LevelMeter meter_;
    juce::Label readout_;
    juce::Rectangle<int> scale_;
    int number_ = 0;
};

/// The output routing choices shared by the strips: Master, then group and fx buses.
struct Routing {
    std::vector<std::string> ids;
    std::vector<std::string> names;

    static Routing from(const project::ProjectData& d)
    {
        Routing r;
        const project::Bus* master = nullptr;
        for (const auto& b : d.buses)
            if (b.kind == "master") master = &b;
        r.ids.push_back(master ? master->id : std::string("bus-master"));
        r.names.push_back("Master");
        for (const char* kind : {"group", "fx"})
            for (const auto& b : d.buses)
                if (b.kind == kind) {
                    r.ids.push_back(b.id);
                    r.names.push_back(b.name);
                }
        return r;
    }
    bool operator==(const Routing& o) const { return ids == o.ids && names == o.names; }
    bool operator!=(const Routing& o) const { return !(*this == o); }
};

/// A clickable line of small text (the instrument status: opens the plug-in window).
class StatusLine final : public juce::Component, public juce::SettableTooltipClient {
public:
    std::function<void()> onClick;
    void set(const juce::String& text, juce::Colour c, bool clickable)
    {
        if (text == text_ && c == colour_ && clickable == clickable_) return;
        text_ = text;
        colour_ = c;
        clickable_ = clickable;
        setMouseCursor(clickable ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        setTooltip(clickable ? text + " - click to open the plug-in window" : text);
        repaint();
    }
    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour(colour_);
        g.fillEllipse(r.getX() + 1.0f, r.getCentreY() - 2.5f, 5.0f, 5.0f);
        g.setColour(clickable_ && isMouseOver() ? colours::text : colours::text_dim);
        g.setFont(juce::FontOptions(10.0f));
        g.drawFittedText(text_, getLocalBounds().withTrimmedLeft(9), juce::Justification::centredLeft, 1, 0.7f);
    }
    void mouseEnter(const juce::MouseEvent&) override { repaint(); }
    void mouseExit(const juce::MouseEvent&) override { repaint(); }
    void mouseUp(const juce::MouseEvent& e) override
    {
        if (clickable_ && onClick && contains(e.getPosition())) onClick();
    }

private:
    juce::String text_;
    juce::Colour colour_;
    bool clickable_ = false;
};

class TrackStrip final : public Strip {
public:
    TrackStrip(AppContext& c, std::string id) : Strip(c), id_(std::move(id))
    {
        addAndMakeVisible(status_);
        status_.onClick = [this] {
            if (context_.show_plugin_window) context_.show_plugin_window(id_);
        };

        output_.setTooltip("Output routing");
        output_.onChange = [this] {
            const int i = output_.getSelectedId() - 1;
            if (i >= 0 && i < static_cast<int>(routing_.ids.size())) context_.session.set_output_bus(id_, routing_.ids[static_cast<std::size_t>(i)]);
        };
        addAndMakeVisible(output_);

        pan_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        pan_.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        pan_.setRange(-1.0, 1.0, 0.01);
        pan_.setRotaryParameters(juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        pan_.setDoubleClickReturnValue(true, 0.0);
        pan_.setMouseDragSensitivity(160);
        pan_.setColour(juce::Slider::rotarySliderFillColourId, colours::accent);
        pan_.setTooltip("Pan - double-click to centre");
        pan_.onValueChange = [this] {
            context_.session.set_pan(id_, pan_.getValue());
            repaint(pan_text_area());
        };
        addAndMakeVisible(pan_);

        style_button(mute_, colours::mute, "Mute");
        style_button(solo_, colours::solo, "Solo");
        mute_.onClick = [this] { context_.session.set_mute(id_, mute_.getToggleState()); };
        solo_.onClick = [this] { context_.session.set_solo(id_, solo_.getToggleState()); };

        style_button(export_, colours::accent_warm, "Include this temp / reference track in deliverable exports");
        export_.onClick = [this] { context_.session.set_include_in_export(id_, export_.getToggleState()); };
        addChildComponent(export_);
        fader_.setTooltip("Track gain - double-click for 0 dB");
        refresh();
    }

    juce::String key() const override { return "t:" + juce::String(id_); }

    void refresh() override
    {
        const auto* t = context_.session.data().find_track(id_);
        if (!t) return;
        auto routing = Routing::from(context_.session.data());
        if (routing != routing_ || output_.getNumItems() == 0) {
            routing_ = std::move(routing);
            output_.clear(juce::dontSendNotification);
            for (std::size_t i = 0; i < routing_.ids.size(); ++i) {
                if (i == 1) output_.addSeparator();
                output_.addItem(routing_.names[i], static_cast<int>(i) + 1);
            }
        }
        int sel = 1;
        for (std::size_t i = 0; i < routing_.ids.size(); ++i)
            if (routing_.ids[i] == t->output_bus_id) sel = static_cast<int>(i) + 1;
        output_.setSelectedId(sel, juce::dontSendNotification);

        if (!pan_.isMouseButtonDown()) pan_.setValue(t->pan, juce::dontSendNotification);
        mute_.setToggleState(t->mute, juce::dontSendNotification);
        solo_.setToggleState(t->solo, juce::dontSendNotification);
        reference_ = t->kind == project::TrackKind::Reference;
        export_.setVisible(reference_);
        status_.setVisible(!reference_);
        export_.setToggleState(t->include_in_export, juce::dontSendNotification);
        name_ = t->name;
        colour_ = colour_from_hex(t->color);
        audible_ = context_.session.audible(*t);
        set_gain_display(t->gain_db);
        update_status();
        repaint();
    }

    void tick(bool slow) override
    {
        meter_.push(context_.engine.take_peak(id_));
        if (slow) update_status();
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        context_.session.select_track(id_);
        if (e.mods.isPopupMenu()) TrackPanel::show_track_menu(context_, id_);
    }

protected:
    void write_gain(double db) override { context_.session.set_gain(id_, db); }
    juce::String name() const override { return name_; }
    juce::Colour colour() const override { return colour_; }
    juce::Colour name_colour() const override
    {
        const auto base = reference_ ? colours::accent_warm : colours::text;
        return audible_ ? base : base.withAlpha(0.45f);
    }
    juce::String caption() const override { return reference_ ? juce::String() : Strip::caption(); }
    bool selected() const override { return context_.session.selected_track() == id_; }

    void layout_head() override
    {
        const int w = getWidth();
        status_.setBounds(6, 36, w - 12, 13);
        output_.setBounds(6, 52, w - 12, 19);
        pan_.setBounds(w / 2 - 18, 74, 36, 36);
        auto ms = juce::Rectangle<int>(8, 124, w - 16, 18);
        mute_.setBounds(ms.removeFromLeft(ms.getWidth() / 2 - 2));
        ms.removeFromLeft(4);
        solo_.setBounds(ms);
        export_.setBounds(getLocalBounds().reduced(10, 0).withTop(getHeight() - 22).withHeight(17));
    }

    void paint_extra(juce::Graphics& g) override
    {
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(10.0f));
        g.drawText(pan_text(pan_.getValue()), pan_text_area(), juce::Justification::centred, false);
        if (reference_) {
            // "TEMP" badge where the instrument status would be.
            auto badge = juce::Rectangle<float>(0.0f, 0.0f, 34.0f, 12.0f).withCentre({static_cast<float>(getWidth()) * 0.5f, 42.5f});
            g.setColour(colours::accent_warm);
            g.fillRoundedRectangle(badge, 2.0f);
            g.setColour(juce::Colour(0xff15171b));
            g.setFont(juce::FontOptions(8.5f, juce::Font::bold));
            g.drawText("TEMP", badge, juce::Justification::centred, false);
        }
    }

private:
    juce::Rectangle<int> pan_text_area() const { return {4, 109, getWidth() - 8, 12}; }

    void update_status()
    {
        const auto* t = context_.session.data().find_track(id_);
        if (!t) return;
        const juce::String s = context_.engine.instrument_status(id_);
        juce::Colour c = colours::placeholder;
        if (s.startsWith("Missing")) c = colours::missing;
        else if (t->assignment.status == project::ResolutionStatus::Exact) c = colours::exact;
        else if (t->assignment.status == project::ResolutionStatus::Fallback) c = colours::fallback;
        status_.set(reference_ ? juce::String("Temp score") : s, c, context_.engine.plugin_for(id_) != nullptr);
    }

    std::string id_;
    std::string name_;
    juce::Colour colour_;
    bool reference_ = false, audible_ = true;
    Routing routing_;
    StatusLine status_;
    juce::ComboBox output_;
    juce::Slider pan_;
    juce::TextButton mute_{"M"}, solo_{"S"}, export_{"EXPORT"};
};

class BusStrip final : public Strip {
public:
    BusStrip(AppContext& c, std::string id, bool master) : Strip(c), id_(std::move(id)), master_(master)
    {
        meter_.set_enabled_meter(master_);
        if (!master_) meter_.setTooltip("Bus metering is not available yet");
        fader_.setTooltip(master_ ? "Master gain - double-click for 0 dB" : "Bus gain - double-click for 0 dB");
        refresh();
    }

    juce::String key() const override { return (master_ ? "m:" : "b:") + juce::String(id_); }

    void refresh() override
    {
        const auto& d = context_.session.data();
        const auto* b = d.find_bus(id_);
        if (!b) {
            fader_.setEnabled(false);
            return;
        }
        name_ = master_ ? juce::String("MASTER") : juce::String(b->name);
        kind_ = b->kind;
        inserts_.clear();
        for (const auto& s : b->inserts)
            inserts_.push_back({juce::String(s.plugin_ref.empty() || s.suggestion ? s.kind : s.plugin_ref) +
                                    (s.suggestion ? " (suggested)" : (s.bypass ? " (bypassed)" : "")),
                                s.suggestion || s.bypass});
        sends_.clear();
        for (const auto& send : b->sends) {
            const auto& to = send.bus_id;
            const double level = send.level_db;
            const auto* target = d.find_bus(to);
            sends_.push_back(juce::String(juce::CharPointer_UTF8("\xe2\x86\x92 ")) + (target ? juce::String(target->name) : juce::String(to)) +
                             " " + short_db(level));
        }
        output_ = "Master";
        if (const auto* o = d.find_bus(b->output); o && o->kind != "master") output_ = o->name;
        // A group bus takes the colour of the first track that feeds it.
        colour_ = master_ ? kMasterColour : (kind_ == "fx" ? kFxColour : kGroupColour);
        if (!master_ && kind_ != "fx")
            for (const auto* t : context_.session.music_tracks())
                if (t->output_bus_id == id_) {
                    colour_ = colour_from_hex(t->color, kGroupColour);
                    break;
                }
        set_gain_display(b->gain_db);
        repaint();
    }

    void tick(bool) override
    {
        if (master_) meter_.push(context_.engine.take_master_peak());
    }

protected:
    void write_gain(double db) override { context_.session.set_bus_gain(id_, db); }
    juce::String name() const override { return name_; }
    juce::Colour colour() const override { return colour_; }
    juce::String caption() const override { return master_ ? juce::String("STEREO OUT") : juce::String(); }

    void paint_extra(juce::Graphics& g) override
    {
        const int w = getWidth();
        // Kind badge
        const juce::String badge = master_ ? "MASTER" : (kind_ == "fx" ? "FX" : "GROUP");
        auto br = juce::Rectangle<float>(0.0f, 37.0f, master_ ? 48.0f : (kind_ == "fx" ? 26.0f : 40.0f), 12.0f)
                      .withCentre({static_cast<float>(w) * 0.5f, 43.0f});
        if (!master_) {
            g.setColour(colour_.withAlpha(0.22f));
            g.fillRoundedRectangle(br, 2.0f);
            g.setColour(colour_.withAlpha(0.8f));
            g.drawRoundedRectangle(br, 2.0f, 1.0f);
            g.setColour(colour_.brighter(0.4f));
            g.setFont(juce::FontOptions(8.5f, juce::Font::bold));
            g.drawText(badge, br, juce::Justification::centred, false);
        }

        // Output (buses route to the master; rerouting buses is not modelled yet).
        auto out = juce::Rectangle<float>(6.0f, 52.0f, static_cast<float>(w) - 12.0f, 19.0f);
        g.setColour(kWell);
        g.fillRoundedRectangle(out, 3.0f);
        g.setColour(colours::outline.withAlpha(0.6f));
        g.drawRoundedRectangle(out, 3.0f, 1.0f);
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(master_ ? juce::String("Main output") : juce::String(juce::CharPointer_UTF8("\xe2\x86\x92 ")) + output_,
                   out.reduced(5.0f, 0.0f), juce::Justification::centredLeft, true);

        // Inserts, then sends.
        auto area = juce::Rectangle<int>(6, 76, w - 12, kHeadHeight - 78);
        auto line = [&](const juce::String& text, juce::Colour c, float size, bool bold = false) {
            if (area.getHeight() < 11) return;
            g.setColour(c);
            g.setFont(juce::FontOptions(size, bold ? juce::Font::bold : juce::Font::plain));
            g.drawFittedText(text, area.removeFromTop(12), juce::Justification::centredLeft, 1, 0.75f);
        };
        if (master_) {
            line("INSERTS", colours::text_dim.withAlpha(0.7f), 8.5f, true);
            line("none", colours::text_dim.withAlpha(0.6f), 10.0f);
            return;
        }
        line("INSERTS", colours::text_dim.withAlpha(0.7f), 8.5f, true);
        if (inserts_.empty()) line("none", colours::text_dim.withAlpha(0.6f), 10.0f);
        for (const auto& [text, dim] : inserts_)
            line(text, dim ? colours::text_dim.withAlpha(0.75f) : colours::text, 10.0f);
        if (!sends_.empty()) {
            area.removeFromTop(2);
            line("SENDS", colours::text_dim.withAlpha(0.7f), 8.5f, true);
            for (const auto& s : sends_) line(s, colours::text, 10.0f);
        }
    }

private:
    std::string id_;
    bool master_;
    juce::String name_, kind_, output_;
    juce::Colour colour_;
    std::vector<std::pair<juce::String, bool>> inserts_;
    std::vector<juce::String> sends_;
};

/// The scrolled row of track and bus strips.
class StripRow final : public juce::Component {
public:
    int separator_x = -1;
    void paint(juce::Graphics& g) override
    {
        if (separator_x < 0) return;
        g.setColour(juce::Colours::black.withAlpha(0.5f));
        g.fillRect(separator_x + kSectionGap / 2 - 1, 6, 2, getHeight() - 12);
        g.setColour(colours::outline.withAlpha(0.4f));
        g.fillRect(separator_x + kSectionGap / 2 + 1, 6, 1, getHeight() - 12);
    }
};

}  // namespace

// --- Mixer -------------------------------------------------------------------------------------

struct MixerComponent::Impl final : private juce::Timer {
    Impl(MixerComponent& o, AppContext& c) : owner(o), context(c)
    {
        owner.setLookAndFeel(&look);
        viewport.setViewedComponent(&row, false);
        viewport.setScrollBarsShown(false, true);
        viewport.setScrollBarThickness(10);
        owner.addAndMakeVisible(viewport);
        listener = context.session.add_listener([this](session::Change change) {
            if (change == session::Change::Document) sync();
            if (change == session::Change::Selection)
                for (auto& s : strips) s->repaint();
        });
        sync();
        startTimerHz(30);
    }

    ~Impl() override
    {
        stopTimer();
        context.session.remove_listener(listener);
    }

    /// Rebuilds the strips when the set of tracks or buses changed, otherwise refreshes values.
    void sync()
    {
        const auto& d = context.session.data();
        std::vector<juce::String> keys;
        for (const auto* t : context.session.music_tracks()) keys.push_back("t:" + juce::String(t->id));
        track_count = static_cast<int>(keys.size());
        for (const char* kind : {"group", "fx"})
            for (const auto& b : d.buses)
                if (b.kind == kind) keys.push_back("b:" + juce::String(b.id));
        std::string master_id;
        for (const auto& b : d.buses)
            if (b.kind == "master") master_id = b.id;

        bool same = keys.size() == strips.size() && master && master_key == master_id;
        for (std::size_t i = 0; same && i < keys.size(); ++i) same = strips[i]->key() == keys[i];
        if (same) {
            for (auto& s : strips) s->refresh();
            master->refresh();
            return;
        }

        strips.clear();
        int n = 0;
        for (const auto* t : context.session.music_tracks()) {
            auto s = std::make_unique<TrackStrip>(context, t->id);
            s->set_number(++n);
            row.addAndMakeVisible(*s);
            strips.push_back(std::move(s));
        }
        for (const char* kind : {"group", "fx"})
            for (const auto& b : d.buses)
                if (b.kind == kind) {
                    strips.push_back(std::make_unique<BusStrip>(context, b.id, false));
                    row.addAndMakeVisible(*strips.back());
                }
        master_key = master_id;
        master = std::make_unique<BusStrip>(context, master_id.empty() ? std::string("bus-master") : master_id, true);
        owner.addAndMakeVisible(*master);
        layout();
    }

    void layout()
    {
        if (owner.getWidth() < 200 || owner.getHeight() < 100) return;
        auto r = owner.getLocalBounds().reduced(6, 6);
        if (master) master->setBounds(r.removeFromRight(kMasterWidth));
        r.removeFromRight(8);
        viewport.setBounds(r);
        int x = 0;
        row.separator_x = -1;
        const int content_w = static_cast<int>(strips.size()) * (kStripWidth + 2) + (track_count > 0 ? kSectionGap : 0);
        const int h = r.getHeight() - (content_w > r.getWidth() ? viewport.getScrollBarThickness() + 2 : 0);
        for (std::size_t i = 0; i < strips.size(); ++i) {
            if (static_cast<int>(i) == track_count && track_count > 0 && i < strips.size()) {
                row.separator_x = x;
                x += kSectionGap;
            }
            strips[i]->setBounds(x, 0, kStripWidth, h);
            x += kStripWidth + 2;
        }
        row.setSize(juce::jmax(x, 1), h);
        if (master) master->setBounds(master->getBounds().withHeight(h));
    }

    void timerCallback() override
    {
        const bool slow = ++ticks % 30 == 0;
        for (auto& s : strips) s->tick(slow);
        if (master) master->tick(slow);
    }

    MixerComponent& owner;
    AppContext& context;
    MixerLook look;
    juce::Viewport viewport;
    StripRow row;
    std::vector<std::unique_ptr<Strip>> strips;
    std::unique_ptr<Strip> master;
    std::string master_key;
    int track_count = 0;
    int listener = 0;
    int ticks = 0;
};

MixerComponent::MixerComponent(AppContext& context) : context_(context)
{
    impl_ = std::make_unique<Impl>(*this, context_);
    setSize(900, 420);
}

MixerComponent::~MixerComponent()
{
    setLookAndFeel(nullptr);
    impl_.reset();
}

void MixerComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff17191d));
    if (impl_->track_count == 0) {
        auto area = impl_->viewport.getBounds();
        area.removeFromLeft(impl_->row.getWidth());
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(13.0f));
        g.drawFittedText("No tracks yet - add a track or a template to start mixing", area.reduced(12),
                         juce::Justification::centred, 3);
    }
}

void MixerComponent::resized()
{
    if (impl_) impl_->layout();
}

MixerWindow::MixerWindow(AppContext& context)
    : juce::DocumentWindow("Mixer", colours::background, juce::DocumentWindow::allButtons)
{
    setUsingNativeTitleBar(true);
    setContentOwned(new MixerComponent(context), true);
    setResizable(true, false);
    setResizeLimits(420, 380, 8000, 3000);
}

void MixerWindow::closeButtonPressed() { setVisible(false); }

}  // namespace vcc::desktop
