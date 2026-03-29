// ---------------------------------------------------------------------------
// main.cpp — mcut_runner entry point
//
// Execution contract:
//   mcut_runner --request <request.json> --result <result.json>
//
// Uses mcut v1.3.0 C API for boolean operations on closed, manifold meshes.
//
// A single McContext (g_ctx) is created once at startup and reused across all
// runs.  mcut has no persistent native mesh type, so the SSA stores flat
// indexed-f64 meshes (McuMesh) that map directly onto mcut's C API arrays.
//
// Requires closed, manifold, non-self-intersecting inputs.
// A non-MC_NO_ERROR result from mcDispatch is reported as invalid_input.
// Zero connected components after dispatch is also invalid_input.
//
// This runner is binary-only: boolean ops with 3+ args return 'unsupported'.
//
// Timing conventions (per-op fields):
//   load-mesh:
//     total_ms   — start before file open, end when McuMesh is in SSA
//     io_ms      — file → flat double verts + int indices
//                  (runner_mesh_helpers::loadFromFileIndexed)
//     import_ms  — int-to-uint32_t index cast + non-empty sanity check;
//                  near-zero cost, included for consistency
//   boolean ops:
//     total_ms      — start when operands are ready, end when result is in SSA
//     operation_ms  — mcDispatch() only
//     export_ms     — mcGetConnectedComponents (TYPE_ALL) +
//                     mcGetConnectedComponentData (TYPE filter to FRAGMENT +
//                      VERTEX_DOUBLE + FACE_TRIANGULATION) +
//                     CC merge with index offsetting; excludes disk write
// ---------------------------------------------------------------------------

#include <mcut/mcut.h>
#include <nlohmann/json.hpp>
#include <runner_mesh_helpers/io.hh>
#include <runner_utils/cli.hh>
#include <runner_utils/ops.hh>
#include <runner_utils/run_loop.hh>
#include <runner_utils/timer.hh>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;
using runner_utils::print_progress_op;
using runner_utils::Timer;
using runner_utils::validate_op_boolean_binary;

// ---------------------------------------------------------------------------
// Global mcut context — created once, reused across all runs
// ---------------------------------------------------------------------------

static McContext g_ctx = MC_NULL_HANDLE;

// ---------------------------------------------------------------------------
// SSA mesh type
//
// Flat indexed-f64 representation.  Exactly what mcut expects on input and
// what we reconstruct from its output.  No kernel promotion needed.
// ---------------------------------------------------------------------------

