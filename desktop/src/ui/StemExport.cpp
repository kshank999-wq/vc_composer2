#include "ui/StemExport.h"

#include <algorithm>
#include <set>

#include "audio/StemRenderer.h"
#include "ui/Dialogs.h"
#include "ui/Theme.h"
#include "vcc/cues/Cues.h"
#include "vcc/project/Stems.h"

namespace vcc::desktop {

namespace {

/// A candidate source: a group bus or a deliverable track.
struct Source {
    std::string id;
    juce::String label;
    bool bus = false;
};

class StemExport final : public juce::Component, private juce::ListBoxModel {
public:
    explicit StemExport(AppContext& context) : context_(context)
    {
        title_.setText("STEMS", juce::dontSendNotification);
        sources_title_.setText("SOURCES", juce::dontSendNotification);
        output_title_.setText("PRINT", juce::dontSendNotification);
        for (auto* l : {&title_, &sources_title_, &output_title_}) {
            l->setFont(juce::FontOptions(11.0f, juce::Font::bold));
            l->setColour(juce::Label::textColourId, colours::text_dim);
            addAndMakeVisible(l);
        }
        stems_box_.setModel(this);
        stems_box_.setRowHeight(30);
        stems_box_.setColour(juce::ListBox::backgroundColourId, colours::panel);
        addAndMakeVisible(stems_box_);
        sources_model_.owner = this;
        sources_box_.setModel(&sources_model_);
        sources_box_.setRowHeight(24);
        sources_box_.setColour(juce::ListBox::backgroundColourId, colours::panel);
        addAndMakeVisible(sources_box_);

        name_.setTextToShowWhenEmpty("Stem name", colours::text_dim);
        name_.onReturnKey = name_.onFocusLost = [this] {
            if (auto* st = current(); st && name_.getText().trim().isNotEmpty() && st->name != name_.getText().trim().toStdString()) {
                st->name = name_.getText().trim().toStdString();
                commit();
            }
        };
        addAndMakeVisible(name_);
        wet_.setButtonText("Include its reverb and delay share");
        wet_.onClick = [this] {
            if (auto* st = current()) {
                st->include_effects = wet_.getToggleState();
                commit();
            }
        };
        addAndMakeVisible(wet_);

        add_.setButtonText("+ Stem");
        add_.onClick = [this] {
            project::StemDef st;
            st.name = "Stem " + std::to_string(stems_.size() + 1);
            stems_.push_back(st);
            commit();
            selected_ = static_cast<int>(stems_.size()) - 1;
            refresh();
        };
        remove_.setButtonText("Remove");
        remove_.onClick = [this] {
            if (selected_ < 0 || selected_ >= static_cast<int>(stems_.size())) return;
            stems_.erase(stems_.begin() + selected_);
            selected_ = std::min(selected_, static_cast<int>(stems_.size()) - 1);
            commit();
        };
        defaults_.setButtonText("Defaults");
        defaults_.setTooltip("Group buses, then instrument families");
        defaults_.onClick = [this] {
            context_.session.set_stems({});
            load();
        };
        for (auto* b : {&add_, &remove_, &defaults_}) addAndMakeVisible(b);

        range_.addItem("Whole project", 1);
        range_.addItem("Every cue", 2);
        range_.setSelectedId(context_.session.data().cues.empty() ? 1 : 2, juce::dontSendNotification);
        range_.onChange = [this] { update_preview(); };
        rate_.addItemList({"44100", "48000", "96000"}, 1);
        rate_.setSelectedId(2, juce::dontSendNotification);
        depth_.addItemList({"16-bit", "24-bit"}, 1);
        depth_.setSelectedId(2, juce::dontSendNotification);
        tail_.setText("3", juce::dontSendNotification);
        tail_.setInputRestrictions(4, "0123456789.");
        mix_.setButtonText("Print the full mix too");
        mix_.setToggleState(true, juce::dontSendNotification);
        preset_.setText("{production}_{code}_{stem}_v{rev}", juce::dontSendNotification);
        preset_.onTextChange = [this] { update_preview(); };
        preview_.setColour(juce::Label::textColourId, colours::accent);
        preview_.setFont(juce::FontOptions(12.0f));
        for (auto* c : std::initializer_list<juce::Component*>{&range_, &rate_, &depth_, &tail_, &mix_, &preset_, &preview_})
            addAndMakeVisible(c);
        for (auto [label, text] : {std::pair{&range_label_, "Range"}, {&rate_label_, "Sample rate"},
                                   {&depth_label_, "Bit depth"}, {&tail_label_, "Tail (s)"}, {&preset_label_, "File names"}}) {
            label->setText(text, juce::dontSendNotification);
            label->setColour(juce::Label::textColourId, colours::text_dim);
            addAndMakeVisible(label);
        }
        coverage_.setJustificationType(juce::Justification::topLeft);
        coverage_.setFont(juce::FontOptions(12.0f));
        addAndMakeVisible(coverage_);
        export_.setButtonText("Export Stems...");
        export_.setColour(juce::TextButton::buttonColourId, colours::accent);
        export_.onClick = [this] { run(); };
        addAndMakeVisible(export_);

        listener_ = context_.session.add_listener([this](session::Change c) {
            if (c == session::Change::Document && !committing_) load();
        });
        load();
        setSize(980, 600);
    }

