#pragma once
// Clock helpers. Which clock to use for which measurement is part of the task: think about it.
#include <chrono>
#include <cstdint>

namespace wail {

// Monotonic clock of THIS host in nanoseconds since an unspecified epoch (std::chrono::steady_clock).
// On Linux this is CLOCK_MONOTONIC, on macOS mach_continuous_time-based: the same value for every
// process of the host, but NOT comparable between different hosts.
inline std::uint64_t mono_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Wall-clock time (std::chrono::system_clock), nanoseconds since the Unix epoch. Can jump (NTP).
inline std::uint64_t wall_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

}  // namespace wail
