#include "vcc/project/Stems.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace vcc::project {

namespace {

bool in_deliverables(const Track& t)
{
    if (!t.include_in_export) return false;
    return t.kind != TrackKind::Folder && t.kind != TrackKind::Video;
}

/// True when `bus_id` is `target` or routes into it.
bool reaches(const ProjectData& d, std::string bus_id, const std::string& target)
{
    for (int guard = 0; guard < 64 && !bus_id.empty(); ++guard) {
        if (bus_id == target) return true;
        const Bus* b = d.find_bus(bus_id);
        if (!b || b->kind == "master") return false;
        bus_id = b->output;
    }
    return false;
}

std::string pretty_family(std::string f)
{
    if (f.empty()) return "Other";
    if (f == "keys") return "Keys";
    if (f == "rhythm") return "Rhythm";
    f[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(f[0])));
    return f;
}

std::string strip_bus_word(std::string name)
{
    for (const char* suffix : {" Bus", " bus", " Group", " group"})
        if (name.size() > std::string(suffix).size() && name.compare(name.size() - std::string(suffix).size(),
                                                                     std::string::npos, suffix) == 0)
            return name.substr(0, name.size() - std::string(suffix).size());
    return name;
}

}  // namespace

std::vector<const Track*> stem_tracks(const StemDef& stem, const ProjectData& d)
{
    std::vector<const Track*> out;
    const std::set<std::string> sources(stem.sources.begin(), stem.sources.end());
    for (const auto& t : d.tracks) {
        if (!in_deliverables(t)) continue;
        bool in = sources.count(t.id) > 0;
        for (const auto& s : stem.sources)
            if (!in && d.find_bus(s) && d.find_bus(s)->kind != "master" && reaches(d, t.output_bus_id, s)) in = true;
        if (in) out.push_back(&t);
    }
    return out;
}

std::vector<StemDef> default_stems(const ProjectData& d, const std::function<std::string(const Track&)>& family_of)
{
    std::vector<StemDef> stems;
    auto add = [&](const std::string& name, std::vector<std::string> sources) {
        StemDef s;
        s.id = "stem-" + std::to_string(stems.size() + 1);
        s.name = name;
        s.sources = std::move(sources);
        stems.push_back(std::move(s));
    };
    // Group buses that deliverable tracks feed directly.
    std::set<std::string> covered;
    for (const auto& b : d.buses) {
        if (b.kind != "group") continue;
        StemDef probe;
        probe.sources = {b.id};
        const auto tracks = stem_tracks(probe, d);
        if (tracks.empty()) continue;
        // Nested groups: a bus inside another group is covered by the outer one.
        if (b.output != "bus-master" && !b.output.empty() && d.find_bus(b.output) &&
            d.find_bus(b.output)->kind == "group")
            continue;
        for (const auto* t : tracks) covered.insert(t->id);
        add(strip_bus_word(b.name), {b.id});
    }
    // The rest by family, in order of first appearance.
    std::vector<std::string> order;
    std::map<std::string, std::vector<std::string>> by_family;
    for (const auto& t : d.tracks) {
        if (!in_deliverables(t) || covered.count(t.id)) continue;
        std::string f = family_of ? family_of(t) : std::string();
        if (f.empty()) f = t.kind == TrackKind::Audio ? "audio" : t.family;
        if (!by_family.count(f)) order.push_back(f);
        by_family[f].push_back(t.id);
    }
    for (const auto& f : order) add(pretty_family(f), by_family[f]);
    return stems;
}

StemCoverage stem_coverage(const std::vector<StemDef>& stems, const ProjectData& d)
{
    StemCoverage c;
    std::map<std::string, int> count;
    for (const auto& s : stems) {
        const auto tracks = stem_tracks(s, d);
        if (tracks.empty()) c.empty.push_back(s.name);
        for (const auto* t : tracks) ++count[t->id];
    }
    for (const auto& t : d.tracks) {
        if (!in_deliverables(t)) continue;
        const int n = count.count(t.id) ? count[t.id] : 0;
        if (n == 0) c.missing.push_back(t.name);
        if (n > 1) c.doubled.push_back(t.name);
    }
    return c;
}

}  // namespace vcc::project
