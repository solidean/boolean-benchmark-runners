#pragma once

// ---------------------------------------------------------------------------
// runner_utils/cli.hh
//
// Minimal CLI parsing for benchmark runners.
//
// Provides a base Config (--request / --result) and a parse_args() that
// handles those two flags.  Runners with extra flags should derive from
// Config and implement their own parse_args that handles the extra flags.
//
// Usage:
//   runner_utils::Config cfg = runner_utils::parse_args(argc, argv, "my_runner");
// ---------------------------------------------------------------------------

#include <iostream>
#include <string>

namespace runner_utils
{

struct Config
{
    std::string request_path;
    std::string result_path;
};

inline Config parse_args(int argc, char** argv, char const* exe_name = "runner")
{
    Config cfg;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if ((arg == "--request" || arg == "-r") && i + 1 < argc)
            cfg.request_path = argv[++i];
        else if (arg == "--result" && i + 1 < argc)
            cfg.result_path = argv[++i];
        else
            std::cerr << "[warn] Unknown argument: " << arg << "\n";
    }
    if (cfg.request_path.empty() || cfg.result_path.empty())
    {
        std::cerr << "Usage: " << exe_name << " --request <req.json> --result <res.json>\n";
        std::exit(1);
    }
    return cfg;
}

} // namespace runner_utils
