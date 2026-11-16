#include "vcc/project/Merge.h"

#include <algorithm>
#include <map>
#include <set>

#include "vcc/project/Project.h"

namespace vcc::project {

namespace {

using Opt = std::optional<json::Value>;

/// The field items of an array are matched by, when every item has one.
const char* key_field(const json::Array& a)
{
    if (a.empty()) return nullptr;
    for (const char* k : {"id", "note", "tick"}) {
        const bool all = std::all_of(a.begin(), a.end(), [&](const json::Value& v) { return v.is_object() && v.get(k); });
        if (all) return k;
    }
    return nullptr;
}

std::string key_of(const json::Value& item, const char* field)
{
    const auto* v = item.get(field);
    if (!v) return {};
    return v->is_string() ? v->as_string() : json::write(*v);
}

struct Merger {
    MergeResult& out;
    /// For descriptions: what the path's parts mean.
    struct Frame {
        std::string key;    ///< Object key or item id.
        std::string label;  ///< "Violins I", "a note", "the mix"...
    };
    std::vector<Frame> where;

    std::string path() const
    {
        std::string p;
        for (const auto& f : where) p += (p.empty() ? "" : "/") + f.key;
        return p;
    }

    std::string describe() const
    {
        // The nearest named thing (a track, a bus, a marker...), then what
        // changed inside it.
        // The outermost named thing: the track (not its version), the bus...
        std::string owner, what;
        for (const auto& f : where)
            if (!f.label.empty() && owner.empty()) owner = f.label;
        const std::string field = where.empty() ? std::string() : where.back().key;
        bool in_note = false;
        for (const auto& f : where) in_note = in_note || f.key == "notes";
        const bool whole_note = where.size() >= 2 && where[where.size() - 2].key == "notes";
        if (whole_note) what = "a note (changed by both)";
        else if (in_note) what = field == "notes" ? "its notes" : "a note's " + field;
        else what = field;
        if (what == "step" || what == "octave" || what == "a note's step" || what == "a note's octave") what = "a note's pitch";
        return owner.empty() ? "The project: " + what : owner + ": " + what;
    }

    void conflict(const Opt& b, const Opt& o, const Opt& t, MergeConflict::Kind kind)
    {
        MergeConflict c;
        c.path = path();
        c.description = describe();
        c.kind = kind;
        c.base = b;
        c.ours = o;
        c.theirs = t;
        out.conflicts.push_back(std::move(c));
    }

    /// Merges one value; nullopt result: absent.
    Opt merge(const Opt& b, const Opt& o, const Opt& t)
    {
        if (o == t) return o;
        if (o == b) {
            ++out.from_theirs;
            return t;
        }
        if (t == b) {
            ++out.from_ours;
            return o;
        }
        // Both changed, differently. A note is one thing: merging its fields
        // separately could make a pitch neither person wrote (C5 and E4
        // becoming E5), so a note changed on both sides is one conflict.
        const bool note = where.size() >= 2 && where[where.size() - 2].key == "notes";
        if (o && t && o->is_object() && t->is_object() && !note) return merge_objects(b, *o, *t);
        if (o && t && o->is_array() && t->is_array()) {
            const auto& oa = o->as_array();
            const auto& ta = t->as_array();
            const json::Array empty;
            const json::Array& ba = b && b->is_array() ? b->as_array() : empty;
            const char* k = key_field(oa.empty() ? (ta.empty() ? ba : ta) : oa);
            if (k && (ta.empty() || key_field(ta)) && (ba.empty() || key_field(ba))) return merge_keyed(ba, oa, ta, k);
            if (oa.size() == ta.size() && ba.size() == oa.size()) return merge_positional(ba, oa, ta);
        }
        // The counter of ids handed out: the larger, never a conflict.
        if (!where.empty() && where.back().key == "next_id" && o && t) return std::max(o->as_int(), t->as_int()) == o->as_int() ? o : t;
        const bool deleted = !o || !t;
        conflict(b, o, t, deleted ? MergeConflict::Kind::ChangedAndDeleted : MergeConflict::Kind::Changed);
        return o;  // ours until the person chooses
    }

    Opt merge_objects(const Opt& b, const json::Value& o, const json::Value& t)
    {
        json::Object result;
        std::vector<std::string> keys;
        auto add = [&](const json::Value& v) {
            for (const auto& [k, _] : v.as_object())
                if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
        };
        add(o);
        add(t);
        if (b && b->is_object()) add(*b);
        for (const auto& k : keys) {
            auto at = [&](const json::Value* v) -> Opt {
                if (!v || !v->is_object()) return std::nullopt;
                const auto* x = v->get(k);
                return x ? Opt(*x) : std::nullopt;
            };
            where.push_back({k, {}});
            const Opt m = merge(at(b ? &*b : nullptr), at(&o), at(&t));
            where.pop_back();
            if (m) result.set(k, *m);
        }
        return json::Value(std::move(result));
    }

