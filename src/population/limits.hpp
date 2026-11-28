#pragma once

#include <cstddef>

namespace population {

// One spot, one future per expiry, and one book per option strike.
inline constexpr std::size_t kMaxComponents = 1024;

}  // namespace population
