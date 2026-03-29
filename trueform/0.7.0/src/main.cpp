// ---------------------------------------------------------------------------
// main.cpp — trueform_runner entry point
//
// Execution contract:
//   trueform_runner --request <request.json> --result <result.json>
//
// Uses tf::make_boolean for union, intersection, and difference on
// triangular polygon meshes.
//
// Input meshes are loaded as indexed f64 (via runner_mesh_helpers), then
// cast to f32 for trueform (which uses float internally).
//
// This runner is binary-only: boolean ops with 3+ args return 'unsupported'.
// The meta-runner generates sequences of binary ops for iterated booleans.
//
// Timing conventions (per-op fields):
//   load-mesh:
//     total_ms   — start before file open, end when polygon is in SSA
//     io_ms      — file → flat double verts + int indices
//                  (runner_mesh_helpers::loadFromFileIndexed)
//     import_ms  — f64→f32 cast + blocked_buffer/points_buffer construction
//                  + tf::make_polygons_buffer
//   boolean ops:
//     total_ms      — start when native operands are ready, end when result is ready
//     operation_ms  — tf::make_boolean() call only
//     export_ms     — points_buffer/faces_buffer data extraction + f32→f64 cast
//                     (excludes disk write)
// ---------------------------------------------------------------------------

#include <nlohmann/json.hpp>
#include <runner_mesh_helpers/io.hh>
#include <runner_utils/cli.hh>
#include <runner_utils/ops.hh>
#include <runner_utils/run_loop.hh>
#include <runner_utils/timer.hh>
#include <trueform/trueform.hpp>

#include <stdexcept>
#include <string>
#include <vector>


using json = nlohmann::json;
using runner_utils::print_progress_op;
using runner_utils::Timer;
using runner_utils::validate_op_boolean_binary;

// SSA storage type: owning triangular mesh buffer (int indices, float coords, 3D).
using TfMeshBuf = tf::polygons_buffer<int, float, 3, 3>;


// ---------------------------------------------------------------------------
// Operation helpers
// ---------------------------------------------------------------------------

enum class OpKind
{
    Union,
    Intersection,
    Difference
};

static tf::boolean_op to_tf_op(OpKind kind)
{
    switch (kind)
    {
    case OpKind::Union: return tf::boolean_op::merge;
    case OpKind::Intersection: return tf::boolean_op::intersection;
    case OpKind::Difference: return tf::boolean_op::left_difference;
    default: throw std::logic_error("unreachable");
    }
}


// ---------------------------------------------------------------------------
// Mesh helpers
// ---------------------------------------------------------------------------

// Build an owning TfMeshBuf from flat f64 verts and int indices.
static TfMeshBuf make_mesh_buf(std::vector<double> const& verts_f64, std::vector<int> const& tris)
{
    tf::blocked_buffer<int, 3> fb;
    for (std::size_t i = 0; i + 2 < tris.size(); i += 3)
        fb.emplace_back(tris[i], tris[i + 1], tris[i + 2]);

    tf::points_buffer<float, 3> pb;
    for (std::size_t i = 0; i + 2 < verts_f64.size(); i += 3)
        pb.emplace_back(static_cast<float>(verts_f64[i]), static_cast<float>(verts_f64[i + 1]),
                        static_cast<float>(verts_f64[i + 2]));

    return tf::make_polygons_buffer(std::move(fb), std::move(pb));
}

