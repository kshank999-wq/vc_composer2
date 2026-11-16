// File > Print Preview... (Main Spec §7 "print preview and PDF export"):
// the full score or any part exactly as it will print - parts at written
// pitch with multi-bar rests, the full score with its layout settings - a
// page at a time or two side by side, then Export PDF from there.
#pragma once

#include <memory>

#include "ui/AppContext.h"
#include "ui/ScoreExchange.h"

namespace vcc::desktop {

class PrintPreview final : public juce::Component {
public:
    explicit PrintPreview(AppContext& context);
    ~PrintPreview() override;

    /// 0: the full score; 1..n: the parts, in track order.
    void show_source(int index);
    int source_count() const;
    juce::String source_name(int index) const;
    void show_page(int page);
    int page() const noexcept { return page_; }
    int page_count() const;
    /// Laid out again (after an edit).
    void refresh();
    const juce::String& message() const noexcept { return message_; }

    void resized() override;
    void paint(juce::Graphics& g) override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    juce::Rectangle<float> page_area(int slot) const;

    AppContext& context_;
    int source_ = 0;
    int page_ = 0;
    std::vector<std::string> part_ids_;
    std::unique_ptr<FullScoreLayout> full_;
    std::vector<notation::Score> part_score_;  ///< The part as printed (the layout reads it).
    engraving::PrintResult part_;
    juce::String message_;
    juce::ComboBox source_box_;
    juce::TextButton prev_{"<"}, next_{">"}, two_up_{"Two pages"}, export_{"Export PDF..."}, refresh_{"Refresh"};
    juce::Label page_label_;
};

void show_print_preview(AppContext& context);
PrintPreview* print_preview();

}  // namespace vcc::desktop
