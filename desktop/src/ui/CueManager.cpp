#include "ui/CueManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>

#include "audio/CueRenderer.h"
#include "ui/Dialogs.h"
#include "ui/Theme.h"
#include "vcc/core/Files.h"
#include "vcc/cues/Cues.h"
#include "vcc/picture/Picture.h"

namespace vcc::desktop {

namespace {

// --- Small helpers ------------------------------------------------------------------------

juce::String utf8(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
std::string str(const juce::String& s) { return s.toStdString(); }
std::string trimmed(const juce::String& s) { return s.trim().toStdString(); }

const juce::String kDash = juce::String::fromUTF8("\xe2\x80\x93");    // –
const juce::String kDot = juce::String::fromUTF8("\xc2\xb7");         // ·
const juce::String kMinus = juce::String::fromUTF8("\xe2\x88\x92");   // −
const juce::String kCross = juce::String::fromUTF8("\xc3\x97");       // ×

std::string today_iso() { return str(juce::Time::getCurrentTime().formatted("%Y-%m-%d")); }

/// "2026-02-30" is not a date; "" is (no deadline).
bool valid_iso_date(const std::string& text)
{
    if (text.empty()) return true;
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') return false;
    for (std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u})
        if (text[i] < '0' || text[i] > '9') return false;
    const int y = std::stoi(text.substr(0, 4)), m = std::stoi(text.substr(5, 2)), d = std::stoi(text.substr(8, 2));
    if (m < 1 || m > 12 || d < 1) return false;
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return d <= days[m - 1] + (m == 2 && leap ? 1 : 0);
}

juce::Colour status_colour(const std::string& status)
{
    if (status == "spotted") return juce::Colour(0xff8c96a3);
    if (status == "writing") return juce::Colour(0xff4f9de8);
    if (status == "sent") return juce::Colour(0xff9b7fe6);
    if (status == "awaiting_feedback") return juce::Colour(0xffe8b53d);
    if (status == "revisions") return juce::Colour(0xffe8803d);
    if (status == "approved") return juce::Colour(0xff4fbf73);
    if (status == "locked") return juce::Colour(0xff2f8a52);
    return colours::text_dim;
}

/// "awaiting_feedback" -> "awaiting feedback".
juce::String status_text(const std::string& status) { return utf8(status).replaceCharacter('_', ' '); }
juce::String status_title(const std::string& status)
{
    auto t = status_text(status);
    return t.substring(0, 1).toUpperCase() + t.substring(1);
}

int status_index(const std::string& status)
{
    const auto& all = project::cue_statuses();
    const auto it = std::find(all.begin(), all.end(), status);
    return it == all.end() ? static_cast<int>(all.size()) : static_cast<int>(it - all.begin());
}

const std::vector<std::pair<std::string, std::string>>& usages()
{
    static const std::vector<std::pair<std::string, std::string>> u = {
        {"BI", "Background instrumental"}, {"BV", "Background vocal"}, {"VI", "Visual instrumental"},
        {"VV", "Visual vocal"},            {"MT", "Main title"},       {"ET", "End title"},
        {"SRC", "Source"},
    };
    return u;
}

const std::vector<std::string>& naming_presets()
{
    static const std::vector<std::string> p = {"{production}_{code}_{title}_v{rev}", "{code} {title}",
                                               "{reel}_{code}_v{rev}_{date}"};
    return p;
}

juce::Colour marker_colour(const std::string& kind)
{
    if (kind == "hit") return colours::playhead;
    if (kind == "sync") return colours::accent;
    if (kind == "streamer") return colours::accent_warm;
    if (kind == "punch") return juce::Colour(0xffd9d9d9);
    return colours::text_dim;
}

/// A small padlock.
void draw_lock(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour colour)
{
    const float w = std::min(r.getWidth(), r.getHeight() * 0.8f);
    auto box = juce::Rectangle<float>(w, w * 0.72f).withCentre({r.getCentreX(), r.getCentreY() + w * 0.2f});
    g.setColour(colour);
    g.fillRoundedRectangle(box, 1.5f);
    juce::Path shackle;
    const float sw = box.getWidth() * 0.62f;
    shackle.addCentredArc(box.getCentreX(), box.getY(), sw * 0.5f, sw * 0.62f, 0.0f, -juce::MathConstants<float>::halfPi,
                          juce::MathConstants<float>::halfPi, true);
    g.strokePath(shackle, juce::PathStrokeType(std::max(1.2f, w * 0.14f)));
}

/// A coloured status pill; returns its width.
float draw_status_pill(juce::Graphics& g, juce::Rectangle<float> area, const std::string& status, float font_size,
                       bool fill_width = false)
{
    const auto colour = status_colour(status);
    const juce::Font font(juce::FontOptions(font_size, juce::Font::bold));
    const juce::String text = status_text(status);
    const bool locked = status == "locked";
    const float text_w = juce::GlyphArrangement::getStringWidth(font, text);
    const float h = std::min(area.getHeight(), font_size + 8.0f);
    float w = text_w + h + (locked ? h * 0.75f : 0.0f);
    if (fill_width) w = std::max(w, area.getWidth());
    w = std::min(w, area.getWidth());
    auto pill = juce::Rectangle<float>(area.getX(), area.getCentreY() - h * 0.5f, w, h);
    g.setColour(colour.withAlpha(locked ? 0.55f : 0.20f));
    g.fillRoundedRectangle(pill, h * 0.5f);
    g.setColour(colour.withAlpha(0.85f));
    g.drawRoundedRectangle(pill.reduced(0.5f), h * 0.5f, 1.0f);
    auto inner = pill.reduced(h * 0.45f, 0.0f);
    if (locked) {
        draw_lock(g, inner.removeFromLeft(h * 0.62f).reduced(0.0f, h * 0.18f), juce::Colours::white.withAlpha(0.9f));
        inner.removeFromLeft(h * 0.14f);
    }
    g.setColour(locked ? juce::Colours::white : colour.brighter(0.35f));
    g.setFont(font);
    g.drawText(text, inner, locked ? juce::Justification::centredLeft : juce::Justification::centred, true);
    return w;
}

/// "1M10" after "1M9": digits compare as numbers.
bool natural_less(const std::string& a, const std::string& b)
{
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const bool da = std::isdigit(static_cast<unsigned char>(a[i])) != 0;
        const bool db = std::isdigit(static_cast<unsigned char>(b[j])) != 0;
        if (da && db) {
            std::size_t ie = i, je = j;
            while (ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie]))) ++ie;
            while (je < b.size() && std::isdigit(static_cast<unsigned char>(b[je]))) ++je;
            const auto na = std::stoll(a.substr(i, std::min<std::size_t>(ie - i, 15)));
            const auto nb = std::stoll(b.substr(j, std::min<std::size_t>(je - j, 15)));
            if (na != nb) return na < nb;
            i = ie;
            j = je;
            continue;
        }
        const auto ca = std::tolower(static_cast<unsigned char>(a[i])), cb = std::tolower(static_cast<unsigned char>(b[j]));
        if (ca != cb) return ca < cb;
        ++i;
        ++j;
    }
    return a.size() - i < b.size() - j;
}

void style_editor(juce::TextEditor& e, const juce::String& id, const juce::String& hint = {})
{
    e.setComponentID(id);
    e.setFont(juce::FontOptions(14.0f));
    e.setIndents(7, 5);
    e.setColour(juce::TextEditor::backgroundColourId, colours::panel_raised);
    e.setColour(juce::TextEditor::outlineColourId, colours::outline);
    e.setColour(juce::TextEditor::focusedOutlineColourId, colours::accent);
    e.setColour(juce::TextEditor::textColourId, colours::text);
    e.setColour(juce::TextEditor::highlightColourId, colours::accent.withAlpha(0.35f));
    if (hint.isNotEmpty()) e.setTextToShowWhenEmpty(hint, colours::text_dim.withAlpha(0.6f));
}

void style_combo(juce::ComboBox& c, const juce::String& id)
{
    c.setComponentID(id);
    c.setColour(juce::ComboBox::backgroundColourId, colours::panel_raised);
    c.setColour(juce::ComboBox::outlineColourId, colours::outline);
    c.setColour(juce::ComboBox::textColourId, colours::text);
    c.setColour(juce::ComboBox::arrowColourId, colours::text_dim);
}

void small_caption(juce::Label& l, const juce::String& text)
{
    l.setText(text, juce::dontSendNotification);
    l.setFont(juce::FontOptions(11.5f, juce::Font::bold));
    l.setColour(juce::Label::textColourId, colours::text_dim);
    l.setJustificationType(juce::Justification::bottomLeft);
    l.setBorderSize({0, 1, 2, 0});
    l.setInterceptsMouseClicks(false, false);
}

void set_status(AppContext& context, juce::Label* local, const juce::String& message)
{
    if (local) local->setText(message, juce::dontSendNotification);
    if (context.status) context.status(message);
}

// --- Status chips ----------------------------------------------------------------------------

/// "3 writing · 2 awaiting feedback · 5 approved"; a chip filters by its status.
class StatusChips final : public juce::Component, public juce::SettableTooltipClient {
public:
    StatusChips() { setTooltip("Cues by status; click a chip to filter by it"); }

    std::function<void(const std::string& status)> on_click;

    void set(const project::ProjectData& d, const std::string& today)
    {
        chips_.clear();
        std::map<std::string, int> counts;
        int late = 0;
        for (const auto& c : d.cues) {
            ++counts[c.status];
            if (cues::overdue(c, today)) ++late;
        }
        for (const auto& s : project::cue_statuses())
            if (counts[s] > 0) chips_.push_back({s, juce::String(counts[s]) + " " + status_text(s), status_colour(s), {}});
        if (late > 0) chips_.push_back({"__overdue", juce::String(late) + " overdue", colours::playhead, {}});
        total_ = static_cast<int>(d.cues.size());
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const juce::Font font(juce::FontOptions(12.0f));
        float x = 0.0f;
        const float h = 22.0f, y = (static_cast<float>(getHeight()) - h) * 0.5f;
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.setColour(colours::text_dim);
        const juce::String total = juce::String(total_) + (total_ == 1 ? " cue" : " cues");
        const float tw = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), total);
        g.drawText(total, juce::Rectangle<float>(x, y, tw + 4.0f, h), juce::Justification::centredLeft);
        x += tw + 10.0f;
        g.setFont(font);
        for (auto& chip : chips_) {
            const float w = juce::GlyphArrangement::getStringWidth(font, chip.text) + 24.0f;
            if (x + w > static_cast<float>(getWidth())) {
                chip.bounds = {};
                continue;
            }
            chip.bounds = {x, y, w, h};
            g.setColour(chip.colour.withAlpha(0.16f));
            g.fillRoundedRectangle(chip.bounds, h * 0.5f);
            g.setColour(chip.colour.withAlpha(0.7f));
            g.drawRoundedRectangle(chip.bounds.reduced(0.5f), h * 0.5f, 1.0f);
            g.setColour(chip.colour);
            g.fillEllipse(x + 8.0f, y + h * 0.5f - 3.0f, 6.0f, 6.0f);
            g.setColour(colours::text);
            g.drawText(chip.text, chip.bounds.withTrimmedLeft(17.0f), juce::Justification::centredLeft, false);
            x += w + 6.0f;
        }
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        for (const auto& chip : chips_)
            if (chip.bounds.contains(e.position) && on_click) on_click(chip.status);
    }

