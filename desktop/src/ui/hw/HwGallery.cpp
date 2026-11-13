#include "ui/hw/HwGallery.h"

namespace vcc::desktop::hw {

namespace {
// Silk labels painted over the panels: {text, x, y, w}.
struct SilkLabel {
    const char* text;
    int x, y, w;
};
const SilkLabel kLabels[] = {
    {"Entry mode", 28, 128, 200},   {"View", 352, 128, 120},     {"Grid", 568, 128, 60},   {"Zoom", 690, 128, 60},
    {"LED colour", 836, 128, 120},  {"LCD colour", 1148, 128, 120}, {"Staff", 1460, 128, 80},
    {"Vol", 168, 258, 30},          {"Pan", 200, 258, 30},       {"Articulation", 268, 208, 200},
    {"Dynamics", 268, 296, 200},    {"Voice", 268, 384, 200},    {"Velocity", 704, 336, 100},
    {"Humanize", 804, 336, 100},    {"Pitch", 1296, 330, 40},    {"Mod", 1340, 330, 40},
    {"Octave", 1398, 208, 80},      {"LEDs", 1520, 208, 80},
};
}  // namespace

Keycap& Gallery::key(std::vector<std::unique_ptr<Keycap>>& into, const juce::String& legend, bool toggles, bool on)
{
    into.push_back(std::make_unique<Keycap>(legend, toggles));
    auto& k = *into.back();
    k.setToggleState(on, juce::dontSendNotification);
    addAndMakeVisible(k);
    return k;
}

Lcd& Gallery::lcd(const juce::String& text, float size, const juce::String& caption)
{
    lcds_.push_back(std::make_unique<Lcd>(text, size));
    auto& l = *lcds_.back();
    l.set_caption(caption);
    addAndMakeVisible(l);
    return l;
}

Panel& Gallery::panel(const juce::String& title, bool screws)
{
    panels_.push_back(std::make_unique<Panel>(title, screws));
    auto& p = *panels_.back();
    addAndMakeVisible(p, 0);  // behind the controls
    return p;
}

Gallery::Gallery()
{
    setOpaque(true);
    Theme::get().addChangeListener(this);

    nameplate_ = &panel({}, true);
    transport_panel_ = &panel("Transport");
    counter_ = &panel({});
    session_panel_ = &panel({});
    master_ = &panel("Master");
    toolbar_ = &panel({});
    header_ = &panel({});
    inspector_ = &panel({});
    performance_ = &panel("Performance");
    mix_ = &panel("Channel");
    wheels_ = &panel({});
    theme_panel_ = &panel({});

    // Nameplate: the view switch.
    key(view_switch_, "Composer", true, true).set_led_dot(true);
    key(view_switch_, "Producer", true).set_led_dot(true);
    for (auto& k : view_switch_) k->setRadioGroupId(1);

    // Transport.
    const Glyph glyphs[] = {Glyph::ReturnToZero, Glyph::Rewind, Glyph::FastForward, Glyph::Stop,
                            Glyph::Play,         Glyph::Record, Glyph::Loop};
    const char* names[] = {"Return to zero", "Rewind", "Fast forward", "Stop", "Play", "Record", "Loop"};
    for (int i = 0; i < 7; ++i) {
        auto& k = key(transport_, names[i], i >= 4, i == 4 || i == 6);
        k.set_glyph(glyphs[i]);
        if (i == 4) {
            k.set_glyph_lit(tok::meter_green);
            k.set_lit_colour(tok::meter_green);
        }
        if (i == 5) k.set_glyph_lit(tok::rec);
    }

    // Counter and session.
    main_counter_ = &lcd("44|3|941", 36.0f, juce::String::fromUTF8("Main  \xc2\xb7  Bars|Beats"));
    sub_counter_ = &lcd("01:42.929", 15.0f, juce::String::fromUTF8("Sub  \xc2\xb7  Min:Sec"));
    range_ = &lcd("START      44|3|941\nEND        44|3|941\nLENGTH      0|0|000", 11.0f);
    tempo_ = &lcd("96.00", 16.0f);
    meter_lcd_ = &lcd("4/4", 16.0f);
    key_lcd_ = &lcd("D MAJ", 16.0f);
    for (auto* l : {tempo_, meter_lcd_, key_lcd_}) l->set_justification(juce::Justification::centred);
    key(session_, "Click", true, true).set_led_dot(true);
    key(session_, "Count-in", true).set_led_dot(true);
    key(session_, "Wait note", true).set_led_dot(true);

    addAndMakeVisible(master_l_);
    addAndMakeVisible(master_r_);
    master_l_.push(-3.2f);
    master_r_.push(-4.5f);

    // Toolbar: entry mode, view, grid, zoom, theme.
    for (const char* n : {"Insert", "Overwrite", "Step", "Real-time"}) key(entry_, n, true, juce::String(n) == "Step");
    for (auto& k : entry_) {
        k->setRadioGroupId(2);
        k->set_led_dot(true);
    }
    for (const char* n : {"Track", "Page", "Mixer"}) key(views_, n, true, juce::String(n) == "Track").setRadioGroupId(3);
    grid_ = &lcd("1/16", 16.0f);
    grid_->set_justification(juce::Justification::centred);
    key(zoom_, "-", false);
    key(zoom_, "+", false);
    key(zoom_, "Fit", false);

    // Theme: the spec's LED, LCD and staff-paper choices, live.
    auto& theme = Theme::get();
    for (const auto& c : Theme::led_choices()) {
        auto& k = key(theme_keys_, c.name, true, c.colour == theme.led());
        k.setRadioGroupId(4);
        k.set_led_dot(true);
        k.set_led_colour(c.colour);
        k.onClick = [colour = c.colour] { Theme::get().set_led(colour); };
    }
    for (const auto& c : Theme::lcd_choices()) {
        auto& k = key(theme_keys_, c.name, true, c.colour == theme.lcd());
        k.setRadioGroupId(5);
        k.set_led_dot(true);
        k.set_led_colour(c.colour);
        k.onClick = [colour = c.colour] { Theme::get().set_lcd(colour); };
    }
    for (const bool paper : {false, true}) {
        auto& k = key(theme_keys_, paper ? "Paper" : "Dark", true, theme.paper() == paper);
        k.setRadioGroupId(6);
        k.onClick = [paper] { Theme::get().set_paper(paper); };
    }

    // A track header: number, scribble strip, R/S/M, staff tag, knobs, meter.
    track_no_ = &lcd("02", 12.0f);
    track_no_->set_justification(juce::Justification::centred);
    scribble_ = &lcd(juce::String::fromUTF8("VIOLINS I\nSTRINGS  \xc2\xb7  SUSTAIN"), 12.0f);
    const juce::Colour rsm_colours[] = {tok::rec, tok::solo, tok::mute};
    const char* rsm_names[] = {"R", "S", "M"};
    for (int i = 0; i < 3; ++i) key(rsm_, rsm_names[i], true, i == 0).set_solid(rsm_colours[i]);
    key(rsm_, juce::String::fromUTF8("Treble \xc2\xb7 8 sel"), false).set_legend_size(10.5f);
    for (int i = 0; i < 2; ++i) {
        small_knobs_.push_back(std::make_unique<SmallKnob>());
        auto& k = *small_knobs_.back();
        k.setRange(i == 0 ? -60.0 : -1.0, i == 0 ? 6.0 : 1.0);
        k.setValue(i == 0 ? -4.5 : -0.3, juce::dontSendNotification);
        k.setDoubleClickReturnValue(true, 0.0);
        k.setTitle(i == 0 ? "Volume" : "Pan");
        addAndMakeVisible(k);
    }
    vol_lcd_ = &lcd("-4.5", 10.0f);
    pan_lcd_ = &lcd("L30", 10.0f);
    for (auto* l : {vol_lcd_, pan_lcd_}) l->set_justification(juce::Justification::centred);
    small_knobs_[0]->onValueChange = [this] { vol_lcd_->set_text(juce::String(small_knobs_[0]->getValue(), 1)); };
    small_knobs_[1]->onValueChange = [this] {
        const int v = juce::roundToInt(small_knobs_[1]->getValue() * 100.0);
        pan_lcd_->set_text(v == 0 ? "C" : (v < 0 ? "L" : "R") + juce::String(std::abs(v)));
    };
    track_meters_.push_back(std::make_unique<Meter>(Meter::Orientation::Vertical, 12));
    addAndMakeVisible(*track_meters_.back());
    track_meters_.back()->push(-9.0f);

    // Inspector keys: articulation 3 x 2, dynamics 4 x 2, voice 1–4.
    for (const char* n : {".", ">", "-", "^", "(", "tr"}) key(artic_, n, true, juce::String(n) == ">");
    for (const char* n : {"pp", "p", "mp", "mf", "f", "ff", "<", ">"}) {
        auto& k = key(dynamics_, n, true, juce::String(n) == "<");
        k.set_legend_size(15.0f);
        k.set_dynamic_legend(true);
    }
    const juce::Colour voice_colours[] = {juce::Colour(0xff4fa8e8), juce::Colour(0xff3ddc84), juce::Colour(0xffffb547),
                                          juce::Colour(0xffe0699a)};
    for (int i = 0; i < 4; ++i) {
        auto& k = key(voices_, juce::String(i + 1), true, i == 0);
        k.setRadioGroupId(7);
        k.set_led_dot(true);
        k.set_led_colour(voice_colours[i]);
    }

    // Performance ring knobs.
    for (auto* k : {&velocity_, &humanize_}) {
        k->setRange(0.0, k == &velocity_ ? 127.0 : 100.0, 1.0);
        addAndMakeVisible(*k);
    }
    velocity_.setTitle("Velocity");
    humanize_.setTitle("Humanize");
    velocity_.setValue(92.0, juce::dontSendNotification);
    humanize_.setValue(18.0, juce::dontSendNotification);
    vel_lcd_ = &lcd("92", 12.0f);
    hum_lcd_ = &lcd("18%", 12.0f);
    for (auto* l : {vel_lcd_, hum_lcd_}) l->set_justification(juce::Justification::centred);
    velocity_.onValueChange = [this] { vel_lcd_->set_text(juce::String(juce::roundToInt(velocity_.getValue()))); };
    humanize_.onValueChange = [this] {
        hum_lcd_->set_text(juce::String(juce::roundToInt(humanize_.getValue())) + "%");
    };

    // Channel: fader, stereo meter, dB readout.
    addAndMakeVisible(fader_);
    fader_.setTitle("Volume");
    fader_.set_db(-4.5, juce::dontSendNotification);
    fader_lcd_ = &lcd("-4.5 dB", 12.0f);
    fader_lcd_->set_justification(juce::Justification::centred);
    fader_.onValueChange = [this] {
        const double db = fader_.db();
        fader_lcd_->set_text(std::isfinite(db) ? juce::String(db, 1) + " dB"
                                               : juce::String(juce::CharPointer_UTF8("-\xe2\x88\x9e dB")));
    };
    addAndMakeVisible(fader_l_);
    addAndMakeVisible(fader_r_);
    fader_l_.push(-6.0f);
    fader_r_.push(-8.0f);

    // Wheels and octave.
    pitch_.setTitle("Pitch bend");
    mod_.setTitle("Modulation");
    mod_.setValue(0.35, juce::dontSendNotification);
    addAndMakeVisible(pitch_);
    addAndMakeVisible(mod_);
    key(octave_, "-", false);
    key(octave_, "+", false);
    octave_lcd_ = &lcd(juce::String::fromUTF8("\xc2\xb1" "0"), 13.0f);
    octave_lcd_->set_justification(juce::Justification::centred);

    // A row of LEDs, on and off, in each lamp colour.
    for (const auto c : {juce::Colour(), tok::meter_green, tok::meter_red, tok::mute})
        for (const bool on : {true, false}) {
            leds_.push_back(std::make_unique<Led>(c));
            leds_.back()->set_on(on);
            addAndMakeVisible(*leds_.back());
        }

    setSize(1680, 470);
}

Gallery::~Gallery() { Theme::get().removeChangeListener(this); }

void Gallery::changeListenerCallback(juce::ChangeBroadcaster*)
{
    // The theme keys follow a change made anywhere (the View menu included).
    const auto& theme = Theme::get();
    for (std::size_t i = 0; i < 4; ++i) {
        theme_keys_[i]->setToggleState(Theme::led_choices()[i].colour == theme.led(), juce::dontSendNotification);
        theme_keys_[4 + i]->setToggleState(Theme::lcd_choices()[i].colour == theme.lcd(), juce::dontSendNotification);
    }
    theme_keys_[8]->setToggleState(!theme.paper(), juce::dontSendNotification);
    theme_keys_[9]->setToggleState(theme.paper(), juce::dontSendNotification);
    repaint();
}

void Gallery::paint(juce::Graphics& g)
{
    g.fillAll(tok::chassis);
}

void Gallery::paintOverChildren(juce::Graphics& g)
{
    // Silk-screen printing on the panels.
    for (const auto& l : kLabels)
        paint::silk(g, l.text, juce::Rectangle<float>(static_cast<float>(l.x), static_cast<float>(l.y),
                                                      static_cast<float>(l.w), 16.0f),
                    10.5f);
    paint::silk(g, "Tempo", {1196.0f, 18.0f, 66.0f, 12.0f}, 10.0f);
    paint::silk(g, "Meter", {1270.0f, 18.0f, 64.0f, 12.0f}, 10.0f);
    paint::silk(g, "Key", {1342.0f, 18.0f, 70.0f, 12.0f}, 10.0f);
    paint::silk(g, "Pre-roll", {560.0f, 22.0f, 60.0f, 12.0f}, 10.0f, juce::Justification::centredRight);
    paint::led(g, {628.0f, 28.0f}, true, Theme::get().led());
    paint::silk(g, "Post", {636.0f, 22.0f, 40.0f, 12.0f}, 10.0f);
    paint::silk(g, "L", {1450.0f, 46.0f, 12.0f, 14.0f}, 10.0f);
    paint::silk(g, "R", {1450.0f, 64.0f, 12.0f, 14.0f}, 10.0f);
    paint::silk(g, "-60   -18   -6   0 dBFS", {1468.0f, 82.0f, 186.0f, 12.0f}, 9.0f);
    for (const float x : {340.0f, 556.0f, 680.0f, 822.0f, 1136.0f, 1448.0f}) paint::groove(g, x, 130.0f, 178.0f);

    // The brand: "COMPOSER" in silk, the "2" lit.
    auto brand = juce::Rectangle<float>(36.0f, 24.0f, 228.0f, 32.0f);
    paint::silk(g, "VC", brand.removeFromLeft(26.0f).withTrimmedTop(8.0f), 11.0f);
    g.setColour(tok::text_bright);
    g.setFont(fonts::silk(24.0f, 0.22f, true));
    const auto word = fonts::silk(24.0f, 0.22f, true);
    g.drawText("COMPOSER", brand, juce::Justification::centredLeft, false);
    const float word_w = juce::GlyphArrangement::getStringWidth(word, "COMPOSER");
    const auto& theme = Theme::get();
    juce::GlyphArrangement two;
    two.addLineOfText(fonts::silk(28.0f, 0.0f, true), "2", brand.getX() + word_w + 4.0f, brand.getBottom() - 4.0f);
    juce::Path p;
    two.createPath(p);
    juce::DropShadow(theme.led_glow(), 8, {}).drawForPath(g, p);
    g.setColour(theme.led());
    g.fillPath(p);
}

void Gallery::resized()
{
    // Top deck (96 px), at the spec's reference positions.
    nameplate_->setBounds(14, 14, 268, 96);
    transport_panel_->setBounds(292, 14, 392, 96);
    counter_->setBounds(694, 14, 480, 96);
    session_panel_->setBounds(1184, 14, 240, 96);
    master_->setBounds(1434, 14, 232, 96);

    view_switch_[0]->setBounds(34, 62, 112, 30);
    view_switch_[1]->setBounds(152, 62, 112, 30);
    for (int i = 0; i < 7; ++i) transport_[static_cast<std::size_t>(i)]->setBounds(306 + i * 53, 46, 46, 42);
    main_counter_->setBounds(708, 24, 266, 76);
    sub_counter_->setBounds(880, 24, 94, 76);
    range_->setBounds(990, 24, 172, 76);
    sub_counter_->toFront(false);
    tempo_->setBounds(1196, 30, 66, 30);
    meter_lcd_->setBounds(1270, 30, 64, 30);
    key_lcd_->setBounds(1342, 30, 70, 30);
    for (int i = 0; i < 3; ++i) session_[static_cast<std::size_t>(i)]->setBounds(1196 + i * 74, 70, 70, 26);
    master_l_.setBounds(1468, 50, 186, 7);
    master_r_.setBounds(1468, 68, 186, 7);

    // Toolbar (58 px).
    toolbar_->setBounds(14, 120, 1652, 66);
    for (int i = 0; i < 4; ++i) entry_[static_cast<std::size_t>(i)]->setBounds(28 + i * 78, 148, 74, 28);
    for (int i = 0; i < 3; ++i) views_[static_cast<std::size_t>(i)]->setBounds(352 + i * 68, 148, 64, 28);
    grid_->setBounds(568, 148, 108, 28);
    for (int i = 0; i < 3; ++i) zoom_[static_cast<std::size_t>(i)]->setBounds(690 + i * 44, 148, 40, 28);
    for (int i = 0; i < 4; ++i) theme_keys_[static_cast<std::size_t>(i)]->setBounds(836 + i * 76, 148, 72, 28);
    for (int i = 0; i < 4; ++i) theme_keys_[static_cast<std::size_t>(4 + i)]->setBounds(1148 + i * 76, 148, 72, 28);
    for (int i = 0; i < 2; ++i) theme_keys_[static_cast<std::size_t>(8 + i)]->setBounds(1460 + i * 76, 148, 72, 28);

    // A track header (220 px).
    header_->setBounds(14, 196, 240, 120);
    track_no_->setBounds(26, 208, 26, 34);
    scribble_->setBounds(58, 208, 182, 34);
    for (int i = 0; i < 3; ++i) rsm_[static_cast<std::size_t>(i)]->setBounds(28 + i * 30, 252, 26, 24);
    rsm_[3]->setBounds(28, 282, 98, 22);
    small_knobs_[0]->setBounds(166, 270, 30, 30);
    small_knobs_[1]->setBounds(198, 270, 30, 30);
    vol_lcd_->setBounds(160, 300, 38, 15);
    pan_lcd_->setBounds(199, 300, 36, 15);
    track_meters_[0]->setBounds(238, 252, 6, 60);

    // Inspector keys.
    inspector_->setBounds(264, 196, 420, 264);
    for (int i = 0; i < 6; ++i)
        artic_[static_cast<std::size_t>(i)]->setBounds(278 + (i % 3) * 132, 226 + (i / 3) * 32, 126, 28);
    for (int i = 0; i < 8; ++i)
        dynamics_[static_cast<std::size_t>(i)]->setBounds(278 + (i % 4) * 99, 314 + (i / 4) * 32, 94, 28);
    for (int i = 0; i < 4; ++i) voices_[static_cast<std::size_t>(i)]->setBounds(278 + i * 99, 402, 94, 28);

    // Performance ring knobs.
    performance_->setBounds(694, 196, 220, 180);
    velocity_.setBounds(716, 236, 84, 84);
    humanize_.setBounds(816, 236, 84, 84);
    vel_lcd_->setBounds(734, 350, 48, 18);
    hum_lcd_->setBounds(834, 350, 48, 18);

    // Channel: fader with its meter.
    mix_->setBounds(924, 196, 240, 264);
    fader_.setBounds(944, 226, 90, 220);
    fader_l_.setBounds(1046, 236, 6, 200);
    fader_r_.setBounds(1056, 236, 6, 200);
    fader_lcd_->setBounds(1074, 236, 78, 22);

    // Wheels, octave, LEDs.
    wheels_->setBounds(1174, 196, 492, 180);
    pitch_.setBounds(1300, 236, 32, 90);
    mod_.setBounds(1344, 236, 32, 90);
    octave_[0]->setBounds(1398, 232, 30, 26);
    octave_[1]->setBounds(1434, 232, 30, 26);
    octave_lcd_->setBounds(1398, 266, 66, 24);
    for (std::size_t i = 0; i < leds_.size(); ++i)
        leds_[i]->setBounds(1520 + static_cast<int>(i % 2) * 18, 232 + static_cast<int>(i / 2) * 20, 12, 12);
    theme_panel_->setBounds(0, 0, 0, 0);
}

}  // namespace vcc::desktop::hw
