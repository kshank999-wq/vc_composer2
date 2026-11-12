#include "ui/VideoWindow.h"

#include <algorithm>
#include <cmath>

#include "ui/Theme.h"
#include "vcc/cues/Cues.h"
#include "vcc/picture/Picture.h"

namespace vcc::desktop {

FrameRate frame_rate_for(double fps, bool drop)
{
    struct Row {
        double fps;
        FrameRate rate;
        FrameRate drop_rate;
    };
    static const Row rows[] = {
        {24000.0 / 1001.0, FrameRate::Fps23_976, FrameRate::Fps23_976},
        {24.0, FrameRate::Fps24, FrameRate::Fps24},
        {25.0, FrameRate::Fps25, FrameRate::Fps25},
        {30000.0 / 1001.0, FrameRate::Fps29_97_NonDrop, FrameRate::Fps29_97_Drop},
        {30.0, FrameRate::Fps30, FrameRate::Fps30},
        {48000.0 / 1001.0, FrameRate::Fps47_952, FrameRate::Fps47_952},
        {48.0, FrameRate::Fps48, FrameRate::Fps48},
        {50.0, FrameRate::Fps50, FrameRate::Fps50},
        {60000.0 / 1001.0, FrameRate::Fps59_94_NonDrop, FrameRate::Fps59_94_Drop},
        {60.0, FrameRate::Fps60, FrameRate::Fps60},
    };
    const Row* best = &rows[1];
    for (const auto& r : rows)
        if (std::abs(r.fps - fps) < std::abs(best->fps - fps)) best = &r;
    return drop ? best->drop_rate : best->rate;
}

juce::File resolve_picture_file(const AppContext& context, const project::PictureVersion& picture)
{
    const juce::File stored(juce::String(picture.path));
    if (stored.existsAsFile()) return stored;
    // Relink: the same file name beside the project (media moved with it).
    if (!context.session.path().empty()) {
        auto beside = juce::File(juce::String(context.session.path())).getSiblingFile(stored.getFileName());
        if (beside.existsAsFile()) return beside;
    }
    return stored;
}

bool import_picture_file(AppContext& context, const juce::File& file, juce::String& error)
{
    media::VideoDecoder decoder;
    std::string why;
    if (!decoder.open(file.getFullPathName().toStdString(), &why)) {
        error = why;
        return false;
    }
    const auto& info = decoder.info();
    project::PictureVersion p;
    p.path = file.getFullPathName().toStdString();
    p.name = file.getFileNameWithoutExtension().toStdString();
    p.rate = frame_rate_for(info.fps, info.timecode.find(';') != std::string::npos);
    // No source timecode in the file: assume it starts where the project does.
    p.start_timecode = info.timecode.empty() ? context.session.data().start_timecode : info.timecode;
    std::replace(p.start_timecode.begin(), p.start_timecode.end(), ';', is_drop_frame(p.rate) ? ';' : ':');
    p.duration_frames = info.frame_count;
    p.width = info.width;
    p.height = info.height;
    context.session.import_picture(p);
    return true;
}

namespace {

/// Decodes on its own thread so the interface never waits for a frame.
class FrameSource final : private juce::Thread {
public:
    FrameSource() : juce::Thread("Picture decoder") { startThread(); }
    ~FrameSource() override { stopThread(2000); }

    /// Opens a file (on the decode thread); `max_width` scales frames for display.
    void open(const juce::File& file)
    {
        const juce::ScopedLock l(lock_);
        pending_file_ = file;
        open_requested_ = true;
        notify();
    }

    void want(double picture_seconds, int max_width)
    {
        wanted_seconds_ = picture_seconds;
        max_width_ = max_width;
        notify();
    }

    /// The latest decoded frame (may be from slightly earlier while decoding).
    juce::Image latest(std::int64_t& index)
    {
        const juce::ScopedLock l(lock_);
        index = image_index_;
        return image_;
    }

    juce::String status()
    {
        const juce::ScopedLock l(lock_);
        return status_;
    }

