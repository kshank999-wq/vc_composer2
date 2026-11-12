#include "ui/TransportBar.h"

#include "ui/Theme.h"
#include "vcc/theory/Pitch.h"

namespace vcc::desktop {

namespace {
const char* const kMeters[] = {"2/4", "3/4", "4/4", "5/4", "6/4", "3/8", "5/8", "6/8", "7/8", "9/8", "12/8", "2/2"};
const char* const kTonics[] = {"C", "G", "D", "A", "E", "B", "F#", "Db", "Ab", "Eb", "Bb", "F"};
}  // namespace

TransportBar::TransportBar(AppContext& context) : context_(context)
{
    for (auto* b : {&rewind_, &play_, &click_, &composer_, &sequencer_, &mixer_, &picture_, &cues_, &chords_, &ostinato_})
        addAndMakeVisible(*b);
    addAndMakeVisible(record_);
    addAndMakeVisible(punch_);
    addAndMakeVisible(pdc_);
    pdc_.setComponentID("transport_pdc");
    pdc_.setClickingTogglesState(true);
    pdc_.setColour(juce::TextButton::buttonOnColourId, colours::accent.withAlpha(0.45f));
    pdc_.setTooltip("Plug-in delay compensation: delays everything to line up with the slowest plug-in path");
    pdc_.onClick = [this] {
        context_.engine.set_delay_compensation(pdc_.getToggleState());
        update_position();
    };
    record_.setTooltip("Record enable (R): recording runs while playing, onto every armed track");
    record_.setComponentID("transport_record");
    record_.onClick = [this] { context_.commands.invokeDirectly(cmd::toggleRecord, true); };
    punch_.setTooltip("Punch in/out: record only inside the range dragged on the sequencer ruler");
    punch_.setComponentID("transport_punch");
    punch_.setColour(juce::TextButton::buttonOnColourId, colours::playhead.withAlpha(0.7f));
    punch_.onClick = [this] { context_.commands.invokeDirectly(cmd::togglePunch, true); };
    rewind_.setTooltip("Return to the start (Home)");
    play_.setTooltip("Play / Stop (Space)");
    click_.setClickingTogglesState(true);
    click_.setTooltip("Metronome");
    rewind_.onClick = [this] { context_.engine.locate(0); };
    play_.onClick = [this] { context_.engine.toggle_play(); update_position(); };
    click_.onClick = [this] { context_.engine.set_metronome(click_.getToggleState()); };
    auto invoke = [this](juce::CommandID id) { return [this, id] { context_.commands.invokeDirectly(id, true); }; };
    composer_.onClick = invoke(cmd::showComposer);
    sequencer_.onClick = invoke(cmd::showSequencer);
    mixer_.onClick = invoke(cmd::showMixer);
    picture_.onClick = invoke(cmd::showVideo);
    cues_.onClick = invoke(cmd::showCueManager);
    chords_.onClick = invoke(cmd::chordExplorer);
    ostinato_.onClick = invoke(cmd::ostinatoCreator);

    for (auto* l : {&bars_, &timecode_}) {
        addAndMakeVisible(*l);
        l->setJustificationType(juce::Justification::centred);
        l->setColour(juce::Label::backgroundColourId, colours::background);
        l->setColour(juce::Label::outlineColourId, colours::outline);
    }
    bars_.setFont(juce::FontOptions(20.0f, juce::Font::bold));
    timecode_.setFont(juce::FontOptions(15.0f));
    timecode_.setColour(juce::Label::textColourId, colours::accent_warm);

    addAndMakeVisible(rec_time_);
    rec_time_.setComponentID("transport_rec_time");
    rec_time_.setJustificationType(juce::Justification::centred);
    rec_time_.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    rec_time_.setColour(juce::Label::backgroundColourId, colours::background);
    rec_time_.setColour(juce::Label::outlineColourId, colours::outline);
    rec_time_.setTooltip("Length of the pass being recorded");
    for (auto* l : {&tempo_label_, &meter_label_, &key_label_, &rec_label_}) {
        addAndMakeVisible(*l);
        l->setFont(juce::FontOptions(11.0f));
        l->setColour(juce::Label::textColourId, colours::text_dim);
    }
    addAndMakeVisible(tempo_);
    tempo_.setEditable(true);
    tempo_.setJustificationType(juce::Justification::centred);
    tempo_.setColour(juce::Label::outlineColourId, colours::outline);
    tempo_.onTextChange = [this] {
        const double bpm = tempo_.getText().getDoubleValue();
        if (!context_.session.set_tempo(bpm)) context_.status("Tempo must be between 20 and 400");
        update_from_document();
    };

    addAndMakeVisible(meter_);
    for (int i = 0; i < static_cast<int>(std::size(kMeters)); ++i) meter_.addItem(kMeters[i], i + 1);
    meter_.onChange = [this] {
        if (auto sig = TimeSignature::parse(meter_.getText().toStdString())) context_.session.set_meter(*sig);
    };

    addAndMakeVisible(key_);
    int id = 1;
    for (const char* mode : {"major", "minor"})
        for (const char* tonic : kTonics) key_.addItem(juce::String(tonic) + " " + mode, id++);
    for (const char* mode : {"dorian", "phrygian", "lydian", "mixolydian"})
        for (const char* tonic : {"C", "D", "E", "F", "G", "A"}) key_.addItem(juce::String(tonic) + " " + mode, id++);
    key_.onChange = [this] { context_.session.set_key(key_.getText().toStdString()); };

    listener_ = context_.session.add_listener([this](session::Change c) {
        if (c == session::Change::Document) update_from_document();
    });
    update_from_document();
    update_position();
}

TransportBar::~TransportBar() { context_.session.remove_listener(listener_); }

void TransportBar::update_from_document()
{
    const auto& d = context_.session.data();
    const double bpm = d.tempo.bpm_at(0);
    const bool whole = std::abs(bpm - std::round(bpm)) < 0.005;
    tempo_.setText(juce::String(bpm, whole ? 0 : 2) + " bpm", juce::dontSendNotification);
    meter_.setText(d.meter.at_bar(1).to_string(), juce::dontSendNotification);
    key_.setText(d.key, juce::dontSendNotification);
}

void TransportBar::update_position()
{
    const auto& d = context_.session.data();
    const Ticks tick = context_.engine.position();
    const int bar = d.meter.bar_at(tick);
    const TimeSignature sig = d.meter.at_bar(bar);
    const Ticks in_bar = tick - d.meter.bar_start(bar);
    const int beat = static_cast<int>(in_bar / sig.beat_ticks()) + 1;
    const int sixteenth = static_cast<int>((in_bar % sig.beat_ticks()) * 4 / sig.beat_ticks()) + 1;
    bars_.setText(juce::String(bar) + " . " + juce::String(beat) + " . " + juce::String(sixteenth),
                  juce::dontSendNotification);

    // Timecode from the project's start timecode and frame rate (Main Spec §13).
    const FrameRate rate = d.frame_rate;
    std::int64_t start_frames = 0;
    if (auto tc = Timecode::parse(d.start_timecode))
        if (auto f = timecode_to_frames(*tc, rate)) start_frames = *f;
    const std::int64_t frames = start_frames + seconds_to_frames(context_.engine.position_seconds(), rate);
    timecode_.setText(frames_to_timecode(frames, rate).to_string(rate) + "  " + to_string(rate),
                      juce::dontSendNotification);

    const bool playing = context_.engine.playing();
    play_.setButtonText(playing ? "Stop" : "Play");
    play_.setColour(juce::TextButton::buttonColourId, playing ? colours::accent : colours::panel_raised);

    const bool recording = context_.engine.recording();
    record_.set_state(context_.engine.record_enabled(), recording);
    const double secs = recording ? context_.engine.record_seconds() : 0.0;
    const int whole = static_cast<int>(secs);
    rec_time_.setText(juce::String(whole / 60) + ":" + juce::String(whole % 60).paddedLeft('0', 2) + "." +
                          juce::String(static_cast<int>((secs - whole) * 10.0)),
                      juce::dontSendNotification);
    rec_time_.setColour(juce::Label::textColourId, recording ? colours::playhead : colours::text_dim);

    // Delay compensation and how much it adds.
    const bool pdc = context_.engine.delay_compensation();
    juce::String pdc_text = "PDC";
    if (pdc) {
        const int samples = context_.engine.compensation_samples();
        double device_rate = 48000.0;
        if (auto* device = context_.engine.devices().getCurrentAudioDevice()) device_rate = device->getCurrentSampleRate();
        if (samples > 0 && device_rate > 0.0) pdc_text << " " << juce::String(1000.0 * samples / device_rate, 1) << " ms";
    } else {
        pdc_text << " off";
    }
    if (pdc_.getToggleState() != pdc) pdc_.setToggleState(pdc, juce::dontSendNotification);
    if (pdc_.getButtonText() != pdc_text) pdc_.setButtonText(pdc_text);
}

void TransportBar::set_punch_state(bool on) { punch_.setToggleState(on, juce::dontSendNotification); }

void TransportBar::RecordButton::set_state(bool enabled, bool recording)
{
    // Repaint every frame while recording, so the pulse moves.
    if (enabled == enabled_ && recording == recording_ && !recording) return;
    enabled_ = enabled;
    recording_ = recording;
    repaint();
}

void TransportBar::RecordButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
    auto r = getLocalBounds().toFloat().reduced(1.0f);
    g.setColour(down ? colours::panel_raised.brighter(0.1f) : colours::panel_raised);
    g.fillRoundedRectangle(r, 4.0f);
    g.setColour(highlighted ? colours::outline.brighter(0.3f) : colours::outline);
    g.drawRoundedRectangle(r, 4.0f, 1.0f);

