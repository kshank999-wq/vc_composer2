#include "ui/shell/TopDeck.h"

#include <cmath>

namespace vcc::desktop::shell {

namespace {
const char* const kMeters[] = {"2/4", "3/4", "4/4", "5/4", "6/4", "3/8", "5/8", "6/8", "7/8", "9/8", "12/8", "2/2"};
const char* const kTonics[] = {"C", "G", "D", "A", "E", "B", "F#", "Db", "Ab", "Eb", "Bb", "F"};

/// Ticks at the counters' resolution (960 to the quarter) from the engine's (2880).
constexpr Ticks kDisplayDivisor = kTicksPerQuarter / 960;

// Natural widths of the deck's panels at the spec's reference size.
constexpr int kNameplate = 268, kTransport = 392, kCounter = 480, kSession = 240, kMasterMin = 200;
constexpr int kRow = 96, kGap = hw::tok::gap_panel;

// Narrowest each flexible panel may go before the deck wraps to two rows.
constexpr int kTransportMin = 300, kCounterMin = 420, kMasterNarrow = 140;

bool one_row(int width)
{
    return width >= kNameplate + kTransportMin + kCounterMin + kSession + kMasterNarrow + 4 * kGap;
}

juce::String min_sec(double seconds)
{
    if (seconds < 0.0) seconds = 0.0;
    const int whole = static_cast<int>(seconds);
    const int ms = static_cast<int>(std::round((seconds - whole) * 1000.0)) % 1000;
    return juce::String(whole / 60).paddedLeft('0', 2) + ":" + juce::String(whole % 60).paddedLeft('0', 2) + "." +
           juce::String(ms).paddedLeft('0', 3);
}

/// A small text entry in a call-out (the tempo).
class Entry final : public juce::Component {
public:
    Entry(const juce::String& text, std::function<void(const juce::String&)> done) : done_(std::move(done))
    {
        editor_.setText(text, false);
        editor_.setJustification(juce::Justification::centred);
        editor_.setFont(hw::fonts::lcd(18.0f));
        editor_.setColour(juce::TextEditor::backgroundColourId, hw::tok::lcd_bg);
        editor_.setColour(juce::TextEditor::textColourId, hw::Theme::get().lcd());
        editor_.selectAll();
        editor_.onReturnKey = [this] { finish(); };
        editor_.onEscapeKey = [this] { close(); };
        addAndMakeVisible(editor_);
        setSize(120, 36);
    }
    void resized() override { editor_.setBounds(getLocalBounds().reduced(3)); }
    void visibilityChanged() override
    {
        if (isShowing()) editor_.grabKeyboardFocus();
    }

private:
    void finish()
    {
        if (done_) done_(editor_.getText());
        close();
    }
    void close()
    {
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss();
    }
    juce::TextEditor editor_;
    std::function<void(const juce::String&)> done_;
};
}  // namespace

juce::String bars_beats_ticks(const MeterMap& meter, Ticks tick)
{
    tick = std::max<Ticks>(0, tick);
    const int bar = meter.bar_at(tick);
    const TimeSignature sig = meter.at_bar(bar);
    const Ticks in_bar = tick - meter.bar_start(bar);
    const Ticks beat_len = std::max<Ticks>(1, sig.beat_ticks());
    const int beat = static_cast<int>(in_bar / beat_len) + 1;
    const Ticks ticks = (in_bar % beat_len) / kDisplayDivisor;
    return juce::String(bar) + "|" + juce::String(beat) + "|" + juce::String(ticks).paddedLeft('0', 3);
}

juce::String length_bbt(const MeterMap& meter, Ticks from, Ticks to)
{
    if (to <= from) return "0|0|000";
    const TimeSignature sig = meter.at_bar(meter.bar_at(from));
    const Ticks bar_len = std::max<Ticks>(1, sig.ticks_per_bar());
    const Ticks beat_len = std::max<Ticks>(1, sig.beat_ticks());
    const Ticks len = to - from;
    const Ticks bars = len / bar_len;
    const Ticks beats = (len % bar_len) / beat_len;
    const Ticks ticks = (len % beat_len) / kDisplayDivisor;
    return juce::String(bars) + "|" + juce::String(beats) + "|" + juce::String(ticks).paddedLeft('0', 3);
}

TopDeck::TopDeck(AppContext& context) : context_(context)
{
    for (auto* p : {&nameplate_, &transport_panel_, &counter_panel_, &session_panel_, &master_panel_})
        addAndMakeVisible(*p);

    // The view switch: two positions, the lit one is the view on screen.
    for (const char* name : {"Composer", "Producer"}) {
        view_.push_back(std::make_unique<hw::Keycap>(name, false));
        auto& k = *view_.back();
        k.set_led_dot(true);
        addAndMakeVisible(k);
    }
    view_[0]->setTooltip("Composer view: the score (Ctrl+Tab switches)");
    view_[1]->setTooltip("Producer view: MIDI, audio and mixing on the same tracks (Ctrl+Tab switches)");
    view_[0]->onClick = [this] {
        if (on_view) on_view(false);
    };
    view_[1]->onClick = [this] {
        if (on_view) on_view(true);
    };

    // Transport.
    const hw::Glyph glyphs[] = {hw::Glyph::ReturnToZero, hw::Glyph::Rewind, hw::Glyph::FastForward, hw::Glyph::Stop,
                                hw::Glyph::Play,         hw::Glyph::Record, hw::Glyph::Loop};
    const char* names[] = {"Return to zero", "Rewind", "Fast forward", "Stop", "Play", "Record", "Loop"};
    const char* tips[] = {"Return to the start (Home)", "Back one bar", "Forward one bar", "Stop",
                          "Play / stop (Space)", "Record enable (R): records while playing, onto every armed track",
                          "Loop playback (coming with the Producer view tracks)"};
    for (int i = 0; i < 7; ++i) {
        transport_.push_back(std::make_unique<hw::Keycap>(names[i], false));
        auto& k = *transport_.back();
        k.set_glyph(glyphs[i]);
        k.setTooltip(tips[i]);
        addAndMakeVisible(k);
    }
    transport_[4]->set_glyph_lit(hw::tok::meter_green);
    transport_[4]->set_lit_colour(hw::tok::meter_green);
    transport_[5]->set_glyph_lit(hw::tok::rec);
    transport_[5]->setComponentID("transport_record");
    transport_[6]->setEnabled(false);
    auto& engine = context_.engine;
    transport_[0]->onClick = [this] { context_.engine.locate(0); };
    transport_[1]->onClick = [this] {
        const auto& m = context_.session.data().meter;
        const Ticks at = context_.engine.position();
        const int bar = m.bar_at(at);
        context_.engine.locate(m.bar_start(at > m.bar_start(bar) ? bar : std::max(1, bar - 1)));
    };
    transport_[2]->onClick = [this] {
        const auto& m = context_.session.data().meter;
        context_.engine.locate(m.bar_start(m.bar_at(context_.engine.position()) + 1));
    };
    transport_[3]->onClick = [this] {
        if (context_.engine.playing()) context_.engine.toggle_play();
        update_position();
    };
    transport_[4]->onClick = [this] {
        context_.engine.toggle_play();
        update_position();
    };
    transport_[5]->onClick = [this] { context_.commands.invokeDirectly(cmd::toggleRecord, true); };
    juce::ignoreUnused(engine);

    // Counters: the main one in bars|beats|ticks; the sub one in min:sec or
    // timecode (click to switch); the range readout.
    main_.set_caption(juce::String::fromUTF8("Main  \xc2\xb7  Bars|Beats"));
    main_.setComponentID("counter_main");
    sub_.set_caption(juce::String::fromUTF8("Sub  \xc2\xb7  Min:Sec"));
    sub_.setTooltip("Click for timecode / minutes and seconds");
    sub_.set_on_click([this] {
        show_timecode_ = !show_timecode_;
        sub_.set_caption(juce::String::fromUTF8(show_timecode_ ? "Sub  \xc2\xb7  Timecode" : "Sub  \xc2\xb7  Min:Sec"));
        update_position();
    });
    for (auto* l : {&main_, &range_, &sub_}) addAndMakeVisible(*l);

    // Session readouts and keys.
    tempo_.set_on_click([this] { edit_tempo(); });
    meter_.set_on_click([this] { choose_meter(); });
    key_.set_on_click([this] { choose_key(); });
    tempo_.setTooltip("Tempo (click to change)");
    meter_.setTooltip("Meter (click to change)");
    key_.setTooltip("Key (click to change)");
    for (auto* l : {&tempo_, &meter_, &key_}) {
        l->set_justification(juce::Justification::centred);
        addAndMakeVisible(*l);
    }
    for (const char* n : {"Click", "Count-in", "Wait note"}) {
        session_keys_.push_back(std::make_unique<hw::Keycap>(n, true));
        auto& k = *session_keys_.back();
        k.set_led_dot(true);
        k.set_legend_size(11.0f);
        addAndMakeVisible(k);
    }
    session_keys_[0]->setTooltip("Metronome");
    session_keys_[0]->onClick = [this] { context_.engine.set_metronome(session_keys_[0]->getToggleState()); };
    session_keys_[1]->setTooltip("Count-in before recording (coming with recording in the Producer view)");
    session_keys_[2]->setTooltip("Wait for the first note before recording (coming with MIDI recording)");
    session_keys_[1]->setEnabled(false);
    session_keys_[2]->setEnabled(false);

    // Master meters.
    addAndMakeVisible(master_l_);
    addAndMakeVisible(master_r_);
    peak_.set_justification(juce::Justification::centred);
    addAndMakeVisible(peak_);

    listener_ = context_.session.add_listener([this](session::Change c) {
        if (c == session::Change::Document) update_from_document();
    });
    update_from_document();
    set_producer(false);
    update_position();
}

TopDeck::~TopDeck() { context_.session.remove_listener(listener_); }

int TopDeck::height_for(int width) { return one_row(width - 2 * hw::tok::pad_page) ? kRow : 2 * kRow + kGap; }

void TopDeck::set_producer(bool producer)
{
    view_[0]->setToggleState(!producer, juce::dontSendNotification);
    view_[1]->setToggleState(producer, juce::dontSendNotification);
}

void TopDeck::set_range(Ticks from, Ticks to)
{
    range_from_ = from;
    range_to_ = to;
}

void TopDeck::update_from_document()
{
    const auto& d = context_.session.data();
    const double bpm = d.tempo.bpm_at(0);
    tempo_.set_text(juce::String(bpm, 2));
    meter_.set_text(d.meter.at_bar(1).to_string());
    // "D major" -> "D MAJ", "E dorian" -> "E DOR".
    auto words = juce::StringArray::fromTokens(juce::String(d.key), " ", "");
    juce::String k = words.isEmpty() ? juce::String("C") : words[0];
    if (words.size() > 1) k << " " << words[1].substring(0, 3).toUpperCase();
    key_.set_text(k);
}

void TopDeck::update_position()
{
    const auto& d = context_.session.data();
    auto& engine = context_.engine;
    const Ticks tick = engine.position();
    main_.set_text(bars_beats_ticks(d.meter, tick));
    if (show_timecode_) {
        std::int64_t start_frames = 0;
        if (auto tc = Timecode::parse(d.start_timecode))
            if (auto f = timecode_to_frames(*tc, d.frame_rate)) start_frames = *f;
        const std::int64_t frames = start_frames + seconds_to_frames(engine.position_seconds(), d.frame_rate);
        sub_.set_text(frames_to_timecode(frames, d.frame_rate).to_string(d.frame_rate));
    } else {
        sub_.set_text(min_sec(engine.position_seconds()));
    }
    const bool range = range_to_ > range_from_;
    const Ticks from = range ? range_from_ : tick;
    const Ticks to = range ? range_to_ : tick;
    range_.set_text("START   " + bars_beats_ticks(d.meter, from) + "\nEND     " + bars_beats_ticks(d.meter, to) +
                    "\nLENGTH  " + length_bbt(d.meter, from, to));

    transport_[4]->setToggleState(engine.playing(), juce::dontSendNotification);
    // Record: solid red when enabled, flashing while a pass is recording.
    const bool armed = engine.record_enabled();
    const bool recording = engine.recording();
    blink_ = (blink_ + 1) % 30;
    transport_[5]->set_solid(recording && blink_ >= 15 ? hw::tok::rec.darker(0.5f) : hw::tok::rec);
    transport_[5]->setToggleState(armed || recording, juce::dontSendNotification);

    // Master meters and the held peak.
    const auto m = engine.take_deck_meter("bus-master");
    auto db = [](float lin) { return lin > 0.0f ? 20.0f * std::log10(lin) : -100.0f; };
    const float l = db(m.left), r = db(m.right);
    master_l_.push(l);
    master_r_.push(r);
    const float loud = std::max(l, r);
    if (loud >= peak_hold_db_ || ++peak_age_ > 90) {
        peak_hold_db_ = std::max(loud, -100.0f);
        peak_age_ = 0;
    }
    peak_.set_text(peak_hold_db_ <= -99.0f ? juce::String("PEAK  -INF") : "PEAK " + juce::String(peak_hold_db_, 1));
}

void TopDeck::edit_tempo()
{
    auto entry = std::make_unique<Entry>(tempo_.text(), [this](const juce::String& text) {
        if (!context_.session.set_tempo(text.getDoubleValue()) && context_.status)
            context_.status("Tempo must be between 20 and 400");
        update_from_document();
    });
    juce::CallOutBox::launchAsynchronously(std::move(entry), tempo_.getScreenBounds(), nullptr);
}

void TopDeck::choose_meter()
{
    juce::PopupMenu m;
    const auto current = context_.session.data().meter.at_bar(1).to_string();
    for (const char* text : kMeters)
        m.addItem(text, true, current == text, [this, text] {
            if (auto sig = TimeSignature::parse(text)) context_.session.set_meter(*sig);
        });
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&meter_));
}

