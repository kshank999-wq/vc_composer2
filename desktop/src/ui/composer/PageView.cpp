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
    settings_ = saved_full_score_settings();
    for (auto* k : {&paper_, &concert_, &bars_, &hide_, &order_}) {
        addAndMakeVisible(*k);
        k->set_legend_size(10.0f);
        k->setWantsKeyboardFocus(false);
    }
    paper_.setTooltip("Paper size");
    concert_.setTooltip("Score in C (concert pitch); off: a transposed score");
    bars_.setTooltip("Bars on a line, at most");
    hide_.setTooltip("Hide the staves of parts that rest for a whole system");
    order_.setTooltip("Orchestral score order (woodwinds, brass, percussion, keyboards, voices, strings); off: track order");
    paper_.onClick = [this] {
        juce::PopupMenu m;
        for (const char* p : {"A3", "Tabloid", "A4", "Letter"})
            m.addItem(p, true, settings_.paper == p, [this, p] {
                auto s = settings_;
                s.paper = p;
                set_settings(s);
            });
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&paper_));
    };
    bars_.onClick = [this] {
        juce::PopupMenu m;
        m.addItem("As many as fit", true, settings_.bars_per_system <= 0, [this] {
            auto s = settings_;
            s.bars_per_system = 0;
            set_settings(s);
        });
        m.addSeparator();
        for (int n : {1, 2, 3, 4, 5, 6, 8, 10, 12})
            m.addItem(juce::String(n) + (n == 1 ? " bar" : " bars") + " per system", true, settings_.bars_per_system == n, [this, n] {
                auto s = settings_;
                s.bars_per_system = n;
                set_settings(s);
            });
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&bars_));
    };
    concert_.onClick = [this] {
        auto s = settings_;
        s.concert_pitch = concert_.getToggleState();
        set_settings(s);
    };
    hide_.onClick = [this] {
        auto s = settings_;
        s.hide_empty_staves = hide_.getToggleState();
        set_settings(s);
    };
    landscape_.setTooltip("Turn the paper sideways: wider lines, more bars on each (prints that way too)");
    landscape_.onClick = [this] {
        auto s = settings_;
        s.landscape = landscape_.getToggleState();
        set_settings(s);
    };
    addAndMakeVisible(landscape_);
    sheet_info_.setComponentID("sheet_info_key");
    sheet_info_.setTooltip("Sheet Info: title, composer and other credits, and which of them print (or click the title)");
    sheet_info_.set_legend_size(10.0f);
    sheet_info_.setWantsKeyboardFocus(false);
    sheet_info_.onClick = [this] { show_score_info(context_); };
    addAndMakeVisible(sheet_info_);
    order_.onClick = [this] {
        auto s = settings_;
        s.score_order = order_.getToggleState();
        set_settings(s);
    };
    refresh_keys();
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

void PageView::set_settings(const FullScoreSettings& s)
{
    settings_ = s;
    save_full_score_settings(s);
    refresh_keys();
    relayout();
}

void PageView::refresh_keys()
{
    paper_.setButtonText("Paper " + juce::String(settings_.paper));
    bars_.setButtonText(settings_.bars_per_system <= 0
                            ? juce::String("Fit bars / line")
                            : juce::String(settings_.bars_per_system) + (settings_.bars_per_system == 1 ? " bar" : " bars") + " / line");
    landscape_.setToggleState(settings_.landscape, juce::dontSendNotification);
    concert_.setToggleState(settings_.concert_pitch, juce::dontSendNotification);
    hide_.setToggleState(settings_.hide_empty_staves, juce::dontSendNotification);
    order_.setToggleState(settings_.score_order, juce::dontSendNotification);
}

void PageView::relayout()
{
    dirty_ = false;
    settings_ = saved_full_score_settings();  // the export dialog may have changed them
    refresh_keys();
    juce::String why;
    layout_ = lay_out_full_score(context_, settings_, why);
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
    const int cols = columns();
    const double fit = (getWidth() - kScroll - (cols + 1.0) * kGap) / (cols * page_w);
    const double printed = layout_->staff_space_mm / 25.4 * 96.0;
    return std::max(1.0, std::min(fit, printed * 1.6) * zoom_);
}