// Extract flat f64 verts and int indices from a TfMeshBuf.
static void extract_indexed(TfMeshBuf const& buf, std::vector<double>& out_verts, std::vector<int>& out_tris)
{
    auto const& pts = buf.points_buffer().data_buffer();
    out_verts.assign(pts.begin(), pts.end()); // float → double implicit

    auto const& fcs = buf.faces_buffer().data_buffer();
    out_tris.assign(fcs.begin(), fcs.end());
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
        std::vector<TfMeshBuf> ssa;
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

                    // io phase — file → flat f64 verts + int indices
                    Timer io_timer;
                    auto [verts_f64, tris] = runner_mesh_helpers::loadFromFileIndexed(path);
                    double const io_ms = io_timer.elapsed_ms();

                    // import phase — f64→f32 cast + build TfMeshBuf
                    Timer import_timer;
                    TfMeshBuf polys = make_mesh_buf(verts_f64, tris);
                    double const import_ms = import_timer.elapsed_ms();

                    // disk write — not timed
                    std::string const file_path = out_dir + "/op_" + std::to_string(i) + ".obj";
                    {
                        std::vector<double> out_verts;
                        std::vector<int> out_tris;
                        extract_indexed(polys, out_verts, out_tris);
                        runner_mesh_helpers::saveToFileIndexed(file_path, out_verts, out_tris);
                    }

                    ssa.push_back(std::move(polys));

                    op_res["status"] = "success";
                    op_res["total_ms"] = op_total_timer.elapsed_ms();
                    op_res["io_ms"] = io_ms;
                    op_res["import_ms"] = import_ms;
                    op_res["file"] = file_path;
                }
                else if (op_str == "boolean-union" || op_str == "boolean-intersection" || op_str == "boolean-difference")
                {
                    validate_op_boolean_binary(i, ops, "trueform runner");

                    auto const args = op.at("args").get<std::vector<int>>();

                    OpKind const kind = (op_str == "boolean-union")        ? OpKind::Union
                                      : (op_str == "boolean-intersection") ? OpKind::Intersection
                                                                           : OpKind::Difference;

                    TfMeshBuf const& lhs = ssa[static_cast<std::size_t>(args[0])];
                    TfMeshBuf const& rhs = ssa[static_cast<std::size_t>(args[1])];

                    // Boolean phase — timed as operation_ms.
                    Timer op_timer;
                    auto [result, labels] = tf::make_boolean(lhs.polygons(), rhs.polygons(), to_tf_op(kind));
                    double const operation_ms = op_timer.elapsed_ms();

                    // Export phase — timed as export_ms (conversion only).
                    Timer export_timer;
                    std::vector<double> out_verts;
                    std::vector<int> out_tris;
                    extract_indexed(result, out_verts, out_tris);
                    double const export_ms = export_timer.elapsed_ms();

                    // Disk write — not timed.
                    std::string const file_path = out_dir + "/op_" + std::to_string(i) + ".obj";
                    runner_mesh_helpers::saveToFileIndexed(file_path, out_verts, out_tris);

                    ssa.push_back(std::move(result));

                    op_res["status"] = "success";
                    op_res["total_ms"] = op_total_timer.elapsed_ms();
                    op_res["operation_ms"] = operation_ms;
                    op_res["export_ms"] = export_ms;
                    op_res["file"] = file_path;
                }
                else
                {
                    op_res["status"] = "unsupported";
                    op_res["total_ms"] = 0.0;
                    op_res["error"] = "unknown op: " + op_str;
                    ssa.push_back(TfMeshBuf{});
                    failed = true;
                }
            }
            catch (runner_utils::unsupported_op const& e)
            {
                op_res["status"] = "unsupported";
                op_res["total_ms"] = 0.0;
                op_res["error"] = e.what();
                ops_result.push_back(op_res);
                failed = true;
            }
            catch (std::exception const& e)
            {
                op_res["status"] = "crash";
                op_res["total_ms"] = op_total_timer.elapsed_ms();
                op_res["error"] = e.what();
                failed = true;
            }
            catch (...)
            {
                op_res["status"] = "crash";
                op_res["total_ms"] = op_total_timer.elapsed_ms();
                op_res["error"] = "unknown exception";
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
        run_res["error"] = e.what();
    }
    catch (...)
    {
        run_res["status"] = "crash";
        run_res["error"] = "unknown exception";
    }

    run_res["duration_ms"] = total_timer.elapsed_ms();
    run_res["ops"] = ops_result;
    return run_res;
}


// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
    runner_utils::Config const cfg = runner_utils::parse_args(argc, argv, "trueform_runner");
    return runner_utils::run_main_loop(cfg, execute_run);
}