private:
    struct Chip {
        std::string status;
        juce::String text;
        juce::Colour colour;
        juce::Rectangle<float> bounds;
    };
    std::vector<Chip> chips_;
    int total_ = 0;
};

// --- Markers inside a cue --------------------------------------------------------------------

class MarkerList final : public juce::Component {
public:
    struct Item {
        std::string kind;
        juce::String timecode;
        juce::String label;
    };

    void set(std::vector<Item> items)
    {
        items_ = std::move(items);
        repaint();
    }
    int preferred_height() const { return items_.empty() ? 26 : static_cast<int>(items_.size()) * 24 + 4; }

    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        if (items_.empty()) {
            g.setColour(colours::text_dim);
            g.setFont(juce::FontOptions(12.5f, juce::Font::italic));
            g.drawText("No hits or sync points inside this cue.", r, juce::Justification::centredLeft);
            return;
        }
        g.setColour(colours::panel);
        g.fillRoundedRectangle(r.toFloat(), 4.0f);
        r.reduce(0, 2);
        for (std::size_t i = 0; i < items_.size(); ++i) {
            auto row = r.removeFromTop(24).reduced(8, 0);
            const auto& it = items_[i];
            const auto c = marker_colour(it.kind);
            g.setColour(c);
            g.fillEllipse(static_cast<float>(row.getX()), static_cast<float>(row.getCentreY()) - 4.0f, 8.0f, 8.0f);
            row.removeFromLeft(16);
            g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
            g.drawText(utf8(it.kind).toUpperCase(), row.removeFromLeft(70), juce::Justification::centredLeft);
            g.setColour(colours::text);
            g.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.5f, juce::Font::plain));
            g.drawText(it.timecode, row.removeFromLeft(96), juce::Justification::centredLeft);
            g.setFont(juce::FontOptions(13.0f));
            g.drawText(it.label.isEmpty() ? juce::String("(no label)") : it.label, row, juce::Justification::centredLeft, true);
        }
    }

private:
    std::vector<Item> items_;
};

// --- Deliverables checklist ------------------------------------------------------------------

class DeliverableList final : public juce::Component {
public:
    std::function<void(int index, const std::string& name, bool done)> on_toggle;
    std::function<void(int index, const std::string& name)> on_remove;

    void set(const std::vector<project::Deliverable>& list, bool read_only)
    {
        std::vector<std::string> names;
        for (const auto& d : list) names.push_back(d.name);
        if (names != names_) {
            rows_.clear();
            names_ = names;
            for (std::size_t i = 0; i < list.size(); ++i) {
                auto row = std::make_unique<Row>();
                const int index = static_cast<int>(i);
                const std::string name = list[i].name;
                row->toggle.setButtonText(utf8(name));
                row->toggle.setComponentID("deliverable." + juce::String(index));
                row->toggle.setColour(juce::ToggleButton::tickColourId, status_colour("approved"));
                row->toggle.setColour(juce::ToggleButton::tickDisabledColourId, colours::outline.brighter(0.3f));
                row->toggle.setColour(juce::ToggleButton::textColourId, colours::text);
                row->toggle.onClick = [this, index, name, t = &row->toggle] {
                    if (on_toggle) on_toggle(index, name, t->getToggleState());
                };
                row->remove.setButtonText(kCross);
                row->remove.setComponentID("deliverable.remove." + juce::String(index));
                row->remove.setTooltip("Remove this deliverable");
                row->remove.onClick = [this, index, name] {
                    if (on_remove) on_remove(index, name);
                };
                addAndMakeVisible(row->toggle);
                addAndMakeVisible(row->remove);
                rows_.push_back(std::move(row));
            }
            resized();
        }
        for (std::size_t i = 0; i < list.size(); ++i) {
            rows_[i]->toggle.setToggleState(list[i].done, juce::dontSendNotification);
            rows_[i]->toggle.setEnabled(!read_only);
            rows_[i]->remove.setVisible(!read_only);
        }
        repaint();
    }

    int preferred_height() const { return rows_.empty() ? 26 : static_cast<int>(rows_.size()) * 26; }

    void paint(juce::Graphics& g) override
    {
        if (rows_.empty()) {
            g.setColour(colours::text_dim);
            g.setFont(juce::FontOptions(12.5f, juce::Font::italic));
            g.drawText("No deliverables listed.", getLocalBounds(), juce::Justification::centredLeft);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        for (auto& row : rows_) {
            auto line = r.removeFromTop(26);
            row->remove.setBounds(line.removeFromRight(26).reduced(2));
            row->toggle.setBounds(line);
        }
    }

private:
    struct Row {
        juce::ToggleButton toggle;
        juce::TextButton remove;
    };
    std::vector<std::unique_ptr<Row>> rows_;
    std::vector<std::string> names_;
};

// --- Detail editor ---------------------------------------------------------------------------

class CueEditor final : public juce::Component {
public:
    CueEditor(AppContext& context, juce::Label& message) : context_(context), message_(message)
    {
        auto caption = [this](juce::Label& l, const juce::String& text) {
            small_caption(l, text);
            addAndMakeVisible(l);
        };
        caption(code_caption_, "CODE");
        caption(title_caption_, "TITLE");
        caption(description_caption_, "DESCRIPTION");
        caption(reel_caption_, "REEL / EPISODE");
        caption(scene_caption_, "SCENE");
        caption(status_caption_, "STATUS");
        caption(usage_caption_, "USAGE");
        caption(revision_caption_, "REVISION");
        caption(deadline_caption_, "DEADLINE");
        caption(composer_caption_, "COMPOSER");
        caption(spot_in_caption_, "SPOT IN");
        caption(spot_out_caption_, "SPOT OUT");
        caption(music_caption_, "MUSIC RANGE");
        caption(transition_in_caption_, "TRANSITION IN");
        caption(transition_out_caption_, "TRANSITION OUT");

        style_editor(code_, "code", "1M1");
        style_editor(title_, "title", "Cue title");
        style_editor(description_, "description", "What happens in the scene");
        style_editor(deadline_, "deadline", "YYYY-MM-DD");
        style_editor(composer_, "composer", "Composer");
        style_editor(spot_in_, "spot_in", "01:00:00:00");
        style_editor(spot_out_, "spot_out", "01:00:00:00");
        style_editor(transition_in_, "transition_in", "e.g. segue from 1M2");
        style_editor(transition_out_, "transition_out", "e.g. hard out on cut");
        style_editor(notes_, "notes", "Director's notes from spotting and feedback");
        style_editor(new_deliverable_, "new_deliverable", "New deliverable (e.g. Stems)");
        const auto mono = juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 14.0f, juce::Font::plain);
        spot_in_.setFont(mono);
        spot_out_.setFont(mono);
        for (auto* e : {&description_, &notes_}) {
            e->setMultiLine(true, true);
            e->setReturnKeyStartsNewLine(true);
            e->setScrollbarsShown(true);
        }

        bind(code_, [](const project::Cue& c) { return c.code; }, [this](const std::string& v) {
            if (v.empty()) return refuse("A cue needs a code.");
            return update([&](project::Cue& c) { c.code = v; }, "Rename cue " + v);
        });
        bind(title_, [](const project::Cue& c) { return c.title; }, [this](const std::string& v) {
            if (v.empty()) return refuse("A cue needs a title.");
            return update([&](project::Cue& c) { c.title = v; }, "Retitle cue");
        });
        bind(description_, [](const project::Cue& c) { return c.description; }, [this](const std::string& v) {
            return update([&](project::Cue& c) { c.description = v; }, "Edit cue description");
        });
        bind(deadline_, [](const project::Cue& c) { return c.deadline; }, [this](const std::string& v) {
            if (!valid_iso_date(v)) return refuse("Deadline must be a date written YYYY-MM-DD, e.g. 2026-12-01.");
            return update([&](project::Cue& c) { c.deadline = v; }, v.empty() ? "Clear cue deadline" : "Set cue deadline");
        });
        bind(composer_, [](const project::Cue& c) { return c.composer; }, [this](const std::string& v) {
            return update([&](project::Cue& c) { c.composer = v; }, "Assign cue composer");
        });
        bind(spot_in_, [](const project::Cue& c) { return c.spot_in; },
             [this](const std::string& v) { return commit_spot(v, trimmed(spot_out_.getText())); });
        bind(spot_out_, [](const project::Cue& c) { return c.spot_out; },
             [this](const std::string& v) { return commit_spot(trimmed(spot_in_.getText()), v); });
        bind(transition_in_, [](const project::Cue& c) { return c.transition_in; }, [this](const std::string& v) {
            return update([&](project::Cue& c) { c.transition_in = v; }, "Edit cue transition in");
        });
        bind(transition_out_, [](const project::Cue& c) { return c.transition_out; }, [this](const std::string& v) {
            return update([&](project::Cue& c) { c.transition_out = v; }, "Edit cue transition out");
        });
        bind(notes_, [](const project::Cue& c) { return c.notes; }, [this](const std::string& v) {
            return update([&](project::Cue& c) { c.notes = v; }, "Edit director notes");
        });

        style_combo(reel_, "reel");
        style_combo(scene_, "scene");
        style_combo(status_, "status");
        style_combo(usage_, "usage");
        for (std::size_t i = 0; i < project::cue_statuses().size(); ++i)
            status_.addItem(status_title(project::cue_statuses()[i]), static_cast<int>(i) + 1);
        for (std::size_t i = 0; i < usages().size(); ++i)
            usage_.addItem(utf8(usages()[i].first) + "  " + kDash + "  " + utf8(usages()[i].second), static_cast<int>(i) + 1);
        reel_.onChange = [this] {
            if (updating_ || reel_.getSelectedId() <= 0) return;
            const auto idx = static_cast<std::size_t>(reel_.getSelectedId() - 1);
            const std::string id = idx < reel_ids_.size() ? reel_ids_[idx] : std::string();
            update(
                [&](project::Cue& c) {
                    if (c.reel_id != id) c.scene_id.clear();
                    c.reel_id = id;
                },
                "Move cue to reel");
        };
        scene_.onChange = [this] {
            if (updating_ || scene_.getSelectedId() <= 0) return;
            const auto idx = static_cast<std::size_t>(scene_.getSelectedId() - 1);
            const std::string id = idx < scene_ids_.size() ? scene_ids_[idx] : std::string();
            update([&](project::Cue& c) { c.scene_id = id; }, "Set cue scene");
        };
        status_.onChange = [this] {
            if (updating_ || status_.getSelectedId() <= 0) return;
            const auto s = project::cue_statuses()[static_cast<std::size_t>(status_.getSelectedId() - 1)];
            if (update([&](project::Cue& c) { c.status = s; }, "Cue status: " + str(status_text(s))))
                say("Status set to " + status_text(s) + (s == "locked" ? " - the cue is now read-only" : juce::String()));
        };
        usage_.onChange = [this] {
            if (updating_ || usage_.getSelectedId() <= 0) return;
            const auto u = usages()[static_cast<std::size_t>(usage_.getSelectedId() - 1)].first;
            update([&](project::Cue& c) { c.usage = u; }, "Set cue usage " + u);
        };

