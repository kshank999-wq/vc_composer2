#include "ui/PrintPreview.h"

#include <algorithm>

#include "AppResources.h"
#include "ui/Dialogs.h"
#include "ui/Theme.h"
#include "vcc/interchange/Transposition.h"
#include "vcc/ui/NotationRenderer.h"

namespace vcc::desktop {

namespace {

juce::Component::SafePointer<dialogs::ToolWindow>& preview_window()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}
PrintPreview*& preview_slot()
{
    static PrintPreview* p = nullptr;
    return p;
}

}  // namespace

PrintPreview::PrintPreview(AppContext& context) : context_(context)
{
    setComponentID("print_preview");
    preview_slot() = this;
    setWantsKeyboardFocus(true);
    source_box_.setTitle("What to preview");
    source_box_.onChange = [this] { show_source(source_box_.getSelectedId() - 1); };
    prev_.setTitle("Previous page");
    next_.setTitle("Next page");
    prev_.onClick = [this] { show_page(page_ - (two_up_.getToggleState() ? 2 : 1)); };
    next_.onClick = [this] { show_page(page_ + (two_up_.getToggleState() ? 2 : 1)); };
    two_up_.setClickingTogglesState(true);
    two_up_.onClick = [this] { repaint(); };
    refresh_.onClick = [this] { refresh(); };
    export_.onClick = [this] {
        context_.commands.invokeDirectly(source_ == 0 ? cmd::exportFullScorePdf : cmd::exportPartsPdf, true);
    };
    page_label_.setFont(juce::FontOptions(12.0f));
    page_label_.setColour(juce::Label::textColourId, colours::text);
    page_label_.setJustificationType(juce::Justification::centred);
    for (auto* c : std::initializer_list<juce::Component*>{&source_box_, &prev_, &next_, &two_up_, &export_, &refresh_, &page_label_})
        addAndMakeVisible(*c);
    refresh();
}

PrintPreview::~PrintPreview()
{
    if (preview_slot() == this) preview_slot() = nullptr;
}

int PrintPreview::source_count() const { return 1 + static_cast<int>(part_ids_.size()); }

juce::String PrintPreview::source_name(int index) const
{
    if (index <= 0) return "Full score";
    const auto* t = context_.session.data().find_track(part_ids_[static_cast<std::size_t>(index - 1)]);
    return t ? "Part: " + juce::String(t->name) : juce::String("Part");
}

void PrintPreview::refresh()
{
    part_ids_.clear();
    for (const auto* t : context_.session.staff_tracks())
        if (t->kind == project::TrackKind::Instrument && t->include_in_export && t->active_version()) part_ids_.push_back(t->id);
    source_box_.clear(juce::dontSendNotification);
    for (int i = 0; i < source_count(); ++i) source_box_.addItem(source_name(i), i + 1);
    show_source(std::min(source_, source_count() - 1));
}

void PrintPreview::show_source(int index)
{
    source_ = std::clamp(index, 0, source_count() - 1);
    source_box_.setSelectedId(source_ + 1, juce::dontSendNotification);
    message_.clear();
    full_.reset();
    part_ = {};
    part_score_.clear();
    if (source_ == 0) {
        full_ = lay_out_full_score(context_, saved_full_score_settings(), message_);
    } else if (const auto* t = context_.session.data().find_track(part_ids_[static_cast<std::size_t>(source_ - 1)])) {
        // As the parts export prints it: written pitch, multi-bar rests.
        engraving::LayoutEngine engine;
        if (music_font().loaded) engine.set_glyph_table(music_font().glyphs);
        engraving::PrintOptions po;
        const auto& d = context_.session.data();
        po.title = d.production.title.empty() ? d.name : d.production.title;
        po.subtitle = t->name;
        po.multirests = true;
        part_score_.push_back(interchange::display_score(t->active_version()->score, false));
        part_ = engraving::lay_out_pages(part_score_.back(), engine, po);
    }
    show_page(0);
}

