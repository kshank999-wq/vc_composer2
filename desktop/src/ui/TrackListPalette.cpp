#include "ui/TrackListPalette.h"

#include "ui/Dialogs.h"
#include "ui/Theme.h"
#include "vcc/templates/Templates.h"

namespace vcc::desktop {

namespace {
juce::Component::SafePointer<dialogs::ToolWindow>& list_window()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}
TrackListPanel*& panel_slot()
{
    static TrackListPanel* p = nullptr;
    return p;
}
constexpr int kCollapsed = 34;
}  // namespace

TrackListPanel::TrackListPanel(AppContext& context, std::function<void(const std::string&)> go_to)
    : context_(context), go_to_(std::move(go_to))
{
    panel_slot() = this;
    setComponentID("track_list_panel");
    title_.setText("Tracks", juce::dontSendNotification);
    title_.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    title_.setColour(juce::Label::textColourId, colours::text);
    addAndMakeVisible(title_);
    collapse_.setComponentID("track_list_collapse");
    collapse_.setTooltip("Collapse to a slim bar (or expand it again)");
    collapse_.onClick = [this] { set_collapsed(!collapsed_); };
    addAndMakeVisible(collapse_);
    search_.setTextToShowWhenEmpty("Find a track or instrument...", colours::text_dim);
    search_.setTitle("Find a track");
    search_.onTextChange = [this] { rebuild(); };
    search_.onReturnKey = [this] {
        for (const auto& [id, text] : rows_)
            if (!id.empty()) {
                pick(id);
                return;
            }
    };
    addAndMakeVisible(search_);
    list_.setModel(this);
    list_.setRowHeight(24);
    list_.setColour(juce::ListBox::backgroundColourId, colours::panel);
    addAndMakeVisible(list_);
    listener_ = context_.session.add_listener([this](session::Change c) {
        if (c == session::Change::Document || c == session::Change::Selection) rebuild();
    });
    rebuild();
    setSize(260, expanded_height_);
}

TrackListPanel::~TrackListPanel()
{
    context_.session.remove_listener(listener_);
    if (panel_slot() == this) panel_slot() = nullptr;
}

void TrackListPanel::set_filter(const juce::String& text)
{
    search_.setText(text, false);
    rebuild();
}

void TrackListPanel::rebuild()
{
    rows_.clear();
    const auto filter = search_.getText().trim().toLowerCase();
    const auto& d = context_.session.data();
    // Grouped by section (folder, else the instrument's family), in track order.
    juce::String section;
    for (const auto& t : d.tracks) {
        if (t.kind == project::TrackKind::Video || t.kind == project::TrackKind::Folder) continue;
        const juce::String name(t.name);
        juce::String family = t.family.empty() ? juce::String("other") : juce::String(templates::family_display(t.family));
        if (!t.folder_id.empty())
            if (const auto* f = d.find_track(t.folder_id)) family = f->name;
        if (filter.isNotEmpty() && !name.toLowerCase().contains(filter) && !family.toLowerCase().contains(filter) &&
            !juce::String(t.instrument_standard_id).toLowerCase().contains(filter))
            continue;
        if (family != section) {
            section = family;
            rows_.emplace_back(std::string(), family.toUpperCase());
        }
        rows_.emplace_back(t.id, name);
    }
    list_.updateContent();
    // The selected track stays highlighted.
    const auto selected = context_.session.selected_track();
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].first == selected && !selected.empty()) list_.selectRow(static_cast<int>(i), true, true);
    list_.repaint();
}

bool TrackListPanel::pick(const std::string& track_id)
{
    if (track_id.empty() || context_.session.data().find_track(track_id) == nullptr) return false;
    if (go_to_) go_to_(track_id);
    return true;
}

void TrackListPanel::set_collapsed(bool collapsed)
{
    collapsed_ = collapsed;
    collapse_.setButtonText(collapsed ? "+" : "-");
    search_.setVisible(!collapsed);
    list_.setVisible(!collapsed);
    if (auto* w = findParentComponentOfClass<juce::DocumentWindow>()) {
        if (collapsed) expanded_height_ = std::max(200, getHeight());
        const int border = w->getHeight() - getHeight();
        w->setSize(w->getWidth(), (collapsed ? kCollapsed : expanded_height_) + border);
    } else {
        setSize(getWidth(), collapsed ? kCollapsed : expanded_height_);
    }
}

void TrackListPanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    const auto& [id, text] = rows_[static_cast<std::size_t>(row)];
    if (id.empty()) {
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.drawText(text, 8, 0, width - 12, height, juce::Justification::bottomLeft, true);
        return;
    }
    if (selected) g.fillAll(colours::accent.withAlpha(0.3f));
    juce::Colour colour = colours::text_dim;
    if (const auto* t = context_.session.data().find_track(id))
        colour = juce::Colour::fromString("ff" + juce::String(t->color).trimCharactersAtStart("#"));
    g.setColour(colour);
    g.fillRoundedRectangle(14.0f, static_cast<float>(height) * 0.3f, 6.0f, static_cast<float>(height) * 0.4f, 2.0f);
    g.setColour(colours::text);
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(text, 28, 0, width - 32, height, juce::Justification::centredLeft, true);
}

void TrackListPanel::listBoxItemClicked(int row, const juce::MouseEvent&)
{
    if (row >= 0 && row < static_cast<int>(rows_.size())) pick(rows_[static_cast<std::size_t>(row)].first);
}

void TrackListPanel::resized()
{
    auto r = getLocalBounds().reduced(6);
    auto top = r.removeFromTop(24);
    collapse_.setBounds(top.removeFromRight(28));
    title_.setBounds(top);
    if (collapsed_) return;
    r.removeFromTop(6);
    search_.setBounds(r.removeFromTop(26));
    r.removeFromTop(6);
    list_.setBounds(r);
}

void TrackListPanel::paint(juce::Graphics& g) { g.fillAll(colours::background); }

void show_track_list(AppContext& context, std::function<void(const std::string&)> go_to)
{
    dialogs::show_tool(list_window(), "Track List", 260, 520,
                       [&context, go_to] { return new TrackListPanel(context, go_to); });
    if (auto* w = list_window().getComponent()) w->setResizeLimits(200, kCollapsed + 30, 600, 3000);
}

void close_track_list()
{
    if (auto* w = list_window().getComponent()) w->closeButtonPressed();
}

bool track_list_open() { return list_window() != nullptr && list_window()->isVisible(); }

TrackListPanel* track_list_panel() { return panel_slot(); }

}  // namespace vcc::desktop