void TopDeck::choose_key()
{
    juce::PopupMenu m;
    const auto current = context_.session.data().key;
    auto add = [&](juce::PopupMenu& into, const juce::String& key) {
        into.addItem(key, true, current == key.toStdString(),
                     [this, key] { context_.session.set_key(key.toStdString()); });
    };
    for (const char* mode : {"major", "minor"}) {
        juce::PopupMenu sub;
        for (const char* tonic : kTonics) add(sub, juce::String(tonic) + " " + mode);
        m.addSubMenu(juce::String(mode).substring(0, 1).toUpperCase() + juce::String(mode).substring(1), sub);
    }
    juce::PopupMenu modes;
    for (const char* mode : {"dorian", "phrygian", "lydian", "mixolydian"})
        for (const char* tonic : {"C", "D", "E", "F", "G", "A"}) add(modes, juce::String(tonic) + " " + mode);
    m.addSubMenu("Modes", modes);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&key_));
}

void TopDeck::paint(juce::Graphics&) {}

void TopDeck::paintOverChildren(juce::Graphics& g)
{
    // The brand: "VC COMPOSER" in silk, the "2" lit.
    {
        const auto b = nameplate_.getBounds().toFloat();
        auto brand = juce::Rectangle<float>(b.getX() + 22.0f, b.getY() + 12.0f, b.getWidth() - 44.0f, 32.0f);
        hw::paint::silk(g, "VC", brand.removeFromLeft(26.0f).withTrimmedTop(8.0f), 11.0f);
        const auto word = hw::fonts::silk(24.0f, 0.22f, true);
        g.setColour(hw::tok::text_bright);
        g.setFont(word);
        g.drawText("COMPOSER", brand, juce::Justification::centredLeft, false);
        const float word_w = juce::GlyphArrangement::getStringWidth(word, "COMPOSER");
        juce::GlyphArrangement two;
        two.addLineOfText(hw::fonts::silk(28.0f, 0.0f, true), "2", brand.getX() + word_w + 4.0f,
                          brand.getBottom() - 4.0f);
        juce::Path p;
        two.createPath(p);
        const auto& theme = hw::Theme::get();
        juce::DropShadow(theme.led_glow(), 8, {}).drawForPath(g, p);
        g.setColour(theme.led());
        g.fillPath(p);
    }
    // Session labels.
    for (auto [lcd, text] : {std::pair<hw::Lcd*, const char*>{&tempo_, "Tempo"}, {&meter_, "Meter"}, {&key_, "Key"}})
        hw::paint::silk(g, text, lcd->getBounds().toFloat().withY(static_cast<float>(lcd->getY() - 16)).withHeight(14.0f),
                        10.0f);
    // Master scale.
    {
        const auto l = master_l_.getBounds().toFloat();
        const auto r = master_r_.getBounds().toFloat();
        hw::paint::silk(g, "L", l.withX(l.getX() - 16.0f).withWidth(12.0f).expanded(0.0f, 4.0f), 10.0f);
        hw::paint::silk(g, "R", r.withX(r.getX() - 16.0f).withWidth(12.0f).expanded(0.0f, 4.0f), 10.0f);
        // The scale, where there is room beside the peak readout.
        const float room = static_cast<float>(peak_.getX()) - l.getX() - 6.0f;
        hw::paint::silk(g, room >= 150.0f ? "-60   -18   -6   0 dBFS" : "dBFS", {l.getX(), r.getBottom() + 8.0f, room, 14.0f},
                        9.0f);
    }
}

