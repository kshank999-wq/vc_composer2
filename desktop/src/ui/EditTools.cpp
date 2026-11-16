#include "ui/EditTools.h"

#include <algorithm>
#include <map>

#include <juce_audio_formats/juce_audio_formats.h>

#include "ui/Dialogs.h"
#include "ui/Theme.h"
#include "ui/hw/HwTheme.h"
#include "vcc/audio/TimeStretch.h"
#include "vcc/session/Relink.h"
#include "vcc/session/Retrospective.h"

namespace vcc::desktop {

namespace {

juce::File processed_file(const AppContext& context, const juce::String& clip_name, const juce::String& tag)
{
    juce::File folder{juce::String(context.session.audio_folder())};
    if (context.session.audio_folder().empty())
        folder = juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                     .getChildFile("VC Composer")
                     .getChildFile("Untitled Audio Files");
    folder.createDirectory();
    juce::String base = juce::File::createLegalFileName(clip_name).replaceCharacters(" ./", "___").trimCharactersAtEnd("_");
    if (base.isEmpty()) base = "Clip";
    for (int n = 1; n < 1000; ++n) {
        const auto f = folder.getChildFile(base + "_" + tag + "_" + juce::String(n).paddedLeft('0', 2) + ".wav");
        if (!f.existsAsFile()) return f;
    }
    return folder.getChildFile(base + "_" + tag + "_" + juce::Uuid().toString().substring(0, 6) + ".wav");
}

/// Reads a clip's source range at its file's own rate.
bool read_clip(const AppContext& context, const project::AudioClip& c, audio::Channels& out, double& rate, juce::String& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const juce::File file{juce::String(context.session.resolve_audio_path(c.file))};
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (!reader) {
        error = file.getFileName() + " could not be read";
        return false;
    }
    rate = reader->sampleRate > 0.0 ? reader->sampleRate : 48000.0;
    const auto start = static_cast<juce::int64>(std::llround(c.source_offset * rate));
    const auto len = static_cast<int>(std::llround(c.length * rate));
    if (len <= 0 || len > static_cast<int>(rate * 60 * 60)) {
        error = "the clip is empty or longer than an hour";
        return false;
    }
    const int channels = static_cast<int>(std::min<unsigned>(std::max<unsigned>(reader->numChannels, 1), 2));
    juce::AudioBuffer<float> buf(channels, len);
    buf.clear();
    reader->read(&buf, 0, len, start, true, channels > 1);
    out.assign(static_cast<std::size_t>(channels), std::vector<float>(static_cast<std::size_t>(len)));
    for (int ch = 0; ch < channels; ++ch)
        std::copy(buf.getReadPointer(ch), buf.getReadPointer(ch) + len, out[static_cast<std::size_t>(ch)].begin());
    return true;
}

bool write_wav(const juce::File& file, const audio::Channels& data, double rate)
{
    if (data.empty()) return false;
    const int channels = static_cast<int>(data.size());
    const int len = static_cast<int>(data.front().size());
    juce::AudioBuffer<float> buf(channels, len);
    for (int ch = 0; ch < channels; ++ch)
        std::copy(data[static_cast<std::size_t>(ch)].begin(), data[static_cast<std::size_t>(ch)].end(), buf.getWritePointer(ch));
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
    auto writer = juce::WavAudioFormat().createWriterFor(
        stream, juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(channels).withBitsPerSample(24));
    return writer && writer->writeFromAudioSampleBuffer(buf, 0, len);
}

/// Processes each clip into a new file, then repoints them all as one step.
juce::String process_clips(AppContext& context, const std::vector<std::pair<std::string, std::string>>& clips,
                           const juce::String& tag, const std::string& label, double ratio,
                           const std::function<audio::Channels(const audio::Channels&, double rate)>& op)
{
    if (clips.empty()) return "Select one or more clips first";
    struct Done {
        std::string track, clip, file;
        double length = 0.0, rate = 0.0;
        int channels = 1;
    };
    std::vector<Done> done;
    juce::StringArray problems;
    for (const auto& [track, id] : clips) {
        const auto* t = context.session.data().find_track(track);
        const project::AudioClip* c = nullptr;
        if (t && t->active_version())
            for (const auto& x : t->active_version()->audio_clips)
                if (x.id == id) c = &x;
        if (!c) continue;
        audio::Channels data;
        double rate = 48000.0;
        juce::String error;
        if (!read_clip(context, *c, data, rate, error)) {
            problems.add(juce::String(c->name) + ": " + error);
            continue;
        }
        const auto result = op(data, rate);
        const auto file = processed_file(context, juce::String(c->name.empty() ? t->name : c->name), tag);
        if (!write_wav(file, result, rate)) {
            problems.add("could not write " + file.getFileName());
            continue;
        }
        Done d;
        d.track = track;
        d.clip = id;
        d.file = session::relink::stored_path(file.getFullPathName().toStdString(), context.session.path());
        d.length = static_cast<double>(result.front().size()) / rate;
        d.rate = rate;
        d.channels = static_cast<int>(result.size());
        done.push_back(d);
    }
    if (done.empty()) return problems.isEmpty() ? juce::String("No clip to process") : "Not done: " + problems.joinIntoString("; ");
    const bool reverse = ratio <= 0.0;
    context.session.edit(label, [&](project::ProjectData& d) {
        for (const auto& x : done) {
            auto* t = d.find_track(x.track);
            auto* v = t ? t->active_version() : nullptr;
            if (!v) continue;
            for (auto& c : v->audio_clips)
                if (c.id == x.clip) {
                    c.file = x.file;
                    c.source_offset = 0.0;
                    if (reverse) {
                        std::swap(c.fade_in, c.fade_out);
                    } else {
                        c.fade_in *= ratio;
                        c.fade_out *= ratio;
                    }
                    c.length = x.length;
                    c.file_sample_rate = x.rate;
                    c.channels = x.channels;
                }
        }
        return true;
    });
    juce::String message = juce::String(label) + ": " + juce::String(static_cast<int>(done.size())) + " clip(s), new files in " +
                           juce::String(context.session.audio_folder().empty() ? "the scratch folder" : "Audio Files") +
                           " (the originals are untouched)";
    if (!problems.isEmpty()) message << " - skipped: " << problems.joinIntoString("; ");
    return message;
}

juce::Component::SafePointer<dialogs::ToolWindow>& filter_window()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}
juce::Component*& filter_slot()
{
    static juce::Component* p = nullptr;
    return p;
}

void save_filter(const PlaybackEngine::InputFilter& f)
{
    auto& ui = hw::ui_settings();
    ui.setValue("midi_filter", juce::String(static_cast<juce::int64>(PlaybackEngine::pack(f))));
    ui.saveIfNeeded();
}

class FilterPanel final : public juce::Component {
public:
    explicit FilterPanel(AppContext& context) : context_(context)
    {
        setComponentID("midi_input_filter");
        filter_slot() = this;
        const auto f = context_.engine.input_filter();
        title_.setText("MIDI input: what plays and records", juce::dontSendNotification);
        title_.setFont(juce::FontOptions(15.0f, juce::Font::bold));
        title_.setColour(juce::Label::textColourId, colours::text);
        for (int ch = 0; ch < 16; ++ch) {
            auto b = std::make_unique<juce::ToggleButton>(juce::String(ch + 1));
            b->setTitle("Channel " + juce::String(ch + 1));
            b->setToggleState((f.channels >> ch) & 1, juce::dontSendNotification);
            b->onClick = [this] { push(); };
            addAndMakeVisible(*b);
            channels_.push_back(std::move(b));
        }
        const char* types[] = {"Notes", "Controllers", "Pitch bend", "Aftertouch", "Program changes"};
        const bool on[] = {f.notes, f.controllers, f.pitch_bend, f.aftertouch, f.program};
        for (int i = 0; i < 5; ++i) {
            auto b = std::make_unique<juce::ToggleButton>(types[i]);
            b->setTitle(types[i]);
            b->setToggleState(on[i], juce::dontSendNotification);
            b->onClick = [this] { push(); };
            addAndMakeVisible(*b);
            types_.push_back(std::move(b));
        }
        auto knob = [this](juce::Slider& s, const juce::String& t, double lo, double hi, double v, bool note) {
            s.setSliderStyle(juce::Slider::IncDecButtons);
            s.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 60, 22);
            s.setRange(lo, hi, 1.0);
            s.setValue(v, juce::dontSendNotification);
            s.setTitle(t);
            if (note)
                s.textFromValueFunction = [](double x) { return juce::MidiMessage::getMidiNoteName(static_cast<int>(x), true, true, 4); };
            s.onValueChange = [this] { push(); };
            addAndMakeVisible(s);
        };
        knob(low_, "Lowest note", 0, 127, f.note_low, true);
        knob(high_, "Highest note", 0, 127, f.note_high, true);
        knob(velocity_, "Minimum velocity", 1, 127, f.velocity_min, false);
        for (auto* l : {&ch_label_, &range_label_, &vel_label_, &type_label_}) {
            l->setFont(juce::FontOptions(11.0f, juce::Font::bold));
            l->setColour(juce::Label::textColourId, colours::text_dim);
            addAndMakeVisible(*l);
        }
        ch_label_.setText("CHANNELS", juce::dontSendNotification);
        range_label_.setText("NOTE RANGE", juce::dontSendNotification);
        vel_label_.setText("MINIMUM VELOCITY (softer notes are ignored)", juce::dontSendNotification);
        type_label_.setText("MESSAGES", juce::dontSendNotification);
        all_.onClick = [this] {
            for (auto& b : channels_) b->setToggleState(true, juce::dontSendNotification);
            for (auto& b : types_) b->setToggleState(true, juce::dontSendNotification);
            low_.setValue(0, juce::dontSendNotification);
            high_.setValue(127, juce::dontSendNotification);
            velocity_.setValue(1, juce::dontSendNotification);
            push();
        };
        addAndMakeVisible(title_);
        addAndMakeVisible(all_);
    }
    ~FilterPanel() override
    {
        if (filter_slot() == this) filter_slot() = nullptr;
    }

