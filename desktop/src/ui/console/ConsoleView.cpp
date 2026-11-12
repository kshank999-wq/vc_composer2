#include "ui/console/ConsoleView.h"

#include "AppResources.h"

namespace vcc::desktop::console {

namespace {

constexpr int kHeaderHeight = 32;
constexpr int kLegendWidth = 20;
constexpr int kBarThickness = 10;
constexpr int kBusGap = 8;
constexpr int kMasterGap = 6;
constexpr int kTimerHz = 30;

const char* const kSectionShort[kSectionCount] = {"INPUT", "FILTER", "EQ", "DYN", "INSERT", "SENDS", "OUTPUT"};

juce::File settings_file() { return user_data_directory().getChildFile("console.settings"); }

void vertical_text(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, juce::Colour colour)
{
    g.saveState();
    const auto c = area.getCentre();
    g.addTransform(juce::AffineTransform::rotation(-juce::MathConstants<float>::halfPi, c.x, c.y));
    const juce::Rectangle<float> rotated(c.x - area.getHeight() * 0.5f, c.y - area.getWidth() * 0.5f, area.getHeight(),
                                         area.getWidth());
    print_text(g, text, rotated, 8.0f, juce::Justification::centred, colour);
    g.restoreState();
}

}  // namespace

// --- Value bubble ------------------------------------------------------------------------------

struct ConsoleView::Tip final : juce::Component {
    Tip()
    {
        setInterceptsMouseClicks(false, false);
        setAlwaysOnTop(true);
    }
    juce::String text;
    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(juce::Colours::black.withAlpha(0.5f));
        g.fillRoundedRectangle(r.translated(0.0f, 1.5f), 4.0f);
        g.setColour(paint::lcd_back.brighter(0.15f));
        g.fillRoundedRectangle(r, 4.0f);
        g.setColour(paint::lcd_text.withAlpha(0.6f));
        g.drawRoundedRectangle(r, 4.0f, 1.0f);
        g.setColour(paint::lcd_text);
        g.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
        g.drawText(text, getLocalBounds(), juce::Justification::centred, false);
    }
};

// --- ConsoleView -----------------------------------------------------------------------------

ConsoleView::ConsoleView(AppContext& context) : context_(context), tip_(std::make_unique<Tip>())
{
    setOpaque(true);
    load_settings();
    addAndMakeVisible(top_clip_);
    addAndMakeVisible(bottom_clip_);
    addAndMakeVisible(master_top_clip_);
    top_clip_.addAndMakeVisible(top_row_);
    bottom_clip_.addAndMakeVisible(bottom_row_);
    for (auto* c : {&top_clip_, &bottom_clip_, &master_top_clip_, &top_row_, &bottom_row_}) c->setInterceptsMouseClicks(false, true);

    for (auto* bar : {&hbar_, &vbar_}) {
        bar->setAutoHide(false);
        bar->addListener(this);
        bar->setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xff4a4f57));
        bar->setColour(juce::ScrollBar::trackColourId, paint::backdrop);
        addAndMakeVisible(*bar);
    }

    for (int i = 0; i < kSectionCount; ++i) {
        auto b = std::make_unique<LedButton>(kSectionShort[i], paint::led_blue,
                                             juce::String("Show the ") + section_title(static_cast<Section>(i)) + " section");
        b->onClick = [this, i] { set_section_shown(static_cast<Section>(i), !layout_.show[static_cast<std::size_t>(i)]); };
        addAndMakeVisible(*b);
        section_buttons_.push_back(std::move(b));
    }
    narrow_button_.onClick = [this] { set_narrow(!layout_.narrow); };
    add_fx_.onClick = [this] { add_bus(true); };
    add_group_.onClick = [this] { add_bus(false); };
    for (auto* b : {&narrow_button_, &add_fx_, &add_group_}) addAndMakeVisible(*b);
    addChildComponent(*tip_);
    update_header_buttons();

    listener_ = context_.session.add_listener([this](session::Change change) {
        if (change == session::Change::Document) sync();
        if (change == session::Change::Selection)
            for (auto& s : strips_) s->update_selection();
    });
    sync();
    startTimerHz(kTimerHz);
}

ConsoleView::~ConsoleView()
{
    stopTimer();
    context_.session.remove_listener(listener_);
    strips_.clear();
    master_.reset();
}

Strip* ConsoleView::strip_for(const std::string& id) const
{
    for (const auto& s : strips_)
        if (s->id() == id) return s.get();
    if (master_ && master_->id() == id) return master_.get();
    return nullptr;
}