        revision_value_.setFont(juce::FontOptions(15.0f, juce::Font::bold));
        revision_value_.setColour(juce::Label::textColourId, colours::text);
        revision_value_.setColour(juce::Label::backgroundColourId, colours::panel);
        revision_value_.setColour(juce::Label::outlineColourId, colours::outline);
        revision_value_.setJustificationType(juce::Justification::centred);
        revision_value_.setComponentID("revision");
        addAndMakeVisible(revision_value_);

        music_range_.setFont(juce::FontOptions(13.0f));
        music_range_.setColour(juce::Label::textColourId, colours::text);
        music_range_.setJustificationType(juce::Justification::centredLeft);
        music_range_.setComponentID("music_range");
        addAndMakeVisible(music_range_);
        drift_.setFont(juce::FontOptions(12.5f));
        drift_.setColour(juce::Label::textColourId, colours::accent_warm);
        drift_.setJustificationType(juce::Justification::centredLeft);
        drift_.setMinimumHorizontalScale(0.75f);
        addAndMakeVisible(drift_);

        auto button = [this](juce::TextButton& b, const juce::String& id, const juce::String& tip) {
            b.setComponentID(id);
            b.setTooltip(tip);
            addAndMakeVisible(b);
        };
        button(revision_up_, "revision_up", "Start a new revision of this cue (one undo step)");
        button(move_music_, "move_music", "Move the cue's music range to the spotted timecodes through the tempo map");
        button(start_from_playhead_, "start_from_playhead", "Set the music start to the playhead position");
        button(end_from_playhead_, "end_from_playhead", "Set the music end to the playhead position");
        button(add_deliverable_, "add_deliverable", "Add a deliverable to this cue's checklist");
        button(unlock_, "unlock", "Unlock this cue (asks first); it returns to Approved");
        unlock_.setColour(juce::TextButton::buttonColourId, status_colour("locked").withAlpha(0.6f));

        revision_up_.onClick = [this] {
            commit_pending();
            if (update([](project::Cue& c) { ++c.revision; }, "New cue revision"))
                if (const auto* c = cue()) say(utf8(c->code) + " is now revision " + juce::String(c->revision));
        };
        move_music_.onClick = [this] { move_music_to_spotting(); };
        start_from_playhead_.onClick = [this] { set_from_playhead(true); };
        end_from_playhead_.onClick = [this] { set_from_playhead(false); };
        add_deliverable_.onClick = [this] { add_deliverable(); };
        new_deliverable_.onReturnKey = [this] { add_deliverable(); };
        unlock_.onClick = [this] { ask_unlock(); };

        deliverables_.on_toggle = [this](int index, const std::string& name, bool done) {
            update(
                [&](project::Cue& c) {
                    const auto i = static_cast<std::size_t>(index);
                    if (i < c.deliverables.size() && c.deliverables[i].name == name) c.deliverables[i].done = done;
                },
                (done ? "Deliverable done: " : "Deliverable not done: ") + name);
        };
        deliverables_.on_remove = [this](int index, const std::string& name) {
            update(
                [&](project::Cue& c) {
                    const auto i = static_cast<std::size_t>(index);
                    if (i < c.deliverables.size() && c.deliverables[i].name == name)
                        c.deliverables.erase(c.deliverables.begin() + index);
                },
                "Remove deliverable " + name);
        };
        addAndMakeVisible(deliverables_);
        addAndMakeVisible(markers_);

        for (auto* e : {&code_, &title_, &description_, &deadline_, &composer_, &spot_in_, &spot_out_, &transition_in_,
                        &transition_out_, &notes_, &new_deliverable_})
            addAndMakeVisible(*e);
        for (auto* c : {&reel_, &scene_, &status_, &usage_}) addAndMakeVisible(*c);
        load(true);
    }

    const std::string& cue_id() const noexcept { return cue_id_; }

    /// Shows another cue; edits typed into the one shown are committed first.
    void show(const std::string& id)
    {
        if (id == cue_id_) return;
        commit_pending();
        cue_id_ = id;
        load(true);
    }

    /// Re-reads the document; a field being typed in is left alone.
    void refresh() { load(false); }

    /// Commits every field whose text differs from the document.
    void commit_pending()
    {
        for (auto& b : bindings_) commit(b);
    }

    int content_height() { return layout(false); }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(colours::background);
        const auto* c = cue();
        if (!c) {
            g.setColour(colours::text_dim);
            g.setFont(juce::FontOptions(15.0f));
            g.drawFittedText("Select a cue in the list to edit it,\nor press New cue to spot one at the playhead.",
                             getLocalBounds().reduced(24).withHeight(120), juce::Justification::centred, 3);
            return;
        }
        // Heading: code, title, status pill, revision, lock.
        auto head = juce::Rectangle<int>(kPad, 10, getWidth() - 2 * kPad, 58);
        g.setColour(colours::panel);
        g.fillRoundedRectangle(head.toFloat().expanded(6.0f, 4.0f), 6.0f);
        g.setColour(status_colour(c->status));
        g.fillRoundedRectangle(head.toFloat().expanded(6.0f, 4.0f).removeFromLeft(4.0f), 2.0f);
        auto line1 = head.removeFromTop(30).withTrimmedLeft(6);
        g.setFont(juce::FontOptions(21.0f, juce::Font::bold));
        g.setColour(colours::text);
        const juce::String code = utf8(c->code);
        const int code_w = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), code) + 12;
        g.drawText(code, line1.removeFromLeft(code_w), juce::Justification::centredLeft);
        g.setFont(juce::FontOptions(16.0f));
        g.drawText(utf8(c->title), line1, juce::Justification::centredLeft, true);
        auto line2 = head.withTrimmedLeft(6);
        const float pw = draw_status_pill(g, line2.toFloat().withWidth(200.0f), c->status, 12.0f);
        line2.removeFromLeft(static_cast<int>(pw) + 10);
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(12.5f));
        juce::String sub = "rev " + juce::String(c->revision);
        const auto times = cues::times_of(*c, context_.session.data());
        sub << "   " << kDot << "   " << utf8(times.duration);
        if (cues::overdue(*c, today_iso())) sub << "   " << kDot << "   overdue";
        if (c->locked()) sub = "Locked " + kDot + " read-only";
        g.drawText(sub, line2, juce::Justification::centredLeft, true);

        // Section headings.
        g.setFont(juce::FontOptions(11.5f, juce::Font::bold));
        for (const auto& [y, text] : sections_) {
            g.setColour(colours::accent.withAlpha(0.9f));
            g.drawText(text, kPad, y, getWidth() - 2 * kPad, 16, juce::Justification::centredLeft);
            g.setColour(colours::outline);
            const int tw = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), text) + 10;
            g.fillRect(kPad + tw, y + 8, getWidth() - 2 * kPad - tw, 1);
        }
        if (bad_field_ != nullptr && bad_field_->isVisible()) {
            g.setColour(colours::missing);
            g.drawRoundedRectangle(bad_field_->getBounds().toFloat().expanded(1.5f), 4.0f, 1.5f);
        }
    }

    void resized() override { layout(true); }

