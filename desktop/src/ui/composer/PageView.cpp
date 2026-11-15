#include "ui/composer/PageView.h"

#include <algorithm>
#include <cmath>

#include "AppResources.h"
#include "ui/hw/HwTheme.h"
#include "vcc/ui/NotationRenderer.h"

namespace vcc::desktop {

namespace {
constexpr int kGap = 24;     ///< Around and between pages.
constexpr int kScroll = 10;  ///< The scroll bar.
}  // namespace

PageView::PageView(AppContext& context) : context_(context)
{
    setComponentID("page_view");
    setWantsKeyboardFocus(true);
    addAndMakeVisible(vbar_);
    vbar_.addListener(this);
    vbar_.setAutoHide(false);
    listener_ = context_.session.add_listener([this](session::Change c) {
        if (c == session::Change::Document || c == session::Change::File) {
            dirty_ = true;
            dirty_age_ = 0;
        }
    });
    startTimerHz(10);
}

PageView::~PageView()
{
    stopTimer();
    context_.session.remove_listener(listener_);
}

void PageView::relayout()
{
    dirty_ = false;
    juce::String why;
    layout_ = lay_out_full_score(context_, FullScoreSettings{}, why);
    message_ = layout_ ? juce::String() : why;
    update_scroll();
    repaint();
}

void PageView::timerCallback()
{
    // Re-laid a moment after the last edit, and only while it is shown.
    if (!dirty_ || !isShowing()) return;
    if (++dirty_age_ >= 3) relayout();
}

void PageView::visibilityChanged()
{
    if (isVisible() && (dirty_ || !layout_)) relayout();
}

void PageView::set_playhead(Ticks tick)
{
    const int bar = context_.session.data().meter.bar_at(tick);
    if (bar == playhead_bar_) return;
    playhead_bar_ = bar;
    repaint();
}

int PageView::page_count() const noexcept { return layout_ ? static_cast<int>(layout_->laid.pages.size()) : 0; }

double PageView::px_per_space() const
{
    if (!layout_ || layout_->laid.pages.empty()) return 6.0;
    // At 100 % a page fills the view's width (less the gaps), up to its
    // printed size at 96 dpi.
    const auto& extent = layout_->laid.pages.front().layout.extent;
    const double page_w = std::max(1.0, extent.width);
    const double fit = (getWidth() - kScroll - 2.0 * kGap) / page_w;
    const double printed = layout_->staff_space_mm / 25.4 * 96.0;
    return std::max(1.0, std::min(fit, printed * 1.6) * zoom_);
}

juce::Rectangle<float> PageView::page_rect(int i) const
{
    if (!layout_ || i < 0 || i >= page_count()) return {};
    const double s = px_per_space();
    float y = static_cast<float>(kGap - scroll_y_);
    for (int k = 0; k < page_count(); ++k) {
        const auto& extent = layout_->laid.pages[static_cast<std::size_t>(k)].layout.extent;
        const auto w = static_cast<float>(extent.width * s), h = static_cast<float>(extent.height * s);
        const float x = std::max(static_cast<float>(kGap), (static_cast<float>(getWidth() - kScroll) - w) / 2.0f);
        if (k == i) return {x, y, w, h};
        y += h + static_cast<float>(kGap);
    }
    return {};
}

int PageView::content_height() const
{
    if (!layout_) return 0;
    const double s = px_per_space();
    double h = kGap;
    for (const auto& p : layout_->laid.pages) h += p.layout.extent.height * s + kGap;
    return static_cast<int>(std::ceil(h));
}

int PageView::bar_at(juce::Point<float> p) const
{
    if (!layout_) return 0;
    const double s = px_per_space();
    for (int i = 0; i < page_count(); ++i) {
        const auto r = page_rect(i);
        if (!r.contains(p)) continue;
        const auto& page = layout_->laid.pages[static_cast<std::size_t>(i)];
        const auto& extent = page.layout.extent;
        const double y = (p.y - r.getY()) / s + extent.y;
        int bar = 0;
        for (const auto& sys : page.systems)
            if (y >= sys.top_y - 1.0) bar = sys.first_bar;
        return bar;
    }
    return 0;
}

void PageView::paint(juce::Graphics& g)
{
    g.fillAll(hw::tok::chassis.darker(0.25f));
    if (!layout_) {
        g.setColour(hw::tok::silk);
        g.setFont(juce::FontOptions(14.0f));
        g.drawFittedText(message_.isEmpty() ? juce::String("Laying out the score...") : message_,
                         getLocalBounds().reduced(30), juce::Justification::centred, 3);
        return;
    }
    const auto& fonts = music_font();
    const double s = px_per_space();
    for (int i = 0; i < page_count(); ++i) {
        const auto r = page_rect(i);
        if (r.getBottom() < 0.0f || r.getY() > static_cast<float>(getHeight())) continue;
        // The sheet, with a shadow.
        g.setColour(juce::Colours::black.withAlpha(0.45f));
        g.fillRect(r.translated(4.0f, 5.0f));
        g.setColour(juce::Colour::fromRGB(250, 249, 246));
        g.fillRect(r);
        const auto& page = layout_->laid.pages[static_cast<std::size_t>(i)];
        // The system the playhead is in.
        for (std::size_t k = 0; k < page.systems.size(); ++k) {
            const auto& sys = page.systems[k];
            if (playhead_bar_ < sys.first_bar || playhead_bar_ > sys.last_bar) continue;
            const double top = sys.top_y;
            const double bottom = k + 1 < page.systems.size() ? page.systems[k + 1].top_y : page.layout.extent.y + page.layout.extent.height;
            const float y0 = r.getY() + static_cast<float>((top - page.layout.extent.y) * s);
            const float y1 = r.getY() + static_cast<float>((bottom - page.layout.extent.y) * s);
            g.setColour(juce::Colour::fromRGB(64, 128, 200).withAlpha(0.08f));
            g.fillRect(juce::Rectangle<float>(r.getX(), y0, r.getWidth(), y1 - y0));
            g.setColour(juce::Colour::fromRGB(64, 128, 200).withAlpha(0.7f));
            g.fillRect(juce::Rectangle<float>(r.getX() + 2.0f, y0, 3.0f, y1 - y0));
        }
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(r.toNearestInt());
        ui::NotationRenderSettings settings;
        settings.pixels_per_staff_space = s;
        settings.origin = {r.getX() - static_cast<float>(page.layout.extent.x * s),
                           r.getY() - static_cast<float>(page.layout.extent.y * s)};
        ui::draw_notation(g, page.layout, fonts.glyphs, settings, fonts.loaded ? &fonts.font : nullptr);
    }
    for (int i = 0; i < page_count(); ++i) {
        const auto r = page_rect(i);
        if (r.getBottom() < 0.0f || r.getY() > static_cast<float>(getHeight())) continue;
        g.setColour(hw::tok::silk);
        g.setFont(juce::FontOptions(10.0f));
        g.drawText("Page " + juce::String(i + 1) + " of " + juce::String(page_count()),
                   juce::Rectangle<float>(r.getX(), r.getBottom() + 4.0f, r.getWidth(), 14.0f), juce::Justification::centredRight);
    }
    // What it is.
    g.setColour(hw::tok::silk);
    g.setFont(juce::FontOptions(10.5f));
    g.drawText(juce::String(layout_->parts) + " parts  -  " + juce::String(page_count()) + (page_count() == 1 ? " page" : " pages") +
                   "  -  as printed (File > Export Full Score)  -  " + juce::String(juce::roundToInt(zoom_ * 100.0)) + "%",
               juce::Rectangle<int>(10, 4, getWidth() - kScroll - 20, 14), juce::Justification::centredLeft);
}

void PageView::resized()
{
    vbar_.setBounds(getWidth() - kScroll, 0, kScroll, getHeight());
    update_scroll();
}

void PageView::update_scroll()
{
    const int content = content_height();
    scroll_y_ = juce::jlimit(0, std::max(0, content - getHeight()), scroll_y_);
    vbar_.setRangeLimits(0.0, std::max(content, getHeight()));
    vbar_.setCurrentRange(scroll_y_, getHeight(), juce::dontSendNotification);
}

void PageView::scrollBarMoved(juce::ScrollBar*, double start)
{
    scroll_y_ = static_cast<int>(start);
    repaint();
}

void PageView::zoom(bool larger)
{
    const double before = zoom_;
    zoom_ = juce::jlimit(0.4, 4.0, zoom_ * (larger ? 1.25 : 0.8));
    // Keep the middle of the view where it was.
    const double mid = scroll_y_ + getHeight() / 2.0;
    scroll_y_ = static_cast<int>(mid * zoom_ / before - getHeight() / 2.0);
    update_scroll();
    repaint();
}

void PageView::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    const int bar = bar_at(e.position);
    if (bar <= 0) return;
    context_.engine.locate(context_.session.data().meter.bar_start(bar));
    set_playhead(context_.session.data().meter.bar_start(bar));
    context_.status("Playhead to bar " + juce::String(bar));
}

void PageView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown()) {
        zoom(w.deltaY > 0);
        return;
    }
    scroll_y_ -= juce::roundToInt(w.deltaY * 300.0f);
    update_scroll();
    repaint();
}

}  // namespace vcc::desktop
