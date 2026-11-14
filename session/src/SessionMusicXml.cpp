// Session: MusicXML import and export.
#include "vcc/session/Session.h"

#include <algorithm>
#include <cctype>

#include "vcc/theory/Pitch.h"

namespace vcc::session {

namespace {

std::string normalise(const std::string& s)
{
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c)) out += static_cast<char>(std::tolower(c));
        else if (!out.empty() && out.back() != ' ') out += ' ';
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

/// Part names as notation programs write them, reduced to the words the
/// registry uses: "Violoncello 2" -> "cello", "Clarinet in B♭ 1" ->
/// "clarinet in bb".
std::string canonical_part_name(const std::string& name)
{
    std::string s = name;
    // Flat and sharp signs before normalising strips them.
    for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
             {"\xE2\x99\xAD", "b"}, {"\xE2\x99\xAF", "#"}}) {
        for (std::size_t p = s.find(from); p != std::string::npos; p = s.find(from, p)) s.replace(p, from.size(), to);
    }
    std::string n = " " + normalise(s) + " ";
    static const std::pair<const char*, const char*> kSynonyms[] = {
        {" b flat ", " bb "}, {" e flat ", " eb "}, {" violoncello ", " cello "}, {" violoncelli ", " celli "},
        {" violoncellos ", " celli "}, {" cellos ", " celli "}, {" contrabass ", " double bass "},
        {" kontrabass ", " double bass "}, {" string bass ", " double bass "}, {" pianoforte ", " piano "},
        {" acoustic grand piano ", " piano "}, {" grand piano ", " piano "}, {" flauto ", " flute "},
        {" fagotto ", " bassoon "}, {" drumset ", " drum kit "}, {" drum set ", " drum kit "},
        {" drums ", " drum kit "}, {" french horn ", " horn "}, {" violino ", " violin "}, {" viole ", " violas "},
        {" cor anglais ", " english horn "},
    };
    for (const auto& [from, to] : kSynonyms)
        for (std::size_t p = n.find(from); p != std::string::npos; p = n.find(from, p)) {
            n.replace(p, std::string(from).size(), to);
            p += std::string(to).size() - 1;
        }
    // Desk and player numbers: "violin 1", "horn iii".
    static const char* kNumbers[] = {" 1 ", " 2 ", " 3 ", " 4 ", " 5 ", " 6 ", " i ", " ii ", " iii ", " iv "};
    bool changed = true;
    while (changed) {
        changed = false;
        for (const char* num : kNumbers) {
            const std::string t = num;
            if (n.size() > t.size() && n.compare(n.size() - t.size(), t.size(), t) == 0) {
                n.erase(n.size() - t.size() + 1);
                changed = true;
            }
        }
    }
    return normalise(n);
}

/// "Bb major", "F# minor", "D dorian" for fifths and a MusicXML mode.
std::string key_text(int fifths, const std::string& xml_mode)
{
    std::string mode = xml_mode;
    int offset = 0;
    if (mode == "minor" || mode == "aeolian") offset = -3;
    else if (mode == "dorian") offset = -2;
    else if (mode == "phrygian") offset = -4;
    else if (mode == "lydian") offset = 1;
    else if (mode == "mixolydian") offset = -1;
    else if (mode == "locrian") offset = -5;
    else mode = "major";
    if (mode == "ionian") mode = "major";
    // The tonic's position on the circle of fifths: F C G D A E B, then sharps.
    const int n = fifths - offset;
    static const char kLetters[] = "FCGDAEB";
    const int idx = ((n + 1) % 7 + 7) % 7;
    const int alter = (n + 1 >= 0) ? (n + 1) / 7 : -((-(n + 1) + 6) / 7);
    std::string tonic(1, kLetters[idx]);
    if (alter > 0) tonic += std::string(static_cast<std::size_t>(alter), '#');
    if (alter < 0) tonic += std::string(static_cast<std::size_t>(-alter), 'b');
    std::string text = tonic + " " + mode;
    if (!theory::Key::parse(text)) text = "C major";
    return text;
}

}  // namespace