    // No detach() here: at shutdown the window can outlive the session (see
    // dialogs::ToolWindow); closing the window detaches first.
    ~StemExport() override = default;

    void detach()
    {
        if (listener_ != 0) context_.session.remove_listener(listener_);
        listener_ = 0;
    }

    void paint(juce::Graphics& g) override { g.fillAll(colours::background); }

    void resized() override
    {
        auto r = getLocalBounds().reduced(14);
        auto left = r.removeFromLeft(280);
        title_.setBounds(left.removeFromTop(20));
        auto buttons = left.removeFromBottom(30);
        add_.setBounds(buttons.removeFromLeft(80));
        buttons.removeFromLeft(6);
        remove_.setBounds(buttons.removeFromLeft(80));
        buttons.removeFromLeft(6);
        defaults_.setBounds(buttons);
        left.removeFromBottom(8);
        stems_box_.setBounds(left);
        r.removeFromLeft(14);

        auto mid = r.removeFromLeft(330);
        sources_title_.setBounds(mid.removeFromTop(20));
        name_.setBounds(mid.removeFromTop(28));
        mid.removeFromTop(6);
        wet_.setBounds(mid.removeFromTop(24));
        mid.removeFromTop(6);
        coverage_.setBounds(mid.removeFromBottom(90));
        mid.removeFromBottom(6);
        sources_box_.setBounds(mid);
        r.removeFromLeft(14);

        output_title_.setBounds(r.removeFromTop(20));
        auto row = [&](juce::Label& l, juce::Component& c) {
            auto line = r.removeFromTop(28);
            l.setBounds(line.removeFromLeft(90));
            c.setBounds(line);
            r.removeFromTop(6);
        };
        row(range_label_, range_);
        row(rate_label_, rate_);
        row(depth_label_, depth_);
        row(tail_label_, tail_);
        mix_.setBounds(r.removeFromTop(24));
        r.removeFromTop(10);
        row(preset_label_, preset_);
        preview_.setBounds(r.removeFromTop(40));
        export_.setBounds(r.removeFromBottom(34));
    }