    media::VideoInfo info()
    {
        const juce::ScopedLock l(lock_);
        return info_;
    }

private:
    void run() override
    {
        media::VideoDecoder decoder;
        double last_seconds = -1.0;
        int last_width = -1;
        while (!threadShouldExit()) {
            juce::File to_open;
            bool reopen = false;
            {
                const juce::ScopedLock l(lock_);
                if (open_requested_) {
                    to_open = pending_file_;
                    reopen = true;
                    open_requested_ = false;
                }
            }
            if (reopen) {
                std::string why;
                const bool ok = to_open != juce::File() && decoder.open(to_open.getFullPathName().toStdString(), &why);
                const juce::ScopedLock l(lock_);
                info_ = ok ? decoder.info() : media::VideoInfo{};
                status_ = ok ? juce::String() : juce::String(why);
                image_ = {};
                image_index_ = -1;
                last_seconds = -1.0;
            }
            const double t = wanted_seconds_.load();
            const int w = max_width_.load();
            if (decoder.is_open() && (std::abs(t - last_seconds) > 1e-6 || w != last_width)) {
                last_seconds = t;
                last_width = w;
                media::Frame f;
                if (decoder.frame_at(t, f, w)) {
                    juce::Image img(juce::Image::ARGB, f.width, f.height, false);
                    {
                        juce::Image::BitmapData px(img, juce::Image::BitmapData::writeOnly);
                        for (int y = 0; y < f.height; ++y)
                            for (int x = 0; x < f.width; ++x) {
                                const std::uint8_t* p =
                                    &f.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(f.width) +
                                             static_cast<std::size_t>(x)) * 4];
                                px.setPixelColour(x, y, juce::Colour(p[0], p[1], p[2]));
                            }
                    }
                    const juce::ScopedLock l(lock_);
                    image_ = img;
                    image_index_ = f.index;
                } else {
                    const juce::ScopedLock l(lock_);
                    image_ = {};
                    image_index_ = -1;
                }
            }
            wait(20);
        }
    }

    juce::CriticalSection lock_;
    juce::File pending_file_;
    bool open_requested_ = false;
    std::atomic<double> wanted_seconds_{0.0};
    std::atomic<int> max_width_{960};
    juce::Image image_;
    std::int64_t image_index_ = -1;
    juce::String status_;
    media::VideoInfo info_;
};

juce::Colour marker_colour(const std::string& kind)
{
    if (kind == "hit") return colours::playhead;
    if (kind == "sync") return colours::accent;
    if (kind == "streamer" || kind == "punch") return colours::accent_warm;
    return colours::text_dim;
}

class VideoView final : public juce::Component, private juce::Timer {
public:
    explicit VideoView(AppContext& c) : context_(c)
    {
        setWantsKeyboardFocus(true);
        for (auto* b : {&import_, &earlier_, &later_, &hit_, &sync_, &compare_, &fullscreen_}) {
            addAndMakeVisible(*b);
            b->setWantsKeyboardFocus(false);
        }
        addAndMakeVisible(cut_);
        addAndMakeVisible(offset_);
        addAndMakeVisible(start_tc_);
        offset_.setColour(juce::Label::textColourId, colours::text_dim);
        offset_.setJustificationType(juce::Justification::centred);
        start_tc_.setEditable(true);
        start_tc_.setColour(juce::Label::outlineColourId, colours::outline);
        start_tc_.setJustificationType(juce::Justification::centred);
        start_tc_.setTooltip("Timecode of the movie's first frame (from the file when it carries one)");
        import_.setTooltip("Import a movie as a new cut; earlier cuts are kept");
        earlier_.setTooltip("Move the picture one frame earlier against the music");
        later_.setTooltip("Move the picture one frame later against the music");
        hit_.setTooltip("Mark a hit at the playhead (H)");
        sync_.setTooltip("Mark a sync point at the playhead");
        compare_.setTooltip("Compare this cut with the previous one and flag what changed");

        import_.onClick = [this] { choose_movie(); };
        earlier_.onClick = [this] { nudge(-1); };
        later_.onClick = [this] { nudge(+1); };
        hit_.onClick = [this] { add_marker("hit"); };
        sync_.onClick = [this] { add_marker("sync"); };
        compare_.onClick = [this] { compare_cuts(); };
        fullscreen_.onClick = [this] {
            if (auto* w = findParentComponentOfClass<juce::ResizableWindow>()) w->setFullScreen(!w->isFullScreen());
        };
        cut_.onChange = [this] {
            const int i = cut_.getSelectedItemIndex();
            const auto& pics = context_.session.data().pictures;
            if (i >= 0 && i < static_cast<int>(pics.size())) context_.session.set_active_picture(pics[static_cast<std::size_t>(i)].id);
        };
        start_tc_.onTextChange = [this] {
            if (const auto* p = context_.session.data().active_picture())
                if (!context_.session.set_picture_start_timecode(p->id, start_tc_.getText().toStdString()))
                    context_.status("Not a valid timecode at the picture's frame rate");
        };
        listener_ = context_.session.add_listener([this](session::Change ch) {
            if (ch == session::Change::Document) refresh();
        });
        refresh();
        startTimerHz(60);
    }

