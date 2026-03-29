#pragma once

// ---------------------------------------------------------------------------
// runner_utils/progress.hh
//
// Console progress helpers.
//
// Usage:
//   runner_utils::print_progress(i, total, "doing something");
//   // prints: [HH:MM:SS.mmm] [i+1/total] doing something
// ---------------------------------------------------------------------------

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace runner_utils
{

inline std::string current_timestamp()
{
    auto now = std::chrono::system_clock::now();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm_buf);
    std::ostringstream oss;
    oss << buf << "." << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}

inline void print_progress(std::size_t idx, std::size_t total, std::string const& msg)
{
    int const w = static_cast<int>(std::to_string(total).size());
    std::cout << "[" << current_timestamp() << "] "
              << "[" << std::setw(w) << std::setfill('0') << (idx + 1)
              << "/" << total << "] "
              << msg << "\n";
    std::cout.flush();
}

} // namespace runner_utils