std::string Session::guess_instrument(const interchange::ImportedPart& part) const
{
    const auto& all = catalogs_.instruments.all();
    if (all.empty()) return {};
    const std::string wanted = canonical_part_name(part.name);
    const std::string wanted_abbr = normalise(part.abbreviation);
    const std::string first_word = wanted.substr(0, wanted.find(' '));
    const int transposition = part.score.staff_count() > 0 ? part.score.staff(0).total_transposition() : 0;
    const bool grand = part.score.staff_count() >= 2;

    const templates::InstrumentDef* best = nullptr;
    int best_score = 0;
    for (const auto& def : all) {
        const std::string name = canonical_part_name(def.name);
        const std::string shortn = normalise(def.short_name);
        int score = 0;
        if (!wanted.empty() && (name == wanted || normalise(def.name) == wanted)) score = 100;
        else if (!wanted.empty() && shortn == wanted) score = 90;
        else if (!name.empty() && (" " + wanted + " ").find(" " + name + " ") != std::string::npos)
            score = 50 + static_cast<int>(name.size());
        else if (!first_word.empty() && name.substr(0, name.find(' ')) == first_word) score = 20;
        if (score == 0 && !wanted_abbr.empty() && shortn == wanted_abbr) score = 40;
        if (score == 0) continue;
        int def_total = def.transposition;
        if (def.clef == "treble_8vb" || def.clef == "bass_8vb") def_total -= 12;
        if (def.clef == "treble_8va") def_total += 12;
        if (def_total == transposition) score += 15;
        if (part.is_percussion != def.unpitched) score -= 30;
        if (grand == def.grand_staff) score += 5;
        if (part.midi_program >= 0 && def.gm_program == part.midi_program) score += 8;
        if (score > best_score) {
            best_score = score;
            best = &def;
        }
    }
    if (best) return best->id;
    if (part.is_percussion && catalogs_.instruments.find("rhythm.drum_kit")) return "rhythm.drum_kit";
    if (part.midi_program >= 0)
        for (const auto& def : all)
            if (def.gm_program == part.midi_program && !def.unpitched && def.grand_staff == grand) return def.id;
    if (catalogs_.instruments.find("keys.piano")) return "keys.piano";
    return all.front().id;
}

