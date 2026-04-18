// ---------------------------------------------------------------------------
// main.cpp — geogram_runner entry point
//
// Execution contract:
//   geogram_runner --request <request.json> --result <result.json>
//
// Uses GEO::mesh_union / mesh_intersection / mesh_difference from
// geogram/mesh/mesh_surface_intersection.h for boolean operations on
// triangulated meshes.
//
// Requires closed, manifold, non-self-intersecting inputs.
//
// This runner is binary-only: boolean ops with 3+ args return 'unsupported'.
// The meta-runner generates sequences of binary ops for iterated booleans.
//
// Timing conventions (per-op fields):
//   load-mesh:
//     total_ms   — start before file open, end when GEO::Mesh is in SSA
//     io_ms      — file → flat double verts + int indices
//                  (runner_mesh_helpers::loadFromFileIndexed)
//     import_ms  — flat f64 verts+indices → GEO::Mesh (vertex/facet population)
//   boolean ops:
//     total_ms      — start when native operands are ready, end when result is ready
//     operation_ms  — mesh_union/intersection/difference call only
//     export_ms     — GEO::Mesh → flat f64 verts+indices (excludes disk write)
//
// Empty result (0 verts/faces) is valid — e.g. intersection of disjoint meshes.
// ---------------------------------------------------------------------------

#include <geogram/basic/common.h>
#include <geogram/mesh/mesh.h>
#include <geogram/mesh/mesh_surface_intersection.h>
#include <nlohmann/json.hpp>
#include <runner_mesh_helpers/io.hh>
#include <runner_utils/cli.hh>
#include <runner_utils/ops.hh>
#include <runner_utils/run_loop.hh>
#include <runner_utils/timer.hh>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>


using json = nlohmann::json;
using runner_utils::print_progress_op;
using runner_utils::Timer;
using runner_utils::validate_op_boolean_binary;

// ---------------------------------------------------------------------------
// Operation helpers
// ---------------------------------------------------------------------------

enum class OpKind
{
    Union,
    Intersection,
    Difference
};

