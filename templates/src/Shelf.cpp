#include "vcc/templates/Shelf.h"

#include <algorithm>

namespace vcc::templates {

bool TemplateShelf::is_favorite(const std::string& id) const
{
    return std::find(favorites_.begin(), favorites_.end(), id) != favorites_.end();
}

void TemplateShelf::set_favorite(const std::string& id, bool on)
{
    if (id.empty()) return;
    const auto it = std::find(favorites_.begin(), favorites_.end(), id);
    if (on && it == favorites_.end()) favorites_.push_back(id);
    if (!on && it != favorites_.end()) favorites_.erase(it);
}

void TemplateShelf::used(const std::string& id)
{
    if (id.empty()) return;
    recent_.erase(std::remove(recent_.begin(), recent_.end(), id), recent_.end());
    recent_.insert(recent_.begin(), id);
    if (recent_.size() > kRecentLimit) recent_.resize(kRecentLimit);
}

void TemplateShelf::forget(const std::string& id)
{
    favorites_.erase(std::remove(favorites_.begin(), favorites_.end(), id), favorites_.end());
    recent_.erase(std::remove(recent_.begin(), recent_.end(), id), recent_.end());
}

json::Value TemplateShelf::to_json() const
{
    json::Array f, r;
    for (const auto& id : favorites_) f.emplace_back(id);
    for (const auto& id : recent_) r.emplace_back(id);
    return json::Object{{"schema_version", 1}, {"favorites", f}, {"recent", r}};
}

TemplateShelf TemplateShelf::from_json(const json::Value& doc)
{
    TemplateShelf s;
    if (const auto* f = doc.get("favorites"); f && f->is_array())
        for (const auto& v : f->as_array())
            if (v.is_string()) s.set_favorite(v.as_string(), true);
    if (const auto* r = doc.get("recent"); r && r->is_array()) {
        const auto& arr = r->as_array();
        for (auto it = arr.rbegin(); it != arr.rend(); ++it)
            if (it->is_string()) s.used(it->as_string());
    }
    return s;
}

}  // namespace vcc::templates
