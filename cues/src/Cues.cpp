#include "vcc/cues/Cues.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>

#include "vcc/picture/Picture.h"

namespace vcc::cues {

namespace {

std::string lower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string reel_name(const project::ProjectData& d, const std::string& id)
{
    for (const auto& r : d.reels)
        if (r.id == id) return r.name;
    return {};
}

std::string scene_name(const project::ProjectData& d, const std::string& id)
{
    for (const auto& s : d.scenes)
        if (s.id == id) return s.name;
    return {};
}

int reel_index(const project::ProjectData& d, const std::string& id)
{
    for (std::size_t i = 0; i < d.reels.size(); ++i)
        if (d.reels[i].id == id) return static_cast<int>(i);
    return static_cast<int>(d.reels.size());
}

std::string csv(const std::string& field)
{
    if (field.find_first_of(",\"\n\r") == std::string::npos) return field;
    std::string out = "\"";
    for (char c : field) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

}  // namespace

CueTimes times_of(const project::Cue& c, const project::ProjectData& d)
{
    CueTimes t;
    t.start_seconds = d.tempo.seconds_at(c.start);
    t.end_seconds = d.tempo.seconds_at(c.end);
    t.in_tc = picture::project_timecode(d, t.start_seconds);
    t.out_tc = picture::project_timecode(d, t.end_seconds);
    const long total = std::lround(std::max(0.0, t.end_seconds - t.start_seconds));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%ld:%02ld", total / 60, total % 60);
    t.duration = buf;
    return t;
}

std::optional<std::int64_t> spot_drift_frames(const project::Cue& c, const project::ProjectData& d)
{
    if (c.spot_in.empty()) return std::nullopt;
    auto spotted = picture::project_seconds_for_timecode(d, c.spot_in);
    if (!spotted) return std::nullopt;
    const double music = d.tempo.seconds_at(c.start);
    return static_cast<std::int64_t>(std::llround((music - *spotted) * frames_per_second(d.frame_rate)));
}

bool overdue(const project::Cue& c, const std::string& today)
{
    if (c.deadline.empty() || today.empty()) return false;
    if (c.status == "approved" || c.status == "locked") return false;
    return c.deadline < today;  // ISO dates compare as strings
}

std::vector<const project::Cue*> list(const project::ProjectData& d, const CueFilter& f)
{
    const std::string q = lower(f.query);
    std::vector<const project::Cue*> out;
    for (const auto& c : d.cues) {
        if (!f.status.empty() && c.status != f.status) continue;
        if (!f.reel_id.empty() && c.reel_id != f.reel_id) continue;
        if (f.overdue_only && !overdue(c, f.today)) continue;
        if (!q.empty()) {
            const std::string hay = lower(c.code + " " + c.title + " " + c.description + " " + c.notes + " " + c.composer);
            if (hay.find(q) == std::string::npos) continue;
        }
        out.push_back(&c);
    }
    std::stable_sort(out.begin(), out.end(), [&](const project::Cue* a, const project::Cue* b) {
        const int ra = reel_index(d, a->reel_id), rb = reel_index(d, b->reel_id);
        return ra != rb ? ra < rb : a->start < b->start;
    });
    return out;
}

std::vector<std::pair<std::string, std::string>> overlaps(const project::ProjectData& d)
{
    std::vector<std::pair<std::string, std::string>> out;
    for (std::size_t i = 0; i < d.cues.size(); ++i)
        for (std::size_t j = i + 1; j < d.cues.size(); ++j) {
            const auto& a = d.cues[i];
            const auto& b = d.cues[j];
            if (a.start < b.end && b.start < a.end) out.emplace_back(a.id, b.id);
        }
    return out;
}

std::string next_code(const project::ProjectData& d, const std::string& reel_id)
{
    const std::string prefix = reel_id.empty() ? "M" : std::to_string(reel_index(d, reel_id) + 1) + "M";
    int n = 1;
    auto taken = [&](const std::string& code) {
        return std::any_of(d.cues.begin(), d.cues.end(), [&](const project::Cue& c) { return c.code == code; });
    };
    while (taken(prefix + std::to_string(n))) ++n;
    return prefix + std::to_string(n);
}

std::string cue_sheet_csv(const project::ProjectData& d)
{
    std::string out = "Cue,Title,Reel,Scene,In,Out,Duration,Usage,Composer,Status,Revision\r\n";
    for (const auto* c : list(d)) {
        const CueTimes t = times_of(*c, d);
        const std::string composer = c->composer.empty() ? d.production.composer : c->composer;
        const std::string fields[] = {c->code, c->title, reel_name(d, c->reel_id), scene_name(d, c->scene_id),
                                      c->spot_in.empty() ? t.in_tc : c->spot_in,
                                      c->spot_out.empty() ? t.out_tc : c->spot_out, t.duration, c->usage, composer,
                                      c->status, std::to_string(c->revision)};
        for (std::size_t i = 0; i < std::size(fields); ++i) out += (i ? "," : "") + csv(fields[i]);
        out += "\r\n";
    }
    return out;
}

std::string spotting_notes(const project::ProjectData& d)
{
    std::string out;
    const std::string title = d.production.title.empty() ? d.name : d.production.title;
    out += "SPOTTING NOTES - " + title + "\n";
    if (!d.production.director.empty()) out += "Director: " + d.production.director + "\n";
    if (!d.production.composer.empty()) out += "Composer: " + d.production.composer + "\n";
    out += "\n";
    std::string reel;
    for (const auto* c : list(d)) {
        const std::string r = reel_name(d, c->reel_id);
        if (r != reel) {
            reel = r;
            out += "== " + (r.empty() ? std::string("No reel") : r) + " ==\n\n";
        }
        const CueTimes t = times_of(*c, d);
        out += c->code + "  \"" + c->title + "\"  " + (c->spot_in.empty() ? t.in_tc : c->spot_in) + " - " +
               (c->spot_out.empty() ? t.out_tc : c->spot_out) + "  (" + t.duration + ")  [" + c->status + ", rev " +
               std::to_string(c->revision) + "]\n";
        if (!c->scene_id.empty()) out += "   Scene: " + scene_name(d, c->scene_id) + "\n";
        if (!c->description.empty()) out += "   " + c->description + "\n";
        if (!c->notes.empty()) out += "   Notes: " + c->notes + "\n";
        if (!c->transition_in.empty()) out += "   In: " + c->transition_in + "\n";
        if (!c->transition_out.empty()) out += "   Out: " + c->transition_out + "\n";
        for (const auto& m : d.markers) {
            if (m.seconds < t.start_seconds || m.seconds > t.end_seconds) continue;
            out += "   * " + m.kind + " " + picture::project_timecode(d, m.seconds) + (m.label.empty() ? "" : "  " + m.label) + "\n";
        }
        out += "\n";
    }
    return out;
}

std::string render_name(const std::string& preset, const project::Cue& c, const project::ProjectData& d,
                        const std::string& date)
{
    const std::string pattern = preset.empty() ? "{production}_{code}_{title}_v{rev}" : preset;
    const std::map<std::string, std::string> tokens = {
        {"{project}", d.name},
        {"{production}", d.production.title.empty() ? d.name : d.production.title},
        {"{reel}", reel_name(d, c.reel_id)},
        {"{code}", c.code},
        {"{title}", c.title},
        {"{rev}", std::to_string(c.revision)},
        {"{status}", c.status},
        {"{date}", date},
    };
    std::string out;
    for (std::size_t i = 0; i < pattern.size();) {
        bool replaced = false;
        if (pattern[i] == '{') {
            for (const auto& [token, value] : tokens)
                if (pattern.compare(i, token.size(), token) == 0) {
                    out += value;
                    i += token.size();
                    replaced = true;
                    break;
                }
        }
        if (!replaced) out += pattern[i++];
    }
    // Safe on every file system: no separators, reserved or control characters.
    std::string safe;
    for (unsigned char ch : out) {
        if (ch < 0x20 || std::string("/\\\\:*?\"<>|").find(static_cast<char>(ch)) != std::string::npos) safe += '_';
        else if (ch == ' ') safe += ' ';
        else safe += static_cast<char>(ch);
    }
    while (!safe.empty() && (safe.back() == ' ' || safe.back() == '.')) safe.pop_back();
    return safe.empty() ? "cue" : safe;
}

}  // namespace vcc::cues