    ~VideoView() override { context_.session.remove_listener(listener_); }

    void refresh()
    {
        const auto& d = context_.session.data();
        cut_.clear(juce::dontSendNotification);
        int active = -1;
        for (std::size_t i = 0; i < d.pictures.size(); ++i) {
            cut_.addItem(d.pictures[i].name, static_cast<int>(i) + 1);
            if (d.pictures[i].id == d.active_picture_id) active = static_cast<int>(i);
        }
        cut_.setSelectedItemIndex(active, juce::dontSendNotification);
        const auto* p = d.active_picture();
        offset_.setText(p ? "Offset " + offset_text(*p) : juce::String("No picture"), juce::dontSendNotification);
        start_tc_.setText(p ? juce::String(p->start_timecode) : juce::String(), juce::dontSendNotification);
        const juce::File file = p ? resolve_picture_file(context_, *p) : juce::File();
        if (file != open_file_) {
            open_file_ = file;
            source_.open(file);
        }
        compare_.setEnabled(d.pictures.size() > 1);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colours::black);
        auto area = getLocalBounds().withTrimmedTop(kToolbar);
        g.setColour(colours::panel);
        g.fillRect(getLocalBounds().removeFromTop(kToolbar));

        const auto& d = context_.session.data();
        const auto* p = d.active_picture();
        const double now = context_.engine.position_seconds();
        if (!media::available()) {
            message(g, area, "This build has no video decoder (FFmpeg). Timecode, markers and cues still work.");
            return;
        }
        if (!p) {
            message(g, area, "Import a movie to score to picture (File > Import Picture, or the Import button).");
            return;
        }
        if (const auto why = source_.status(); why.isNotEmpty()) {
            message(g, area, "Cannot show " + juce::String(p->name) + ": " + why +
                                 "\nIf the file moved, put it beside the project or import it again.");
            return;
        }

        // The frame, fitted and centred.
        std::int64_t index = -1;
        const juce::Image frame = source_.latest(index);
        juce::Rectangle<float> picture_area = area.toFloat();
        if (frame.isValid()) {
            const float scale = std::min(area.getWidth() / static_cast<float>(frame.getWidth()),
                                         area.getHeight() / static_cast<float>(frame.getHeight()));
            picture_area = juce::Rectangle<float>(frame.getWidth() * scale, frame.getHeight() * scale)
                               .withCentre(area.toFloat().getCentre());
            g.drawImage(frame, picture_area, juce::RectanglePlacement::stretchToFit);
        } else {
            g.setColour(colours::text_dim);
            g.drawText(index < 0 ? "No picture at this point (before the first frame or after the last)" : "",
                       area, juce::Justification::centred);
        }

        // Changed regions from comparing cuts: a red frame and a note.
        for (const auto& r : p->changed_regions)
            if (now >= r.start_seconds && now < r.end_seconds) {
                g.setColour(colours::playhead);
                g.drawRect(picture_area, 4.0f);
                badge(g, picture_area.toNearestInt().removeFromBottom(40).reduced(10, 6), "Picture changed: " + r.note,
                      colours::playhead);
            }

        // Streamers sweep left to right and end on the hit; the punch flashes.
        for (const auto& m : d.markers) {
            if (auto progress = picture::streamer_progress(m, now)) {
                const float x = picture_area.getX() + picture_area.getWidth() * static_cast<float>(*progress);
                g.setColour(marker_colour(m.kind).withAlpha(0.9f));
                g.fillRect(juce::Rectangle<float>(x - 3.0f, picture_area.getY(), 6.0f, picture_area.getHeight()));
            }
            if (picture::punch_visible(m, now)) {
                const float r = picture_area.getHeight() * 0.18f;
                g.setColour(juce::Colours::white.withAlpha(0.85f));
                g.fillEllipse(juce::Rectangle<float>(r * 2, r * 2).withCentre(picture_area.getCentre()));
                g.setColour(juce::Colours::black);
                g.setFont(juce::FontOptions(18.0f, juce::Font::bold));
                g.drawText(m.label.empty() ? juce::String(m.kind).toUpperCase() : juce::String(m.label),
                           juce::Rectangle<float>(r * 2, r * 2).withCentre(picture_area.getCentre()),
                           juce::Justification::centred);
            }
        }

