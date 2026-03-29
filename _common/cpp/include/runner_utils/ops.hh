#pragma once

// ---------------------------------------------------------------------------
// runner_utils/ops.hh
//
// Per-op helpers shared across runners.
//
//   print_progress_op(i, ops)
//     Print progress for ops[i].  Call once at the top of the dispatch loop,
//     before branching on op type.
//
//   validate_op_boolean_binary(i, ops, runner_name)
//     Validate that ops[i] is a well-formed binary boolean op.
//     Throws unsupported_op   if args.size() > 2.
//     Throws std::runtime_error for any other violation.
// ---------------------------------------------------------------------------

#include <runner_utils/progress.hh>

#include <nlohmann/json.hpp>

#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace runner_utils
{

// Exception type for binary-only runners that receive 3+ args.
// Catch this before std::exception and report status = "unsupported".
struct unsupported_op : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// Print a progress line for ops[i].
//   load-mesh  →  #i = loading "path" (name)
//   boolean-*  →  #i = boolean-union #0 #1
inline void print_progress_op(std::size_t i, nlohmann::json const& ops)
{
    auto const& op     = ops[i];
    std::string op_str = op.at("op").get<std::string>();
    std::ostringstream msg;

    if (op_str == "load-mesh")
    {
        std::string const path = op.at("path").get<std::string>();
        std::string const name = op.value("name", "");
        msg << "#" << i << " = loading \"" << path << "\"";
        if (!name.empty())
            msg << " (" << name << ")";
    }
    else
    {
        msg << "#" << i << " = " << op_str;
        auto const args = op.at("args").get<std::vector<int>>();
        for (auto a : args)
            msg << " #" << a;
    }

    print_progress(i, ops.size(), msg.str());
}

// Validate that ops[i] is a well-formed binary boolean op.
// args must be non-empty, at most 2 elements, and each must be < i
// (i.e., must refer to a prior op already in SSA storage).
inline void validate_op_boolean_binary(std::size_t i,
                                       nlohmann::json const& ops,
                                       std::string_view runner_name)
{
    auto const args = ops[i].at("args").get<std::vector<int>>();

    if (args.empty())
        throw std::runtime_error("args must not be empty");

    if (args.size() > 2)
        throw unsupported_op(std::string(runner_name)
                             + " is binary-only; variadic ops (3+ args) are not supported");

    for (auto idx : args)
        if (idx < 0 || static_cast<std::size_t>(idx) >= i)
            throw std::runtime_error("arg index out of range: " + std::to_string(idx));
}

} // namespace runner_utils