private:
    static constexpr int kPad = 14;
    static constexpr int kField = 28;
    static constexpr int kCaption = 17;

    struct Binding {
        juce::TextEditor* editor;
        std::function<std::string(const project::Cue&)> get;
        std::function<bool(const std::string&)> set;
    };

    void bind(juce::TextEditor& e, std::function<std::string(const project::Cue&)> get,
              std::function<bool(const std::string&)> set)
    {
        bindings_.push_back({&e, std::move(get), std::move(set)});
        const std::size_t index = bindings_.size() - 1;
        if (!e.isMultiLine()) e.onReturnKey = [this, index] { commit(bindings_[index]); };
        e.onFocusLost = [this, index] { commit(bindings_[index]); };
        e.onEscapeKey = [this, index] {
            if (const auto* c = cue()) bindings_[index].editor->setText(utf8(bindings_[index].get(*c)), false);
        };
        e.onTextChange = [this, &e] {
            if (bad_field_ == &e) {
                bad_field_ = nullptr;
                repaint();
            }
        };
    }

    void commit(Binding& b)
    {
        const auto* c = cue();
        if (!c || c->locked() || b.editor->isReadOnly()) return;
        const std::string text = b.editor->isMultiLine() ? str(b.editor->getText().trimEnd()) : trimmed(b.editor->getText());
        if (text == b.get(*c)) return;
        if (!b.set(text)) {
            // Refused: the field shows what the document holds again.
            if (const auto* now = cue()) b.editor->setText(utf8(b.get(*now)), false);
        }
        else if (bad_field_ == b.editor) {
            bad_field_ = nullptr;
            repaint();
        }
    }

    bool refuse(const juce::String& why)
    {
        say(why);
        return false;
    }

    void say(const juce::String& text) { set_status(context_, &message_, text); }

    const project::Cue* cue() const { return cue_id_.empty() ? nullptr : context_.session.data().find_cue(cue_id_); }

    bool update(const std::function<void(project::Cue&)>& change, const std::string& label)
    {
        if (cue_id_.empty()) return false;
        const auto* c = cue();
        if (c && c->locked()) return refuse(utf8(c->code) + " is locked. Unlock it first to change it.");
        const bool ok = context_.session.update_cue(cue_id_, change, label);
        if (!ok) say("The cue could not be changed.");
        return ok;
    }

    bool commit_spot(const std::string& in_text, const std::string& out_text)
    {
        const auto* c = cue();
        if (!c) return false;
        if (in_text.empty() && out_text.empty())
            return update(
                [](project::Cue& cue_) {
                    cue_.spot_in.clear();
                    cue_.spot_out.clear();
                },
                "Clear cue spotting");
        if (in_text.empty() || out_text.empty()) {
            say("Enter both spot in and spot out to spot the cue.");
            return true;  // keep what was typed until the other field is filled in
        }
        const auto& d = context_.session.data();
        const auto in_s = picture::project_seconds_for_timecode(d, in_text);
        const auto out_s = picture::project_seconds_for_timecode(d, out_text);
        if (!in_s || !out_s) {
            bad_field_ = !in_s ? &spot_in_ : &spot_out_;
            repaint();
            return refuse("Not a timecode: \"" + utf8(!in_s ? in_text : out_text) + "\" (use HH:MM:SS:FF).");
        }
        if (*out_s <= *in_s) {
            bad_field_ = &spot_out_;
            repaint();
            return refuse("Spot out must be after spot in.");
        }
        const std::string in_tc = picture::project_timecode(d, *in_s), out_tc = picture::project_timecode(d, *out_s);
        if (in_tc == c->spot_in && out_tc == c->spot_out) {
            spot_in_.setText(utf8(in_tc), false);
            spot_out_.setText(utf8(out_tc), false);
            return true;
        }
        if (!context_.session.spot_cue(cue_id_, in_tc, out_tc, false)) return refuse("The cue could not be spotted.");
        say("Spotted " + utf8(c->code) + " at " + utf8(in_tc) + " " + kDash + " " + utf8(out_tc));
        return true;
    }

    void move_music_to_spotting()
    {
        commit_pending();
        const auto* c = cue();
        if (!c) return;
        if (c->spot_in.empty() || c->spot_out.empty()) {
            say("Spot the cue first (spot in and spot out), then move its music there.");
            return;
        }
        const std::string code = c->code;
        if (context_.session.spot_cue(cue_id_, c->spot_in, c->spot_out, true))
            say("Moved the music of " + utf8(code) + " to its spotting");
        else
            say("The music could not be moved (is the cue locked?).");
    }

    void set_from_playhead(bool start)
    {
        commit_pending();
        const Ticks pos = context_.engine.position();
        if (update(
                [&](project::Cue& c) {
                    if (start) c.start = pos;
                    else c.end = pos;
                },
                start ? "Set cue start from playhead" : "Set cue end from playhead"))
            say(juce::String(start ? "Cue start" : "Cue end") + " set to bar " +
                juce::String(context_.session.data().meter.bar_at(pos)));
    }

    void add_deliverable()
    {
        const std::string name = trimmed(new_deliverable_.getText());
        if (name.empty()) {
            new_deliverable_.grabKeyboardFocus();
            return;
        }
        if (update([&](project::Cue& c) { c.deliverables.push_back({name, false}); }, "Add deliverable " + name))
            new_deliverable_.clear();
    }

    void ask_unlock()
    {
        const auto* c = cue();
        if (!c || !c->locked()) return;
        const juce::String code = utf8(c->code);
        juce::Component::SafePointer<CueEditor> safe(this);
        const std::string id = cue_id_;
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle("Unlock " + code + "?")
                .withMessage(code + " is locked: it has been approved and its deliverables may already have been "
                                    "delivered.\n\nUnlocking returns it to Approved so it can be edited again.")
                .withButton("Unlock")
                .withButton("Cancel")
                .withAssociatedComponent(this),
            [safe, id, code](int result) {
                if (safe == nullptr || result != 1) return;
                if (safe->context_.session.update_cue(id, [](project::Cue& cue_) { cue_.status = "approved"; },
                                                       "Unlock cue " + code.toStdString(), true))
                    safe->say(code + " unlocked (status: approved)");
            });
    }

    void set_text(juce::TextEditor& e, const std::string& value, bool force)
    {
        if (!force && e.hasKeyboardFocus(true)) return;
        const auto text = utf8(value);
        if (e.getText() != text) e.setText(text, false);
    }

    void load(bool force)
    {
        const juce::ScopedValueSetter<bool> guard(updating_, true);
        const auto& d = context_.session.data();
        const auto* c = cue();
        if (!c) cue_id_.clear();
        const bool shown = c != nullptr;
        for (int i = 0; i < getNumChildComponents(); ++i) getChildComponent(i)->setVisible(shown);
        if (!shown) {
            bad_field_ = nullptr;
            repaint();
            return;
        }
        if (force) bad_field_ = nullptr;

        for (auto& b : bindings_) set_text(*b.editor, b.get(*c), force);

        // Reels and scenes (scenes of the cue's reel only).
        reel_ids_.assign(1, std::string());
        reel_.clear(juce::dontSendNotification);
        reel_.addItem("(no reel)", 1);
        int reel_sel = 1;
        for (const auto& r : d.reels) {
            reel_ids_.push_back(r.id);
            reel_.addItem(utf8(r.name), static_cast<int>(reel_ids_.size()));
            if (r.id == c->reel_id) reel_sel = static_cast<int>(reel_ids_.size());
        }
        reel_.setSelectedId(reel_sel, juce::dontSendNotification);
        scene_ids_.assign(1, std::string());
        scene_.clear(juce::dontSendNotification);
        scene_.addItem("(no scene)", 1);
        int scene_sel = 1;
        for (const auto& s : d.scenes) {
            if (s.reel_id != c->reel_id && s.id != c->scene_id) continue;
            scene_ids_.push_back(s.id);
            scene_.addItem(utf8(s.name), static_cast<int>(scene_ids_.size()));
            if (s.id == c->scene_id) scene_sel = static_cast<int>(scene_ids_.size());
        }
        scene_.setSelectedId(scene_sel, juce::dontSendNotification);
        status_.setSelectedId(status_index(c->status) + 1, juce::dontSendNotification);
        int usage_sel = 0;
        for (std::size_t i = 0; i < usages().size(); ++i)
            if (usages()[i].first == c->usage) usage_sel = static_cast<int>(i) + 1;
        usage_.setSelectedId(usage_sel, juce::dontSendNotification);
        revision_value_.setText("v" + juce::String(c->revision), juce::dontSendNotification);

        // Music range in bars and timecode, and how far it is from the spotting.
        const auto times = cues::times_of(*c, d);
        const int first_bar = d.meter.bar_at(c->start);
        const int last_bar = d.meter.bar_at(std::max(c->start, c->end - 1));
        music_range_.setText("Bars " + juce::String(first_bar) + kDash + juce::String(last_bar) + "    " + utf8(times.in_tc) +
                                 " " + kDash + " " + utf8(times.out_tc) + "    (" + utf8(times.duration) + ")",
                             juce::dontSendNotification);
        if (const auto drift = cues::spot_drift_frames(*c, d); drift && *drift != 0)
            drift_.setText("Music is " + juce::String(std::llabs(*drift)) + " frames " + (*drift > 0 ? "late" : "early") +
                               " against the spot-in.",
                           juce::dontSendNotification);
        else if (!c->spot_in.empty())
            drift_.setText(juce::String::fromUTF8("\xe2\x9c\x93") + " Music starts on the spot-in.", juce::dontSendNotification);
        else
            drift_.setText("Not spotted yet.", juce::dontSendNotification);
        drift_.setColour(juce::Label::textColourId, drift_.getText().startsWith("Music is ") ? colours::accent_warm
                                                                                                 : colours::text_dim);

        deliverables_.set(c->deliverables, c->locked());

        // Hits and sync points inside the cue (or attached to it).
        std::vector<MarkerList::Item> items;
        std::vector<const project::Marker*> inside;
        for (const auto& m : d.markers)
            if (m.cue_id == c->id || (m.seconds >= times.start_seconds - 1e-6 && m.seconds <= times.end_seconds + 1e-6))
                inside.push_back(&m);
        std::sort(inside.begin(), inside.end(), [](auto* a, auto* b) { return a->seconds < b->seconds; });
        for (const auto* m : inside) items.push_back({m->kind, utf8(picture::project_timecode(d, m->seconds)), utf8(m->label)});
        markers_.set(std::move(items));

        // Locked cues are read-only, with an Unlock button.
        const bool locked = c->locked();
        locked_ = locked;
        for (auto& b : bindings_) b.editor->setReadOnly(locked);
        new_deliverable_.setReadOnly(locked);
        for (auto* combo : {&reel_, &scene_, &status_, &usage_}) combo->setEnabled(!locked);
        for (auto* button : {&revision_up_, &move_music_, &start_from_playhead_, &end_from_playhead_, &add_deliverable_})
            button->setEnabled(!locked);
        move_music_.setEnabled(!locked && !c->spot_in.empty() && !c->spot_out.empty());
        new_deliverable_.setVisible(!locked);
        add_deliverable_.setVisible(!locked);
        unlock_.setVisible(locked);
        for (auto& b : bindings_)
            b.editor->setColour(juce::TextEditor::backgroundColourId, locked ? colours::panel : colours::panel_raised);

        const int h = layout(false);
        if (getHeight() != h) setSize(getWidth(), h);
        else layout(true);
        repaint();
    }

    /// Positions everything (when `apply`) and returns the content height.
    int layout(bool apply)
    {
        const int w = std::max(300, getWidth()) - 2 * kPad;
        int y = 10 + 58 + 14;
        sections_.clear();
        auto place = [apply](juce::Component& comp, int x, int yy, int ww, int hh) {
            if (apply) comp.setBounds(x, yy, ww, hh);
        };
        auto section = [&](const juce::String& title) {
            sections_.push_back({y, title});
            y += 22;
        };
        const int gap = 10;
        const int half = (w - gap) / 2;
        auto pair = [&](juce::Label& lc, juce::Component& left, juce::Label& rc, juce::Component& right, int left_w = -1) {
            const int lw = left_w < 0 ? half : left_w;
            place(lc, kPad, y, lw, kCaption);
            place(rc, kPad + lw + gap, y, w - lw - gap, kCaption);
            place(left, kPad, y + kCaption, lw, kField);
            place(right, kPad + lw + gap, y + kCaption, w - lw - gap, kField);
            y += kCaption + kField + 8;
        };

        if (apply) unlock_.setBounds(kPad + w - 96, 10 + 32, 96, 24);

        section("CUE");
        pair(code_caption_, code_, title_caption_, title_, 92);
        place(description_caption_, kPad, y, w, kCaption);
        place(description_, kPad, y + kCaption, w, 46);
        y += kCaption + 46 + 8;
        pair(reel_caption_, reel_, scene_caption_, scene_);
        pair(status_caption_, status_, usage_caption_, usage_);
        // Revision: value + button | deadline.
        place(revision_caption_, kPad, y, half, kCaption);
        place(deadline_caption_, kPad + half + gap, y, w - half - gap, kCaption);
        place(revision_value_, kPad, y + kCaption, 44, kField);
        place(revision_up_, kPad + 50, y + kCaption, half - 50, kField);
        place(deadline_, kPad + half + gap, y + kCaption, w - half - gap, kField);
        y += kCaption + kField + 8;
        place(composer_caption_, kPad, y, w, kCaption);
        place(composer_, kPad, y + kCaption, w, kField);
        y += kCaption + kField + 14;

        section("SPOTTING & MUSIC");
        pair(spot_in_caption_, spot_in_, spot_out_caption_, spot_out_);
        place(drift_, kPad, y - 4, w, 20);
        y += 22;
        place(move_music_, kPad, y, w, kField);
        y += kField + 8;
        place(music_caption_, kPad, y, w, kCaption);
        place(music_range_, kPad, y + kCaption, w, 22);
        y += kCaption + 24;
        place(start_from_playhead_, kPad, y, half, kField);
        place(end_from_playhead_, kPad + half + gap, y, w - half - gap, kField);
        y += kField + 8;
        pair(transition_in_caption_, transition_in_, transition_out_caption_, transition_out_);
        y += 6;

        section("DIRECTOR NOTES");
        place(notes_, kPad, y, w, 92);
        y += 92 + 14;

        section("DELIVERABLES");
        const int dh = deliverables_.preferred_height();
        place(deliverables_, kPad, y, w, dh);
        y += dh + 4;
        if (!locked_) {
            place(new_deliverable_, kPad, y, w - 70, kField);
            place(add_deliverable_, kPad + w - 64, y, 64, kField);
            y += kField;
        }
        y += 14;

        section("HITS & SYNC POINTS");
        const int mh = markers_.preferred_height();
        place(markers_, kPad, y, w, mh);
        y += mh + 18;
        return y;
    }

    AppContext& context_;
    juce::Label& message_;
    std::string cue_id_;
    bool updating_ = false;
    bool locked_ = false;
    std::vector<Binding> bindings_;
    juce::TextEditor* bad_field_ = nullptr;
    std::vector<std::pair<int, juce::String>> sections_;
    std::vector<std::string> reel_ids_, scene_ids_;

    juce::Label code_caption_, title_caption_, description_caption_, reel_caption_, scene_caption_, status_caption_,
        usage_caption_, revision_caption_, deadline_caption_, composer_caption_, spot_in_caption_, spot_out_caption_,
        music_caption_, transition_in_caption_, transition_out_caption_;
    juce::TextEditor code_, title_, description_, deadline_, composer_, spot_in_, spot_out_, transition_in_,
        transition_out_, notes_, new_deliverable_;
    juce::ComboBox reel_, scene_, status_, usage_;
    juce::Label revision_value_, music_range_, drift_;
    juce::TextButton revision_up_{"+1 revision"}, move_music_{"Move music to spotting"},
        start_from_playhead_{"Start = playhead"}, end_from_playhead_{"End = playhead"}, add_deliverable_{"Add"},
        unlock_{"Unlock..."};
    DeliverableList deliverables_;
    MarkerList markers_;
};