    void push()
    {
        PlaybackEngine::InputFilter f;
        f.channels = 0;
        for (int ch = 0; ch < 16; ++ch)
            if (channels_[static_cast<std::size_t>(ch)]->getToggleState()) f.channels = static_cast<std::uint16_t>(f.channels | (1u << ch));
        f.note_low = static_cast<std::uint8_t>(std::min(low_.getValue(), high_.getValue()));
        f.note_high = static_cast<std::uint8_t>(std::max(low_.getValue(), high_.getValue()));
        f.velocity_min = static_cast<std::uint8_t>(velocity_.getValue());
        f.notes = types_[0]->getToggleState();
        f.controllers = types_[1]->getToggleState();
        f.pitch_bend = types_[2]->getToggleState();
        f.aftertouch = types_[3]->getToggleState();
        f.program = types_[4]->getToggleState();
        context_.engine.set_input_filter(f);
        save_filter(f);
    }

    void paint(juce::Graphics& g) override { g.fillAll(colours::panel); }
    void resized() override
    {
        auto b = getLocalBounds().reduced(12);
        auto top = b.removeFromTop(26);
        all_.setBounds(top.removeFromRight(110));
        title_.setBounds(top);
        b.removeFromTop(8);
        ch_label_.setBounds(b.removeFromTop(16));
        for (int row = 0; row < 2; ++row) {
            auto r = b.removeFromTop(24);
            for (int i = 0; i < 8; ++i) channels_[static_cast<std::size_t>(row * 8 + i)]->setBounds(r.removeFromLeft(r.getWidth() / (8 - i)));
        }
        b.removeFromTop(8);
        range_label_.setBounds(b.removeFromTop(16));
        auto r = b.removeFromTop(26);
        low_.setBounds(r.removeFromLeft(150));
        r.removeFromLeft(10);
        high_.setBounds(r.removeFromLeft(150));
        b.removeFromTop(8);
        vel_label_.setBounds(b.removeFromTop(16));
        velocity_.setBounds(b.removeFromTop(26).removeFromLeft(150));
        b.removeFromTop(8);
        type_label_.setBounds(b.removeFromTop(16));
        r = b.removeFromTop(24);
        for (int i = 0; i < 5; ++i) types_[static_cast<std::size_t>(i)]->setBounds(r.removeFromLeft(r.getWidth() / (5 - i)));
    }

private:
    AppContext& context_;
    juce::Label title_, ch_label_, range_label_, vel_label_, type_label_;
    std::vector<std::unique_ptr<juce::ToggleButton>> channels_, types_;
    juce::Slider low_, high_, velocity_;
    juce::TextButton all_{"Let all through"};
};

}  // namespace

