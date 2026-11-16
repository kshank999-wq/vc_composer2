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
