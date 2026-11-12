#include "vcc/core/Ids.h"

#include <cstdio>

namespace vcc {

std::string IdAllocator::next(const char* prefix)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s-%04llu", prefix, static_cast<unsigned long long>(next_++));
    return buf;
}

}  // namespace vcc
