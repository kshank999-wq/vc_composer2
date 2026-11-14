#include "ui/shell/PrintPanel.h"

#include "ui/Theme.h"

namespace vcc::desktop::shell {

namespace {
constexpr int kRow = 18;
}

PrintPanel::PrintPanel(AppContext& context) : hw::Panel("Print to audio"), context_(context)
{
    for (auto* k : {&selection_, &song_, &fx_, &play_audio_, &record_}) {
        addAndMakeVisible(*k);
        k->setWantsKeyboardFocus(false);
        k->set_legend_size(10.0f);
    }
    addAndMakeVisible(lcd_);
    selection_.setRadioGroupId(0x7071);
    song_.setRadioGroupId(0x7071);
    song_.setToggleState(true, juce::dontSendNotification);
    for (auto* k : {&selection_, &song_, &fx_, &play_audio_}) k->set_led_dot(true);
    play_audio_.setToggleState(true, juce::dontSendNotification);
    selection_.setTooltip("Print only the selected range (bars dragged on the ruler)");
    song_.setTooltip("Print from the start to the end of the music");
    fx_.setTooltip("Print with the channel strip and inserts; they are bypassed while the track plays the print");
    play_audio_.setTooltip("When done, each track plays its printed audio instead of its MIDI");
    record_.set_solid(hw::tok::rec);
    record_.set_legend_size(11.0f);
    record_.setComponentID("print_record");
    record_.setTooltip("Print the ticked tracks onto their own audio lanes (48 kHz / 24-bit WAV, offline)");
    record_.onClick = [this] {
        if (job_) cancel();
        else start();
    };
    listener_ = context_.session.add_listener([this](session::Change c) {
        if (c == session::Change::Document || c == session::Change::Selection) refresh();
    });
    refresh();
}

PrintPanel::~PrintPanel()
{
    stopTimer();
    context_.session.remove_listener(listener_);
}

void PrintPanel::refresh()
{
    rows_.clear();
    std::set<std::string> live;
    for (const auto* t : context_.session.music_tracks()) {
        if (t->kind != project::TrackKind::Instrument && t->kind != project::TrackKind::MidiOnly) continue;
        rows_.push_back({t->id, juce::String(t->name), colour_from_hex(t->color)});
        live.insert(t->id);
    }
    for (auto it = ticked_.begin(); it != ticked_.end();) it = live.count(*it) ? std::next(it) : ticked_.erase(it);
    // Nothing ticked: the selected track is.
    if (ticked_.empty() && live.count(context_.session.selected_track())) ticked_.insert(context_.session.selected_track());
    selection_.setEnabled(context_.session.selection().has_range());
    if (!selection_.isEnabled() && selection_.getToggleState()) song_.setToggleState(true, juce::sendNotification);
    repaint();
}

void PrintPanel::set_ticked(const std::string& track_id, bool on)
{
    if (on) ticked_.insert(track_id);
    else ticked_.erase(track_id);
    repaint();
}

void PrintPanel::start()
{
    std::vector<std::string> tracks;
    for (const auto& r : rows_)
        if (ticked_.count(r.id)) tracks.push_back(r.id);
    if (tracks.empty()) {
        if (context_.status) context_.status("Tick the tracks to print first");
        return;
    }
    print_tracks(tracks, fx_.getToggleState());
}

void PrintPanel::print_tracks(const std::vector<std::string>& tracks, bool include_fx)
{
    if (job_ || tracks.empty()) return;
    context_.engine.stop();
    PrintRequest rq;
    rq.tracks = tracks;
    rq.include_fx = include_fx;
    rq.play_audio = play_audio_.getToggleState();
    const auto& sel = context_.session.selection();
    if (selection_.getToggleState() && sel.has_range()) {
        rq.start = sel.from;
        rq.end = sel.to;
        rq.whole_range = true;
    } else {
        rq.start = 0;
        rq.end = std::max<Ticks>(context_.session.content_end(), kTicksPerWhole);
    }
    job_ = std::make_unique<PrintJob>(context_, std::move(rq));
    record_.setButtonText("Cancel");
    lcd_.set_text(job_->status() + "\n" + juce::String(0) + "%");
    startTimerHz(30);
}

void PrintPanel::cancel()
{
    if (!job_) return;
    job_->cancel();
    job_->step(0.0);
    finished();
}

void PrintPanel::finish_now()
{
    if (!job_) return;
    while (job_->step(1000.0)) {
    }
    finished();
}

void PrintPanel::timerCallback()
{
    if (!job_) {
        stopTimer();
        return;
    }
    // A slice of rendering per tick keeps the window responsive.
    if (!job_->step(28.0)) {
        finished();
        return;
    }
    lcd_.set_text(job_->status() + "\n" + juce::String(juce::roundToInt(job_->progress() * 100.0)) + "%");
    repaint();
}

void PrintPanel::finished()
{
    stopTimer();
    if (!job_) return;
    last_ = job_->items();
    int ok = 0;
    for (const auto& i : last_) ok += i.ok ? 1 : 0;
    lcd_.set_text(job_->cancelled() ? juce::String("CANCELLED") : "DONE  " + juce::String(ok) + " / " + juce::String(last_.size()) +
                                                                    "\n48 kHz  24-BIT  WAV");
    if (context_.status) context_.status(job_->summary());
    job_.reset();
    record_.setButtonText("Record score to audio");
    repaint();
}

juce::Rectangle<int> PrintPanel::list_area() const
{
    auto c = content();
    c.removeFromTop(4);
    return c.withHeight(std::max(kRow * 2, c.getHeight() - 146));
}

void PrintPanel::resized()
{
    auto c = content();
    const auto list = list_area();
    c.setTop(list.getBottom() + 8);
    auto row = c.removeFromTop(24);
    const int half = (row.getWidth() - 6) / 2;
    selection_.setBounds(row.removeFromLeft(half));
    song_.setBounds(row.removeFromRight(half));
    c.removeFromTop(6);
    row = c.removeFromTop(24);
    fx_.setBounds(row.removeFromLeft(half));
    play_audio_.setBounds(row.removeFromRight(half));
    c.removeFromTop(8);
    record_.setBounds(c.removeFromTop(30));
    c.removeFromTop(6);
    lcd_.setBounds(c.removeFromTop(34));
}

void PrintPanel::mouseUp(const juce::MouseEvent& e)
{
    const auto list = list_area();
    if (!list.contains(e.getPosition()) || job_) return;
    const int i = (e.y - list.getY()) / kRow + scroll_;
    if (i < 0 || i >= static_cast<int>(rows_.size())) return;
    const auto& id = rows_[static_cast<std::size_t>(i)].id;
    set_ticked(id, !ticked_.count(id));
}

void PrintPanel::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    const int visible = std::max(1, list_area().getHeight() / kRow);
    const int most = std::max(0, static_cast<int>(rows_.size()) - visible);
    scroll_ = juce::jlimit(0, most, scroll_ + (w.deltaY < 0 ? 1 : w.deltaY > 0 ? -1 : 0));
    repaint();
}