// --- The window content --------------------------------------------------------------------

enum Column {
    colCode = 1,
    colTitle,
    colReel,
    colScene,
    colIn,
    colOut,
    colDur,
    colStatus,
    colRev,
    colDeadline,
    colComposer,
    colDrift,
};

struct Row {
    std::string id;
    juce::String code, title, reel, scene, in_tc, out_tc, duration, deadline, composer;
    std::string status;
    int reel_index = 0;
    Ticks start = 0, end = 0;
    double seconds = 0.0;
    int revision = 1;
    bool overdue = false;
    std::optional<std::int64_t> drift;
    juce::StringArray overlaps;
};

class CueManager final : public juce::Component, private juce::TableListBoxModel, private juce::AsyncUpdater {
public:
    explicit CueManager(AppContext& context) : context_(context), editor_(context, message_)
    {
        setComponentID("cue_manager");

        // Production.
        small_caption(production_caption_, "PRODUCTION");
        addAndMakeVisible(production_caption_);
        style_editor(production_title_, "production_title", "Film or series title");
        style_editor(production_director_, "production_director", "Director");
        style_editor(production_composer_, "production_composer", "Composer");
        production_title_.setFont(juce::FontOptions(15.0f, juce::Font::bold));
        for (auto* e : {&production_title_, &production_director_, &production_composer_}) {
            e->onReturnKey = [this] { commit_production(); };
            e->onFocusLost = [this] { commit_production(); };
            addAndMakeVisible(*e);
        }
        add_reel_.setComponentID("add_reel");
        add_scene_.setComponentID("add_scene");
        add_reel_.setTooltip("Add a reel or episode");
        add_scene_.setTooltip("Add a scene to the filtered reel (or the selected cue's reel)");
        add_reel_.onClick = [this] { ask_add_reel(); };
        add_scene_.onClick = [this] { ask_add_scene(); };
        addAndMakeVisible(add_reel_);
        addAndMakeVisible(add_scene_);

        // Filters.
        style_combo(status_filter_, "status_filter");
        status_filter_.addItem("All statuses", 1);
        for (std::size_t i = 0; i < project::cue_statuses().size(); ++i)
            status_filter_.addItem(status_title(project::cue_statuses()[i]), static_cast<int>(i) + 2);
        status_filter_.setSelectedId(1, juce::dontSendNotification);
        status_filter_.onChange = [this] { rebuild(); };
        style_combo(reel_filter_, "reel_filter");
        reel_filter_.onChange = [this] { rebuild(); };
        style_editor(search_, "search", "Search code, title, notes...");
        search_.onTextChange = [this] { rebuild(); };
        search_.onEscapeKey = [this] { search_.clear(); };
        overdue_only_.setComponentID("overdue_only");
        overdue_only_.setColour(juce::ToggleButton::tickColourId, colours::playhead);
        overdue_only_.onClick = [this] { rebuild(); };
        chips_.on_click = [this](const std::string& s) {
            if (s == "__overdue") {
                overdue_only_.setToggleState(!overdue_only_.getToggleState(), juce::dontSendNotification);
            }
            else {
                const int id = status_index(s) + 2;
                status_filter_.setSelectedId(status_filter_.getSelectedId() == id ? 1 : id, juce::dontSendNotification);
            }
            rebuild();
        };
        for (juce::Component* c : std::initializer_list<juce::Component*>{&status_filter_, &reel_filter_, &search_, &overdue_only_, &chips_})
            addAndMakeVisible(c);

        // Actions.
        auto action = [this](juce::TextButton& b, const juce::String& id, const juce::String& tip) {
            b.setComponentID(id);
            b.setTooltip(tip);
            addAndMakeVisible(b);
        };
        action(new_cue_, "new_cue", "A new 8-bar cue starting at the bar under the playhead");
        action(duplicate_, "duplicate", "Copy the selected cue (new code, same range and notes)");
        action(delete_, "delete", "Delete the selected cue (asks first; locked cues are refused)");
        action(goto_, "goto", "Move the playhead to the selected cue's start (or double-click a row)");
        new_cue_.setColour(juce::TextButton::buttonColourId, colours::accent.withAlpha(0.75f));
        new_cue_.onClick = [this] { new_cue(); };
        duplicate_.onClick = [this] { duplicate_cue(); };
        delete_.onClick = [this] { ask_delete(); };
        goto_.onClick = [this] { go_to(selected_id_); };
        shown_count_.setFont(juce::FontOptions(12.5f));
        shown_count_.setColour(juce::Label::textColourId, colours::text_dim);
        shown_count_.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(shown_count_);

        // Dashboard.
        table_.setComponentID("cue_table");
        table_.setModel(this);
        table_.setRowHeight(30);
        table_.setMultipleSelectionEnabled(true);
        table_.setColour(juce::ListBox::backgroundColourId, colours::panel);
        table_.setColour(juce::ListBox::outlineColourId, colours::outline);
        table_.setOutlineThickness(1);
        auto& header = table_.getHeader();
        header.setColour(juce::TableHeaderComponent::backgroundColourId, colours::panel_raised);
        header.setColour(juce::TableHeaderComponent::textColourId, colours::text_dim);
        header.setColour(juce::TableHeaderComponent::outlineColourId, colours::outline);
        header.setColour(juce::TableHeaderComponent::highlightColourId, colours::accent.withAlpha(0.25f));
        const int column_flags = juce::TableHeaderComponent::visible | juce::TableHeaderComponent::resizable |
                          juce::TableHeaderComponent::sortable;
        header.addColumn("Code", colCode, 52, 46, 120, column_flags);
        header.addColumn("Title", colTitle, 170, 100, 600, column_flags);
        header.addColumn("Reel", colReel, 64, 44, 200, column_flags);
        header.addColumn("Scene", colScene, 100, 50, 260, column_flags);
        header.addColumn("In", colIn, 92, 90, 140, column_flags);
        header.addColumn("Out", colOut, 92, 90, 140, column_flags);
        header.addColumn("Dur", colDur, 48, 44, 90, column_flags);
        header.addColumn("Status", colStatus, 132, 116, 220, column_flags);
        header.addColumn("Rev", colRev, 40, 36, 70, column_flags);
        header.addColumn("Deadline", colDeadline, 88, 84, 140, column_flags);
        header.addColumn("Composer", colComposer, 100, 60, 260, column_flags);
        header.addColumn("Drift", colDrift, 58, 52, 120, column_flags);
        header.setStretchToFitActive(true);
        addAndMakeVisible(table_);

        // Detail editor.
        detail_.setComponentID("cue_detail");
        detail_.setViewedComponent(&editor_, false);
        detail_.setScrollBarsShown(true, false);
        detail_.setScrollBarThickness(10);
        addAndMakeVisible(detail_);

        // Exports and rendering.
        action(export_csv_, "export_csv", "Write the cue sheet (code, title, reel, scene, in, out, duration, usage...) as CSV");
        action(export_notes_, "export_notes", "Write spotting notes for the director and editor as plain text");
        action(render_selected_, "render_selected", "Bounce the selected cues to WAV files");
        action(render_approved_, "render_approved", "Bounce every approved and locked cue to WAV files");
        export_csv_.onClick = [this] { export_text(true); };
        export_notes_.onClick = [this] { export_text(false); };
        render_selected_.onClick = [this] { render(selected_ids(), "selected"); };
        render_approved_.onClick = [this] {
            std::vector<std::string> ids;
            for (const auto* c : cues::list(context_.session.data()))
                if (c->status == "approved" || c->status == "locked") ids.push_back(c->id);
            render(ids, "approved");
        };
        small_caption(preset_caption_, "BOUNCE NAMING");
        preset_caption_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(preset_caption_);
        style_combo(preset_, "naming_preset");
        for (std::size_t i = 0; i < naming_presets().size(); ++i)
            preset_.addItem(utf8(naming_presets()[i]), static_cast<int>(i) + 1);
        preset_.setSelectedId(1, juce::dontSendNotification);
        preset_.setEditableText(true);
        preset_.setTooltip("Tokens: {production} {project} {reel} {code} {title} {rev} {status} {date}");
        preset_.onChange = [this] { update_preview(); };
        addAndMakeVisible(preset_);
        style_combo(bit_depth_, "bit_depth");
        bit_depth_.addItem("24-bit", 24);
        bit_depth_.addItem("16-bit", 16);
        bit_depth_.setSelectedId(24, juce::dontSendNotification);
        addAndMakeVisible(bit_depth_);
        preview_.setComponentID("naming_preview");
        preview_.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.5f, juce::Font::plain));
        preview_.setColour(juce::Label::textColourId, colours::accent);
        preview_.setMinimumHorizontalScale(0.7f);
        addAndMakeVisible(preview_);
        message_.setComponentID("message");
        message_.setFont(juce::FontOptions(12.5f));
        message_.setColour(juce::Label::textColourId, colours::text_dim);
        message_.setMinimumHorizontalScale(0.8f);
        addAndMakeVisible(message_);

        listener_ = context_.session.add_listener(
            [safe = juce::Component::SafePointer<CueManager>(this)](session::Change change) {
                if (safe != nullptr && change == session::Change::Document) safe->triggerAsyncUpdate();
            });
        refresh_all();
        if (!rows_.empty()) select_cue(rows_.front().id);
        setSize(1180, 720);
    }

    ~CueManager() override
    {
        // The session may already be gone at shutdown: nothing here touches it.
        table_.setModel(nullptr);
        detail_.setViewedComponent(nullptr, false);
    }

    /// Stops listening (called while the session is alive, when the window closes).
    void detach()
    {
        editor_.commit_pending();
        if (listener_ != 0) context_.session.remove_listener(listener_);
        listener_ = 0;
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(colours::background);
        g.setColour(colours::panel);
        g.fillRect(top_area_);
        g.fillRect(bottom_area_);
        g.setColour(colours::outline);
        g.fillRect(top_area_.getX(), top_area_.getBottom() - 1, top_area_.getWidth(), 1);
        g.fillRect(bottom_area_.getX(), bottom_area_.getY(), bottom_area_.getWidth(), 1);
        g.fillRect(detail_.getX() - 1, top_area_.getBottom(), 1, bottom_area_.getY() - top_area_.getBottom());
    }

    void paintOverChildren(juce::Graphics& g) override
    {
        if (!rows_.empty()) return;
        auto r = table_.getBounds().withTrimmedTop(table_.getHeaderHeight()).reduced(20);
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(15.0f));
        const bool filtered = !context_.session.data().cues.empty();
        g.drawFittedText(filtered ? "No cues match the filters."
                                  : "No cues yet.\nMove the playhead to where the music starts and press New cue.",
                         r.withHeight(80), juce::Justification::centred, 3);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        // Top bar: production row + filter row.
        top_area_ = r.removeFromTop(92);
        auto top = top_area_.reduced(12, 8);
        auto row1 = top.removeFromTop(38);
        production_caption_.setBounds(row1.removeFromLeft(84).withTrimmedTop(10).withTrimmedBottom(8));
        add_scene_.setBounds(row1.removeFromRight(82).reduced(0, 5));
        row1.removeFromRight(6);
        add_reel_.setBounds(row1.removeFromRight(76).reduced(0, 5));
        row1.removeFromRight(14);
        const int pw = row1.getWidth();
        production_title_.setBounds(row1.removeFromLeft(pw * 46 / 100).reduced(0, 4));
        row1.removeFromLeft(8);
        production_director_.setBounds(row1.removeFromLeft((row1.getWidth() - 8) / 2).reduced(0, 4));
        row1.removeFromLeft(8);
        production_composer_.setBounds(row1.reduced(0, 4));
        top.removeFromTop(6);
        auto row2 = top.removeFromTop(30);
        status_filter_.setBounds(row2.removeFromLeft(160));
        row2.removeFromLeft(8);
        reel_filter_.setBounds(row2.removeFromLeft(140));
        row2.removeFromLeft(8);
        search_.setBounds(row2.removeFromLeft(std::min(240, std::max(140, row2.getWidth() / 4))));
        row2.removeFromLeft(10);
        overdue_only_.setBounds(row2.removeFromLeft(118));
        row2.removeFromLeft(10);
        chips_.setBounds(row2);

        // Bottom bar: naming + preview, then exports and renders.
        bottom_area_ = r.removeFromBottom(84);
        auto bottom = bottom_area_.reduced(12, 8);
        auto b1 = bottom.removeFromTop(30);
        preset_caption_.setBounds(b1.removeFromLeft(104));
        preset_.setBounds(b1.removeFromLeft(std::min(300, b1.getWidth() / 3)));
        b1.removeFromLeft(8);
        bit_depth_.setBounds(b1.removeFromLeft(84));
        b1.removeFromLeft(12);
        preview_.setBounds(b1);
        bottom.removeFromTop(8);
        auto b2 = bottom.removeFromTop(30);
        export_csv_.setBounds(b2.removeFromLeft(186));
        b2.removeFromLeft(8);
        export_notes_.setBounds(b2.removeFromLeft(140));
        render_approved_.setBounds(b2.removeFromRight(180));
        b2.removeFromRight(8);
        render_selected_.setBounds(b2.removeFromRight(150));
        b2.removeFromRight(12);
        b2.removeFromLeft(12);
        message_.setBounds(b2);

        // Detail editor on the right.
        const int dw = juce::jlimit(320, 440, r.getWidth() * 30 / 100);
        detail_.setBounds(r.removeFromRight(dw));
        editor_.setSize(dw - detail_.getScrollBarThickness(), editor_.content_height());

        // Actions above the dashboard.
        auto list_area = r.reduced(12, 10);
        auto actions = list_area.removeFromTop(30);
        new_cue_.setBounds(actions.removeFromLeft(96));
        actions.removeFromLeft(6);
        duplicate_.setBounds(actions.removeFromLeft(90));
        actions.removeFromLeft(6);
        delete_.setBounds(actions.removeFromLeft(74));
        actions.removeFromLeft(6);
        goto_.setBounds(actions.removeFromLeft(86));
        shown_count_.setBounds(actions);
        shown_count_.setVisible(actions.getWidth() >= 110);
        list_area.removeFromTop(8);
        // Scene and Composer give way first when the dashboard is narrow (the
        // header's right-click menu still shows or hides any column).
        const int tw = list_area.getWidth();
        if (tw != last_table_width_) {
            auto& header = table_.getHeader();
            header.setColumnVisible(colComposer, tw >= 1000);
            header.setColumnVisible(colScene, tw >= 900);
            last_table_width_ = tw;
        }
        table_.setBounds(list_area);
    }