void TopDeck::resized()
{
    const int w = getWidth();
    const bool single = one_row(w);
    // Row 1: nameplate, transport, counter (and, on one row, session and master).
    int x = 0;
    auto place = [&](juce::Component& c, int width, int y) {
        c.setBounds(x, y, width, kRow);
        x += width + kGap;
    };
    if (single) {
        // Natural widths when they fit; otherwise transport, counter and
        // master give up the shortfall in proportion to what they can spare.
        int transport = kTransport, counter = kCounter;
        const int natural = kNameplate + kTransport + kCounter + kSession + kMasterMin + 4 * kGap;
        if (w < natural) {
            const int short_by = natural - w;
            const int spare_t = kTransport - kTransportMin, spare_c = kCounter - kCounterMin,
                      spare_m = kMasterMin - kMasterNarrow;
            const int spare = spare_t + spare_c + spare_m;
            transport -= short_by * spare_t / spare;
            counter -= short_by * spare_c / spare;
        }
        place(nameplate_, kNameplate, 0);
        place(transport_panel_, transport, 0);
        place(counter_panel_, counter, 0);
        place(session_panel_, kSession, 0);
        master_panel_.setBounds(x, 0, std::max(kMasterNarrow, w - x), kRow);
    } else {
        place(nameplate_, kNameplate, 0);
        place(transport_panel_, kTransport, 0);
        counter_panel_.setBounds(x, 0, std::max(320, w - x), kRow);
        x = 0;
        place(session_panel_, kSession, kRow + kGap);
        master_panel_.setBounds(x, kRow + kGap, std::max(kMasterMin, w - x), kRow);
    }

    // Nameplate: the switch under the brand.
    {
        const auto b = nameplate_.getBounds();
        const int kw = (b.getWidth() - 44) / 2;
        view_[0]->setBounds(b.getX() + 20, b.getY() + 54, kw, 30);
        view_[1]->setBounds(b.getX() + 24 + kw, b.getY() + 54, kw, 30);
    }
    // Transport keys.
    {
        const auto b = transport_panel_.getBounds();
        const int kw = (b.getWidth() - 24 - 6 * 7) / 7;
        for (int i = 0; i < 7; ++i)
            transport_[static_cast<std::size_t>(i)]->setBounds(b.getX() + 12 + i * (kw + 7), b.getY() + 34, kw, 44);
    }
    // Counters: main | sub | start, end and length.
    {
        auto r = counter_panel_.getBounds().reduced(12, 10).withTrimmedBottom(2);
        range_.setBounds(r.removeFromRight(r.getWidth() >= 440 ? 160 : 132));
        r.removeFromRight(8);
        sub_.setBounds(r.removeFromRight(104));
        r.removeFromRight(4);
        main_.setBounds(r);
        main_.set_size(r.getWidth() >= 230 ? 34.0f : 28.0f);
    }
    // Session: tempo, meter, key; click, count-in, wait note.
    {
        const auto b = session_panel_.getBounds().reduced(12, 0);
        const int lw = (b.getWidth() - 16) / 3;
        tempo_.setBounds(b.getX(), b.getY() + 24, lw, 30);
        meter_.setBounds(b.getX() + lw + 8, b.getY() + 24, lw, 30);
        key_.setBounds(b.getX() + 2 * (lw + 8), b.getY() + 24, lw, 30);
        for (int i = 0; i < 3; ++i)
            session_keys_[static_cast<std::size_t>(i)]->setBounds(b.getX() + i * (lw + 8), b.getY() + 62, lw, 26);
    }
    // Master: L and R meters, the scale, the held peak.
    {
        const auto b = master_panel_.getBounds();
        const int mw = b.getWidth() - 48;
        master_l_.setBounds(b.getX() + 34, b.getY() + 32, mw, 7);
        master_r_.setBounds(b.getX() + 34, b.getY() + 46, mw, 7);
        peak_.setBounds(b.getRight() - 96, b.getY() + 66, 84, 20);
    }
}

}  // namespace vcc::desktop::shell
