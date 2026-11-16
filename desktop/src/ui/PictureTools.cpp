#include "ui/PictureTools.h"

#include <algorithm>
#include <cmath>

#include "audio/PlaybackEngine.h"
#include "ui/Dialogs.h"
#include "ui/Theme.h"
#include "ui/VideoWindow.h"
#include "vcc/media/VideoDecoder.h"
#include "vcc/media/VideoEncoder.h"

namespace vcc::desktop {

namespace {

juce::Component::SafePointer<dialogs::ToolWindow>& finder_window()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}
TempoFinder*& finder_slot()
{
    static TempoFinder* p = nullptr;
    return p;
}

constexpr int kMaxRows = 8;

struct GridChoice {
    const char* name;
    Ticks ticks;
};
const GridChoice kGrids[] = {{"Half notes", kTicksPerQuarter * 2},
                             {"Dotted quarters", kTicksPerQuarter * 3 / 2},
                             {"Quarter notes", kTicksPerQuarter},
                             {"Eighth notes", kTicksPerQuarter / 2},
                             {"Triplet eighths", kTicksPerQuarter / 3},
                             {"Sixteenth notes", kTicksPerQuarter / 4}};

juce::String bar_beat(const project::ProjectData& d, Ticks tick)
{
    const auto pos = picture::locate_in_music(d, d.tempo.seconds_at(tick));
    return "bar " + juce::String(pos.bar) + " beat " + juce::String(pos.beat, 2).trimCharactersAtEnd("0").trimCharactersAtEnd(".");
}

}  // namespace

// --- Tempo finder -------------------------------------------------------------------------

TempoFinder::TempoFinder(AppContext& context) : context_(context)
{
    setComponentID("tempo_finder");
    finder_slot() = this;
    title_.setText("Steady tempos that put the hits on the beat", juce::dontSendNotification);
    title_.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    min_label_.setText("Slowest (bpm)", juce::dontSendNotification);
    max_label_.setText("Fastest (bpm)", juce::dontSendNotification);
    grid_label_.setText("Hits land on", juce::dontSendNotification);
    tol_label_.setText("Within (frames)", juce::dontSendNotification);
    for (auto* l : {&title_, &start_label_, &results_label_, &min_label_, &max_label_, &grid_label_, &tol_label_}) {
        l->setColour(juce::Label::textColourId, colours::text);
        addAndMakeVisible(*l);
    }
    auto box = [this](juce::Slider& s, const juce::String& name, double lo, double hi, double step, double v) {
        s.setSliderStyle(juce::Slider::IncDecButtons);
        s.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 64, 22);
        s.setRange(lo, hi, step);
        s.setValue(v, juce::dontSendNotification);
        s.setTitle(name);
        addAndMakeVisible(s);
    };
    box(min_bpm_, "Slowest tempo", 20, 400, 1, 60);
    box(max_bpm_, "Fastest tempo", 20, 400, 1, 180);
    box(tolerance_, "Tolerance in frames", 0.25, 4.0, 0.25, 1.0);
    grid_.setTitle("Hits land on");
    for (int i = 0; i < static_cast<int>(std::size(kGrids)); ++i) grid_.addItem(kGrids[i].name, i + 1);
    grid_.setSelectedId(3, juce::dontSendNotification);
    addAndMakeVisible(grid_);
    find_.onClick = [this] { find(); };
    from_playhead_.onClick = [this] { set_start(context_.engine.position()); };
    addAndMakeVisible(find_);
    addAndMakeVisible(from_playhead_);
    for (int i = 0; i < kMaxRows; ++i) {
        auto b = std::make_unique<juce::TextButton>("Use");
        b->setTitle("Use result " + juce::String(i + 1));
        b->onClick = [this, i] { context_.status(use(i)); };
        addChildComponent(*b);
        use_.push_back(std::move(b));
        auto l = std::make_unique<juce::Label>();
        l->setColour(juce::Label::textColourId, colours::text);
        l->setFont(juce::FontOptions(13.0f));
        addChildComponent(*l);
        rows_.push_back(std::move(l));
    }
    const auto& sel = context_.session.selection();
    set_start(sel.to > sel.from ? sel.from : context_.engine.position());
}

TempoFinder::~TempoFinder()
{
    if (finder_slot() == this) finder_slot() = nullptr;
}