void ConsoleView::sync()
{
    const auto& d = context_.session.data();
    std::vector<std::string> keys;
    for (const auto* t : context_.session.music_tracks()) keys.push_back("t:" + t->id);
    for (const auto& b : d.buses)
        if (b.kind == "group") keys.push_back("g:" + b.id);
    for (const auto& b : d.buses)
        if (b.kind == "fx") keys.push_back("f:" + b.id);
    for (const auto& b : d.buses)
        if (b.kind == "master") keys.push_back("m:" + b.id);
    if (keys == keys_ && master_) {
        for (auto& s : strips_) s->refresh();
        master_->refresh();
        return;
    }
    keys_ = std::move(keys);
    rebuild();
}

void ConsoleView::rebuild()
{
    const auto& d = context_.session.data();
    strips_.clear();
    master_.reset();
    std::vector<Strip::FxBus> fx;
    for (const auto& b : d.buses)
        if (b.kind == "fx") fx.push_back({b.id, b.name});
    layout_.fx_count = static_cast<int>(fx.size());

    int n = 0;
    for (const auto* t : context_.session.music_tracks()) {
        strips_.push_back(std::make_unique<Strip>(context_, Strip::Kind::Track, t->id, layout_, fx, ++n));
    }
    track_count_ = n;
    for (const char* kind : {"group", "fx"})
        for (const auto& b : d.buses)
            if (b.kind == kind)
                strips_.push_back(std::make_unique<Strip>(context_, std::string(kind) == "fx" ? Strip::Kind::Fx : Strip::Kind::Group,
                                                          b.id, layout_, fx));
    std::string master_id = "bus-master";
    for (const auto& b : d.buses)
        if (b.kind == "master") master_id = b.id;
    master_ = std::make_unique<Strip>(context_, Strip::Kind::Master, master_id, layout_, std::vector<Strip::FxBus>{});

    for (auto& s : strips_) {
        top_row_.addAndMakeVisible(s->top());
        bottom_row_.addAndMakeVisible(s->bottom());
    }
    master_top_clip_.addAndMakeVisible(master_->top());
    addAndMakeVisible(master_->bottom());
    tip_->toFront(false);
    layout_all();
    repaint();
}

void ConsoleView::layout_all()
{
    auto r = getLocalBounds();
    if (r.getWidth() < 100 || r.getHeight() < 100) return;
    header_ = r.removeFromTop(kHeaderHeight);
    legend_ = r.removeFromLeft(kLegendWidth);

    // Header buttons.
    {
        auto h = header_.reduced(8, 7);
        h.removeFromLeft(150);
        for (auto& b : section_buttons_) {
            b->setBounds(h.removeFromLeft(50));
            h.removeFromLeft(3);
        }
        add_group_.setBounds(h.removeFromRight(72));
        h.removeFromRight(4);
        add_fx_.setBounds(h.removeFromRight(72));
        h.removeFromRight(14);
        narrow_button_.setBounds(h.removeFromRight(66));
    }

    const int mw = layout_.master_width();
    auto master_area = r.removeFromRight(mw);
    r.removeFromRight(kMasterGap);
    strips_area_ = r;

    const int content_top = layout_.top_height();
    const int avail = r.getHeight() - kBarThickness;
    bottom_h_ = juce::jlimit(juce::jmax(0, juce::jmin(380, avail - 80)), juce::jmax(520, avail / 2), avail / 2);
    top_h_ = avail - bottom_h_;
    if (top_h_ > content_top) {
        top_h_ = content_top;
        bottom_h_ = avail - top_h_;
    }
    top_h_ = juce::jmax(0, top_h_);

    const int sw = layout_.strip_width();
    int x = 0;
    bus_gap_x_ = -1;
    for (std::size_t i = 0; i < strips_.size(); ++i) {
        if (static_cast<int>(i) == track_count_ && track_count_ > 0) {
            bus_gap_x_ = x;
            x += kBusGap;
        }
        auto& top = strips_[i]->top();
        auto& bottom = strips_[i]->bottom();
        const bool same_size = top.getWidth() == sw && top.getHeight() == content_top && bottom.getHeight() == bottom_h_;
        top.setBounds(x, 0, sw, content_top);
        bottom.setBounds(x, 0, sw, bottom_h_);
        if (same_size) strips_[i]->relayout();  // otherwise resized() laid them out
        x += sw;
    }
    const int row_w = juce::jmax(1, x);
    top_row_.setSize(row_w, content_top);
    bottom_row_.setSize(row_w, bottom_h_);

    const int view_w = r.getWidth() - kBarThickness;
    top_clip_.setBounds(r.getX(), r.getY(), view_w, top_h_);
    vbar_.setBounds(r.getX() + view_w, r.getY(), kBarThickness, top_h_);
    bottom_clip_.setBounds(r.getX(), r.getY() + top_h_, view_w, bottom_h_);
    hbar_.setBounds(r.getX(), r.getY() + top_h_ + bottom_h_, view_w, kBarThickness);

    master_top_clip_.setBounds(master_area.getX(), master_area.getY(), mw, top_h_);
    if (master_) {
        master_->top().setBounds(0, 0, mw, content_top);
        master_->bottom().setBounds(master_area.getX(), master_area.getY() + top_h_, mw, bottom_h_ + kBarThickness);
        master_->relayout();
    }

    hbar_.setRangeLimits(0.0, static_cast<double>(row_w), juce::dontSendNotification);
    hbar_.setCurrentRange(static_cast<double>(hx_), static_cast<double>(view_w), juce::dontSendNotification);
    vbar_.setRangeLimits(0.0, static_cast<double>(content_top), juce::dontSendNotification);
    vbar_.setCurrentRange(static_cast<double>(vy_), static_cast<double>(top_h_), juce::dontSendNotification);
    hx_ = juce::roundToInt(hbar_.getCurrentRangeStart());
    vy_ = juce::roundToInt(vbar_.getCurrentRangeStart());
    apply_scroll();
    repaint();
}

