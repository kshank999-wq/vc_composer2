// The composer's template shelf (Templates Supplement §3 "favorites"):
// starred templates and the ones used most recently, by id, saved as a
// small JSON file beside the user's templates. Ids that no longer resolve
// (a deleted custom template) are kept harmlessly and skipped on display.
#pragma once

#include <string>
#include <vector>

#include "vcc/core/Json.h"

namespace vcc::templates {

class TemplateShelf {
public:
    static constexpr std::size_t kRecentLimit = 10;

    bool is_favorite(const std::string& id) const;
    void set_favorite(const std::string& id, bool on);
    /// Newest first, at most kRecentLimit.
    void used(const std::string& id);
    /// Forgets an id entirely (a template removed).
    void forget(const std::string& id);

    const std::vector<std::string>& favorites() const noexcept { return favorites_; }
    const std::vector<std::string>& recent() const noexcept { return recent_; }

    json::Value to_json() const;
    static TemplateShelf from_json(const json::Value& doc);

private:
    std::vector<std::string> favorites_;  ///< In the order starred.
    std::vector<std::string> recent_;
};

}  // namespace vcc::templates
