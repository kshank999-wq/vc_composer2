#include "ui/shell/KeyboardDeck.h"

#include <algorithm>
#include <cmath>

#include "ui/shell/TopDeck.h"

namespace vcc::desktop::shell {

namespace {
constexpr int kHeaderTop = 5, kHeaderH = 22, kBodyTop = 32, kScrollbar = 6, kPadBottom = 6;
constexpr int kLeftColumn = 168, kRightColumn = 260;

int keyboard_height(double zoom) { return static_cast<int>(std::lround(96.0 + (zoom - 1.0) * 48.0)); }

juce::String value_name(const notation::Duration& d)
{
    const char* names[] = {"2/1", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64"};
    juce::String s = names[std::clamp(static_cast<int>(d.value), 0, 7)];
    for (int i = 0; i < d.dots; ++i) s << ".";
    if (d.tuplet.is_tuplet()) s << " " << juce::String(d.tuplet.actual) << ":" << juce::String(d.tuplet.normal);
    return s;
}
}  // namespace

// --- Keys ------------------------------------------------------------------------------------

PianoKeys::PianoKeys() { set_zoom(1.0); }

bool PianoKeys::is_black(int midi)
{
    const int pc = ((midi % 12) + 12) % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

int PianoKeys::white_index(int midi)
{
    midi = std::clamp(midi, kLowest, kHighest);
    int index = -1;
    for (int n = kLowest; n <= midi; ++n)
        if (!is_black(n)) ++index;
    return std::max(0, index);
}

void PianoKeys::set_zoom(double zoom)
{
    zoom_ = std::clamp(zoom, 1.0, 3.0);
    setSize(static_cast<int>(std::lround(kWhiteKeys * key_width())), keyboard_height(zoom_));
    repaint();
}

void PianoKeys::set_lit(const std::bitset<128>& lit)
{
    if (lit == lit_) return;
    lit_ = lit;
    repaint();
}

float PianoKeys::centre_of(int midi) const
{
    const float w = static_cast<float>(getWidth()) / kWhiteKeys;
    const int i = white_index(midi);
    // A black key sits on the line after its white key (spec: left = (i + 1) / 52 - 0.3 / 52).
    return is_black(midi) ? (static_cast<float>(i) + 1.0f) * w : (static_cast<float>(i) + 0.5f) * w;
}

int PianoKeys::note_at(juce::Point<float> p) const
{
    if (!getLocalBounds().toFloat().contains(p)) return -1;
    const float w = static_cast<float>(getWidth()) / kWhiteKeys;
    // Black keys first: they are in front.
    if (p.y < static_cast<float>(getHeight()) * 0.62f) {
        for (int n = kLowest; n <= kHighest; ++n) {
            if (!is_black(n)) continue;
            const float left = (static_cast<float>(white_index(n)) + 1.0f - 0.3f) * w;
            if (p.x >= left && p.x < left + 0.6f * w) return n;
        }
    }
    const int i = std::clamp(static_cast<int>(p.x / w), 0, kWhiteKeys - 1);
    int index = -1;
    for (int n = kLowest; n <= kHighest; ++n)
        if (!is_black(n) && ++index == i) return n;
    return -1;
}

int PianoKeys::velocity_at(int note, juce::Point<float> p) const
{
    // Deeper on the key is louder, as a finger nearer the fallboard plays harder.
    const float h = static_cast<float>(getHeight()) * (is_black(note) ? 0.62f : 1.0f);
    const float depth = std::clamp(p.y / std::max(1.0f, h), 0.0f, 1.0f);
    return std::clamp(static_cast<int>(std::lround(30.0f + depth * 97.0f)), 1, 127);
}

void PianoKeys::paint(juce::Graphics& g)
{
    const float w = static_cast<float>(getWidth()) / kWhiteKeys;
    const float h = static_cast<float>(getHeight());
    const auto led = hw::Theme::get().led();
    int index = 0;
    for (int n = kLowest; n <= kHighest; ++n) {
        if (is_black(n)) continue;
        const juce::Rectangle<float> r(static_cast<float>(index) * w + 0.5f, 0.0f, w - 1.0f, h - 1.0f);
        if (lit_[static_cast<std::size_t>(n)]) {
            g.setColour(led);
            g.fillRoundedRectangle(r, 2.5f);
        } else {
            juce::ColourGradient grad(juce::Colour(0xfff5f4f0), 0.0f, 0.0f, juce::Colour(0xffcfcec8), 0.0f, h, false);
            grad.addColour(0.55, juce::Colour(0xffe3e2dc));
            g.setGradientFill(grad);
            g.fillRoundedRectangle(r, 2.5f);
            // Inset bottom shadow.
            g.setColour(juce::Colours::black.withAlpha(0.18f));
            g.fillRect(r.withTop(r.getBottom() - 4.0f).reduced(1.0f, 0.0f));
        }
        g.setColour(juce::Colour(0xff8a8983));
        g.drawRoundedRectangle(r, 2.5f, 0.6f);
        if (n % 12 == 0) {
            // C labels, and the LCD dot on middle C.
            g.setColour(juce::Colour(0xff55544f));
            g.setFont(hw::fonts::lcd(9.0f));
            const int octave = n / 12 - 1;
            g.drawText("C" + juce::String(octave), r.withTop(r.getBottom() - 14.0f).toNearestInt(),
                       juce::Justification::centred, false);
            if (n == 60) {
                g.setColour(hw::Theme::get().lcd());
                g.fillEllipse(juce::Rectangle<float>(5.0f, 5.0f).withCentre({r.getCentreX(), r.getBottom() - 20.0f}));
            }
        }
        ++index;
    }
    for (int n = kLowest; n <= kHighest; ++n) {
        if (!is_black(n)) continue;
        const float left = (static_cast<float>(white_index(n)) + 1.0f - 0.3f) * w;
        const juce::Rectangle<float> r(left, 0.0f, 0.6f * w, h * 0.62f);
        if (lit_[static_cast<std::size_t>(n)]) {
            juce::Path p;
            p.addRoundedRectangle(r, 2.0f);
            juce::DropShadow(led.withAlpha(0.7f), 8, {}).drawForPath(g, p);
            g.setColour(led);
            g.fillPath(p);
        } else {
            g.setGradientFill(juce::ColourGradient::vertical(juce::Colour(0xff2e2e2e), r.getY(), juce::Colour(0xff0d0d0d), r.getBottom()));
            g.fillRoundedRectangle(r, 2.0f);
            g.setColour(juce::Colours::white.withAlpha(0.08f));
            g.fillRect(r.withTop(r.getBottom() - 3.0f).reduced(1.5f, 0.0f));
        }
    }
    // The lip of the well over the tops of the keys.
    g.setGradientFill(juce::ColourGradient::vertical(juce::Colours::black.withAlpha(0.55f), 0.0f, juce::Colours::transparentBlack, 6.0f));
    g.fillRect(0.0f, 0.0f, static_cast<float>(getWidth()), 6.0f);
}

void PianoKeys::mouseDown(const juce::MouseEvent& e)
{
    down_ = note_at(e.position);
    if (down_ >= 0 && on_press) on_press(down_, velocity_at(down_, e.position), e.mods.isShiftDown());
}

void PianoKeys::mouseDrag(const juce::MouseEvent& e)
{
    // A glissando: sliding onto another key releases the last one. Only the
    // first key enters a note.
    const int n = note_at(e.position);
    if (n == down_ || n < 0) return;
    if (down_ >= 0 && on_release) on_release(down_);
    down_ = n;
    if (on_press) on_press(-1 - n, velocity_at(n, e.position), false);
}

void PianoKeys::mouseUp(const juce::MouseEvent&)
{
    if (down_ >= 0 && on_release) on_release(down_);
    down_ = -1;
}

void PianoKeys::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if ((e.mods.isCommandDown() || e.mods.isCtrlDown()) && on_zoom_wheel) {
        on_zoom_wheel(w.deltaY != 0.0f ? w.deltaY : w.deltaX, e.position.x);
        return;
    }
    juce::Component::mouseWheelMove(e, w);  // the viewport scrolls sideways
}

// --- Zoom slider ---------------------------------------------------------------------------------

class KeyboardDeck::ZoomSlider final : public juce::Slider {
public:
    ZoomSlider() : juce::Slider(juce::Slider::LinearHorizontal, juce::Slider::NoTextBox)
    {
        setRange(1.0, 3.0, 0.05);
        setValue(1.0, juce::dontSendNotification);
        setWantsKeyboardFocus(false);
        setTitle("Key zoom");
    }
    void paint(juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        const auto slot = b.withSizeKeepingCentre(b.getWidth() - 12.0f, 6.0f);
        hw::paint::well(g, slot, 3.0f);
        const float x = slot.getX() + static_cast<float>(valueToProportionOfLength(getValue())) * slot.getWidth();
        g.setColour(hw::Theme::get().led().withAlpha(0.85f));
        g.fillRoundedRectangle(slot.withRight(x).reduced(1.0f), 2.0f);
        const auto cap = juce::Rectangle<float>(12.0f, b.getHeight() - 4.0f).withCentre({x, b.getCentreY()});
        hw::paint::keycap(g, cap, false);
        g.setColour(juce::Colours::white.withAlpha(0.7f));
        g.drawVerticalLine(static_cast<int>(cap.getCentreX()), cap.getY() + 4.0f, cap.getBottom() - 6.0f);
    }
};

// --- Deck ------------------------------------------------------------------------------------

KeyboardDeck::KeyboardDeck(AppContext& context)
    : hw::Panel({}, true), context_(context), slider_owner_(std::make_unique<ZoomSlider>()), slider_(*slider_owner_)
{
    viewport_.setViewedComponent(&keys_, false);
    viewport_.setScrollBarsShown(false, true);
    viewport_.setScrollBarThickness(kScrollbar);
    viewport_.setWantsKeyboardFocus(false);
    for (auto* c : std::initializer_list<juce::Component*>{&viewport_, &slider_, &minus_, &plus_, &fit_, &to_sel_, &middle_c_,
                                                           &oct_down_, &oct_up_, &chord_, &tie_, &enter_, &range_lcd_,
                                                           &zoom_lcd_, &octave_lcd_, &step_lcd_, &pitch_, &mod_}) {
        addAndMakeVisible(c);
        c->setWantsKeyboardFocus(false);
    }
    for (auto* k : {&minus_, &plus_, &fit_, &to_sel_, &middle_c_, &oct_down_, &oct_up_, &chord_, &tie_, &enter_})
        k->set_legend_size(10.5f);
    for (auto* l : {&zoom_lcd_, &octave_lcd_, &range_lcd_}) l->set_justification(juce::Justification::centred);
    chord_.set_led_dot(true);
    enter_.set_led_dot(true);
    enter_.setToggleState(true, juce::dontSendNotification);
    enter_.setTooltip("Step input: a key played here is entered at the Composer view's cursor");
    chord_.setTooltip("Chord: each key played adds to the chord just entered (or Shift-click)");
    tie_.setTooltip("Tie the note just entered to the next");
    minus_.setTooltip("Smaller keys (-0.25x)");
    plus_.setTooltip("Larger keys (+0.25x)");
    fit_.setTooltip("All 88 keys in view (1x)");
    to_sel_.setTooltip("Zoom to the selected notes (about 2.5x)");
    middle_c_.setTooltip("Centre on middle C");
    oct_down_.setTooltip("MIDI keyboard input an octave lower");
    oct_up_.setTooltip("MIDI keyboard input an octave higher");
    pitch_.setTooltip("Pitch bend (springs back)");
    mod_.setTooltip("Modulation (CC 1)");

    slider_.onValueChange = [this] { set_zoom(slider_.getValue()); };
    minus_.onClick = [this] { set_zoom(zoom_ - 0.25); };
    plus_.onClick = [this] { set_zoom(zoom_ + 0.25); };
    fit_.onClick = [this] { set_zoom(1.0); };
    to_sel_.onClick = [this] { zoom_to_selection(); };
    middle_c_.onClick = [this] { centre_on(60); };
    oct_down_.onClick = [this] { set_octave(octave_ - 1); };
    oct_up_.onClick = [this] { set_octave(octave_ + 1); };
    tie_.onClick = [this] {
        if (on_command) on_command(composer::Command::ToggleTie);
    };
    pitch_.onValueChange = [this] {
        const int v = std::clamp(static_cast<int>(std::lround((pitch_.getValue() + 1.0) * 8191.5)), 0, 16383);
        context_.engine.send_live(juce::MidiMessage::pitchWheel(1, v));
    };
    mod_.onValueChange = [this] {
        context_.engine.send_live(
            juce::MidiMessage::controllerEvent(1, 1, std::clamp(static_cast<int>(std::lround(mod_.getValue() * 127.0)), 0, 127)));
    };
    keys_.on_press = [this](int note, int velocity, bool shift) { press(note, velocity, shift); };
    keys_.on_release = [this](int note) { context_.engine.send_live(juce::MidiMessage::noteOff(1, note)); };
    keys_.on_zoom_wheel = [this](float steps, float x) {
        // Zoom about the pointer: the key under it stays under it.
        const double frac = static_cast<double>(x) / std::max(1, keys_.getWidth());
        const double on_screen = static_cast<double>(x) - viewport_.getViewPositionX();
        const double z = std::clamp(zoom_ + steps * 0.5, 1.0, 3.0);
        if (std::abs(z - zoom_) < 1e-6) return;
        zoom_ = z;
        keys_.set_zoom(zoom_);
        slider_.setValue(zoom_, juce::dontSendNotification);
        zoom_lcd_.set_text(juce::String(zoom_, 2) + "x");
        context_.session.set_key_zoom(zoom_);
        if (on_height_changed) on_height_changed();
        resized();
        scroll_to(frac * keys_.getWidth() - on_screen, false);
    };
    listener_ = context_.session.add_listener([this](session::Change c) {
        if (c != session::Change::Document) return;
        // Each project remembers its key zoom.
        if (context_.session.document_generation() != generation_) {
            generation_ = context_.session.document_generation();
            set_zoom(context_.session.data().key_zoom);
        }
    });
    generation_ = context_.session.document_generation();
    zoom_ = 0.0;  // so the first set_zoom lays out
    set_zoom(context_.session.data().key_zoom);
    startTimerHz(30);
}

KeyboardDeck::~KeyboardDeck()
{
    stopTimer();
    context_.session.remove_listener(listener_);
}

int KeyboardDeck::preferred_height() const { return keyboard_height(zoom_) + kBodyTop + kScrollbar + kPadBottom; }

void KeyboardDeck::set_zoom(double zoom)
{
    zoom = std::clamp(std::round(zoom * 20.0) / 20.0, 1.0, 3.0);
    if (std::abs(zoom - zoom_) < 1e-6) return;
    const int anchor = anchor_note();
    const bool taller = std::abs(keyboard_height(zoom) - keyboard_height(zoom_)) > 0;
    zoom_ = zoom;
    keys_.set_zoom(zoom_);
    slider_.setValue(zoom_, juce::dontSendNotification);
    zoom_lcd_.set_text(juce::String(zoom_, 2) + "x");
    context_.session.set_key_zoom(zoom_);
    if (taller && on_height_changed) on_height_changed();
    resized();
    centre_on(anchor);
}

void KeyboardDeck::centre_on(int midi)
{
    // scrollLeft = (anchorWhiteIndex + 0.5) x keyWidth - viewportWidth / 2
    const double left = (PianoKeys::white_index(midi) + 0.5) * keys_.key_width() - viewport_.getViewWidth() / 2.0;
    scroll_to(left, false);
}

void KeyboardDeck::scroll_to(double left, bool animate)
{
    const double max_left = std::max(0, keys_.getWidth() - viewport_.getViewWidth());
    left = std::clamp(left, 0.0, max_left);
    if (!animate) {
        scroll_step_ = -1;
        viewport_.setViewPosition(static_cast<int>(std::lround(left)), 0);
        return;
    }
    scroll_from_ = viewport_.getViewPositionX();
    scroll_to_ = left;
    scroll_step_ = 0;
}

std::bitset<128> KeyboardDeck::selection_notes() const
{
    std::bitset<128> out;
    const auto& sel = context_.session.selection();
    if (sel.notes.empty()) return out;
    const auto* t = context_.session.data().find_track(sel.track);
    if (!t) return out;
    // As they sound (the render): a transposing staff's notes light the keys they play.
    const auto& r = render_.render(*t);
    for (std::size_t i = 0; i < r.notes.size(); ++i)
        if (std::binary_search(sel.notes.begin(), sel.notes.end(), r.note_ids[i]))
            out.set(static_cast<std::size_t>(std::clamp(r.notes[i].pitch, 0, 127)));
    return out;
}

int KeyboardDeck::anchor_note() const
{
    auto middle = [](const std::bitset<128>& b) {
        int lo = -1, hi = -1;
        for (int n = 0; n < 128; ++n)
            if (b[static_cast<std::size_t>(n)]) {
                if (lo < 0) lo = n;
                hi = n;
            }
        return lo < 0 ? -1 : (lo + hi) / 2;
    };
    if (const int m = middle(selection_notes()); m >= 0) return m;
    if (const int m = middle(keys_.lit()); m >= 0) return m;
    return 60;
}

std::pair<int, int> KeyboardDeck::visible_range() const
{
    const float left = static_cast<float>(viewport_.getViewPositionX());
    const float right = left + static_cast<float>(viewport_.getViewWidth());
    int lo = -1, hi = -1;
    for (int n = PianoKeys::kLowest; n <= PianoKeys::kHighest; ++n) {
        const float c = keys_.centre_of(n);
        if (c >= left && c <= right) {
            if (lo < 0) lo = n;
            hi = n;
        }
    }
    return {lo, hi};
}

void KeyboardDeck::zoom_to_selection()
{
    const auto sel = selection_notes();
    if (sel.none()) {
        if (context_.status) context_.status("Select notes to zoom the keyboard to them");
        return;
    }
    if (std::abs(zoom_ - 2.5) < 1e-6) centre_on(anchor_note());
    else set_zoom(2.5);
}

void KeyboardDeck::set_octave(int octaves)
{
    octave_ = std::clamp(octaves, -4, 4);
    context_.engine.set_live_octave(octave_);
    octave_lcd_.set_text(octave_ == 0 ? juce::String::fromUTF8("\xc2\xb1" "0") : (octave_ > 0 ? "+" : "") + juce::String(octave_));
}

void KeyboardDeck::press(int note, int velocity, bool shift)
{
    // A glissando's later keys (passed as -1 - note) sound but do not enter notes.
    const bool enters = note >= 0;
    if (note < 0) note = -1 - note;
    last_velocity_ = velocity;
    context_.engine.send_live(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(velocity)));
    if (enters && enter_.getToggleState() && on_step) on_step(note, shift || chord_.getToggleState());
    refresh();
}

void KeyboardDeck::refresh()
{
    // Lit keys: the selection's notes, or what is sounding while it plays,
    // and any key held on a MIDI keyboard or here.
    auto lit = selection_notes();
    const bool playing = context_.engine.playing();
    if (lit.none() && playing)
        if (const auto* t = context_.session.data().find_track(context_.session.selected_track())) {
            const Ticks now = context_.engine.position();
            const auto& r = render_.render(*t);
            for (const auto& n : r.notes)
                if (n.start <= now && now < n.end()) lit.set(static_cast<std::size_t>(std::clamp(n.pitch, 0, 127)));
        }
    for (int n = 0; n < 128; ++n)
        if (context_.engine.live_held(n)) lit.set(static_cast<std::size_t>(n));
    keys_.set_lit(lit);
    // Follow: a newly lit note off-screen is eased into view.
    if (lit != last_lit_ && lit.any()) {
        int lo = 128, hi = -1;
        for (int n = 0; n < 128; ++n)
            if (lit[static_cast<std::size_t>(n)]) {
                lo = std::min(lo, n);
                hi = std::max(hi, n);
            }
        const auto [first, last] = visible_range();
        if (first < 0 || lo < first || hi > last)
            scroll_to((PianoKeys::white_index((lo + hi) / 2) + 0.5) * keys_.key_width() - viewport_.getViewWidth() / 2.0, true);
    }
    last_lit_ = lit;
    if (scroll_step_ >= 0) {
        // 150 ms at 30 Hz, eased out.
        scroll_step_ = std::min(scroll_step_ + 1, 5);
        const double t = scroll_step_ / 5.0, e = 1.0 - (1.0 - t) * (1.0 - t);
        viewport_.setViewPosition(static_cast<int>(std::lround(scroll_from_ + (scroll_to_ - scroll_from_) * e)), 0);
        if (scroll_step_ >= 5) scroll_step_ = -1;
    }
    // The step readout: where the next note goes, its value, voice and velocity.
    juce::String text;
    if (const auto cur = cursor ? cursor() : std::nullopt) {
        const auto& m = context_.session.data().meter;
        const int bar = m.bar_at(cur->tick);
        const auto beat_ticks = std::max<Ticks>(1, m.at_bar(bar).beat_ticks());
        const double beat = 1.0 + static_cast<double>(cur->tick - m.bar_start(bar)) / static_cast<double>(beat_ticks);
        juce::String b = juce::String(beat, 2);
        while (b.contains(".") && (b.endsWith("0") || b.endsWith("."))) b = b.dropLastCharacters(1);
        text << "NEXT " << juce::String::fromUTF8("\xe2\x96\xb8") << " BAR " << bar << juce::String::fromUTF8(" \xc2\xb7 ") << "BEAT "
             << b << "\n"
             << "VALUE " << (cur->rest_mode ? "REST " : "") << value_name(cur->duration) << juce::String::fromUTF8(" \xc2\xb7 ")
             << "VOICE " << (cur->voice + 1) << juce::String::fromUTF8(" \xc2\xb7 ") << "VEL " << last_velocity_;
    } else {
        text << "STEP INPUT\nSelect an instrument track";
    }
    step_lcd_.set_text(text);
}

void KeyboardDeck::resized()
{
    auto r = getLocalBounds().reduced(18, 0);
    // Header: name, range and Middle C on the left; key zoom on the right.
    auto header = r.withY(kHeaderTop).withHeight(kHeaderH);
    header.removeFromLeft(78);  // "KEYBOARD"
    range_lcd_.setBounds(header.removeFromLeft(132).reduced(0, 1));
    header.removeFromLeft(8);
    middle_c_.setBounds(header.removeFromLeft(78));
    to_sel_.setBounds(header.removeFromRight(124));
    header.removeFromRight(6);
    fit_.setBounds(header.removeFromRight(68));
    header.removeFromRight(8);
    zoom_lcd_.setBounds(header.removeFromRight(58).reduced(0, 1));
    header.removeFromRight(6);
    plus_.setBounds(header.removeFromRight(26));
    header.removeFromRight(4);
    slider_.setBounds(header.removeFromRight(150));
    header.removeFromRight(4);
    minus_.setBounds(header.removeFromRight(26));

    auto body = r.withTop(kBodyTop).withBottom(getHeight() - kPadBottom);
    // Left: the wheels, then the octave shift.
    auto left = body.removeFromLeft(kLeftColumn);
    const int wheel_h = std::min(84, left.getHeight() - 12);
    pitch_.setBounds(left.getX() + 2, left.getY() + 12, 28, wheel_h);
    mod_.setBounds(left.getX() + 40, left.getY() + 12, 28, wheel_h);
    auto oct = juce::Rectangle<int>(left.getX() + 84, left.getY() + 12, 74, 60);
    octave_lcd_.setBounds(oct.removeFromTop(22));
    oct.removeFromTop(6);
    oct_down_.setBounds(oct.removeFromLeft(34).withHeight(24));
    oct_up_.setBounds(oct.removeFromRight(34).withHeight(24));
    body.removeFromLeft(10);
    // Right: step input.
    auto right = body.removeFromRight(kRightColumn);
    body.removeFromRight(10);
    right.removeFromTop(10);
    step_lcd_.setBounds(right.removeFromTop(std::min(48, right.getHeight() - 32)).withTrimmedTop(2));
    right.removeFromTop(6);
    auto keys_row = right.removeFromTop(26);
    const int kw = (keys_row.getWidth() - 12) / 3;
    chord_.setBounds(keys_row.removeFromLeft(kw));
    tie_.setBounds(keys_row.withTrimmedLeft(6).removeFromLeft(kw));
    enter_.setBounds(keys_row.removeFromRight(kw));
    // The keyboard in its well, scrolling sideways.
    viewport_.setBounds(body.withHeight(keys_.getHeight() + kScrollbar));
}

void KeyboardDeck::paintOverChildren(juce::Graphics& g)
{
    hw::paint::silk(g, "Keyboard", {30.0f, static_cast<float>(kHeaderTop), 80.0f, static_cast<float>(kHeaderH)}, 11.0f);
    hw::paint::silk(g, "Key zoom",
                    {static_cast<float>(minus_.getX() - 70), static_cast<float>(kHeaderTop), 64.0f, static_cast<float>(kHeaderH)},
                    10.0f, juce::Justification::centredRight);
    const float label_y = static_cast<float>(pitch_.getY() - 12);
    hw::paint::silk(g, "Pitch", {static_cast<float>(pitch_.getX() - 6), label_y, 40.0f, 10.0f}, 9.0f, juce::Justification::centred);
    hw::paint::silk(g, "Mod", {static_cast<float>(mod_.getX() - 6), label_y, 40.0f, 10.0f}, 9.0f, juce::Justification::centred);
    hw::paint::silk(g, "Octave", {static_cast<float>(octave_lcd_.getX()), label_y, 74.0f, 10.0f}, 9.0f, juce::Justification::centred);
    hw::paint::silk(g, "Step input", {static_cast<float>(step_lcd_.getX()), label_y, 120.0f, 10.0f}, 9.0f);
    // The keyboard's recessed well, with a lip over the keys.
    const auto well = viewport_.getBounds().toFloat().expanded(3.0f, 3.0f);
    g.setColour(juce::Colour(0xff08090a));
    g.drawRoundedRectangle(well, 4.0f, 3.0f);
}

}  // namespace vcc::desktop::shell