        // Burn-in: project timecode, picture frame, bars/beats, cue.
        const auto music = picture::locate_in_music(d, now);
        juce::String lines = juce::String(picture::project_timecode(d, now)) + "   bar " + juce::String(music.bar) + " beat " +
                             juce::String(music.beat, 2);
        lines << "\nframe " << juce::String(picture::frame_at(d, *p, now)) << " of " << juce::String(p->duration_frames)
              << "   " << to_string(p->rate) << " fps";
        for (const auto& c : d.cues)
            if (music.tick >= c.start && music.tick < c.end) lines << "\n" << juce::String(c.code) << "  " << juce::String(c.title);
        auto burn = picture_area.toNearestInt().removeFromTop(78).removeFromLeft(380).reduced(10, 8);
        g.setColour(juce::Colours::black.withAlpha(0.6f));
        g.fillRoundedRectangle(burn.toFloat(), 4.0f);
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 15.0f, juce::Font::bold));
        g.drawFittedText(lines, burn.reduced(8, 4), juce::Justification::topLeft, 3);

        // The next marker and how far away it is.
        for (const auto& m : d.markers)
            if (m.seconds >= now) {
                const double in = m.seconds - now;
                if (in < 10.0)
                    badge(g, picture_area.toNearestInt().removeFromTop(40).removeFromRight(300).reduced(10, 6),
                          juce::String(m.kind).toUpperCase() + (m.label.empty() ? "" : "  " + juce::String(m.label)) +
                              "  in " + juce::String(in, 2) + " s",
                          marker_colour(m.kind));
                break;
            }

        if (picture::rate_mismatch(d, *p))
            badge(g, picture_area.toNearestInt().removeFromBottom(40).removeFromRight(420).reduced(10, 6),
                  juce::String("Picture is ") + to_string(p->rate) + " fps but the project counts " +
                      to_string(d.frame_rate) + " - timecode will not line up",
                  colours::accent_warm);
    }

    void resized() override
    {
        auto r = getLocalBounds().removeFromTop(kToolbar).reduced(8, 6);
        import_.setBounds(r.removeFromLeft(80));
        r.removeFromLeft(6);
        cut_.setBounds(r.removeFromLeft(170));
        r.removeFromLeft(6);
        earlier_.setBounds(r.removeFromLeft(34));
        offset_.setBounds(r.removeFromLeft(110));
        later_.setBounds(r.removeFromLeft(34));
        r.removeFromLeft(6);
        start_tc_.setBounds(r.removeFromLeft(110));
        r.removeFromLeft(10);
        hit_.setBounds(r.removeFromLeft(56));
        r.removeFromLeft(4);
        sync_.setBounds(r.removeFromLeft(56));
        fullscreen_.setBounds(r.removeFromRight(86));
        r.removeFromRight(6);
        compare_.setBounds(r.removeFromRight(120));
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::spaceKey) {
            context_.engine.toggle_play();
            return true;
        }
        if (key.getTextCharacter() == 'h' || key.getTextCharacter() == 'H') {
            add_marker("hit");
            return true;
        }
        if (key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey) {
            step_frame(key == juce::KeyPress::leftKey ? -1 : 1);
            return true;
        }
        return false;
    }

    void mouseDown(const juce::MouseEvent&) override { grabKeyboardFocus(); }

