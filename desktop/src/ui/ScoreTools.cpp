#include "ui/ScoreTools.h"

#include <algorithm>

namespace vcc::desktop {

namespace {

std::pair<Ticks, Ticks> range_or_song(const AppContext& c)
{
    const auto& sel = c.session.selection();
    if (sel.has_range()) return {sel.from, sel.to};
    return {0, std::max<Ticks>(c.session.content_end(), kTicksPerWhole)};
}

juce::AlertWindow* dialog(const juce::String& title, const juce::String& message)
{
    auto* w = new juce::AlertWindow(title, message, juce::MessageBoxIconType::NoIcon);
    w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    return w;
}

}  // namespace

void ask_lyrics(AppContext& c)
{
    const auto& sel = c.session.selection();
    const auto* t = c.session.data().find_track(sel.track);
    if (!t || !t->active_version()) {
        c.status("Select a track (and its notes) to add lyrics");
        return;
    }
    std::vector<notation::EventId> notes = sel.notes;
    if (notes.empty()) {
        const auto [from, to] = range_or_song(c);
        const auto& sc = t->active_version()->score;
        for (std::size_t s = 0; s < sc.staff_count(); ++s)
            for (const auto& n : sc.notes(s))
                if (n.start_tick >= from && n.start_tick < to) notes.push_back(n.id);
    }
    auto* w = dialog("Lyrics", "One syllable per note, in order. Hyphens split a word (Hal-le-lu-jah); "
                               "_ holds a syllable over a note. Leave empty to clear.");
    w->addTextEditor("text", "", "Lyrics");
    const std::string track = sel.track;
    w->enterModalState(true, juce::ModalCallbackFunction::create([&c, w, track, notes](int r) {
                           if (r != 1) return;
                           const int n = c.session.set_lyrics(track, notes, w->getTextEditorContents("text").toStdString());
                           c.status(n > 0 ? "Lyrics on " + juce::String(n) + " note(s)" : juce::String("Lyrics cleared"));
                       }),
                       true);
}

void ask_explode(AppContext& c)
{
    const auto track = c.session.selected_track();
    if (!c.session.data().find_track(track)) {
        c.status("Select the track whose chords to explode");
        return;
    }
    auto* w = dialog("Explode", "Split the chords of the selected range (or the whole track) into new tracks, top note first.");
    w->addTextEditor("parts", "3", "Parts");
    w->enterModalState(true, juce::ModalCallbackFunction::create([&c, w, track](int r) {
                           if (r != 1) return;
                           const int parts = w->getTextEditorContents("parts").getIntValue();
                           const auto [from, to] = range_or_song(c);
                           const auto ids = c.session.explode(track, from, to, std::clamp(parts, 2, 16));
                           c.status(ids.empty() ? juce::String("Nothing to explode there")
                                                : "Exploded into " + juce::String(static_cast<int>(ids.size())) + " tracks");
                       }),
                       true);
}

juce::String spread_to_section(AppContext& c, const std::string& source, session::Session::Section section)
{
    using S = session::Session::Section;
    const auto targets = c.session.section_tracks(section);
    const char* const strings[] = {"violins I", "violins II", "violas", "celli", "basses"};
    const char* const winds[] = {"flute", "oboe", "clarinet", "bassoon"};
    const char* const brass[] = {"trumpet", "horn", "trombone", "tuba"};
    const char* const choir[] = {"soprano", "alto", "tenor", "bass"};
    const char* const* names = section == S::Strings ? strings : section == S::Woodwinds ? winds : section == S::Brass ? brass : choir;
    const juce::String what = section == S::Strings     ? "strings"
                              : section == S::Woodwinds ? "woodwinds"
                              : section == S::Brass     ? "brass"
                                                        : "choir";
    if (std::all_of(targets.begin(), targets.end(), [](const std::string& t) { return t.empty(); }))
        return "No " + what + " in this project to spread to";
    const auto [from, to] = range_or_song(c);
    const int n = c.session.spread_chords(source, from, to, targets, section == S::Strings);
    if (n == 0) return "No chords there to spread (two or more notes at once)";
    juce::StringArray missing, order;
    for (std::size_t k = 0; k < targets.size(); ++k) {
        (targets[k].empty() ? missing : order).add(names[k]);
    }
    return "Spread " + juce::String(n) + (n == 1 ? " chord" : " chords") + " over the " + what +
           (section == S::Strings ? ", open: " : ": ") + "a note each, top down (" + order.joinIntoString(", ") + ")" +
           (missing.isEmpty() ? juce::String() : "; no " + missing.joinIntoString(", ") + " here");
}

juce::String spread_to_strings(AppContext& c, const std::string& source)
{
    return spread_to_section(c, source, session::Session::Section::Strings);
}

juce::PopupMenu sketch_menu(AppContext& c, const std::string& source)
{
    juce::PopupMenu m;
    const auto* src = c.session.data().find_track(source);
    if (!src) return m;
    using VP = session::Session::VoicePart;
    // A line of the chord to any instrument; every note (a chord) only to one that plays chords.
    const std::pair<VP, const char*> parts[] = {{VP::Top, "Send Top Line to"},       {VP::Second, "Send 2nd Line to"},
                                                {VP::Third, "Send 3rd Line to"},     {VP::Middle, "Send Middle to"},
                                                {VP::Bass, "Send Bass Line to"},     {VP::All, "Send All Notes (chords) to"}};
    for (const auto& [part, label] : parts) {
        juce::PopupMenu targets;
        for (const auto* t : c.session.music_tracks()) {
            if (t->id == source || t->articulations_expanded) continue;
            const bool chords = session::Session::plays_chords(t->instrument_standard_id);
            if (part == VP::All && !chords) continue;  // a single-line instrument never gets a chord
            const std::string id = t->id;
            const VP which = part;
            targets.addItem(juce::String(t->name), [&c, source, id, which] {
                const auto [from, to] = range_or_song(c);
                const int n = c.session.send_notes(source, from, to, which, id);
                const auto* target = c.session.data().find_track(id);
                const juce::String name(target ? target->name : std::string());
                c.status(n < 0    ? name + " plays one line: send it the top, 2nd, 3rd, middle or bass line"
                         : n == 0 ? juce::String("No notes of that kind there")
                                  : "Sent " + juce::String(n) + (n == 1 ? " note" : " notes") + " to " + name);
            });
        }
        if (targets.getNumItems() == 0) targets.addItem(part == VP::All ? "No chord instruments (piano, harp, guitar...)" : "No other tracks", false, false, [] {});
        m.addSubMenu(label, targets);
    }
    m.addSeparator();
    using S = session::Session::Section;
    const std::pair<S, const char*> sections[] = {{S::Strings, "Spread Chords to Strings (open, 5 parts)"},
                                                  {S::Woodwinds, "Spread Chords to Woodwinds (4 parts)"},
                                                  {S::Brass, "Spread Chords to Brass (4 parts)"},
                                                  {S::Choir, "Spread Chords to Choir (SATB)"}};
    for (const auto& [section, label] : sections) {
        const auto ids = c.session.section_tracks(section);
        const bool any = std::any_of(ids.begin(), ids.end(), [](const std::string& t) { return !t.empty(); });
        const S which = section;
        m.addItem(label, any, false, [&c, source, which] { c.status(spread_to_section(c, source, which)); });
    }
    return m;
}

void ask_implode(AppContext& c)
{
    const auto target = c.session.selected_track();
    if (!c.session.data().find_track(target)) {
        c.status("Select the track to implode onto");
        return;
    }
    std::vector<std::pair<std::string, std::string>> tracks;
    for (const auto& t : c.session.data().tracks)
        if (t.id != target && t.kind == project::TrackKind::Instrument) tracks.emplace_back(t.id, t.name);
    if (tracks.empty()) {
        c.status("No other instrument tracks to implode");
        return;
    }
    auto* w = dialog("Implode", "Onto the selected track as chords: the names of the tracks to combine, separated by commas.");
    juce::StringArray names;
    for (const auto& [id, name] : tracks) names.add(juce::String(name));
    w->addTextEditor("tracks", names.joinIntoString(", "), "Tracks");
    w->enterModalState(true, juce::ModalCallbackFunction::create([&c, w, target, tracks](int r) {
                           if (r != 1) return;
                           juce::StringArray wanted;
                           wanted.addTokens(w->getTextEditorContents("tracks"), ",", "");
                           wanted.trim();
                           std::vector<std::string> sources;
                           for (const auto& [id, name] : tracks)
                               if (wanted.contains(juce::String(name))) sources.push_back(id);
                           const auto [from, to] = range_or_song(c);
                           c.status(c.session.implode(sources, target, from, to) ? "Imploded " + juce::String(static_cast<int>(sources.size())) +
                                                                                       " tracks"
                                                                                 : juce::String("Nothing to implode there"));
                       }),
                       true);
}

void ask_repeat(AppContext& c)
{
    const auto& d = c.session.data();
    const auto& sel = c.session.selection();
    const int first = sel.has_range() ? d.meter.bar_at(sel.from) : 1;
    const int last = sel.has_range() ? d.meter.bar_at(std::max<Ticks>(sel.from, sel.to - 1)) : first + 3;
    auto* w = dialog("Repeat", "Bars to repeat; times played; and how many bars at the end are a first ending (0 for none: "
                               "the same number after them are the second ending).");
    w->addTextEditor("from", juce::String(first), "From bar");
    w->addTextEditor("to", juce::String(last), "To bar");
    w->addTextEditor("times", "2", "Times");
    w->addTextEditor("ending", "0", "Ending bars");
    w->enterModalState(true, juce::ModalCallbackFunction::create([&c, w](int r) {
                           if (r != 1) return;
                           notation::RepeatSection rep;
                           rep.start_bar = w->getTextEditorContents("from").getIntValue();
                           rep.end_bar = w->getTextEditorContents("to").getIntValue();
                           rep.times = w->getTextEditorContents("times").getIntValue();
                           rep.ending_bars = w->getTextEditorContents("ending").getIntValue();
                           auto all = c.session.data().repeats;
                           all.erase(std::remove_if(all.begin(), all.end(),
                                                    [&](const notation::RepeatSection& x) {
                                                        return x.start_bar <= rep.end_bar && rep.start_bar <= x.end_bar;
                                                    }),
                                     all.end());
                           if (rep.start_bar >= 1 && rep.end_bar >= rep.start_bar) all.push_back(rep);
                           c.status(c.session.set_repeats(all) ? "Repeat bars " + juce::String(rep.start_bar) + "-" +
                                                                     juce::String(rep.end_bar)
                                                               : juce::String("No change"));
                       }),
                       true);
}

juce::String unroll(AppContext& c)
{
    if (c.session.data().repeats.empty()) return "There are no repeats to unroll";
    const int added = c.session.unroll_repeats();
    return "Repeats written out: " + juce::String(added) + " bar(s) added (audio clips and markers stay where they were)";
}

}  // namespace vcc::desktop