juce::String reverse_clips(AppContext& context, const std::vector<std::pair<std::string, std::string>>& clips)
{
    return process_clips(context, clips, "REV", "Reverse", 0.0,
                         [](const audio::Channels& in, double) { return audio::reversed(in); });
}

juce::String stretch_clips(AppContext& context, const std::vector<std::pair<std::string, std::string>>& clips, double ratio)
{
    ratio = std::clamp(ratio, 0.25, 4.0);
    return process_clips(context, clips, "STR", "Time stretch", ratio,
                         [ratio](const audio::Channels& in, double rate) { return audio::time_stretch(in, ratio, rate); });
}

void ask_time_stretch(AppContext& context, const std::vector<std::pair<std::string, std::string>>& clips)
{
    if (clips.empty()) {
        context.status("Select one or more clips first");
        return;
    }
    auto* w = new juce::AlertWindow("Time Stretch",
                                    "New length as a percentage of the old (pitch is kept): 200 is twice as long, 80 is "
                                    "shorter and faster. 25-400.",
                                    juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("percent", "100", "Length %");
    w->addButton("Stretch", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([&context, clips, w](int r) {
                           if (r != 1) return;
                           const double pct = w->getTextEditorContents("percent").getDoubleValue();
                           if (pct < 25.0 || pct > 400.0) {
                               context.status("Time stretch: give 25-400 %");
                               return;
                           }
                           context.status(stretch_clips(context, clips, pct / 100.0));
                       }),
                       true);
}

juce::String capture_midi(AppContext& context, const std::string& track_id)
{
    const auto* t = context.session.data().find_track(track_id);
    if (!t || t->kind == project::TrackKind::Audio || t->kind == project::TrackKind::Folder ||
        t->kind == project::TrackKind::Aux)
        return "Select an instrument or MIDI track to capture onto";
    const auto& tempo = context.session.data().tempo;
    const double rate = context.engine.sample_rate();
    std::vector<session::LiveMidi> events;
    for (const auto& e : context.engine.live_history()) {
        session::LiveMidi m;
        m.status = e.bytes[0];
        m.data1 = e.bytes[1];
        m.data2 = e.bytes[2];
        m.wall_seconds = e.wall_ms / 1000.0;
        m.tick = e.position >= 0 && rate > 0.0 ? tempo.tick_at_seconds(static_cast<double>(e.position) / rate) : -1;
        events.push_back(m);
    }
    auto region = session::capture_performance(events, tempo, context.engine.position());
    if (!region) return "Nothing played on a MIDI keyboard to capture";
    const int notes = static_cast<int>(region->notes.size());
    const auto ids = context.session.add_recorded_regions({{track_id, *region}}, "Capture MIDI", true);
    if (ids.empty()) return "Could not capture onto " + juce::String(t->name);
    context.engine.clear_live_history();
    return "Captured " + juce::String(notes) + " note(s) onto " + juce::String(t->name) + " at bar " +
           juce::String(context.session.data().meter.bar_at(region->start));
}

void show_midi_input_filter(AppContext& context)
{
    dialogs::show_tool(filter_window(), "MIDI Input Filter", 560, 330, [&context] { return new FilterPanel(context); });
}

juce::Component* midi_input_filter_panel() { return filter_slot(); }

void restore_midi_input_filter(AppContext& context)
{
    const auto v = hw::ui_settings().getValue("midi_filter");
    if (v.isNotEmpty()) context.engine.set_input_filter(PlaybackEngine::unpack(static_cast<std::uint64_t>(v.getLargeIntValue())));
}

}  // namespace vcc::desktop
