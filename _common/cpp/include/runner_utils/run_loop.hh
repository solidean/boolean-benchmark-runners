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
// The result file is also flushed periodically during the run loop (every
// ~10s) so that if the process is killed by the meta-runner's watchdog the
// completed cases are still recoverable on disk. Writes go through a
// .tmp file + rename so a kill mid-flush cannot leave a partial JSON.
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

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace runner_utils
{
template <typename ConfigT, typename ExecuteRun>
int run_main_loop(ConfigT const& cfg, ExecuteRun&& execute_run, nlohmann::json extra_res_fields = {})
{
    using json = nlohmann::json;
    using clock = std::chrono::steady_clock;
    constexpr auto flush_interval = std::chrono::seconds(10);

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
    // Atomic result writer: serialize to <result_path>.tmp then rename.
    // Returns true on success; logs and returns false on failure.
    // -----------------------------------------------------------------------
    auto write_result = [&](bool fatal_on_error) -> bool
    {
        std::string const tmp_path = cfg.result_path + ".tmp";
        try
        {
            {
                std::ofstream f(tmp_path);
                if (!f.is_open())
                    throw std::runtime_error("Cannot open result path for writing: " + tmp_path);
                f << res.dump(2) << "\n";
            }
            std::filesystem::rename(tmp_path, cfg.result_path);
            return true;
        }
        catch (std::exception const& e)
        {
            if (fatal_on_error)
                std::cerr << "[fatal] Failed to write result: " << e.what() << "\n";
            else
                std::cerr << "[warn] Failed to flush partial result: " << e.what() << "\n";
            std::error_code ec;
            std::filesystem::remove(tmp_path, ec);
            return false;
        }
    };

    // -----------------------------------------------------------------------
    // Execute runs
    // -----------------------------------------------------------------------
    if (!req.contains("runs") || !req["runs"].is_array())
    {
        std::cerr << "[fatal] Request missing 'runs' array.\n";
        write_result(false);
        return 1;
    }

    auto last_flush = clock::now();
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

        auto const now = clock::now();
        if (now - last_flush >= flush_interval)
        {
            write_result(false);
            last_flush = clock::now();
        }
    }

    // -----------------------------------------------------------------------
    // Final write
    // -----------------------------------------------------------------------
    if (!write_result(true))
        return 1;

    return 0;
}

} // namespace runner_utils