namespace vcc::desktop {

std::string articulation_part(const AppContext& c, const std::string& track_id)
{
    const auto* t = c.session.data().find_track(track_id);
    if (!t) return {};
    return t->articulation_parent.empty() ? t->id : t->articulation_parent;
}

namespace {

/// One tick box per sound, with how many of the part's notes play it.
class SoundChoice final : public juce::Component {
public:
    explicit SoundChoice(const std::vector<session::Session::ArticulationSplit>& splits)
    {
        for (const auto& s : splits) {
            auto b = std::make_unique<juce::ToggleButton>(
                juce::String(s.name) + "  -  " +
                (s.notes > 0 ? juce::String(s.notes) + (s.notes == 1 ? " note" : " notes") : juce::String("no notes yet")));
            b->setToggleState(true, juce::dontSendNotification);
            b->setTitle(juce::String(s.name));
            addAndMakeVisible(*b);
            boxes_.push_back({s.group, std::move(b)});
        }
        setSize(380, static_cast<int>(boxes_.size()) * 26 + 4);
    }
    void resized() override
    {
        int y = 2;
        for (auto& [g, b] : boxes_) {
            b->setBounds(0, y, getWidth(), 24);
            y += 26;
        }
    }
    std::vector<std::string> chosen() const
    {
        std::vector<std::string> out;
        for (const auto& [g, b] : boxes_)
            if (b->getToggleState()) out.push_back(g);
        return out;
    }

private:
    std::vector<std::pair<std::string, std::unique_ptr<juce::ToggleButton>>> boxes_;
};

}  // namespace

void ask_expand_articulations(AppContext& c)
{
    const auto part = articulation_part(c, c.session.selected_track());
    const auto* t = c.session.data().find_track(part);
    if (!t || t->kind != project::TrackKind::Instrument) {
        c.status("Select the instrument track to expand");
        return;
    }
    if (t->articulations_expanded) {
        c.status("Already expanded: Consolidate to One Track first");
        return;
    }
    const auto splits = c.session.preview_articulation_tracks(part);
    if (splits.empty()) {
        c.status("This track has no articulation map to expand by");
        return;
    }
    auto* w = new juce::AlertWindow(
        "Expand Articulations to Tracks",
        juce::String(t->name) + ": a track per sound, each with the instrument, routing and mix of the part. "
                                "The notes stay in the part's score; each track plays the ones for its sound. "
                                "Each loads its own instrument, so this uses more memory.",
        juce::MessageBoxIconType::NoIcon);
    w->setComponentID("expand_articulations");
    auto choice = std::make_shared<SoundChoice>(splits);
    w->addCustomComponent(choice.get());
    w->addButton("Expand", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([&c, w, part, choice](int r) {
                           w->removeCustomComponent(0);
                           if (r != 1) return;
                           const auto groups = choice->chosen();
                           if (groups.empty()) {
                               c.status("No sounds chosen: nothing expanded");
                               return;
                           }
                           const auto ids = c.session.expand_articulations(part, groups);
                           c.status(ids.empty() ? juce::String("Nothing to expand")
                                                : "Expanded into " + juce::String(static_cast<int>(ids.size())) +
                                                      " articulation tracks");
                       }),
                       true);
}

juce::String consolidate_articulations(AppContext& c)
{
    const auto part = articulation_part(c, c.session.selected_track());
    if (!c.session.consolidate_articulations(part)) return "Select an expanded part (or one of its articulation tracks)";
    c.session.select_track(part);
    return "Consolidated: one track plays every articulation again";
}

}  // namespace vcc::desktop