void ConsoleView::relayout_strips() { layout_all(); }

void ConsoleView::apply_scroll()
{
    top_row_.setTopLeftPosition(-hx_, -vy_);
    bottom_row_.setTopLeftPosition(-hx_, 0);
    if (master_) master_->top().setTopLeftPosition(0, -vy_);
    repaint(legend_);
}

void ConsoleView::scroll_to(int x, int y)
{
    hbar_.setCurrentRangeStart(static_cast<double>(x), juce::sendNotificationSync);
    vbar_.setCurrentRangeStart(static_cast<double>(y), juce::sendNotificationSync);
}

void ConsoleView::scrollBarMoved(juce::ScrollBar* bar, double start)
{
    if (bar == &hbar_) hx_ = juce::roundToInt(start);
    else vy_ = juce::roundToInt(start);
    apply_scroll();
}

void ConsoleView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto p = e.getEventRelativeTo(this).getPosition();
    const bool in_top = p.y < strips_area_.getY() + top_h_;
    const float dx = wheel.deltaX, dy = wheel.deltaY;
    const bool horizontal = !in_top || e.mods.isShiftDown() || std::abs(dx) > std::abs(dy);
    const float amount = (std::abs(dx) > std::abs(dy) ? dx : dy) * (wheel.isReversed ? -1.0f : 1.0f);
    auto& bar = horizontal ? hbar_ : vbar_;
    bar.setCurrentRangeStart(bar.getCurrentRangeStart() - static_cast<double>(amount) * 160.0, juce::sendNotificationSync);
}

void ConsoleView::set_narrow(bool narrow)
{
    if (narrow == layout_.narrow) return;
    layout_.narrow = narrow;
    update_header_buttons();
    save_settings();
    layout_all();
}

void ConsoleView::set_section_shown(Section s, bool shown)
{
    layout_.show[static_cast<std::size_t>(s)] = shown;
    update_header_buttons();
    save_settings();
    layout_all();
}

void ConsoleView::update_header_buttons()
{
    for (std::size_t i = 0; i < section_buttons_.size(); ++i) section_buttons_[i]->set_lit(layout_.show[i]);
    narrow_button_.set_lit(layout_.narrow);
}

void ConsoleView::add_bus(bool fx)
{
    begin_gesture();
    if (context_.commands.invokeDirectly(fx ? cmd::addAuxBus : cmd::addGroupBus, true)) return;
    context_.session.add_bus(fx ? "FX" : "Group", fx ? "fx" : "group");
}

void ConsoleView::timerCallback()
{
    ++ticks_;
    for (auto& s : strips_) s->tick(ticks_);
    if (master_) master_->tick(ticks_);
    if (ticks_ % kTimerHz == 0)
        for (auto& s : strips_) s->slow_tick();
}

void ConsoleView::resized() { layout_all(); }

