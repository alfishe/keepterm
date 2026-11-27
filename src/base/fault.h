// Fault injection points. Compiled to nothing unless KT_FAULTS is on.
//
//   if (KT_FAULT_POINT("archive.after_close")) return make_error(Errc::io_error, "injected");
//
// Configured by KEEPTERM_FAULTS="name:action[@hits];...", read once:
//   action: abort       the process ends at once with exit code 86, no cleanup (like kill -9)
//           fail        the point returns true (the caller fails the operation)
//           hang        the thread blocks forever
//           delay=<ms>  the thread sleeps, then the point returns false
//   hits:   n | a-b | p=<probability>; none = every hit. Hits count from 1 per point.
#pragma once

#include <cstdint>
#include <string_view>

namespace kt::fault {

#ifdef KT_FAULTS
bool hit(std::string_view name);
// The hits of a point so far (tests).
uint64_t hits(std::string_view name);
// Replaces the configuration (tests; normally KEEPTERM_FAULTS) and clears the counters.
void configure(std::string_view spec);
inline constexpr bool kEnabled = true;
#else
inline constexpr bool hit(std::string_view) { return false; }
inline constexpr uint64_t hits(std::string_view) { return 0; }
inline void configure(std::string_view) {}
inline constexpr bool kEnabled = false;
#endif

}  // namespace kt::fault

#define KT_FAULT_POINT(name) (::kt::fault::hit(name))