// Returns a newly-allocated result mesh.  Caller owns it.
// Throws on geogram error.
static std::unique_ptr<GEO::Mesh> apply_op(GEO::Mesh const& lhs, GEO::Mesh const& rhs, OpKind kind)
{
    auto result = std::make_unique<GEO::Mesh>(3, false);
    switch (kind)
    {
    case OpKind::Union: GEO::mesh_union(*result, lhs, rhs); break;
    case OpKind::Intersection: GEO::mesh_intersection(*result, lhs, rhs); break;
    case OpKind::Difference: GEO::mesh_difference(*result, lhs, rhs); break;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Conversion helpers
// ---------------------------------------------------------------------------

// flat f64 verts + int indices → GEO::Mesh
static std::unique_ptr<GEO::Mesh> to_geo_mesh(std::vector<double> const& verts, std::vector<int> const& tris)
{
    auto m = std::make_unique<GEO::Mesh>(3, false);
    m->vertices.set_double_precision();

    GEO::index_t const n_verts = static_cast<GEO::index_t>(verts.size() / 3);
    m->vertices.create_vertices(n_verts);
    for (GEO::index_t i = 0; i < n_verts; ++i)
    {
        double* p = m->vertices.point_ptr(i);
        p[0] = verts[3 * i];
        p[1] = verts[3 * i + 1];
        p[2] = verts[3 * i + 2];
    }

    GEO::index_t const n_tris = static_cast<GEO::index_t>(tris.size() / 3);
    m->facets.create_triangles(n_tris);
    for (GEO::index_t f = 0; f < n_tris; ++f)
    {
        m->facets.set_vertex(f, 0, static_cast<GEO::index_t>(tris[3 * f]));
        m->facets.set_vertex(f, 1, static_cast<GEO::index_t>(tris[3 * f + 1]));
        m->facets.set_vertex(f, 2, static_cast<GEO::index_t>(tris[3 * f + 2]));
    }

    return m;
}

// GEO::Mesh → flat f64 verts + int indices
static void from_geo_mesh(GEO::Mesh const& m, std::vector<double>& out_verts, std::vector<int>& out_tris)
{
    GEO::index_t const nv = m.vertices.nb();
    out_verts.resize(nv * 3);
    for (GEO::index_t i = 0; i < nv; ++i)
    {
        double const* p = m.vertices.point_ptr(i);
        out_verts[3 * i] = p[0];
        out_verts[3 * i + 1] = p[1];
        out_verts[3 * i + 2] = p[2];
    }

    GEO::index_t const nf = m.facets.nb();
    out_tris.resize(nf * 3);
    for (GEO::index_t f = 0; f < nf; ++f)
    {
        out_tris[3 * f] = static_cast<int>(m.facets.vertex(f, 0));
        out_tris[3 * f + 1] = static_cast<int>(m.facets.vertex(f, 1));
        out_tris[3 * f + 2] = static_cast<int>(m.facets.vertex(f, 2));
    }
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

        // SSA storage: ssa[i] = result of op i (owned).
        std::vector<std::unique_ptr<GEO::Mesh>> ssa;
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

                    // import phase — flat f64 → GEO::Mesh
                    Timer import_timer;
                    auto geo_mesh = to_geo_mesh(verts, tris);
                    double const import_ms = import_timer.elapsed_ms();

                    // disk write — not timed
                    std::string const file_path = out_dir + "/op_" + std::to_string(i) + ".obj";
                    {
                        std::vector<double> wv;
                        std::vector<int> wt;
                        from_geo_mesh(*geo_mesh, wv, wt);
                        runner_mesh_helpers::saveToFileIndexed(file_path, wv, wt);
                    }

                    ssa.push_back(std::move(geo_mesh));

                    op_res["status"] = "success";
                    op_res["total_ms"] = op_total_timer.elapsed_ms();
                    op_res["io_ms"] = io_ms;
                    op_res["import_ms"] = import_ms;
                    op_res["file"] = file_path;
                }
                else if (op_str == "boolean-union" || op_str == "boolean-intersection" || op_str == "boolean-difference")
                {
                    validate_op_boolean_binary(i, ops, "geogram runner");

                    auto const args = op.at("args").get<std::vector<int>>();

                    OpKind const kind = (op_str == "boolean-union")        ? OpKind::Union
                                      : (op_str == "boolean-intersection") ? OpKind::Intersection
                                                                           : OpKind::Difference;

                    GEO::Mesh const& lhs = *ssa[static_cast<std::size_t>(args[0])];
                    GEO::Mesh const& rhs = *ssa[static_cast<std::size_t>(args[1])];

                    // Boolean phase — timed as operation_ms.
                    Timer op_timer;
                    auto result = apply_op(lhs, rhs, kind);
                    double const operation_ms = op_timer.elapsed_ms();

                    // Export phase — timed as export_ms (conversion only).
                    Timer export_timer;
                    std::vector<double> out_verts;
                    std::vector<int> out_tris;
                    from_geo_mesh(*result, out_verts, out_tris);
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
                    ssa.push_back(std::make_unique<GEO::Mesh>(3, false));
                    failed = true;
                }
            }
            catch (runner_utils::unsupported_op const& e)
            {
                op_res["status"] = "unsupported";
                op_res["total_ms"] = 0.0;
                op_res["error"] = e.what();
                failed = true;
            }
            catch (std::exception const& e)
            {
                op_res["status"] = "crash";
                op_res["total_ms"] = op_total_timer.elapsed_ms();
                op_res["error"] = e.what();
                ssa.push_back(std::make_unique<GEO::Mesh>(3, false));
                failed = true;
            }
            catch (...)
            {
                op_res["status"] = "crash";
                op_res["total_ms"] = op_total_timer.elapsed_ms();
                op_res["error"] = "unknown exception";
                ssa.push_back(std::make_unique<GEO::Mesh>(3, false));
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
    GEO::initialize(GEO::GEOGRAM_INSTALL_NONE);
    runner_utils::Config const cfg = runner_utils::parse_args(argc, argv, "geogram_runner");
    return runner_utils::run_main_loop(cfg, execute_run);
}