ImportSummary Session::import_musicxml(const std::string& xml, bool replace_project)
{
    ImportSummary summary;
    interchange::ImportResult r = interchange::import_musicxml(xml);
    if (!r.ok) {
        summary.error = r.error;
        return summary;
    }
    summary.warnings = r.warnings;

    const auto& d0 = data();
    const bool empty = std::none_of(d0.tracks.begin(), d0.tracks.end(), [](const project::Track& t) {
        return t.kind != project::TrackKind::Folder && t.kind != project::TrackKind::Video;
    });
    const bool take_maps = replace_project || empty;

    struct Planned {
        std::string id, version, instrument;
        const interchange::ImportedPart* part;
    };
    std::vector<Planned> planned;
    for (const auto& p : r.parts)
        planned.push_back({project_.ids().next("trk"), project_.ids().next("ver"), guess_instrument(p), &p});

    // Meter and key the parts will be synced to.
    MeterMap meter;
    if (!r.time_signatures.empty()) {
        meter = MeterMap(TimeSignature{r.time_signatures.front().numerator, r.time_signatures.front().denominator});
        for (std::size_t i = 1; i < r.time_signatures.size(); ++i)
            meter.set(r.time_signatures[i].bar,
                      TimeSignature{r.time_signatures[i].numerator, r.time_signatures[i].denominator});
    }
    const std::string key = r.keys.empty() ? std::string("C major") : key_text(r.keys.front().fifths, r.keys.front().mode);

    if (!take_maps) {
        bool same_meter = d0.meter.changes().size() == meter.changes().size();
        for (std::size_t i = 0; same_meter && i < meter.changes().size(); ++i)
            same_meter = d0.meter.changes()[i].bar == meter.changes()[i].bar &&
                         d0.meter.changes()[i].signature == meter.changes()[i].signature;
        if (!same_meter)
            summary.warnings.push_back("the project's meter was kept; the imported parts were laid out on it");
        auto a = theory::Key::parse(d0.key), b = theory::Key::parse(key);
        if (a && b && a->fifths() != b->fifths())
            summary.warnings.push_back("the project's key (" + d0.key + ") was kept; the file is in " + key);
    }

    const bool ok = project_.edit("Import MusicXML", [&](project::ProjectData& d) {
        if (replace_project) {
            d.tracks.erase(std::remove_if(d.tracks.begin(), d.tracks.end(),
                                          [](const project::Track& t) { return t.kind != project::TrackKind::Video; }),
                           d.tracks.end());
            for (auto& s : d.snapshots) s.track_versions.clear();
            if (!r.title.empty()) d.name = r.title;
            if (!r.composer.empty()) d.production.composer = r.composer;
        }
        if (take_maps) {
            d.meter = meter;
            d.key = key;
            if (!r.tempos.empty()) {
                const double first = r.tempos.front().tick == 0 ? r.tempos.front().bpm : 120.0;
                d.tempo = TempoMap(std::clamp(first, 20.0, 400.0));
                for (const auto& t : r.tempos)
                    if (t.tick > 0) d.tempo.set(t.tick, std::clamp(t.bpm, 20.0, 400.0));
            }
        }
        for (const auto& p : planned) {
            project::Track t;
            t.id = p.id;
            t.name = d.unique_track_name(p.part->name.empty() ? std::string("Part") : p.part->name);
            t.kind = project::TrackKind::Instrument;
            t.instrument_standard_id = p.instrument;
            t.include_in_export = true;
            t.output_bus_id = "bus-master";
            t.versions.push_back({p.version, "Version 1", {}, p.part->score, {}, {}, {}, std::nullopt, {}, {}});
            t.active_version_id = p.version;
            const templates::InstrumentDef* def = catalogs_.instruments.find(p.instrument);
            if (def) {
                t.family = def->family;
                t.color = templates::family_color(def->family);
            }
            t.assignment.status = project::ResolutionStatus::Placeholder;
            t.assignment.placeholder_program =
                p.part->midi_program >= 0 ? p.part->midi_program : (def ? std::max(0, def->gm_program) : 0);
            t.assignment.patch_display = "GM placeholder";
            t.assignment.message = "Built-in placeholder sound until a library patch or plug-in is assigned.";
            if (p.part->is_percussion || (def && def->unpitched)) t.midi_channel = 9;
            else if (p.part->midi_channel >= 0 && p.part->midi_channel <= 15) t.midi_channel = p.part->midi_channel;
            d.tracks.push_back(std::move(t));
        }
        return true;
    });
    if (!ok) {
        summary.error = "the import could not be applied to the project";
        return summary;
    }
    for (const auto& p : planned) summary.track_ids.push_back(p.id);
    summary.ok = true;
    document_changed();
    if (!summary.track_ids.empty()) select_track(summary.track_ids.front());
    return summary;
}

std::string Session::export_musicxml(const interchange::ExportOptions& options, std::vector<std::string>* warnings) const
{
    interchange::ExportOptions opts = options;
    for (const auto& t : data().tracks) {
        auto& info = opts.part_info[t.id];
        const templates::InstrumentDef* def = instrument_for(t);
        if (info.abbreviation.empty() && def) info.abbreviation = def->short_name;
        if (info.midi_program < 0) {
            if (t.assignment.status == project::ResolutionStatus::Placeholder) info.midi_program = t.assignment.placeholder_program;
            else if (def && def->gm_program >= 0) info.midi_program = def->gm_program;
        }
    }
    return interchange::export_musicxml(data(), opts, warnings);
}

}  // namespace vcc::session