void TempoFinder::set_start(Ticks tick)
{
    start_ = std::max<Ticks>(0, tick);
    const auto& d = context_.session.data();
    start_label_.setText("From " + bar_beat(d, start_) + "  (" +
                             juce::String(picture::project_timecode(d, d.tempo.seconds_at(start_))) + ")",
                         juce::dontSendNotification);
    refresh();
}

void TempoFinder::refresh()
{
    for (auto& h : hits_) removeChildComponent(h.toggle.get());
    hits_.clear();
    const auto& d = context_.session.data();
    const double from = d.tempo.seconds_at(start_);
    // In the selection when there is one, else every hit after the start.
    const auto& sel = context_.session.selection();
    const double until = sel.to > sel.from && sel.from == start_ ? d.tempo.seconds_at(sel.to) : 1.0e12;
    std::vector<const project::Marker*> markers;
    for (const auto& m : d.markers)
        if ((m.kind == "hit" || m.kind == "sync") && m.seconds > from + 1e-6) markers.push_back(&m);
    std::sort(markers.begin(), markers.end(), [](auto* a, auto* b) { return a->seconds < b->seconds; });
    for (const auto* m : markers) {
        Hit h;
        h.id = m->id;
        h.seconds = m->seconds;
        h.toggle = std::make_unique<juce::ToggleButton>((m->label.empty() ? juce::String(m->kind) : juce::String(m->label)) + "  " +
                                                        juce::String(picture::project_timecode(d, m->seconds)));
        h.toggle->setToggleState(m->seconds <= until, juce::dontSendNotification);
        addAndMakeVisible(*h.toggle);
        hits_.push_back(std::move(h));
    }
    results_.clear();
    show_results();
    resized();
}

void TempoFinder::set_hit_enabled(int index, bool on)
{
    if (index >= 0 && index < hit_count()) hits_[static_cast<std::size_t>(index)].toggle->setToggleState(on, juce::dontSendNotification);
}

void TempoFinder::set_range(double lo, double hi)
{
    min_bpm_.setValue(lo, juce::dontSendNotification);
    max_bpm_.setValue(hi, juce::dontSendNotification);
}

void TempoFinder::set_grid(Ticks grid)
{
    for (int i = 0; i < static_cast<int>(std::size(kGrids)); ++i)
        if (kGrids[i].ticks == grid) grid_.setSelectedId(i + 1, juce::dontSendNotification);
}

void TempoFinder::set_tolerance(double frames) { tolerance_.setValue(frames, juce::dontSendNotification); }

picture::TempoSearch TempoFinder::search() const
{
    picture::TempoSearch s;
    s.min_bpm = min_bpm_.getValue();
    s.max_bpm = max_bpm_.getValue();
    s.tolerance_frames = tolerance_.getValue();
    s.grid = kGrids[std::clamp(grid_.getSelectedId() - 1, 0, static_cast<int>(std::size(kGrids)) - 1)].ticks;
    s.fps = frames_per_second(context_.session.data().frame_rate);
    s.max_results = kMaxRows;
    return s;
}

const std::vector<picture::TempoCandidate>& TempoFinder::find()
{
    std::vector<double> chosen;
    for (const auto& h : hits_)
        if (h.toggle->getToggleState()) chosen.push_back(h.seconds);
    results_ = chosen.empty() ? std::vector<picture::TempoCandidate>{}
                              : picture::find_tempos(context_.session.data().tempo.seconds_at(start_), chosen, search());
    // A single hit fits almost any tempo: with several chosen, only fits that
    // land at least two are worth offering.
    const int least = std::min<int>(2, static_cast<int>(chosen.size()));
    results_.erase(std::remove_if(results_.begin(), results_.end(),
                                  [least](const picture::TempoCandidate& c) { return c.hits_on < least; }),
                   results_.end());
    show_results();
    return results_;
}

