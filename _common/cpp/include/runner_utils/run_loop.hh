#pragma once

// ---------------------------------------------------------------------------
// runner_utils/run_loop.hh
//
// Main request/response loop for benchmark runners.
//
// run_main_loop() handles:
//   - loading the request JSON from cfg.request_path
//   - building the result envelope  (kind / version / id / runs)
//   - iterating over runs and calling execute_run for each
//   - printing per-run progress to stderr
//   - writing the result JSON to cfg.result_path
//
// Template parameters:
//   ConfigT     — must be (or derive from) runner_utils::Config
//   ExecuteRun  — callable: (ConfigT const& cfg, json const& run_entry) -> json
//                 The returned json must contain at least "status" and "duration_ms".
//
// extra_res_fields (optional) — a json object whose key/value pairs are merged
//   into the result envelope before the runs are executed.  Use this for
//   runner-specific envelope fields (e.g. {"regularize": true} for the nef runner).
//
// Usage:
//   return runner_utils::run_main_loop(cfg, execute_run);
//   return runner_utils::run_main_loop(cfg, execute_run, {{"regularize", cfg.regularize}});
// ---------------------------------------------------------------------------

#include "cli.hh"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>
#include <string>

namespace runner_utils
{
template <typename ConfigT, typename ExecuteRun>
int run_main_loop(ConfigT const& cfg, ExecuteRun&& execute_run, nlohmann::json extra_res_fields = {})
{
    using json = nlohmann::json;

    // -----------------------------------------------------------------------
    // Load request
    // -----------------------------------------------------------------------
    json req;
    try
    {
        std::ifstream f(cfg.request_path);
        if (!f.is_open())
            throw std::runtime_error("Cannot open request file: " + cfg.request_path);
        req = json::parse(f);
    }
    catch (std::exception const& e)
    {
        std::cerr << "[fatal] Failed to parse request: " << e.what() << "\n";
        return 1;
    }

    // -----------------------------------------------------------------------
    // Build result envelope
    // -----------------------------------------------------------------------
    json res;
    res["kind"] = req.value("kind", "boolean-benchmark");
    res["version"] = req.value("version", 1);
    res["id"] = req.value("id", "");
    for (auto& [key, val] : extra_res_fields.items())
        res[key] = val;
    res["runs"] = json::array();

    // -----------------------------------------------------------------------
    // Execute runs
    // -----------------------------------------------------------------------
    if (!req.contains("runs") || !req["runs"].is_array())
    {
        std::cerr << "[fatal] Request missing 'runs' array.\n";
        try
        {
            std::ofstream f(cfg.result_path);
            f << res.dump(2) << "\n";
        }
        catch (...)
        {
        }
        return 1;
    }

    for (auto& run_entry : req["runs"])
    {
        std::string const run_id = run_entry.at("case_id").get<std::string>();
        std::size_t const op_count = run_entry.contains("operations") && run_entry["operations"].is_array()
                                       ? run_entry["operations"].size()
                                       : 0;
        std::cerr << "[run] " << run_id << " (" << op_count << " ops)\n";
        json run_result = execute_run(cfg, run_entry);
        run_result["case_id"] = run_id;
        std::string const status = run_result["status"];
        std::cerr << "      → " << status << "  (" << run_result["duration_ms"].get<double>() << " ms total)\n";
        res["runs"].push_back(std::move(run_result));
    }

    // -----------------------------------------------------------------------
    // Write result
    // -----------------------------------------------------------------------
    try
    {
        std::ofstream f(cfg.result_path);
        if (!f.is_open())
            throw std::runtime_error("Cannot open result path for writing: " + cfg.result_path);
        f << res.dump(2) << "\n";
    }
    catch (std::exception const& e)
    {
        std::cerr << "[fatal] Failed to write result: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

} // namespace runner_utils