    // Sources list: tick a group bus or a track.
    struct SourcesModel final : juce::ListBoxModel {
        StemExport* owner = nullptr;
        int getNumRows() override { return static_cast<int>(owner->candidates_.size()); }
        void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool) override
        {
            if (row < 0 || row >= getNumRows()) return;
            const auto& c = owner->candidates_[static_cast<std::size_t>(row)];
            const auto* st = owner->current();
            const bool on = st && std::find(st->sources.begin(), st->sources.end(), c.id) != st->sources.end();
            const bool via_bus = !on && st && owner->covered_.count(c.id);
            auto box = juce::Rectangle<int>(8, (h - 14) / 2, 14, 14).toFloat();
            g.setColour(colours::outline);
            g.drawRoundedRectangle(box, 3.0f, 1.0f);
            if (on || via_bus) {
                g.setColour(on ? colours::accent : colours::text_dim);
                g.fillRoundedRectangle(box.reduced(3.0f), 2.0f);
            }
            g.setColour(c.bus ? colours::accent_warm : (via_bus ? colours::text_dim : colours::text));
            g.setFont(juce::FontOptions(13.0f, c.bus ? juce::Font::bold : juce::Font::plain));
            g.drawText(c.label + (via_bus ? "  (via bus)" : ""), 30, 0, w - 34, h, juce::Justification::centredLeft);
        }
        void listBoxItemClicked(int row, const juce::MouseEvent&) override { owner->toggle_source(row); }
    };

private:
    // Stems list.
    int getNumRows() override { return static_cast<int>(stems_.size()); }
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= getNumRows()) return;
        const auto& st = stems_[static_cast<std::size_t>(row)];
        if (selected) g.fillAll(colours::selection);
        const bool on = !skip_.count(st.id);
        auto box = juce::Rectangle<int>(8, (h - 14) / 2, 14, 14).toFloat();
        g.setColour(colours::outline);
        g.drawRoundedRectangle(box, 3.0f, 1.0f);
        if (on) {
            g.setColour(colours::accent);
            g.fillRoundedRectangle(box.reduced(3.0f), 2.0f);
        }
        const auto tracks = project::stem_tracks(st, context_.session.data());
        g.setColour(on ? colours::text : colours::text_dim);
        g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
        g.drawText(juce::String(st.name), 30, 0, w - 120, h, juce::Justification::centredLeft);
        g.setColour(tracks.empty() ? colours::missing : colours::text_dim);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText(juce::String(static_cast<int>(tracks.size())) + " trk" + (st.include_effects ? "  wet" : "  dry"),
                   w - 90, 0, 84, h, juce::Justification::centredRight);
    }
    void listBoxItemClicked(int row, const juce::MouseEvent& e) override
    {
        if (row < 0 || row >= getNumRows()) return;
        if (e.x < 28) {
            const auto& id = stems_[static_cast<std::size_t>(row)].id;
            if (!skip_.erase(id)) skip_.insert(id);
            stems_box_.repaint();
            update_preview();
        }
        selected_ = row;
        show_selected();
    }

    project::StemDef* current()
    {
        return selected_ >= 0 && selected_ < static_cast<int>(stems_.size()) ? &stems_[static_cast<std::size_t>(selected_)]
                                                                            : nullptr;
    }

    void load()
    {
        stems_ = context_.session.stems();
        candidates_.clear();
        const auto& d = context_.session.data();
        for (const auto& b : d.buses)
            if (b.kind == "group") candidates_.push_back({b.id, juce::String(b.name), true});
        for (const auto* t : context_.session.project().export_tracks())
            candidates_.push_back({t->id, juce::String(t->name), false});
        if (selected_ >= static_cast<int>(stems_.size())) selected_ = static_cast<int>(stems_.size()) - 1;
        if (selected_ < 0 && !stems_.empty()) selected_ = 0;
        refresh();
    }

    void refresh()
    {
        stems_box_.updateContent();
        stems_box_.selectRow(selected_, false, true);
        stems_box_.repaint();
        show_selected();
        // Coverage of the whole set.
        const auto cov = project::stem_coverage(stems_, context_.session.data());
        juce::String text;
        if (cov.complete()) {
            text = juce::CharPointer_UTF8("\xe2\x9c\x93 Every deliverable track is in exactly one stem: the stems add up to the mix.");
            coverage_.setColour(juce::Label::textColourId, colours::exact);
        } else {
            auto list = [](const std::vector<std::string>& v) {
                juce::StringArray a;
                for (const auto& s : v) a.add(s);
                return a.joinIntoString(", ");
            };
            if (!cov.missing.empty()) text << "In no stem: " << list(cov.missing) << "\n";
            if (!cov.doubled.empty()) text << "In two stems: " << list(cov.doubled) << "\n";
            coverage_.setColour(juce::Label::textColourId, colours::accent_warm);
        }
        if (!cov.empty.empty()) {
            juce::StringArray a;
            for (const auto& s : cov.empty) a.add(s);
            text << "Empty: " << a.joinIntoString(", ");
        }
        coverage_.setText(text, juce::dontSendNotification);
        update_preview();
    }

    void show_selected()
    {
        const auto* st = current();
        name_.setEnabled(st != nullptr);
        wet_.setEnabled(st != nullptr);
        name_.setText(st ? juce::String(st->name) : juce::String(), juce::dontSendNotification);
        wet_.setToggleState(st && st->include_effects, juce::dontSendNotification);
        covered_.clear();
        if (st)
            for (const auto* t : project::stem_tracks(*st, context_.session.data())) covered_.insert(t->id);
        sources_box_.updateContent();
        sources_box_.repaint();
    }

    void toggle_source(int row)
    {
        auto* st = current();
        if (!st || row < 0 || row >= static_cast<int>(candidates_.size())) return;
        const auto& id = candidates_[static_cast<std::size_t>(row)].id;
        auto it = std::find(st->sources.begin(), st->sources.end(), id);
        if (it != st->sources.end()) st->sources.erase(it);
        else st->sources.push_back(id);
        commit();
    }

    void commit()
    {
        committing_ = true;
        context_.session.set_stems(stems_);
        committing_ = false;
        stems_ = context_.session.data().stems;  // ids assigned
        refresh();
    }

    StemRenderRequest request() const
    {
        StemRenderRequest rq;
        for (const auto& st : stems_)
            if (!skip_.count(st.id)) rq.stem_ids.push_back(st.id);
        rq.include_mix = mix_.getToggleState();
        rq.ranges = range_.getSelectedId() == 2 ? cue_ranges(context_) : std::vector<StemRange>{whole_project_range(context_)};
        rq.sample_rate = rate_.getText().getIntValue();
        rq.bit_depth = depth_.getSelectedId() == 1 ? 16 : 24;
        rq.tail_seconds = juce::jlimit(0.0, 30.0, tail_.getText().getDoubleValue());
        rq.name_preset = preset_.getText().toStdString();
        return rq;
    }

    void update_preview()
    {
        const auto rq = request();
        const int files = static_cast<int>(rq.ranges.size() * (rq.stem_ids.size() + (rq.include_mix ? 1u : 0u)));
        juce::String example;
        if (!rq.ranges.empty() && !stems_.empty()) {
            std::string preset = rq.name_preset;
            const std::string stem = stems_.front().name;
            for (auto at = preset.find("{stem}"); at != std::string::npos; at = preset.find("{stem}", at + stem.size()))
                preset.replace(at, 6, stem);
            const auto& d = context_.session.data();
            project::Cue stand_in;
            stand_in.code = rq.ranges.front().label;
            stand_in.title = rq.ranges.front().label;
            const auto* cue = d.find_cue(rq.ranges.front().cue_id);
            example = juce::String(cues::render_name(preset, cue ? *cue : stand_in, d, "")) + ".wav";
        }
        preview_.setText(example + "\n" + juce::String(files) + " file" + (files == 1 ? "" : "s") +
                             (rq.ranges.empty() ? juce::String(" - no cues to print") : juce::String()),
                         juce::dontSendNotification);
        export_.setEnabled(files > 0);
    }

    void run()
    {
        chooser_ = std::make_unique<juce::FileChooser>(
            "Choose a folder for the stems", juce::File::getSpecialLocation(juce::File::userMusicDirectory));
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe = juce::Component::SafePointer<StemExport>(this)](const juce::FileChooser& fc) {
                                  if (safe == nullptr || !fc.getResult().isDirectory()) return;
                                  safe->print(fc.getResult(), false);
                              });
    }

    void print(const juce::File& folder, bool overwrite)
    {
        auto rq = request();
        rq.folder = folder;
        rq.overwrite_protected = overwrite;
        juce::MouseCursor::showWaitCursor();
        const auto r = render_stems(context_, rq);
        juce::MouseCursor::hideWaitCursor();

        int ok = 0, skipped = 0, failed = 0;
        juce::StringArray notes;
        for (const auto& f : r.files) {
            if (f.ok) ++ok;
            else if (f.skipped_protected) ++skipped;
            else {
                ++failed;
                notes.add(juce::String(f.stem) + ": " + f.message);
            }
            if (f.ok && (f.message.contains("silent") || f.message.contains("clips"))) notes.add(f.message);
        }
        for (const auto& s : r.ranges)
            if (s.sum_checked)
                notes.add(juce::String(s.label) + ": stems " +
                          (s.sum_error_db < -60.0 ? "add up to the mix" : "do NOT add up to the mix") + " (" +
                          (std::isfinite(s.sum_error_db) ? juce::String(s.sum_error_db, 1) + " dB" : juce::String("exact")) +
                          ")");
        for (const auto& w : r.warnings) notes.add(w);
        juce::String summary = juce::String(ok) + " file" + (ok == 1 ? "" : "s") + " printed to " + folder.getFullPathName();
        if (failed) summary << ", " << failed << " failed";
        if (skipped) summary << ", " << skipped << " not overwritten (approved or locked cues)";
        context_.status(summary);
        const auto text = summary + (notes.isEmpty() ? juce::String() : "\n\n" + notes.joinIntoString("\n"));
        if (skipped > 0) {
            juce::AlertWindow::showAsync(
                juce::MessageBoxOptions()
                    .withIconType(juce::MessageBoxIconType::QuestionIcon)
                    .withTitle("Export Stems")
                    .withMessage(text + "\n\nReplace the approved or locked cues' files too?")
                    .withButton("Replace")
                    .withButton("Keep"),
                [safe = juce::Component::SafePointer<StemExport>(this), folder](int choice) {
                    if (safe != nullptr && choice == 1) safe->print(folder, true);
                });
        } else {
            juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                             .withIconType(failed ? juce::MessageBoxIconType::WarningIcon
                                                                  : juce::MessageBoxIconType::InfoIcon)
                                             .withTitle("Export Stems")
                                             .withMessage(text)
                                             .withButton("OK"),
                                         [](int) {});
        }
    }

    AppContext& context_;
    std::vector<project::StemDef> stems_;
    std::set<std::string> skip_;  ///< stems not to print this time
    std::vector<Source> candidates_;
    std::set<std::string> covered_;
    int selected_ = 0;
    bool committing_ = false;
    int listener_ = 0;
    SourcesModel sources_model_;
    juce::Label title_, sources_title_, output_title_, coverage_, preview_;
    juce::Label range_label_, rate_label_, depth_label_, tail_label_, preset_label_;
    juce::ListBox stems_box_, sources_box_;
    juce::TextEditor name_, tail_, preset_;
    juce::ToggleButton wet_, mix_;
    juce::TextButton add_, remove_, defaults_, export_;
    juce::ComboBox range_, rate_, depth_;
    std::unique_ptr<juce::FileChooser> chooser_;
};

juce::Component::SafePointer<dialogs::ToolWindow> stem_window;

}  // namespace

void show_stem_export(AppContext& context)
{
    dialogs::show_tool(
        stem_window, "Export Stems", 980, 600, [&context] { return new StemExport(context); },
        [] {
            if (stem_window != nullptr)
                if (auto* w = dynamic_cast<StemExport*>(stem_window->getContentComponent())) w->detach();
        });
}

}  // namespace vcc::desktop