void TempoFinder::show_results()
{
    int total = 0;
    for (const auto& h : hits_)
        if (h.toggle->getToggleState()) ++total;
    results_label_.setText(hits_.empty() ? juce::String("No hit points after the start: mark some with Transport > Mark Hit")
                           : results_.empty() ? juce::String(total) + " hit(s) chosen. Find Tempos lists the best fits."
                                              : juce::String("Best first (hits on the beat, then the smallest errors):"),
                           juce::dontSendNotification);
    for (int i = 0; i < kMaxRows; ++i) {
        const bool shown = i < static_cast<int>(results_.size());
        use_[static_cast<std::size_t>(i)]->setVisible(shown);
        rows_[static_cast<std::size_t>(i)]->setVisible(shown);
        if (!shown) continue;
        const auto& c = results_[static_cast<std::size_t>(i)];
        rows_[static_cast<std::size_t>(i)]->setText(juce::String(c.bpm, 2) + " bpm   " + juce::String(c.hits_on) + " of " +
                                                        juce::String(total) + " on the beat   worst " +
                                                        juce::String(c.worst_frames, 2) + " frame(s)",
                                                    juce::dontSendNotification);
    }
}

juce::String TempoFinder::use(int index)
{
    if (index < 0 || index >= static_cast<int>(results_.size())) return "Find tempos first";
    const auto c = results_[static_cast<std::size_t>(index)];
    // Through the last hit on the beat; the music after it keeps its tempo.
    Ticks last = 0;
    std::size_t k = 0;
    for (const auto& h : hits_) {
        if (!h.toggle->getToggleState()) continue;
        if (k < c.hit_ticks.size() && c.hit_ticks[k] > 0 && std::abs(c.error_frames[k]) <= search().tolerance_frames + 1e-9)
            last = std::max(last, c.hit_ticks[k]);
        ++k;
    }
    if (!context_.session.set_steady_tempo(start_, start_ + last, c.bpm)) return "That tempo could not be applied";
    return "Tempo " + juce::String(c.bpm, 2) + " bpm from " + bar_beat(context_.session.data(), start_) + ": " +
           juce::String(c.hits_on) + " hit(s) on the beat";
}

void TempoFinder::paint(juce::Graphics& g) { g.fillAll(colours::background); }

void TempoFinder::resized()
{
    auto r = getLocalBounds().reduced(14);
    title_.setBounds(r.removeFromTop(24));
    auto line = r.removeFromTop(28);
    from_playhead_.setBounds(line.removeFromRight(150).reduced(0, 2));
    start_label_.setBounds(line);
    r.removeFromTop(6);
    for (auto& h : hits_) {
        if (r.getHeight() < 200) break;
        h.toggle->setBounds(r.removeFromTop(24));
    }
    r.removeFromTop(8);
    auto row = [&](juce::Component& c, juce::Label& label) {
        auto l = r.removeFromTop(28);
        c.setBounds(l.removeFromRight(260).reduced(0, 2));
        label.setBounds(l);
    };
    row(min_bpm_, min_label_);
    row(max_bpm_, max_label_);
    row(grid_, grid_label_);
    row(tolerance_, tol_label_);
    find_.setBounds(r.removeFromTop(32).removeFromRight(160).reduced(0, 3));
    results_label_.setBounds(r.removeFromTop(26));
    for (int i = 0; i < kMaxRows; ++i) {
        auto l = r.removeFromTop(26);
        use_[static_cast<std::size_t>(i)]->setBounds(l.removeFromRight(64).reduced(0, 2));
        rows_[static_cast<std::size_t>(i)]->setBounds(l);
    }
}

void show_tempo_finder(AppContext& context)
{
    dialogs::show_tool(finder_window(), "Tempo Finder", 560, 640, [&context] { return new TempoFinder(context); });
    if (auto* f = finder_slot()) {
        const auto& sel = context.session.selection();
        f->set_start(sel.to > sel.from ? sel.from : context.engine.position());
    }
}

TempoFinder* tempo_finder() { return finder_slot(); }

// --- Picture with score ---------------------------------------------------------------------

std::pair<double, double> picture_range(const AppContext& context)
{
    const auto& d = context.session.data();
    const auto* pic = d.active_picture();
    if (!pic) return {0.0, 0.0};
    return {picture::project_seconds_for_frame(d, *pic, 0), picture::project_seconds_for_frame(d, *pic, pic->duration_frames)};
}