void ConsoleView::paint(juce::Graphics& g)
{
    g.fillAll(paint::backdrop);

    // Header: a dark meter-bridge style rail.
    {
        const auto h = header_.toFloat();
        fill_panel(g, h, juce::Colour(0xff17181b));
        g.setColour(juce::Colours::black);
        g.fillRect(h.getX(), h.getBottom() - 1.0f, h.getWidth(), 1.0f);
        g.setColour(juce::Colours::white.withAlpha(0.06f));
        g.fillRect(h.getX(), h.getY(), h.getWidth(), 1.0f);
        print_text(g, "CHANNEL CONSOLE", {12.0f, h.getY() + 4.0f, 140.0f, 14.0f}, 11.0f, juce::Justification::centredLeft,
                   paint::print);
        int buses = 0;
        for (const auto& b : context_.session.data().buses)
            if (b.kind != "master") ++buses;
        print_text(g, juce::String(track_count_) + " CH  /  " + juce::String(buses) + " BUS",
                   {12.0f, h.getY() + 17.0f, 140.0f, 10.0f}, 7.5f, juce::Justification::centredLeft, paint::print_dim);
    }

    // Legend down the left edge, following the scroll.
    {
        const auto l = legend_.toFloat();
        fill_panel_cached(g, legend_, juce::Colour(0xff131417));
        g.setColour(juce::Colours::black);
        g.fillRect(l.getRight() - 1.0f, l.getY(), 1.0f, l.getHeight());
        g.saveState();
        g.reduceClipRegion(legend_.withHeight(top_h_).withY(strips_area_.getY()));
        for (const auto& p : layout_.placed()) {
            const float y = static_cast<float>(strips_area_.getY() + p.y - vy_);
            vertical_text(g, section_title(p.section), {l.getX(), y + 2.0f, l.getWidth(), static_cast<float>(p.height) - 4.0f},
                          paint::print_dim);
            g.setColour(juce::Colours::white.withAlpha(0.08f));
            g.fillRect(l.getX() + 3.0f, y + static_cast<float>(p.height) - 1.0f, l.getWidth() - 6.0f, 1.0f);
        }
        g.restoreState();
        const float by = static_cast<float>(strips_area_.getY() + top_h_);
        g.setColour(juce::Colours::black);
        g.fillRect(l.getX(), by, l.getWidth(), 1.0f);
        vertical_text(g, "PAN", {l.getX(), by + 4.0f, l.getWidth(), 50.0f}, paint::print_dim);
        vertical_text(g, "M / S", {l.getX(), by + 58.0f, l.getWidth(), 46.0f}, paint::print_dim);
        vertical_text(g, "FADER", {l.getX(), by + 110.0f, l.getWidth(), static_cast<float>(bottom_h_) - 180.0f}, paint::print_dim);
        vertical_text(g, "NAME", {l.getX(), by + static_cast<float>(bottom_h_) - 46.0f, l.getWidth(), 44.0f}, paint::print_dim);
    }

    // Bus gap rail and empty state.
    if (track_count_ == 0) {
        auto area = strips_area_;
        area.removeFromLeft(juce::jmax(0, top_row_.getWidth() - hx_));
        if (area.getWidth() > 120) {
            g.setColour(paint::print_dim);
            g.setFont(juce::FontOptions(13.0f));
            g.drawFittedText("No tracks yet - add a track or a template to start mixing", area.reduced(16),
                             juce::Justification::centred, 3);
        }
    }
}

void ConsoleView::show_tip(juce::Component& source, const juce::String& text)
{
    tip_->text = text;
    const auto area = getLocalArea(&source, source.getLocalBounds());
    const int w = juce::jmax(64, juce::GlyphArrangement::getStringWidthInt(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)), text) + 16);
    const int h = 20;
    int y = area.getY() - h - 4;
    if (y < 0) y = area.getBottom() + 4;
    const int x = juce::jlimit(0, juce::jmax(0, getWidth() - w), area.getCentreX() - w / 2);
    tip_->setBounds(x, y, w, h);
    tip_->setVisible(true);
    tip_->toFront(false);
    tip_->repaint();
}

void ConsoleView::hide_tip() { tip_->setVisible(false); }

void ConsoleView::load_settings()
{
    const auto file = settings_file();
    if (!file.existsAsFile()) return;
    const auto xml = juce::parseXML(file);
    if (!xml) return;
    layout_.narrow = xml->getBoolAttribute("narrow", false);
    for (int i = 0; i < kSectionCount; ++i)
        layout_.show[static_cast<std::size_t>(i)] = xml->getBoolAttribute("show" + juce::String(i), true);
}

void ConsoleView::save_settings() const
{
    juce::XmlElement xml("CONSOLE");
    xml.setAttribute("narrow", layout_.narrow);
    for (int i = 0; i < kSectionCount; ++i) xml.setAttribute("show" + juce::String(i), layout_.show[static_cast<std::size_t>(i)]);
    const auto file = settings_file();
    file.getParentDirectory().createDirectory();
    xml.writeTo(file);
}

}  // namespace vcc::desktop::console
