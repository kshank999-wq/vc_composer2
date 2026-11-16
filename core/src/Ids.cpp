#include "vcc/core/Ids.h"

#include <cstdio>

namespace vcc {

std::string IdAllocator::next(const char* prefix)
{
    char buf[64];
    if (site_ != 0)
        std::snprintf(buf, sizeof buf, "%s-%x-%04llu", prefix, site_, static_cast<unsigned long long>(next_++));
    else
        std::snprintf(buf, sizeof buf, "%s-%04llu", prefix, static_cast<unsigned long long>(next_++));
    return buf;
}

}  // namespace vcc