int PrintPreview::page_count() const
{
    if (source_ == 0) return full_ ? static_cast<int>(full_->laid.pages.size()) : 0;
    return static_cast<int>(part_.pages.size());
}

void PrintPreview::show_page(int page)
{
    page_ = std::clamp(page, 0, std::max(0, page_count() - 1));
    const int n = page_count();
    page_label_.setText(n == 0 ? (message_.isEmpty() ? juce::String("Nothing to print") : message_)
                               : "Page " + juce::String(page_ + 1) + " of " + juce::String(n),
                        juce::dontSendNotification);
    prev_.setEnabled(page_ > 0);
    next_.setEnabled(page_ + 1 < n);
    repaint();
}

juce::Rectangle<float> PrintPreview::page_area(int slot) const
{
    auto area = getLocalBounds().withTrimmedTop(44).reduced(16).toFloat();
    const bool two = two_up_.getToggleState();
    if (two) {
        const float w = (area.getWidth() - 16.0f) * 0.5f;
        area = slot == 0 ? area.withWidth(w) : area.withTrimmedLeft(w + 16.0f);
    }
    return area;
}

void PrintPreview::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
    const int n = page_count();
    const auto& fonts = music_font();
    for (int slot = 0; slot < (two_up_.getToggleState() ? 2 : 1); ++slot) {
        const int index = page_ + slot;
        if (index >= n) break;
        const engraving::PrintedPage& page =
            source_ == 0 ? full_->laid.pages[static_cast<std::size_t>(index)] : part_.pages[static_cast<std::size_t>(index)];
        const auto& ext = page.layout.extent;
        const auto area = page_area(slot);
        const double scale = std::min(area.getWidth() / ext.width, area.getHeight() / ext.height);
        const juce::Rectangle<float> sheet(area.getX() + static_cast<float>((area.getWidth() - ext.width * scale) * 0.5),
                                           area.getY(), static_cast<float>(ext.width * scale),
                                           static_cast<float>(ext.height * scale));
        g.setColour(juce::Colours::black.withAlpha(0.45f));
        g.fillRect(sheet.translated(4.0f, 5.0f));
        g.setColour(juce::Colour::fromRGB(250, 249, 246));
        g.fillRect(sheet);
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(sheet.toNearestInt());
        ui::NotationRenderSettings settings;
        settings.pixels_per_staff_space = scale;
        settings.origin = {sheet.getX() - static_cast<float>(ext.x * scale), sheet.getY() - static_cast<float>(ext.y * scale)};
        ui::draw_notation(g, page.layout, fonts.glyphs, settings, fonts.loaded ? &fonts.font : nullptr);
    }
}

bool PrintPreview::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::rightKey || key == juce::KeyPress::pageDownKey) {
        show_page(page_ + 1);
        return true;
    }
    if (key == juce::KeyPress::leftKey || key == juce::KeyPress::pageUpKey) {
        show_page(page_ - 1);
        return true;
    }
    return false;
}

void PrintPreview::resized()
{
    auto top = getLocalBounds().reduced(10, 8).removeFromTop(28);
    source_box_.setBounds(top.removeFromLeft(260));
    top.removeFromLeft(10);
    prev_.setBounds(top.removeFromLeft(32));
    page_label_.setBounds(top.removeFromLeft(150));
    next_.setBounds(top.removeFromLeft(32));
    top.removeFromLeft(10);
    two_up_.setBounds(top.removeFromLeft(100));
    export_.setBounds(top.removeFromRight(120));
    top.removeFromRight(8);
    refresh_.setBounds(top.removeFromRight(80));
}

void show_print_preview(AppContext& context)
{
    dialogs::show_tool(preview_window(), "Print Preview", 980, 820, [&context] { return new PrintPreview(context); });
    if (auto* p = preview_slot()) p->refresh();
}

PrintPreview* print_preview() { return preview_slot(); }

}  // namespace vcc::desktop