private:
    static constexpr int kToolbar = 40;

    void timerCallback() override
    {
        const auto& d = context_.session.data();
        if (const auto* p = d.active_picture()) {
            const double t = picture::picture_seconds_at(d, *p, context_.engine.position_seconds());
            source_.want(t + 1e-6, std::max(320, std::min(1920, getWidth())));
        }
        repaint();
    }

    static void message(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& text)
    {
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(15.0f));
        g.drawFittedText(text, area.reduced(40), juce::Justification::centred, 4);
    }

    static void badge(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, juce::Colour c)
    {
        g.setColour(juce::Colours::black.withAlpha(0.65f));
        g.fillRoundedRectangle(r.toFloat(), 4.0f);
        g.setColour(c);
        g.drawRoundedRectangle(r.toFloat(), 4.0f, 1.5f);
        g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
        g.drawFittedText(text, r.reduced(8, 2), juce::Justification::centredLeft, 1);
    }

    juce::String offset_text(const project::PictureVersion& p) const
    {
        const double frames = p.offset_seconds * frames_per_second(p.rate);
        return (frames >= 0 ? "+" : "") + juce::String(static_cast<int>(std::lround(frames))) + " fr";
    }

    void nudge(int frames)
    {
        if (const auto* p = context_.session.data().active_picture())
            context_.session.set_picture_offset(p->id, p->offset_seconds + frames / frames_per_second(p->rate));
    }

    void step_frame(int frames)
    {
        const auto& d = context_.session.data();
        const double fps = d.active_picture() ? frames_per_second(d.active_picture()->rate) : frames_per_second(d.frame_rate);
        const double t = std::max(0.0, context_.engine.position_seconds() + frames / fps + 0.25 / fps);
        context_.engine.locate(d.tempo.tick_at_seconds(t));
    }

    void add_marker(const std::string& kind)
    {
        const double now = context_.engine.position_seconds();
        context_.session.add_marker(now, kind);
        const auto music = picture::locate_in_music(context_.session.data(), now);
        context_.status(juce::String(kind == "hit" ? "Hit" : "Sync point") + " at " +
                        picture::project_timecode(context_.session.data(), now) + " - bar " + juce::String(music.bar) +
                        ", " + juce::String(std::abs(music.offset_ms), 0) + " ms " +
                        (music.offset_ms >= 0 ? "after" : "before") + " the nearest beat");
    }

    void choose_movie()
    {
        chooser_ = std::make_unique<juce::FileChooser>("Import picture", juce::File(), "*.mov;*.mp4;*.m4v;*.mxf;*.avi;*.mkv");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this](const juce::FileChooser& fc) {
                                  if (fc.getResult() == juce::File()) return;
                                  juce::String error;
                                  if (!import_picture_file(context_, fc.getResult(), error))
                                      juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                             "Could not import picture", error);
                              });
    }

    void compare_cuts()
    {
        const auto& d = context_.session.data();
        const auto* now = d.active_picture();
        if (!now || d.pictures.size() < 2) return;
        const project::PictureVersion* previous = nullptr;
        for (const auto& p : d.pictures) {
            if (p.id == now->id) break;
            previous = &p;
        }
        if (!previous) previous = &d.pictures.front();
        if (previous->id == now->id) return;
        const juce::File a = resolve_picture_file(context_, *previous), b = resolve_picture_file(context_, *now);
        const std::string now_id = now->id;
        const double start = picture::project_seconds_for_frame(d, *now, 0);

        class Job final : public juce::ThreadWithProgressWindow {
        public:
            Job(juce::File x, juce::File y) : juce::ThreadWithProgressWindow("Comparing cuts...", true, true), a_(x), b_(y) {}
            void run() override
            {
                media::VideoDecoder da, db;
                if (!da.open(a_.getFullPathName().toStdString()) || !db.open(b_.getFullPathName().toStdString())) return;
                const double step = 1.0;
                const double length = std::max(da.info().duration_seconds, db.info().duration_seconds);
                for (double t = 0.0; t < length && !threadShouldExit(); t += step) {
                    setProgress(t / length);
                    auto fa = static_cast<std::int64_t>(t * da.info().fps), fb = static_cast<std::int64_t>(t * db.info().fps);
                    if (auto s = da.signature(fa)) old_.push_back(*s);
                    if (auto s = db.signature(fb)) new_.push_back(*s);
                }
                done_ = !threadShouldExit();
            }
            juce::File a_, b_;
            std::vector<std::uint64_t> old_, new_;
            bool done_ = false;
        };
        Job job(a, b);
        if (!job.runThread() || !job.done_) return;
        auto regions = picture::changed_regions(job.old_, job.new_, 1.0, start);
        context_.session.set_changed_regions(now_id, regions);
        context_.status(regions.empty() ? juce::String("No picture changes found between the cuts")
                                         : juce::String(regions.size()) + " changed region(s) flagged - review them and approve offsets");
    }

    AppContext& context_;
    int listener_ = 0;
    FrameSource source_;
    juce::File open_file_;
    juce::TextButton import_{"Import..."}, earlier_{"-1"}, later_{"+1"}, hit_{"Hit"}, sync_{"Sync"},
        compare_{"Compare cuts"}, fullscreen_{"Full screen"};
    juce::ComboBox cut_;
    juce::Label offset_, start_tc_;
    std::unique_ptr<juce::FileChooser> chooser_;
};

class VideoWindow final : public juce::DocumentWindow {
public:
    explicit VideoWindow(AppContext& c) : juce::DocumentWindow("Picture", juce::Colours::black, juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        setContentOwned(new VideoView(c), false);
        setResizable(true, false);
        setResizeLimits(480, 320, 8000, 5000);
        centreWithSize(1024, 640);
    }
    void closeButtonPressed() override { setVisible(false); }
};

juce::Component::SafePointer<VideoWindow> video_window;

}  // namespace

void close_video_window() { delete video_window.getComponent(); }

juce::DocumentWindow* video_window_if_open() { return video_window.getComponent(); }

void show_video_window(AppContext& context)
{
    if (video_window == nullptr) video_window = new VideoWindow(context);
    video_window->setVisible(true);
    video_window->toFront(true);
}

}  // namespace vcc::desktop
