#include "ui/shell/Layouts.h"

#include "ui/hw/HwTheme.h"

namespace vcc::desktop::layouts {

namespace {
constexpr const char* kSaved = "view_layouts";
constexpr const char* kStartup = "view_layout_startup";
constexpr const char* kLast = "view_layout_last";

void store(const juce::String& key, const juce::var& v)
{
    hw::ui_settings().setValue(key, juce::JSON::toString(v, true));
    hw::ui_settings().saveIfNeeded();
}
}  // namespace

bool ViewLayout::operator==(const ViewLayout& o) const
{
    return name == o.name && producer == o.producer && page_view == o.page_view && bottom_mixer == o.bottom_mixer &&
           dock_hidden == o.dock_hidden && work_view == o.work_view && mixer_full == o.mixer_full &&
           console_window == o.console_window && console_display == o.console_display;
}

juce::var to_var(const ViewLayout& l)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("name", l.name);
    o->setProperty("view", l.producer ? "producer" : "composer");
    o->setProperty("page_view", l.page_view);
    o->setProperty("bottom_mixer", l.bottom_mixer);
    o->setProperty("dock_hidden", l.dock_hidden);
    o->setProperty("work_view", l.work_view);
    o->setProperty("mixer_full", l.mixer_full);
    o->setProperty("console_window", l.console_window);
    o->setProperty("console_display", l.console_display);
    return juce::var(o);
}

ViewLayout from_var(const juce::var& v)
{
    ViewLayout l;
    l.name = v.getProperty("name", "").toString();
    l.producer = v.getProperty("view", "composer").toString() == "producer";
    l.page_view = static_cast<bool>(v.getProperty("page_view", false));
    l.bottom_mixer = static_cast<bool>(v.getProperty("bottom_mixer", true));
    l.dock_hidden = static_cast<bool>(v.getProperty("dock_hidden", false));
    l.work_view = static_cast<bool>(v.getProperty("work_view", false));
    l.mixer_full = static_cast<bool>(v.getProperty("mixer_full", false));
    l.console_window = static_cast<bool>(v.getProperty("console_window", false));
    l.console_display = v.getProperty("console_display", "").toString();
    return l;
}

std::vector<ViewLayout> built_in()
{
    ViewLayout compose;
    compose.name = "Compose";
    ViewLayout produce;
    produce.name = "Produce";
    produce.producer = true;
    ViewLayout mix;
    mix.name = "Mix";
    mix.producer = true;
    mix.mixer_full = true;
    ViewLayout work;
    work.name = "Work";
    work.work_view = true;
    work.dock_hidden = true;
    return {compose, produce, mix, work};
}

std::vector<ViewLayout> saved()
{
    std::vector<ViewLayout> out;
    const auto v = juce::JSON::parse(hw::ui_settings().getValue(kSaved, "[]"));
    if (const auto* a = v.getArray())
        for (const auto& item : *a) {
            auto l = from_var(item);
            if (l.name.isNotEmpty()) out.push_back(l);
        }
    return out;
}

static void store_saved(const std::vector<ViewLayout>& list)
{
    juce::Array<juce::var> a;
    for (const auto& l : list) a.add(to_var(l));
    store(kSaved, juce::var(a));
}

void save(const ViewLayout& layout)
{
    auto list = saved();
    bool replaced = false;
    for (auto& l : list)
        if (l.name == layout.name) {
            l = layout;
            replaced = true;
        }
    if (!replaced) list.push_back(layout);
    store_saved(list);
}

bool remove(const juce::String& name)
{
    auto list = saved();
    const auto before = list.size();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const ViewLayout& l) { return l.name == name; }), list.end());
    if (list.size() == before) return false;
    store_saved(list);
    if (startup() == name) set_startup({});
    return true;
}

std::optional<ViewLayout> find(const juce::String& name)
{
    for (const auto& l : saved())
        if (l.name == name) return l;
    for (const auto& l : built_in())
        if (l.name == name) return l;
    return std::nullopt;
}

juce::String startup() { return hw::ui_settings().getValue(kStartup, ""); }

void set_startup(const juce::String& name)
{
    hw::ui_settings().setValue(kStartup, name);
    hw::ui_settings().saveIfNeeded();
}

std::optional<ViewLayout> last()
{
    const auto text = hw::ui_settings().getValue(kLast, "");
    if (text.isEmpty()) return std::nullopt;
    const auto v = juce::JSON::parse(text);
    if (!v.isObject()) return std::nullopt;
    return from_var(v);
}

void set_last(const ViewLayout& layout) { store(kLast, to_var(layout)); }

std::optional<ViewLayout> to_open_with()
{
    if (const auto name = startup(); name.isNotEmpty())
        if (auto l = find(name)) return l;
    return last();
}

}  // namespace vcc::desktop::layouts