void PrintPanel::paintOverChildren(juce::Graphics& g)
{
    const auto list = list_area();
    hw::paint::well(g, list.toFloat().expanded(2.0f), 3.0f);
    juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(list);
    for (int i = scroll_; i < static_cast<int>(rows_.size()); ++i) {
        const auto r = juce::Rectangle<int>(list.getX(), list.getY() + (i - scroll_) * kRow, list.getWidth(), kRow);
        if (r.getY() >= list.getBottom()) break;
        const auto& row = rows_[static_cast<std::size_t>(i)];
        const bool on = ticked_.count(row.id) > 0;
        const bool now = job_ && job_->current_index() < job_->track_count() &&
                         job_->items()[static_cast<std::size_t>(job_->current_index())].track_id == row.id;
        hw::paint::led(g, {static_cast<float>(r.getX()) + 9.0f, static_cast<float>(r.getCentreY())}, on || now,
                       now ? hw::tok::rec : hw::Theme::get().led());
        g.setColour(row.colour);
        g.fillRect(juce::Rectangle<int>(r.getX() + 18, r.getY() + 4, 3, r.getHeight() - 8));
        g.setColour(on ? hw::tok::text : hw::tok::silk);
        g.setFont(hw::fonts::ui(11.5f));
        g.drawText(row.name, r.withTrimmedLeft(26).withTrimmedRight(4), juce::Justification::centredLeft, true);
    }
    // More tracks than fit: say so (the wheel scrolls the list).
    if (static_cast<int>(rows_.size()) * kRow > list.getHeight()) {
        const int shown = list.getHeight() / kRow;
        hw::paint::silk(g, juce::String(scroll_ + 1) + "-" + juce::String(std::min<int>(static_cast<int>(rows_.size()), scroll_ + shown)) +
                               " of " + juce::String(rows_.size()),
                        list.toFloat().removeFromBottom(12.0f).withTrimmedRight(4.0f), 8.5f, juce::Justification::centredRight);
    }
    if (job_) {
        // The progress bar under the LCD's text.
        const auto bar = lcd_.getBounds().toFloat().removeFromBottom(4.0f).reduced(4.0f, 0.0f);
        g.setColour(hw::tok::rec.withAlpha(0.85f));
        g.fillRect(bar.withWidth(bar.getWidth() * static_cast<float>(job_->progress())));
    }
}

}  // namespace vcc::desktop::shell