    Opt merge_positional(const json::Array& b, const json::Array& o, const json::Array& t)
    {
        json::Array result;
        for (std::size_t i = 0; i < o.size(); ++i) {
            where.push_back({std::to_string(i), {}});
            const Opt m = merge(Opt(b[i]), Opt(o[i]), Opt(t[i]));
            where.pop_back();
            if (m) result.push_back(*m);
        }
        return json::Value(std::move(result));
    }

    Opt merge_keyed(const json::Array& b, const json::Array& o, const json::Array& t, const char* field)
    {
        std::map<std::string, const json::Value*> bm, om, tm;
        for (const auto& v : b) bm[key_of(v, field)] = &v;
        for (const auto& v : o) om[key_of(v, field)] = &v;
        for (const auto& v : t) tm[key_of(v, field)] = &v;
        auto get = [](const std::map<std::string, const json::Value*>& m, const std::string& k) -> Opt {
            const auto it = m.find(k);
            return it == m.end() ? std::nullopt : Opt(*it->second);
        };
        // Order: ours, with items only theirs inserted after the item that
        // precedes them in theirs.
        std::vector<std::string> order;
        for (const auto& v : o) order.push_back(key_of(v, field));
        std::string prev;
        for (const auto& v : t) {
            const auto k = key_of(v, field);
            if (!om.count(k)) {
                auto pos = prev.empty() ? order.begin() : std::find(order.begin(), order.end(), prev);
                if (pos != order.end() && !prev.empty()) ++pos;
                prev = *order.insert(pos, k);
                continue;
            }
            prev = k;
        }
        for (const auto& v : b) {  // deleted on both sides: gone; else visited below
            const auto k = key_of(v, field);
            if (std::find(order.begin(), order.end(), k) == order.end() && (om.count(k) || tm.count(k)))
                order.push_back(k);
        }
        json::Array result;
        for (const auto& k : order) {
            const Opt ov = get(om, k), tv = get(tm, k), bv = get(bm, k);
            // What names this item, for the description: the name both
            // sides started from, when there was one (a rename on both
            // sides is "Violins I: name", not one side's new name).
            std::string label;
            const json::Value* any = bv ? &*bv : ov ? &*ov : tv ? &*tv : nullptr;
            if (any)
                if (const auto* n = any->get("name"); n && n->is_string()) label = n->as_string();
            where.push_back({k, label});
            const Opt m = merge(bv, ov, tv);
            where.pop_back();
            if (m) result.push_back(*m);
        }
        return json::Value(std::move(result));
    }
};

/// Sets `value` (or removes, when absent) at a slash path in a document.
void put(json::Value& doc, const std::vector<std::string>& parts, std::size_t i, const Opt& value)
{
    if (i == parts.size()) return;
    const bool last = i + 1 == parts.size();
    const auto& key = parts[i];
    if (doc.is_object()) {
        auto& obj = doc.object();
        if (last) {
            if (value) obj.set(key, *value);
            else obj.erase(key);
            return;
        }
        if (auto* next = obj.find(key)) put(*next, parts, i + 1, value);
        return;
    }
    if (doc.is_array()) {
        auto& arr = doc.array();
        const char* field = key_field(arr);
        for (std::size_t n = 0; n < arr.size(); ++n) {
            const bool match = field ? key_of(arr[n], field) == key : std::to_string(n) == key;
            if (!match) continue;
            if (last) {
                if (value) arr[n] = *value;
                else arr.erase(arr.begin() + static_cast<std::ptrdiff_t>(n));
            } else {
                put(arr[n], parts, i + 1, value);
            }
            return;
        }
        if (last && value) arr.push_back(*value);  // theirs kept an item ours deleted
    }
}

}  // namespace

MergeResult merge_projects(const json::Value& base, const json::Value& ours, const json::Value& theirs)
{
    MergeResult out;
    Merger m{out, {}};
    const Opt merged = m.merge(Opt(base), Opt(ours), Opt(theirs));
    out.merged = merged ? *merged : json::Value();
    return out;
}

json::Value MergeResult::resolve(const std::vector<bool>& take_theirs) const
{
    json::Value doc = merged;
    for (std::size_t i = 0; i < conflicts.size() && i < take_theirs.size(); ++i) {
        if (!take_theirs[i]) continue;
        std::vector<std::string> parts;
        std::string part;
        for (char c : conflicts[i].path) {
            if (c == '/') {
                parts.push_back(part);
                part.clear();
            } else {
                part += c;
            }
        }
        parts.push_back(part);
        put(doc, parts, 0, conflicts[i].theirs);
    }
    return doc;
}

}  // namespace vcc::project
