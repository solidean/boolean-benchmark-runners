// ---------------------------------------------------------------------------
// main.cpp — mesh_arrangements_runner entry point
//
// Execution contract:
//   mesh_arrangements_runner --request <request.json> --result <result.json>
//
// Uses igl::copyleft::cgal::mesh_boolean from libigl v2.6.0 for boolean
// operations on triangulated meshes (Zhou, Grinspun, Zorin, Jacobson —
// "Mesh Arrangements for Solid Geometry", SIGGRAPH 2016).
//
// Requires closed manifold inputs. Self-intersections are handled internally
// via the arrangement construction (that is the paper's contribution).
//
// This runner is binary-only: boolean ops with 3+ args return 'unsupported'.
// The meta-runner generates sequences of binary ops for iterated booleans.
//
// Timing conventions (per-op fields):
//   load-mesh:
//     total_ms   — start before file open, end when Eigen V/F matrices are in SSA
//     io_ms      — file → flat double verts + int indices
//                  (runner_mesh_helpers::loadFromFileIndexed)
//     import_ms  — flat f64 verts+indices → Eigen::MatrixXd/MatrixXi
//   boolean ops:
//     total_ms      — start when native operands are ready, end when result is ready
//     operation_ms  — mesh_boolean call only
//     export_ms     — Eigen matrices → flat f64 verts+indices (excludes disk write)
//
// Empty result (0 verts/faces) is valid — e.g. intersection of disjoint meshes.
// ---------------------------------------------------------------------------

#include <igl/copyleft/cgal/mesh_boolean.h>
#include <nlohmann/json.hpp>
#include <runner_mesh_helpers/io.hh>
#include <runner_utils/cli.hh>
#include <runner_utils/ops.hh>
#include <runner_utils/run_loop.hh>
#include <runner_utils/timer.hh>

#include <Eigen/Core>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>


using json = nlohmann::json;
using runner_utils::print_progress_op;
using runner_utils::Timer;
using runner_utils::validate_op_boolean_binary;

// Native mesh representation: (V, F) Eigen matrices stored by value.
struct EigenMesh
{
    Eigen::MatrixXd V;  // Vx3, double
    Eigen::MatrixXi F;  // Fx3, int
};

// ---------------------------------------------------------------------------
// Conversion helpers
// ---------------------------------------------------------------------------

// flat f64 verts + int indices → EigenMesh
static EigenMesh to_eigen_mesh(std::vector<double> const& verts, std::vector<int> const& tris)
{
    EigenMesh m;
    std::size_t const nv = verts.size() / 3;
    std::size_t const nf = tris.size() / 3;

    m.V.resize(static_cast<Eigen::Index>(nv), 3);
    for (std::size_t i = 0; i < nv; ++i)
    {
        m.V(static_cast<Eigen::Index>(i), 0) = verts[3 * i];
        m.V(static_cast<Eigen::Index>(i), 1) = verts[3 * i + 1];
        m.V(static_cast<Eigen::Index>(i), 2) = verts[3 * i + 2];
    }

    m.F.resize(static_cast<Eigen::Index>(nf), 3);
    for (std::size_t f = 0; f < nf; ++f)
    {
        m.F(static_cast<Eigen::Index>(f), 0) = tris[3 * f];
        m.F(static_cast<Eigen::Index>(f), 1) = tris[3 * f + 1];
        m.F(static_cast<Eigen::Index>(f), 2) = tris[3 * f + 2];
    }

    return m;
}

// EigenMesh → flat f64 verts + int indices
static void from_eigen_mesh(EigenMesh const& m,
                             std::vector<double>& out_verts,
                             std::vector<int>& out_tris)
{
    Eigen::Index const nv = m.V.rows();
    out_verts.resize(static_cast<std::size_t>(nv) * 3);
    for (Eigen::Index i = 0; i < nv; ++i)
    {
        out_verts[static_cast<std::size_t>(i) * 3]     = m.V(i, 0);
        out_verts[static_cast<std::size_t>(i) * 3 + 1] = m.V(i, 1);
        out_verts[static_cast<std::size_t>(i) * 3 + 2] = m.V(i, 2);
    }

    Eigen::Index const nf = m.F.rows();
    out_tris.resize(static_cast<std::size_t>(nf) * 3);
    for (Eigen::Index f = 0; f < nf; ++f)
    {
        out_tris[static_cast<std::size_t>(f) * 3]     = m.F(f, 0);
        out_tris[static_cast<std::size_t>(f) * 3 + 1] = m.F(f, 1);
        out_tris[static_cast<std::size_t>(f) * 3 + 2] = m.F(f, 2);
    }
}