private:
    // --- Refresh -------------------------------------------------------------------------------

    void handleAsyncUpdate() override { refresh_all(); }

    void refresh_all()
    {
        const auto& d = context_.session.data();
        auto set_field = [](juce::TextEditor& e, const std::string& v) {
            if (!e.hasKeyboardFocus(true) && e.getText() != utf8(v)) e.setText(utf8(v), false);
        };
        set_field(production_title_, d.production.title);
        set_field(production_director_, d.production.director);
        set_field(production_composer_, d.production.composer);

        // Reel filter: keep the chosen reel while it exists.
        const std::string chosen = reel_filter_id();
        reel_filter_ids_.assign(1, std::string());
        reel_filter_.clear(juce::dontSendNotification);
        reel_filter_.addItem("All reels", 1);
        int sel = 1;
        for (const auto& reel : d.reels) {
            reel_filter_ids_.push_back(reel.id);
            reel_filter_.addItem(utf8(reel.name), static_cast<int>(reel_filter_ids_.size()));
            if (reel.id == chosen) sel = static_cast<int>(reel_filter_ids_.size());
        }
        reel_filter_.setSelectedId(sel, juce::dontSendNotification);
        editor_.refresh();
        // The selected cue came back (undo of a delete, reopened file): show it again.
        if (editor_.cue_id().empty() && !selected_id_.empty() && d.find_cue(selected_id_)) editor_.show(selected_id_);
        rebuild();
    }

    std::string reel_filter_id() const
    {
        const auto idx = static_cast<std::size_t>(std::max(0, reel_filter_.getSelectedId() - 1));
        return idx < reel_filter_ids_.size() ? reel_filter_ids_[idx] : std::string();
    }

    std::string status_filter() const
    {
        const int id = status_filter_.getSelectedId();
        return id >= 2 ? project::cue_statuses()[static_cast<std::size_t>(id - 2)] : std::string();
    }

    void rebuild()
    {
        const auto& d = context_.session.data();
        cues::CueFilter filter;
        filter.status = status_filter();
        filter.reel_id = reel_filter_id();
        filter.query = trimmed(search_.getText());
        filter.overdue_only = overdue_only_.getToggleState();
        filter.today = today_iso();

        std::map<std::string, juce::StringArray> overlapping;
        for (const auto& [a, b] : cues::overlaps(d)) {
            const auto* ca = d.find_cue(a);
            const auto* cb = d.find_cue(b);
            if (ca && cb) {
                overlapping[a].add(utf8(cb->code));
                overlapping[b].add(utf8(ca->code));
            }
        }
        auto reel_of = [&](const std::string& id, int* index) {
            for (std::size_t i = 0; i < d.reels.size(); ++i)
                if (d.reels[i].id == id) {
                    *index = static_cast<int>(i);
                    return utf8(d.reels[i].name);
                }
            *index = static_cast<int>(d.reels.size());
            return juce::String();
        };
        auto scene_of = [&](const std::string& id) {
            for (const auto& s : d.scenes)
                if (s.id == id) return utf8(s.name);
            return juce::String();
        };

        rows_.clear();
        for (const auto* c : cues::list(d, filter)) {
            Row row;
            row.id = c->id;
            row.code = utf8(c->code);
            row.title = utf8(c->title);
            row.reel = reel_of(c->reel_id, &row.reel_index);
            row.scene = scene_of(c->scene_id);
            const auto times = cues::times_of(*c, d);
            row.in_tc = utf8(times.in_tc);
            row.out_tc = utf8(times.out_tc);
            row.duration = utf8(times.duration);
            row.seconds = times.end_seconds - times.start_seconds;
            row.start = c->start;
            row.end = c->end;
            row.status = c->status;
            row.revision = c->revision;
            row.deadline = utf8(c->deadline);
            row.overdue = cues::overdue(*c, filter.today);
            row.composer = utf8(c->composer);
            row.drift = cues::spot_drift_frames(*c, d);
            if (auto it = overlapping.find(c->id); it != overlapping.end()) row.overlaps = it->second;
            rows_.push_back(std::move(row));
        }
        sort_rows();

        table_.updateContent();
        {
            const juce::ScopedValueSetter<bool> guard(updating_selection_, true);
            juce::SparseSet<int> keep;
            for (std::size_t i = 0; i < rows_.size(); ++i)
                if (std::find(multi_selection_.begin(), multi_selection_.end(), rows_[i].id) != multi_selection_.end() ||
                    rows_[i].id == selected_id_)
                    keep.addRange({static_cast<int>(i), static_cast<int>(i) + 1});
            table_.setSelectedRows(keep, juce::dontSendNotification);
        }
        table_.repaint();

        chips_.set(d, filter.today);
        shown_count_.setText(juce::String(rows_.size()) + " of " + juce::String(d.cues.size()) + " cues shown",
                             juce::dontSendNotification);
        const auto* sel = d.find_cue(selected_id_);
        duplicate_.setEnabled(sel != nullptr);
        delete_.setEnabled(sel != nullptr);
        goto_.setEnabled(sel != nullptr);
        render_selected_.setEnabled(!selected_ids().empty());
        bool any_approved = false;
        for (const auto& c : d.cues) any_approved = any_approved || c.status == "approved" || c.status == "locked";
        render_approved_.setEnabled(any_approved);
        add_scene_.setEnabled(!d.reels.empty());
        update_preview();
        repaint();
    }

    void sort_rows()
    {
        if (sort_column_ == 0) return;
        auto key_less = [this](const Row& a, const Row& b) {
            switch (sort_column_) {
                case colCode: return a.reel_index != b.reel_index ? a.reel_index < b.reel_index
                                                                  : natural_less(str(a.code), str(b.code));
                case colTitle: return a.title.compareIgnoreCase(b.title) < 0;
                case colReel: return a.reel_index < b.reel_index;
                case colScene: return a.scene.compareIgnoreCase(b.scene) < 0;
                case colIn: return a.start < b.start;
                case colOut: return a.end < b.end;
                case colDur: return a.seconds < b.seconds;
                case colStatus: return status_index(a.status) < status_index(b.status);
                case colRev: return a.revision < b.revision;
                case colDeadline:
                    if (a.deadline.isEmpty() != b.deadline.isEmpty()) return b.deadline.isEmpty();
                    return a.deadline < b.deadline;
                case colComposer: return a.composer.compareIgnoreCase(b.composer) < 0;
                case colDrift: {
                    const auto da = a.drift ? std::llabs(*a.drift) : -1, db = b.drift ? std::llabs(*b.drift) : -1;
                    return da < db;
                }
                default: return false;
            }
        };
        std::stable_sort(rows_.begin(), rows_.end(), [&](const Row& a, const Row& b) {
            return sort_forwards_ ? key_less(a, b) : key_less(b, a);
        });
    }

    // --- Table model -----------------------------------------------------------------------------

    int getNumRows() override { return static_cast<int>(rows_.size()); }

    void paintRowBackground(juce::Graphics& g, int row, int width, int height, bool selected) override
    {
        auto bg = (row % 2 == 0) ? colours::panel : colours::panel.brighter(0.045f);
        g.fillAll(bg);
        if (selected) {
            g.fillAll(colours::accent.withAlpha(0.22f));
            g.setColour(colours::accent);
            g.fillRect(0, 0, 3, height);
        }
        g.setColour(colours::outline.withAlpha(0.45f));
        g.fillRect(0, height - 1, width, 1);
    }

    void paintCell(juce::Graphics& g, int row_number, int column, int width, int height, bool) override
    {
        if (row_number < 0 || row_number >= static_cast<int>(rows_.size())) return;
        const Row& row = rows_[static_cast<std::size_t>(row_number)];
        auto r = juce::Rectangle<int>(0, 0, width, height).reduced(7, 0);
        auto text = [&](const juce::String& t, juce::Colour c, float size = 13.5f, bool bold = false,
                        juce::Justification just = juce::Justification::centredLeft) {
            g.setColour(c);
            g.setFont(juce::FontOptions(size, bold ? juce::Font::bold : juce::Font::plain));
            g.drawText(t, r, just, true);
        };
        auto mono = [&](const juce::String& t, juce::Colour c) {
            g.setColour(c);
            g.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
            g.drawText(t, r.expanded(2, 0), juce::Justification::centredLeft, true);
        };
        switch (column) {
            case colCode: text(row.code, colours::text, 13.5f, true); break;
            case colTitle: {
                if (!row.overlaps.isEmpty()) {
                    const juce::Font f(juce::FontOptions(10.5f, juce::Font::bold));
                    const float tw = juce::GlyphArrangement::getStringWidth(f, "overlap") + 12.0f;
                    if (static_cast<float>(r.getWidth()) > tw + 40.0f) {
                        auto tag = r.removeFromRight(static_cast<int>(tw)).toFloat().withSizeKeepingCentre(tw, 17.0f);
                        g.setColour(colours::accent_warm.withAlpha(0.15f));
                        g.fillRoundedRectangle(tag, 8.0f);
                        g.setColour(colours::accent_warm.withAlpha(0.8f));
                        g.drawRoundedRectangle(tag.reduced(0.5f), 8.0f, 1.0f);
                        g.setFont(f);
                        g.drawText("overlap", tag, juce::Justification::centred);
                        r.removeFromRight(4);
                    }
                }
                text(row.title, colours::text);
                break;
            }
            case colReel: text(row.reel, colours::text_dim, 13.0f); break;
            case colScene: text(row.scene, colours::text_dim, 13.0f); break;
            case colIn: mono(row.in_tc, colours::text); break;
            case colOut: mono(row.out_tc, colours::text); break;
            case colDur: mono(row.duration, colours::text_dim); break;
            case colStatus: draw_status_pill(g, r.toFloat(), row.status, 11.5f); break;
            case colRev: text("v" + juce::String(row.revision), colours::text_dim, 13.0f); break;
            case colDeadline:
                if (row.overdue) {
                    g.setColour(colours::missing.withAlpha(0.16f));
                    g.fillRoundedRectangle(r.toFloat().withSizeKeepingCentre(static_cast<float>(r.getWidth()) + 6.0f, 20.0f), 4.0f);
                }
                text(row.deadline, row.overdue ? colours::missing.brighter(0.2f) : colours::text, 13.0f, row.overdue);
                break;
            case colComposer: text(row.composer, colours::text_dim, 13.0f); break;
            case colDrift:
                if (row.drift && *row.drift != 0)
                    text((*row.drift > 0 ? "+" : kMinus) + juce::String(std::llabs(*row.drift)) + " fr", colours::accent_warm,
                         12.5f, true);
                break;
            default: break;
        }
    }

    juce::String getCellTooltip(int row_number, int column) override
    {
        if (row_number < 0 || row_number >= static_cast<int>(rows_.size())) return {};
        const Row& row = rows_[static_cast<std::size_t>(row_number)];
        if (column == colTitle && !row.overlaps.isEmpty())
            return "Overlaps " + row.overlaps.joinIntoString(", ") + " (allowed for transitions)";
        if (column == colDrift && row.drift && *row.drift != 0)
            return "The music starts " + juce::String(std::llabs(*row.drift)) + " frames " +
                   (*row.drift > 0 ? "after" : "before") + " where the cue was spotted";
        if (column == colDeadline && row.overdue) return "Past its deadline and not yet approved";
        return {};
    }

    void cellDoubleClicked(int row_number, int, const juce::MouseEvent&) override
    {
        if (row_number >= 0 && row_number < static_cast<int>(rows_.size())) go_to(rows_[static_cast<std::size_t>(row_number)].id);
    }

    void returnKeyPressed(int last_row) override
    {
        if (last_row >= 0 && last_row < static_cast<int>(rows_.size())) go_to(rows_[static_cast<std::size_t>(last_row)].id);
    }

    void deleteKeyPressed(int) override { ask_delete(); }

    void sortOrderChanged(int column, bool forwards) override
    {
        sort_column_ = column;
        sort_forwards_ = forwards;
        rebuild();
    }

    void selectedRowsChanged(int last_row) override
    {
        if (updating_selection_) return;
        multi_selection_.clear();
        const auto set = table_.getSelectedRows();
        for (int i = 0; i < set.size(); ++i)
            if (const int r = set[i]; r >= 0 && r < static_cast<int>(rows_.size()))
                multi_selection_.push_back(rows_[static_cast<std::size_t>(r)].id);
        if (last_row >= 0 && last_row < static_cast<int>(rows_.size())) {
            selected_id_ = rows_[static_cast<std::size_t>(last_row)].id;
            editor_.show(selected_id_);
            detail_.setViewPosition(0, 0);
        }
        const auto* sel = context_.session.data().find_cue(selected_id_);
        duplicate_.setEnabled(sel != nullptr);
        delete_.setEnabled(sel != nullptr);
        goto_.setEnabled(sel != nullptr);
        render_selected_.setEnabled(!selected_ids().empty());
        update_preview();
    }

    // --- Selection and actions ---------------------------------------------------------------------

    std::vector<std::string> selected_ids() const
    {
        std::vector<std::string> ids;
        for (const auto& row : rows_)
            if (row.id == selected_id_ ||
                std::find(multi_selection_.begin(), multi_selection_.end(), row.id) != multi_selection_.end())
                ids.push_back(row.id);
        if (ids.empty() && !selected_id_.empty() && context_.session.data().find_cue(selected_id_))
            ids.push_back(selected_id_);
        return ids;
    }

    void select_cue(const std::string& id)
    {
        selected_id_ = id;
        multi_selection_.assign(1, id);
        for (std::size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].id == id) {
                const juce::ScopedValueSetter<bool> guard(updating_selection_, true);
                table_.selectRow(static_cast<int>(i));
                table_.scrollToEnsureRowIsOnscreen(static_cast<int>(i));
            }
        editor_.show(id);
        rebuild();
    }

    void say(const juce::String& text) { set_status(context_, &message_, text); }

    void go_to(const std::string& id)
    {
        const auto& d = context_.session.data();
        const auto* c = d.find_cue(id);
        if (!c) return;
        context_.engine.locate(c->start);
        say("Playhead at " + utf8(c->code) + " \"" + utf8(c->title) + "\" (bar " + juce::String(d.meter.bar_at(c->start)) +
            ", " + utf8(cues::times_of(*c, d).in_tc) + ")");
    }

    void new_cue()
    {
        editor_.commit_pending();
        const auto& d = context_.session.data();
        const Ticks pos = context_.engine.position();
        const int bar = d.meter.bar_at(pos);
        const Ticks start = d.meter.bar_start(bar);
        const Ticks end = start + d.meter.span(bar, 8);
        std::string reel = reel_filter_id();
        if (reel.empty() && !d.reels.empty()) {
            reel = d.reels.front().id;
            if (const auto* sel = d.find_cue(selected_id_); sel && !sel->reel_id.empty()) reel = sel->reel_id;
        }
        const std::string id = context_.session.add_cue(start, end, reel);
        if (id.empty() || !context_.session.data().find_cue(id)) {
            say("The cue could not be added.");
            return;
        }
        // Make sure the new cue is visible.
        status_filter_.setSelectedId(1, juce::dontSendNotification);
        overdue_only_.setToggleState(false, juce::dontSendNotification);
        search_.setText({}, false);
        refresh_all();
        select_cue(id);
        const auto* c = context_.session.data().find_cue(id);
        say("New cue " + utf8(c->code) + " at bar " + juce::String(bar) + " (8 bars) - give it a title");
    }

    void duplicate_cue()
    {
        editor_.commit_pending();
        const auto* src = context_.session.data().find_cue(selected_id_);
        if (!src) return;
        const project::Cue copy = *src;
        const std::string id = context_.session.add_cue(copy.start, copy.end, copy.reel_id, copy.title + " (copy)");
        if (id.empty()) return;
        context_.session.update_cue(
            id,
            [&](project::Cue& c) {
                c.description = copy.description;
                c.notes = copy.notes;
                c.scene_id = copy.scene_id;
                c.spot_in = copy.spot_in;
                c.spot_out = copy.spot_out;
                c.deadline = copy.deadline;
                c.composer = copy.composer;
                c.template_id = copy.template_id;
                c.usage = copy.usage;
                c.transition_in = copy.transition_in;
                c.transition_out = copy.transition_out;
                c.deliverables = copy.deliverables;
                for (auto& dl : c.deliverables) dl.done = false;
                c.status = (copy.status == "approved" || copy.status == "locked") ? "writing" : copy.status;
            },
            "Duplicate cue " + copy.code);
        refresh_all();
        select_cue(id);
        if (const auto* c = context_.session.data().find_cue(id))
            say("Duplicated " + utf8(copy.code) + " as " + utf8(c->code));
    }

    void ask_delete()
    {
        const auto* c = context_.session.data().find_cue(selected_id_);
        if (!c) return;
        const juce::String name = utf8(c->code) + " \"" + utf8(c->title) + "\"";
        if (c->locked()) {
            say(name + " is locked and cannot be deleted. Unlock it first.");
            juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                             .withIconType(juce::MessageBoxIconType::InfoIcon)
                                             .withTitle("Cue is locked")
                                             .withMessage(name + " is locked and cannot be deleted.\nUnlock it first.")
                                             .withButton("OK")
                                             .withAssociatedComponent(this),
                                         [](int) {});
            return;
        }
        juce::Component::SafePointer<CueManager> safe(this);
        const std::string id = c->id;
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                         .withIconType(juce::MessageBoxIconType::QuestionIcon)
                                         .withTitle("Delete cue?")
                                         .withMessage("Delete " + name + "?\nIts music stays on the timeline; Undo restores the cue.")
                                         .withButton("Delete")
                                         .withButton("Cancel")
                                         .withAssociatedComponent(this),
                                     [safe, id, name](int result) {
                                         if (safe == nullptr || result != 1) return;
                                         if (safe->context_.session.remove_cue(id)) safe->say("Deleted " + name);
                                         else safe->say(name + " could not be deleted.");
                                     });
    }

    void commit_production()
    {
        const auto& d = context_.session.data();
        project::Production p = d.production;
        p.title = trimmed(production_title_.getText());
        p.director = trimmed(production_director_.getText());
        p.composer = trimmed(production_composer_.getText());
        if (p.title == d.production.title && p.director == d.production.director && p.composer == d.production.composer)
            return;
        if (context_.session.set_production(p)) say("Production details updated");
    }

    void ask_name(const juce::String& title, const juce::String& message, const juce::String& initial,
                  std::function<void(const std::string&)> done)
    {
        auto* w = new juce::AlertWindow(title, message, juce::MessageBoxIconType::NoIcon, this);
        w->addTextEditor("name", initial);
        w->addButton("Add", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<juce::AlertWindow> sw(w);
        juce::Component::SafePointer<CueManager> safe(this);
        w->enterModalState(true, juce::ModalCallbackFunction::create([sw, safe, done = std::move(done)](int result) {
                               if (result != 1 || sw == nullptr || safe == nullptr) return;
                               const auto name = trimmed(sw->getTextEditorContents("name"));
                               if (!name.empty()) done(name);
                           }),
                           true);
    }

    void ask_add_reel()
    {
        const auto n = context_.session.data().reels.size() + 1;
        ask_name("Add reel", "Name of the new reel or episode:", "Reel " + juce::String(n), [this](const std::string& name) {
            context_.session.add_reel(name);
            say("Added " + utf8(name));
        });
    }

    void ask_add_scene()
    {
        const auto& d = context_.session.data();
        std::string reel = reel_filter_id();
        if (reel.empty())
            if (const auto* c = d.find_cue(selected_id_)) reel = c->reel_id;
        if (reel.empty() && !d.reels.empty()) reel = d.reels.front().id;
        if (reel.empty()) {
            say("Add a reel first; scenes belong to a reel.");
            return;
        }
        juce::String reel_name;
        int count = 0;
        for (const auto& r : d.reels)
            if (r.id == reel) reel_name = utf8(r.name);
        for (const auto& s : d.scenes) count += s.reel_id == reel ? 1 : 0;
        ask_name("Add scene", "New scene in " + reel_name + ":", "Sc. " + juce::String(count + 1),
                 [this, reel, reel_name](const std::string& name) {
                     context_.session.add_scene(reel, name);
                     say("Added " + utf8(name) + " to " + reel_name);
                 });
    }

    // --- Exports ---------------------------------------------------------------------------------

    juce::File default_folder() const
    {
        const auto& path = context_.session.path();
        if (!path.empty()) return juce::File(utf8(path)).getParentDirectory();
        return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    }

    juce::String production_name() const
    {
        const auto& d = context_.session.data();
        auto name = utf8(d.production.title.empty() ? d.name : d.production.title);
        return juce::File::createLegalFileName(name.isEmpty() ? juce::String("Cues") : name);
    }

    void export_text(bool csv)
    {
        editor_.commit_pending();
        const auto file = default_folder().getChildFile(production_name() + (csv ? " cue sheet.csv" : " spotting notes.txt"));
        chooser_ = std::make_unique<juce::FileChooser>(csv ? "Export cue sheet" : "Export spotting notes", file,
                                                       csv ? "*.csv" : "*.txt");
        juce::Component::SafePointer<CueManager> safe(this);
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                                  juce::FileBrowserComponent::warnAboutOverwriting,
                              [safe, csv](const juce::FileChooser& fc) {
                                  if (safe == nullptr) return;
                                  auto target = fc.getResult();
                                  if (target == juce::File()) return;
                                  if (target.getFileExtension().isEmpty()) target = target.withFileExtension(csv ? "csv" : "txt");
                                  const auto& d = safe->context_.session.data();
                                  const std::string body = csv ? cues::cue_sheet_csv(d) : cues::spotting_notes(d);
                                  std::string error;
                                  if (files::write_file_atomic(str(target.getFullPathName()), body, &error))
                                      safe->say((csv ? "Cue sheet written to " : "Spotting notes written to ") +
                                                target.getFileName());
                                  else {
                                      safe->say("Export failed: " + utf8(error));
                                      juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                                                       .withIconType(juce::MessageBoxIconType::WarningIcon)
                                                                       .withTitle("Export failed")
                                                                       .withMessage(utf8(error))
                                                                       .withButton("OK"),
                                                                   [](int) {});
                                  }
                              });
    }

    // --- Rendering ---------------------------------------------------------------------------------

    std::string preset() const
    {
        const auto text = trimmed(preset_.getText());
        return text.empty() ? naming_presets().front() : text;
    }

    void update_preview()
    {
        const auto& d = context_.session.data();
        const auto* c = d.find_cue(selected_id_);
        if (!c && !rows_.empty()) c = d.find_cue(rows_.front().id);
        if (!c) {
            preview_.setText("(select a cue to preview its file name)", juce::dontSendNotification);
            return;
        }
        preview_.setText(juce::String::fromUTF8("\xe2\x86\x92 ") + utf8(cues::render_name(preset(), *c, d, today_iso())) +
                             ".wav",
                         juce::dontSendNotification);
    }

    void render(std::vector<std::string> ids, const juce::String& what)
    {
        editor_.commit_pending();
        if (ids.empty()) {
            say(what == "approved" ? "No approved or locked cues to render." : "Select one or more cues to render.");
            return;
        }
        chooser_ = std::make_unique<juce::FileChooser>("Render " + juce::String(ids.size()) + " " + what +
                                                           (ids.size() == 1 ? " cue" : " cues") + " to folder",
                                                       default_folder());
        juce::Component::SafePointer<CueManager> safe(this);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe, ids = std::move(ids)](const juce::FileChooser& fc) {
                                  if (safe == nullptr) return;
                                  const auto folder = fc.getResult();
                                  if (folder == juce::File()) return;
                                  safe->run_render(ids, folder);
                              });
    }

    void run_render(const std::vector<std::string>& ids, const juce::File& folder)
    {
        if (!folder.isDirectory() && !folder.createDirectory()) {
            say("Cannot create the folder " + folder.getFullPathName());
            return;
        }
        CueRenderRequest request;
        request.cue_ids = ids;
        request.folder = folder;
        request.name_preset = preset();
        request.sample_rate = context_.session.data().sample_rate > 0 ? context_.session.data().sample_rate : 48000;
        request.bit_depth = bit_depth_.getSelectedId() == 16 ? 16 : 24;
        say("Rendering " + juce::String(ids.size()) + (ids.size() == 1 ? " cue..." : " cues..."));
        auto results = render_cues(context_, request);

        std::vector<std::string> protected_ids;
        juce::StringArray protected_names;
        for (const auto& r : results)
            if (r.skipped_protected) {
                protected_ids.push_back(r.cue_id);
                protected_names.add(r.file.getFileName());
            }
        if (protected_ids.empty()) {
            show_summary(results, folder);
            return;
        }
        juce::Component::SafePointer<CueManager> safe(this);
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle("Overwrite approved deliverables?")
                .withMessage(juce::String(protected_ids.size()) +
                             (protected_ids.size() == 1 ? " approved or locked cue already has" : " approved or locked cues already have") +
                             " a delivered file:\n\n" + protected_names.joinIntoString("\n") +
                             "\n\nOverwrite them with new bounces? Other cues were rendered either way.")
                .withButton("Overwrite")
                .withButton("Keep existing files")
                .withAssociatedComponent(this),
            [safe, request, results, protected_ids](int answer) mutable {
                if (safe == nullptr) return;
                if (answer == 1) {
                    request.cue_ids = protected_ids;
                    request.overwrite_protected = true;
                    const auto again = render_cues(safe->context_, request);
                    for (const auto& r : again)
                        for (auto& old : results)
                            if (old.cue_id == r.cue_id) old = r;
                }
                safe->show_summary(results, request.folder);
            });
    }

    void show_summary(const std::vector<CueRenderResult>& results, const juce::File& folder)
    {
        const auto& d = context_.session.data();
        int ok = 0, silent = 0, clipping = 0, skipped = 0, failed = 0;
        juce::StringArray lines;
        for (const auto& r : results) {
            const auto* c = d.find_cue(r.cue_id);
            const juce::String code = c ? utf8(c->code) : utf8(r.cue_id);
            juce::String line = code + "   " + r.file.getFileName();
            if (r.skipped_protected) {
                ++skipped;
                line << "   - kept existing (approved deliverable)";
            }
            else if (!r.ok) {
                ++failed;
                line << "   - FAILED" << (r.message.isNotEmpty() ? ": " + r.message : juce::String());
            }
            else {
                ++ok;
                const float db = r.peak > 0.0f ? juce::Decibels::gainToDecibels(r.peak) : -144.0f;
                line << "   (peak " << (r.peak > 0.0f ? juce::String(db, 1) + " dBFS" : juce::String("-inf")) << ")";
                if (r.peak < 1e-4f) {
                    ++silent;
                    line << "   SILENT - check the cue range and track mutes";
                }
                else if (r.peak > 1.0f) {
                    ++clipping;
                    line << "   CLIPPING - lower the mix";
                }
            }
            lines.add(line);
        }
        juce::String head = juce::String(ok) + " of " + juce::String(results.size()) + " cues rendered to " +
                            folder.getFullPathName();
        if (silent) head << "\n" << silent << (silent == 1 ? " bounce is" : " bounces are") << " silent.";
        if (clipping) head << "\n" << clipping << (clipping == 1 ? " bounce clips" : " bounces clip") << " (peak above 0 dBFS).";
        if (skipped) head << "\n" << skipped << " approved deliverable(s) kept unchanged.";
        if (failed) head << "\n" << failed << " failed.";
        say(juce::String(ok) + " cue(s) rendered" + (silent ? ", " + juce::String(silent) + " silent" : juce::String()) +
            (clipping ? ", " + juce::String(clipping) + " clipping" : juce::String()) +
            (failed ? ", " + juce::String(failed) + " failed" : juce::String()));
        const bool warn = silent || clipping || failed;
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                         .withIconType(warn ? juce::MessageBoxIconType::WarningIcon
                                                            : juce::MessageBoxIconType::InfoIcon)
                                         .withTitle("Render by cue")
                                         .withMessage(head + "\n\n" + lines.joinIntoString("\n"))
                                         .withButton("OK")
                                         .withAssociatedComponent(this),
                                     [](int) {});
    }

    AppContext& context_;
    int listener_ = 0;
    juce::Rectangle<int> top_area_, bottom_area_;

    juce::Label production_caption_;
    juce::TextEditor production_title_, production_director_, production_composer_;
    juce::TextButton add_reel_{"+ Reel"}, add_scene_{"+ Scene"};
    juce::ComboBox status_filter_, reel_filter_;
    std::vector<std::string> reel_filter_ids_{std::string()};
    juce::TextEditor search_;
    juce::ToggleButton overdue_only_{"Overdue only"};
    StatusChips chips_;

    juce::TextButton new_cue_{"New cue"}, duplicate_{"Duplicate"}, delete_{"Delete"}, goto_{"Go to cue"};
    juce::Label shown_count_;
    juce::TableListBox table_{"cues"};
    juce::Label message_;
    juce::Viewport detail_;
    CueEditor editor_;

    juce::Label preset_caption_, preview_;
    juce::ComboBox preset_, bit_depth_;
    juce::TextButton export_csv_{"Export cue sheet (CSV)..."}, export_notes_{"Spotting notes..."},
        render_selected_{"Render selected..."}, render_approved_{"Render all approved..."};
    std::unique_ptr<juce::FileChooser> chooser_;

    std::vector<Row> rows_;
    std::string selected_id_;
    std::vector<std::string> multi_selection_;
    int sort_column_ = 0;
    int last_table_width_ = -1;
    bool sort_forwards_ = true;
    bool updating_selection_ = false;
};

juce::Component::SafePointer<dialogs::ToolWindow> cue_window;

}  // namespace

void show_cue_manager(AppContext& context)
{
    dialogs::show_tool(
        cue_window, "Cue Manager", 1180, 720, [&context] { return new CueManager(context); },
        [] {
            if (cue_window != nullptr)
                if (auto* manager = dynamic_cast<CueManager*>(cue_window->getContentComponent())) manager->detach();
        });
}

}  // namespace vcc::desktop
