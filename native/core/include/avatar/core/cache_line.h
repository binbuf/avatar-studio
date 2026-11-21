#pragma once

#include <cstddef>

namespace avatar::core {

/// Conservative cache-line size for false-sharing avoidance.
///
/// The C++20 `std::hardware_destructive_interference_size` is not used because
/// GCC warns when it is used in an ABI-relevant context and the value differs
/// between targets. The data plane only needs the two cache lines not to share
/// a line on the supported x86-64 and arm64 desktop targets, where 64 bytes is
/// the safe lower bound.
inline constexpr std::size_t kCacheLineSize = 64;

} // namespace avatar::core