// ---------------------------------------------------------------------------
// Operation helpers
// ---------------------------------------------------------------------------

static EigenMesh apply_op(EigenMesh const& lhs, EigenMesh const& rhs,
                           igl::MeshBooleanType type)
{
    EigenMesh result;
    Eigen::VectorXi J;
    igl::copyleft::cgal::mesh_boolean(lhs.V, lhs.F, rhs.V, rhs.F, type,
                                       result.V, result.F, J);
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
        std::vector<EigenMesh> ssa;
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
                    auto [verts, tris] = runner_mesh_helpers::loadFromFileIndexed(path);
                    double const io_ms = io_timer.elapsed_ms();

                    // import phase — flat f64 → Eigen matrices
                    Timer import_timer;
                    EigenMesh mesh = to_eigen_mesh(verts, tris);
                    double const import_ms = import_timer.elapsed_ms();

                    // disk write — not timed
                    std::string const file_path = out_dir + "/op_" + std::to_string(i) + ".obj";
                    {
                        std::vector<double> wv;
                        std::vector<int> wt;
                        from_eigen_mesh(mesh, wv, wt);
                        runner_mesh_helpers::saveToFileIndexed(file_path, wv, wt);
                    }

                    ssa.push_back(std::move(mesh));

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
                    validate_op_boolean_binary(i, ops, "mesh_arrangements_runner");

                    auto const args = op.at("args").get<std::vector<int>>();

                    igl::MeshBooleanType const type =
                        (op_str == "boolean-union")        ? igl::MESH_BOOLEAN_TYPE_UNION
                      : (op_str == "boolean-intersection") ? igl::MESH_BOOLEAN_TYPE_INTERSECT
                                                           : igl::MESH_BOOLEAN_TYPE_MINUS;

                    EigenMesh const& lhs = ssa[static_cast<std::size_t>(args[0])];
                    EigenMesh const& rhs = ssa[static_cast<std::size_t>(args[1])];

                    // Boolean phase — timed as operation_ms.
                    Timer op_timer;
                    EigenMesh result = apply_op(lhs, rhs, type);
                    double const operation_ms = op_timer.elapsed_ms();

                    // Export phase — timed as export_ms (conversion only).
                    Timer export_timer;
                    std::vector<double> out_verts;
                    std::vector<int> out_tris;
                    from_eigen_mesh(result, out_verts, out_tris);
                    double const export_ms = export_timer.elapsed_ms();

                    // Disk write — not timed.
                    std::string const file_path = out_dir + "/op_" + std::to_string(i) + ".obj";
                    runner_mesh_helpers::saveToFileIndexed(file_path, out_verts, out_tris);

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
                    ssa.push_back(EigenMesh{});
                    failed = true;
                }
            }
            catch (runner_utils::unsupported_op const& e)
            {
                op_res["status"]   = "unsupported";
                op_res["total_ms"] = 0.0;
                op_res["error"]    = e.what();
                failed = true;
            }
            catch (std::exception const& e)
            {
                op_res["status"]   = "crash";
                op_res["total_ms"] = op_total_timer.elapsed_ms();
                op_res["error"]    = e.what();
                ssa.push_back(EigenMesh{});
                failed = true;
            }
            catch (...)
            {
                op_res["status"]   = "crash";
                op_res["total_ms"] = op_total_timer.elapsed_ms();
                op_res["error"]    = "unknown exception";
                ssa.push_back(EigenMesh{});
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
    runner_utils::Config const cfg = runner_utils::parse_args(argc, argv, "mesh_arrangements_runner");
    return runner_utils::run_main_loop(cfg, execute_run);
}