namespace {

/// The overlays, drawn on one frame at project time `t`.
void draw_overlays(juce::Graphics& g, int w, int h, const project::ProjectData& d, double t, const PictureExportRequest& rq)
{
    const float unit = static_cast<float>(h) / 360.0f;
    if (rq.burn_hits) {
        for (const auto& m : d.markers) {
            if (m.kind != "hit" && m.kind != "sync" && m.kind != "streamer" && m.kind != "punch") continue;
            if (auto p = picture::streamer_progress(m, t)) {
                // A bar sweeping left to right, arriving as the hit lands.
                const float x = static_cast<float>(*p) * static_cast<float>(w);
                g.setColour(juce::Colour(0xffffd23f).withAlpha(0.85f));
                g.fillRect(x - 4.0f * unit, 0.0f, 8.0f * unit, static_cast<float>(h));
            }
            if (picture::punch_visible(m, t)) {
                const float r = static_cast<float>(h) * 0.18f;
                g.setColour(juce::Colours::white.withAlpha(0.55f));
                g.fillEllipse(static_cast<float>(w) * 0.5f - r, static_cast<float>(h) * 0.5f - r, 2 * r, 2 * r);
            }
        }
    }
    if (rq.watermark.isNotEmpty()) {
        g.setColour(juce::Colours::white.withAlpha(0.28f));
        g.setFont(juce::FontOptions(30.0f * unit, juce::Font::bold));
        g.drawFittedText(rq.watermark, 0, h / 2 - static_cast<int>(40 * unit), w, static_cast<int>(80 * unit),
                         juce::Justification::centred, 1);
    }
    if (rq.burn_timecode) {
        const juce::String tc = picture::project_timecode(d, t);
        const auto pos = picture::locate_in_music(d, t);
        const juce::String bars = "Bar " + juce::String(pos.bar) + "  Beat " + juce::String(static_cast<int>(std::floor(pos.beat)));
        const float fh = 18.0f * unit;
        g.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), fh, juce::Font::bold));
        const juce::Rectangle<float> box(static_cast<float>(w) * 0.5f - 90.0f * unit, static_cast<float>(h) - 34.0f * unit,
                                         180.0f * unit, 26.0f * unit);
        g.setColour(juce::Colours::black.withAlpha(0.7f));
        g.fillRect(box);
        g.setColour(juce::Colours::white);
        g.drawText(tc, box, juce::Justification::centred, false);
        const juce::Rectangle<float> bar_box(8.0f * unit, 8.0f * unit, 150.0f * unit, 24.0f * unit);
        g.setColour(juce::Colours::black.withAlpha(0.7f));
        g.fillRect(bar_box);
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(14.0f * unit, juce::Font::bold));
        g.drawText(bars, bar_box, juce::Justification::centred, false);
    }
}

}  // namespace