juce::Rectangle<float> PageView::page_rect(int i) const
{
    if (!layout_ || i < 0 || i >= page_count()) return {};
    // Rows of `columns()` pages (all pages of a score are one size), centred.
    const double s = px_per_space();
    const int cols = columns();
    const auto& extent = layout_->laid.pages[static_cast<std::size_t>(i)].layout.extent;
    const auto w = static_cast<float>(extent.width * s), h = static_cast<float>(extent.height * s);
    const float row_w = static_cast<float>(cols) * w + static_cast<float>((cols - 1) * kGap);
    const float x0 = std::max(static_cast<float>(kGap), (static_cast<float>(getWidth() - kScroll) - row_w) / 2.0f);
    const int row = i / cols, col = i % cols;
    return {x0 + static_cast<float>(col) * (w + static_cast<float>(kGap)),
            static_cast<float>(bar_top() + kGap - scroll_y_) + static_cast<float>(row) * (h + static_cast<float>(kGap)), w, h};
}

int PageView::content_height() const
{
    if (!layout_) return 0;
    if (page_count() == 0) return bar_top() + kGap;
    const double s = px_per_space();
    const int rows = (page_count() + columns() - 1) / columns();
    const double page_h = layout_->laid.pages.front().layout.extent.height * s;
    return static_cast<int>(std::ceil(bar_top() + kGap + rows * (page_h + kGap)));
}

int PageView::columns() const noexcept
{
    // Wide enough for two pages as tall as the view (a spread), and more than one page.
    if (page_count() < 2) return 1;
    const auto& extent = layout_->laid.pages.front().layout.extent;
    const double view_h = std::max(1, getHeight() - bar_top() - 2 * kGap);
    const double page_w_at_height = view_h * extent.width / std::max(1.0, extent.height);
    return getWidth() - kScroll >= 2.0 * page_w_at_height + 3.0 * kGap ? 2 : 1;
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
    // The settings bar, over the pages, and what it shows.
    g.setColour(hw::tok::panel_bottom.withAlpha(0.96f));
    g.fillRect(0, 0, getWidth() - kScroll, bar_top());
    g.setColour(hw::tok::edge);
    g.drawHorizontalLine(bar_top() - 1, 0.0f, static_cast<float>(getWidth() - kScroll));
    g.setColour(hw::tok::silk);
    g.setFont(juce::FontOptions(10.5f));
    g.drawText(juce::String(layout_->parts) + " parts  -  " + juce::String(page_count()) + (page_count() == 1 ? " page" : " pages") +
                   "  -  " + juce::String(juce::roundToInt(zoom_ * 100.0)) + "%",
               juce::Rectangle<int>(sheet_info_.getRight() + 12, 0, getWidth() - kScroll - sheet_info_.getRight() - 20, bar_top()),
               juce::Justification::centredRight);
}

void PageView::resized()
{
    vbar_.setBounds(getWidth() - kScroll, 0, kScroll, getHeight());
    auto bar = juce::Rectangle<int>(8, 6, getWidth() - kScroll - 16, bar_top() - 12);
    paper_.setBounds(bar.removeFromLeft(96));
    bar.removeFromLeft(6);
    concert_.setBounds(bar.removeFromLeft(110));
    bar.removeFromLeft(6);
    bars_.setBounds(bar.removeFromLeft(96));
    bar.removeFromLeft(6);
    hide_.setBounds(bar.removeFromLeft(96));
    bar.removeFromLeft(6);
    order_.setBounds(bar.removeFromLeft(128));
    bar.removeFromLeft(6);
    landscape_.setBounds(bar.removeFromLeft(96));
    bar.removeFromLeft(6);
    sheet_info_.setBounds(bar.removeFromLeft(96));
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

bool PageView::title_at(juce::Point<float> p) const
{
    if (!layout_ || page_count() == 0) return false;
    const auto r = page_rect(0);
    if (!r.contains(p)) return false;
    const auto& page = layout_->laid.pages.front();
    if (page.systems.empty()) return true;
    const double y = (p.y - r.getY()) / px_per_space() + page.layout.extent.y;
    return y < page.systems.front().top_y - 1.0;
}

void PageView::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    if (title_at(e.position)) {
        show_score_info(context_);
        return;
    }
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