    const float d = std::min(r.getWidth(), r.getHeight()) * 0.5f;
    const auto dot = r.withSizeKeepingCentre(d, d);
    const juce::Colour red{0xffe23b2e};
    if (enabled_ || recording_) {
        // Glow: steady when armed, pulsing (about once a second) while recording.
        float glow = 0.55f;
        if (recording_) {
            const double phase = std::fmod(juce::Time::getMillisecondCounterHiRes() / 1000.0, 1.0);
            glow = 0.35f + 0.65f * static_cast<float>(0.5 + 0.5 * std::cos(phase * juce::MathConstants<double>::twoPi));
        }
        for (int i = 3; i >= 1; --i) {
            g.setColour(red.withAlpha(0.12f * glow * static_cast<float>(4 - i)));
            g.fillEllipse(dot.expanded(static_cast<float>(i) * 2.5f));
        }
        g.setColour(recording_ ? red.interpolatedWith(juce::Colours::white, 0.25f * glow) : red);
        g.fillEllipse(dot);
    } else {
        g.setColour(red.withAlpha(highlighted ? 0.6f : 0.4f));
        g.fillEllipse(dot);
        g.setColour(red.withAlpha(0.9f));
        g.drawEllipse(dot, 1.2f);
    }
}

void TransportBar::paint(juce::Graphics& g)
{
    g.fillAll(colours::panel);
    g.setColour(colours::outline);
    g.drawHorizontalLine(getHeight() - 1, 0.0f, static_cast<float>(getWidth()));
}

