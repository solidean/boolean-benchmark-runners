#pragma once

// ---------------------------------------------------------------------------
// runner_utils/timer.hh
//
// Lightweight steady-clock timer.
//
// Usage:
//   runner_utils::Timer t;
//   double ms = t.elapsed_ms();
// ---------------------------------------------------------------------------

#include <chrono>

namespace runner_utils
{

struct Timer
{
    using Clock = std::chrono::steady_clock;
    Clock::time_point start_ = Clock::now();

    double elapsed_ms() const
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
    }
};

} // namespace runner_utils