PictureExportResult export_picture(AppContext& context, const PictureExportRequest& rq)
{
    PictureExportResult result;
    const auto& d = context.session.data();
    const auto* pic = d.active_picture();
    if (!pic) {
        result.message = "Import a picture first (File > Import Picture...)";
        return result;
    }
    if (!media::encoding_available()) {
        result.message = "This build cannot write movies";
        return result;
    }
    media::VideoDecoder decoder;
    std::string error;
    if (!decoder.open(resolve_picture_file(context, *pic).getFullPathName().toStdString(), &error)) {
        result.message = "The picture could not be opened: " + juce::String(error);
        return result;
    }
    const auto& info = decoder.info();
    const int fps_num = info.fps_num > 0 ? info.fps_num : 24, fps_den = info.fps_num > 0 ? info.fps_den : 1;
    int w = info.width, h = info.height;
    if (rq.max_width > 0 && w > rq.max_width) {
        h = static_cast<int>(std::lround(static_cast<double>(h) * rq.max_width / w));
        w = rq.max_width;
    }
    w &= ~1;
    h &= ~1;
    const double start = rq.start_seconds;
    const auto frames = static_cast<std::int64_t>(std::floor((rq.end_seconds - start) * fps_num / fps_den + 1e-6));
    if (frames <= 0 || w < 2 || h < 2) {
        result.message = "Nothing to export in that range";
        return result;
    }

    media::EncodeSettings settings;
    settings.width = w;
    settings.height = h;
    settings.fps_num = fps_num;
    settings.fps_den = fps_den;
    settings.sample_rate = rq.sample_rate;
    settings.channels = 2;
    settings.timecode = picture::project_timecode(d, start);
    const auto temp = rq.file.getSiblingFile(rq.file.getFileNameWithoutExtension() + ".partial" + rq.file.getFileExtension());
    temp.deleteFile();
    media::VideoEncoder encoder;
    if (!encoder.open(temp.getFullPathName().toStdString(), settings, &error)) {
        result.message = "The movie could not be started: " + juce::String(error);
        return result;
    }
    result.codec = encoder.video_codec();

    // The score through its own offline engine, as the stems print it. Time
    // before the project's start (a leader) is silence.
    PlaybackEngine engine(context.session, context.plugins);
    constexpr int kBlock = 512;
    engine.prepare_offline(rq.sample_rate, kBlock);
    engine.set_export_mode(true);
    std::int64_t silence = start < 0.0 ? static_cast<std::int64_t>(std::llround(-start * rq.sample_rate)) : 0;
    engine.locate(d.tempo.tick_at_seconds(std::max(0.0, start)));
    engine.play();
    juce::AudioBuffer<float> block(2, kBlock);
    std::vector<float> interleaved;

    juce::Image image(juce::Image::ARGB, w, h, true);
    media::Frame source, out;
    out.width = w;
    out.height = h;
    out.rgba.resize(static_cast<std::size_t>(w * h * 4));
    bool ok = true;
    std::int64_t samples_done = 0;
    for (std::int64_t n = 0; n < frames && ok; ++n) {
        if (rq.progress && !rq.progress(static_cast<double>(n) / static_cast<double>(frames))) {
            result.cancelled = true;
            ok = false;
            break;
        }
        const double t = start + static_cast<double>(n) * fps_den / fps_num;
        const std::int64_t index = picture::frame_at(d, *pic, t);
        juce::Graphics g(image);
        g.fillAll(juce::Colours::black);
        if (index >= 0 && index < pic->duration_frames && decoder.frame(index, source, w)) {
            juce::Image::BitmapData bits(image, juce::Image::BitmapData::writeOnly);
            const int sw = std::min(source.width, w), sh = std::min(source.height, h);
            const int ox = (w - sw) / 2, oy = (h - sh) / 2;
            for (int y = 0; y < sh; ++y) {
                const std::uint8_t* s = source.rgba.data() + static_cast<std::size_t>(y * source.width * 4);
                auto* p = reinterpret_cast<juce::PixelARGB*>(bits.getPixelPointer(ox, y + oy));
                for (int x = 0; x < sw; ++x, s += 4, ++p) p->setARGB(255, s[0], s[1], s[2]);
            }
        }
        draw_overlays(g, w, h, d, t, rq);
        {
            const juce::Image::BitmapData bits(image, juce::Image::BitmapData::readOnly);
            std::uint8_t* o = out.rgba.data();
            for (int y = 0; y < h; ++y) {
                const auto* p = reinterpret_cast<const juce::PixelARGB*>(bits.getLinePointer(y));
                for (int x = 0; x < w; ++x, o += 4, ++p) {
                    o[0] = p->getRed();
                    o[1] = p->getGreen();
                    o[2] = p->getBlue();
                    o[3] = 255;
                }
            }
        }
        ok = encoder.add_frame(out);

        // The frame's share of the audio, rounded so it never drifts.
        const auto until = static_cast<std::int64_t>(std::llround(static_cast<double>(n + 1) * rq.sample_rate * fps_den / fps_num));
        while (ok && samples_done < until) {
            const int count = static_cast<int>(std::min<std::int64_t>(kBlock, until - samples_done));
            const int quiet = static_cast<int>(std::min<std::int64_t>(silence, count));
            block.clear();
            if (count > quiet) engine.render_offline(block, count - quiet);
            silence -= quiet;
            interleaved.assign(static_cast<std::size_t>(count * 2), 0.0f);
            for (int i = quiet; i < count; ++i)
                for (int ch = 0; ch < 2; ++ch) {
                    const float v = block.getSample(ch, i - quiet);
                    interleaved[static_cast<std::size_t>(i * 2 + ch)] = v;
                    result.peak = std::max(result.peak, std::abs(v));
                }
            ok = encoder.add_audio(interleaved.data(), count);
            samples_done += count;
        }
    }
    engine.stop();
    result.frames = encoder.frames_written();
    const bool finished = encoder.finish(&error);
    if (!ok || !finished) {
        temp.deleteFile();
        if (!result.cancelled) result.message = "Writing the movie failed" + (error.empty() ? juce::String() : ": " + juce::String(error));
        else result.message = "Export cancelled";
        return result;
    }
    rq.file.deleteFile();
    if (!temp.moveFileTo(rq.file)) {
        temp.deleteFile();
        result.message = "The movie could not be saved there";
        return result;
    }
    result.ok = true;
    result.message = rq.file.getFileName() + ": " + juce::String(result.frames) + " frames (" + result.codec + ")";
    if (result.peak < 1.0e-4f) result.message << ", the score is silent";
    return result;
}