void TransportBar::resized()
{
    auto r = getLocalBounds().reduced(8, 6);
    auto take = [&](int w) {
        auto a = r.removeFromLeft(w);
        r.removeFromLeft(6);
        return a;
    };
    rewind_.setBounds(take(40));
    play_.setBounds(take(64));
    record_.setBounds(take(40));
    punch_.setBounds(take(54));
    click_.setBounds(take(50));
    pdc_.setBounds(take(84));
    r.removeFromLeft(6);
    bars_.setBounds(take(112));
    timecode_.setBounds(take(160));
    auto field = [&](juce::Label& label, juce::Component& c, int w) {
        auto a = take(w);
        label.setBounds(a.removeFromTop(12));
        c.setBounds(a.withTrimmedTop(&c == &rec_time_ ? 1 : 0));
    };
    field(rec_label_, rec_time_, 64);
    r.removeFromLeft(6);
    field(tempo_label_, tempo_, 84);
    field(meter_label_, meter_, 70);
    field(key_label_, key_, 124);

    // The view and tool buttons on the right; those that would run into the
    // transport on a narrow window are hidden.
    const int left_edge = key_.getRight() + 12;
    auto right = getLocalBounds().reduced(8, 6);
    for (auto* b : {&ostinato_, &chords_, &cues_, &picture_, &mixer_, &sequencer_, &composer_}) {
        b->setBounds(right.removeFromRight(b == &sequencer_ ? 80 : 60));
        right.removeFromRight(6);
    }
    for (auto* b : {&ostinato_, &chords_, &cues_, &picture_, &mixer_, &sequencer_, &composer_})
        b->setVisible(b->getX() >= left_edge);
}

}  // namespace vcc::desktop
