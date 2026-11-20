#include "ui/console/SectionZoom.h"

#include <map>

#include "ui/SteadyWindow.h"
#include "ui/console/RackGear.h"
#include "ui/hw/HwTheme.h"

namespace vcc::desktop::console {

namespace {

constexpr float kScale = 3.0f;

std::string key_of(const std::string& id, Section s) { return id + "#" + std::to_string(static_cast<int>(s)); }

/// The enlarged section: a strip of its own showing only that section,
/// drawn three times the size. It follows the document and its meters run.
class ZoomContent final : public juce::Component, private juce::Timer {
public:
    ZoomContent(AppContext& c, Strip::Kind kind, const std::string& id, Section section, std::vector<Strip::FxBus> fx, int number,
                std::function<void()> close)
        : context_(c), id_(id), close_(std::move(close))
    {
        setOpaque(true);
        layout_.show.fill(false);
        layout_.show[static_cast<std::size_t>(section)] = true;
        layout_.fx_count = static_cast<int>(fx.size());
        strip_ = std::make_unique<Strip>(c, kind, id, layout_, std::move(fx), number);
        // Its title closes the window (back into the mixer).
        strip_->on_heading = [this](Section) { close_(); };
        auto& top = strip_->top();
        const int w = kind == Strip::Kind::Master ? layout_.master_width() : layout_.strip_width();
        top.setBounds(0, 0, w, layout_.top_height());
        top.setTransform(juce::AffineTransform::scale(kScale));
        addAndMakeVisible(top);
        setSize(juce::roundToInt(static_cast<float>(w) * kScale), juce::roundToInt(static_cast<float>(layout_.top_height()) * kScale));
        listener_ = c.session.add_listener([this](session::Change change) {
            if (change != session::Change::Document && change != session::Change::Selection) return;
            const auto& d = context_.session.data();
            if (!d.find_track(id_) && !d.find_bus(id_)) {
                juce::MessageManager::callAsync([f = close_] { f(); });
                return;
            }
            strip_->refresh();
            strip_->update_selection();
        });
        strip_->refresh();
        startTimerHz(30);
    }
    ~ZoomContent() override
    {
        stopTimer();
        context_.session.remove_listener(listener_);
        removeAllChildren();
        strip_.reset();
    }
    void paint(juce::Graphics& g) override { g.fillAll(hw::tok::chassis); }

private:
    void timerCallback() override
    {
        ++ticks_;
        strip_->tick(ticks_);
        strip_->follow_automation();
        if (ticks_ % 30 == 0) strip_->slow_tick();
        if (ticks_ % 15 == 0) strip_->delay_tick();
    }

    AppContext& context_;
    std::string id_;
    std::function<void()> close_;
    ConsoleLayout layout_;  // before the strip, which keeps a reference to it
    std::unique_ptr<Strip> strip_;
    int listener_ = 0;
    int ticks_ = 0;
};

class ZoomWindow final : public juce::DocumentWindow {
public:
    ZoomWindow(const juce::String& title, std::function<void()> close)
        : juce::DocumentWindow(title, hw::tok::chassis, juce::DocumentWindow::closeButton), close_(std::move(close))
    {
        setUsingNativeTitleBar(true);
    }
    void closeButtonPressed() override { juce::MessageManager::callAsync([f = close_] { f(); }); }

private:
    std::function<void()> close_;
};

std::map<std::string, std::unique_ptr<ZoomWindow>>& zooms()
{
    static std::map<std::string, std::unique_ptr<ZoomWindow>> z;
    return z;
}

}  // namespace

void toggle_section_zoom(AppContext& context, Strip::Kind kind, const std::string& id, Section section,
                         const std::vector<Strip::FxBus>& fx_buses, int number)
{
    // Inserts: the rack of gear, opened or put away.
    if (section == Section::Inserts) {
        if (auto* rack = rack_window(id); rack && rack->isVisible()) {
            rack->setVisible(false);
            return;
        }
        show_rack(context, id, 0);
        return;
    }
    const auto key = key_of(id, section);
    auto& all = zooms();
    if (all.count(key)) {
        all.erase(key);
        return;
    }
    const auto& d = context.session.data();
    juce::String name = id;
    if (const auto* t = d.find_track(id)) name = t->name;
    else if (const auto* b = d.find_bus(id)) name = b->name;
    auto close = [key] {
        auto& z = zooms();
        if (z.count(key)) z.erase(key);
    };
    auto window = std::make_unique<ZoomWindow>(juce::String(section_title(section)).toLowerCase().substring(0, 1).toUpperCase() +
                                                   juce::String(section_title(section)).toLowerCase().substring(1) + " - " + name,
                                               close);
    auto* content = new ZoomContent(context, kind, id, section, fx_buses, number, close);
    window->setContentOwned(content, true);
    // Near the pointer, kept on screen.
    const auto mouse = juce::Desktop::getInstance().getMainMouseSource().getScreenPosition().roundToInt();
    auto area = juce::Desktop::getInstance().getDisplays().getDisplayForPoint(mouse)->userArea;
    auto bounds = window->getBounds().withPosition(mouse.translated(24, -40));
    window->setBounds(bounds.constrainedWithin(area));
    window->setVisible(true);
    use_steady_renderer(*window);
    window->toFront(true);
    all[key] = std::move(window);
}

juce::DocumentWindow* section_zoom_window(const std::string& id, Section section)
{
    auto& all = zooms();
    const auto it = all.find(key_of(id, section));
    return it == all.end() ? nullptr : it->second.get();
}

void close_section_zooms() { zooms().clear(); }

}  // namespace vcc::desktop::console