void show_picture_export(AppContext& context)
{
    const auto& d = context.session.data();
    if (!d.active_picture()) {
        context.status("Import a picture first (File > Import Picture...)");
        return;
    }
    auto* w = new juce::AlertWindow("Export Picture with Score",
                                    "The active cut with the stereo score mix, for director review.",
                                    juce::MessageBoxIconType::NoIcon);
    juce::StringArray ranges{"Whole picture"};
    const auto& sel = context.session.selection();
    if (sel.to > sel.from) ranges.add("Selection");
    std::vector<std::string> cue_ids;
    for (const auto& c : d.cues)
        if (c.end > c.start) {
            ranges.add("Cue " + juce::String(c.code) + (c.title.empty() ? juce::String() : " " + juce::String(c.title)));
            cue_ids.push_back(c.id);
        }
    w->addComboBox("range", ranges, "Range");
    w->addComboBox("burn", {"Timecode, bar, streamers and punches", "Timecode and bar only", "Clean picture"}, "Burn in");
    w->addTextEditor("watermark", "DRAFT - FOR REVIEW ONLY", "Watermark");
    w->addComboBox("size", {"1280 wide", "1920 wide", "960 wide", "As the picture"}, "Size");
    w->addButton("Export...", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    const bool has_selection = sel.to > sel.from;
    w->enterModalState(true, juce::ModalCallbackFunction::create([&context, w, cue_ids, has_selection](int r) {
                           if (r != 1) return;
                           auto rq = std::make_shared<PictureExportRequest>();
                           const auto& dd = context.session.data();
                           int choice = w->getComboBoxComponent("range")->getSelectedItemIndex();
                           auto [a, b] = picture_range(context);
                           if (has_selection && choice == 1) {
                               a = dd.tempo.seconds_at(context.session.selection().from);
                               b = dd.tempo.seconds_at(context.session.selection().to);
                           } else if (choice >= (has_selection ? 2 : 1)) {
                               const auto* cue = dd.find_cue(cue_ids[static_cast<std::size_t>(choice - (has_selection ? 2 : 1))]);
                               if (cue) {
                                   a = dd.tempo.seconds_at(cue->start);
                                   b = dd.tempo.seconds_at(cue->end);
                               }
                           }
                           rq->start_seconds = a;
                           rq->end_seconds = b;
                           const int burn = w->getComboBoxComponent("burn")->getSelectedItemIndex();
                           rq->burn_timecode = burn <= 1;
                           rq->burn_hits = burn == 0;
                           rq->watermark = w->getTextEditorContents("watermark").trim();
                           const int sizes[] = {1280, 1920, 960, 0};
                           rq->max_width = sizes[std::clamp(w->getComboBoxComponent("size")->getSelectedItemIndex(), 0, 3)];
                           const juce::String title = dd.production.title.empty() ? juce::String(dd.name) : juce::String(dd.production.title);
                           auto chooser = std::make_shared<juce::FileChooser>(
                               "Save the review movie",
                               juce::File::getSpecialLocation(juce::File::userMoviesDirectory)
                                   .getChildFile(juce::File::createLegalFileName(title + " review.mp4")),
                               "*.mp4;*.mov");
                           chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                                                [&context, rq, chooser](const juce::FileChooser& fc) {
                                                    auto file = fc.getResult();
                                                    if (file == juce::File()) return;
                                                    if (!file.hasFileExtension("mp4;mov")) file = file.withFileExtension("mp4");
                                                    rq->file = file;
                                                    juce::MouseCursor::showWaitCursor();
                                                    const auto res = export_picture(context, *rq);
                                                    juce::MouseCursor::hideWaitCursor();
                                                    context.status(res.message);
                                                });
                       }),
                       true);
}

}  // namespace vcc::desktop