struct McuMesh
{
    std::vector<double>   verts;   // [x0,y0,z0, x1,y1,z1, ...]
    std::vector<uint32_t> indices; // [i0,i1,i2, ...] triangle triplets (0-based)
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Throw a descriptive error if res != MC_NO_ERROR.
static void mc_check(McResult res, char const* where)
{
    if (res != MC_NO_ERROR)
        throw std::runtime_error(std::string(where) + " failed (McResult=" + std::to_string(res) + ")");
}

// Convert McuMesh indices (uint32_t) to int for runner_mesh_helpers::saveToFileIndexed.
static void save_mcu_mesh(McuMesh const& m, std::string const& path)
{
    std::vector<int> idx_i(m.indices.begin(), m.indices.end());
    runner_mesh_helpers::saveToFileIndexed(path, m.verts, idx_i);
}

// ---------------------------------------------------------------------------
// Boolean operation helpers
// ---------------------------------------------------------------------------

enum class OpKind
{
    Union,
    Intersection,
    Difference
};

// Boolean filter flags for each operation type.
//
// mcut terminology:
//   fragment  — a piece of the source mesh (A) after cutting by B
//   patch     — a piece of the cut mesh (B) after cutting by A
//
//   SEALING_OUTSIDE — fragment is the part of A geometrically outside B,
//                     sealed with the patches of B that face outward from A
//   SEALING_INSIDE  — fragment is the part of A geometrically inside B,
//                     sealed with the patches of B that face inward from A
//   PATCH_OUTSIDE   — patches of B that lie outside A
//   PATCH_INSIDE    — patches of B that lie inside A
//
// Boolean mappings (source=A, cut-mesh=B).
//
// Both a LOCATION flag and a SEALING flag are required; without an explicit
// location flag MCUT does not generate fragment CCs at all.
//
//   union        (A∪B):  A above/outside B, sealed exterior → LOCATION_ABOVE | SEALING_OUTSIDE
//   intersection (A∩B):  A below/inside  B, sealed interior → LOCATION_BELOW | SEALING_INSIDE
//   difference   (A−B):  A above/outside B, sealed interior → LOCATION_ABOVE | SEALING_INSIDE
//
// "Sealed interior" for difference: the hole carved into A by B is capped with
// the inward-facing surface of B, giving a closed solid.
static McFlags filter_flags(OpKind kind)
{
    switch (kind)
    {
    case OpKind::Union:
        return MC_DISPATCH_FILTER_FRAGMENT_LOCATION_ABOVE | MC_DISPATCH_FILTER_FRAGMENT_SEALING_OUTSIDE;
    case OpKind::Intersection:
        return MC_DISPATCH_FILTER_FRAGMENT_LOCATION_BELOW | MC_DISPATCH_FILTER_FRAGMENT_SEALING_INSIDE;
    case OpKind::Difference:
        return MC_DISPATCH_FILTER_FRAGMENT_LOCATION_ABOVE | MC_DISPATCH_FILTER_FRAGMENT_SEALING_INSIDE;
    }
    return 0; // unreachable
}

// Execute one boolean dispatch and return {result, operation_ms}.
// The caller times export separately (CC extraction + merge).
static McResult dispatch_bool(McuMesh const& lhs, McuMesh const& rhs,
                               OpKind kind, double& out_operation_ms)
{
    // Build per-face size arrays (all triangles → all 3).
    McUint32 const lhs_face_count = static_cast<McUint32>(lhs.indices.size() / 3);
    McUint32 const rhs_face_count = static_cast<McUint32>(rhs.indices.size() / 3);
    std::vector<McUint32> lhs_fsizes(lhs_face_count, 3u);
    std::vector<McUint32> rhs_fsizes(rhs_face_count, 3u);

    McUint32 const lhs_vert_count = static_cast<McUint32>(lhs.verts.size() / 3);
    McUint32 const rhs_vert_count = static_cast<McUint32>(rhs.verts.size() / 3);

    Timer op_timer;
    McResult const res = mcDispatch(
        g_ctx,
        MC_DISPATCH_VERTEX_ARRAY_DOUBLE | filter_flags(kind),
        // source mesh (A)
        lhs.verts.data(), lhs.indices.data(), lhs_fsizes.data(),
        lhs_vert_count, lhs_face_count,
        // cut mesh (B)
        rhs.verts.data(), rhs.indices.data(), rhs_fsizes.data(),
        rhs_vert_count, rhs_face_count);
    out_operation_ms = op_timer.elapsed_ms();

    return res;
}

// Extract fragment connected components from the last dispatch and merge them
// into a single McuMesh.  Returns the merged mesh (export_ms is measured by caller).
//
// Queries all CCs (TYPE_ALL) then filters to FRAGMENT type only; the dispatch
// flags already ensure only the desired fragment is generated, but seam and
// input-copy CCs may also be present and must be skipped.
static McuMesh extract_and_merge()
{
    McUint32 num_cc = 0;
    mc_check(
        mcGetConnectedComponents(g_ctx, MC_CONNECTED_COMPONENT_TYPE_ALL, 0, nullptr, &num_cc),
        "mcGetConnectedComponents (count)");

    if (num_cc == 0)
        throw std::runtime_error("zero connected components — inputs may be non-intersecting, "
                                 "non-manifold, or otherwise invalid");

    std::vector<McConnectedComponent> ccs(num_cc);
    mc_check(
        mcGetConnectedComponents(g_ctx, MC_CONNECTED_COMPONENT_TYPE_ALL,
                                 num_cc, ccs.data(), nullptr),
        "mcGetConnectedComponents (fill)");

    McuMesh result;

    for (McConnectedComponent cc : ccs)
    {
        // --- skip non-fragment CCs (seams, input copies, patches) ---
        McConnectedComponentType cc_type = MC_CONNECTED_COMPONENT_TYPE_ALL;
        mc_check(
            mcGetConnectedComponentData(g_ctx, cc, MC_CONNECTED_COMPONENT_DATA_TYPE,
                                        sizeof(McConnectedComponentType), &cc_type, nullptr),
            "mcGetConnectedComponentData TYPE");
        if (cc_type != MC_CONNECTED_COMPONENT_TYPE_FRAGMENT)
            continue;

        // --- vertices (double precision) ---
        McSize vbytes = 0;
        mc_check(
            mcGetConnectedComponentData(g_ctx, cc, MC_CONNECTED_COMPONENT_DATA_VERTEX_DOUBLE,
                                        0, nullptr, &vbytes),
            "mcGetConnectedComponentData VERTEX_DOUBLE (size)");

        std::vector<double> cc_verts(vbytes / sizeof(double));
        mc_check(
            mcGetConnectedComponentData(g_ctx, cc, MC_CONNECTED_COMPONENT_DATA_VERTEX_DOUBLE,
                                        vbytes, cc_verts.data(), nullptr),
            "mcGetConnectedComponentData VERTEX_DOUBLE (data)");

        // --- triangulated faces ---
        McSize fbytes = 0;
        mc_check(
            mcGetConnectedComponentData(g_ctx, cc, MC_CONNECTED_COMPONENT_DATA_FACE_TRIANGULATION,
                                        0, nullptr, &fbytes),
            "mcGetConnectedComponentData FACE_TRIANGULATION (size)");

        std::vector<McUint32> cc_tris(fbytes / sizeof(McUint32));
        mc_check(
            mcGetConnectedComponentData(g_ctx, cc, MC_CONNECTED_COMPONENT_DATA_FACE_TRIANGULATION,
                                        fbytes, cc_tris.data(), nullptr),
            "mcGetConnectedComponentData FACE_TRIANGULATION (data)");

        // --- merge into result (offset indices by current vertex count) ---
        McUint32 const offset = static_cast<McUint32>(result.verts.size() / 3);
        result.verts.insert(result.verts.end(), cc_verts.begin(), cc_verts.end());
        for (McUint32 idx : cc_tris)
            result.indices.push_back(idx + offset);
    }

    mc_check(
        mcReleaseConnectedComponents(g_ctx, num_cc, ccs.data()),
        "mcReleaseConnectedComponents");

    if (result.verts.empty())
        throw std::runtime_error("no fragment components found — boolean result is empty");

    return result;
}

// ---------------------------------------------------------------------------
// Per-run execution
// ---------------------------------------------------------------------------

static json execute_run(runner_utils::Config const& /*cfg*/, json const& run_req)
{
    json run_res;
    Timer total_timer;
    json ops_result = json::array();
    bool failed = false;

    try
    {
        auto const& ops = run_req.at("operations");
        std::string const out_dir = run_req.at("out_dir").get<std::string>();
        if (out_dir.empty())
            throw std::runtime_error("out_dir is required but was empty");

        // SSA storage: ssa[i] = result of op i.
        std::vector<McuMesh> ssa;
        ssa.reserve(ops.size());

        for (std::size_t i = 0; i < ops.size(); ++i)
        {
            auto const& op = ops[i];
            std::string const op_str = op.at("op").get<std::string>();
            json op_res;
            Timer op_total_timer;

            print_progress_op(i, ops);

            try
            {
                if (op_str == "load-mesh")
                {
                    std::string const path = op.at("path").get<std::string>();

                    // io phase — file → flat double verts + int indices
                    Timer io_timer;
                    auto [verts_d, tris_i] = runner_mesh_helpers::loadFromFileIndexed(path);
                    double const io_ms = io_timer.elapsed_ms();

                    // import phase — cast int→uint32_t + sanity check
                    Timer import_timer;
                    if (verts_d.empty() || tris_i.empty())
                        throw std::runtime_error("loaded mesh is empty (no vertices or faces)");
                    McuMesh loaded;
                    loaded.verts = std::move(verts_d);
                    loaded.indices.reserve(tris_i.size());
                    for (int idx : tris_i)
                        loaded.indices.push_back(static_cast<uint32_t>(idx));
                    double const import_ms = import_timer.elapsed_ms();

                    // disk write — not timed
                    std::string const file_path = out_dir + "/op_" + std::to_string(i) + ".obj";
                    save_mcu_mesh(loaded, file_path);

                    ssa.push_back(std::move(loaded));

                    op_res["status"]    = "success";
                    op_res["total_ms"]  = op_total_timer.elapsed_ms();
                    op_res["io_ms"]     = io_ms;
                    op_res["import_ms"] = import_ms;
                    op_res["file"]      = file_path;
                }
                else if (op_str == "boolean-union"
                      || op_str == "boolean-intersection"
                      || op_str == "boolean-difference")
                {
                    validate_op_boolean_binary(i, ops, "mcut_runner");

                    auto const args = op.at("args").get<std::vector<int>>();

                    OpKind const kind = (op_str == "boolean-union")        ? OpKind::Union
                                      : (op_str == "boolean-intersection") ? OpKind::Intersection
                                                                           : OpKind::Difference;

                    McuMesh const& lhs = ssa[args[0]];
                    McuMesh const& rhs = ssa[args[1]];

                    // Boolean phase — timed as operation_ms.
                    double operation_ms = 0.0;
                    McResult const dispatch_res = dispatch_bool(lhs, rhs, kind, operation_ms);

                    if (dispatch_res != MC_NO_ERROR)
                    {
                        op_res["status"]       = "invalid_input";
                        op_res["total_ms"]     = op_total_timer.elapsed_ms();
                        op_res["operation_ms"] = operation_ms;
                        op_res["error"]        = "mcDispatch returned error (McResult="
                                                 + std::to_string(dispatch_res)
                                                 + ") — inputs may be non-manifold, "
                                                   "self-intersecting, or otherwise invalid";
                        ssa.push_back(McuMesh{});
                        ops_result.push_back(op_res);
                        failed = true;
                        break;
                    }

                    // Export phase — timed as export_ms.
                    Timer export_timer;
                    McuMesh result = extract_and_merge();
                    double const export_ms = export_timer.elapsed_ms();

                    // Disk write — not timed.
                    std::string const file_path = out_dir + "/op_" + std::to_string(i) + ".obj";
                    save_mcu_mesh(result, file_path);

                    ssa.push_back(std::move(result));

                    op_res["status"]       = "success";
                    op_res["total_ms"]     = op_total_timer.elapsed_ms();
                    op_res["operation_ms"] = operation_ms;
                    op_res["export_ms"]    = export_ms;
                    op_res["file"]         = file_path;
                }
                else
                {
                    op_res["status"]   = "unsupported";
                    op_res["total_ms"] = 0.0;
                    op_res["error"]    = "unknown op: " + op_str;
                    ssa.push_back(McuMesh{});
                    failed = true;
                }
            }
            catch (runner_utils::unsupported_op const& e)
            {
                op_res["status"]   = "unsupported";
                op_res["total_ms"] = 0.0;
                op_res["error"]    = e.what();
                ops_result.push_back(op_res);
                failed = true;
            }
            catch (std::exception const& e)
            {
                op_res["status"]   = "crash";
                op_res["total_ms"] = op_total_timer.elapsed_ms();
                op_res["error"]    = e.what();
                failed = true;
            }
            catch (...)
            {
                op_res["status"]   = "crash";
                op_res["total_ms"] = op_total_timer.elapsed_ms();
                op_res["error"]    = "unknown exception";
                failed = true;
            }

            ops_result.push_back(op_res);

            if (failed)
                break; // fail-fast
        }

        run_res["status"] = failed ? "crash" : "success";
    }
    catch (std::exception const& e)
    {
        run_res["status"] = "crash";
        run_res["error"]  = e.what();
    }
    catch (...)
    {
        run_res["status"] = "crash";
        run_res["error"]  = "unknown exception";
    }

    run_res["duration_ms"] = total_timer.elapsed_ms();
    run_res["ops"]         = ops_result;
    return run_res;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
    runner_utils::Config const cfg = runner_utils::parse_args(argc, argv, "mcut_runner");

    McResult const create_res = mcCreateContext(&g_ctx, MC_NULL_HANDLE);
    if (create_res != MC_NO_ERROR)
    {
        std::cerr << "[fatal] mcCreateContext failed (McResult=" << create_res << ")\n";
        return 1;
    }

    int const exit_code = runner_utils::run_main_loop(cfg, execute_run);

    mcReleaseContext(g_ctx);
    return exit_code;
}
