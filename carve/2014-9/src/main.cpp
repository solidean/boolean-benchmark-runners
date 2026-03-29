// ---------------------------------------------------------------------------
// main.cpp — carve_runner entry point
//
// Execution contract:
//   carve_runner --request <request.json> --result <result.json>
//
// Uses carve::csg::CSG::compute for union, intersection, and difference
// on closed manifold meshes.
//
// Requires closed, manifold inputs.
// Invalid meshes are caught at import time (isClosed() check).
//
// This runner is binary-only: boolean ops with 3+ args return 'unsupported'.
// The meta-runner generates sequences of binary ops for iterated booleans.
//
// Timing conventions (per-op fields):
//   load-mesh:
//     total_ms   — start before file open, end when MeshSet is in SSA
//     io_ms      — file -> flat double verts + int indices
//                  (runner_mesh_helpers::loadFromFileIndexed)
//     import_ms  — flat f64 verts+indices -> carve::input::PolyhedronData
//                  -> carve::mesh::MeshSet<3> + isClosed() validation
//   boolean ops:
//     total_ms      — start when native operands are ready, end when result is ready
//     operation_ms  — CSG::compute() call only
//     export_ms     — MeshSet face/vertex traversal + fan triangulation (excludes disk write)
// ---------------------------------------------------------------------------

#include <carve/csg.hpp>
#include <carve/input.hpp>
#include <carve/mesh.hpp>
#include <nlohmann/json.hpp>
#include <runner_mesh_helpers/io.hh>
#include <runner_utils/cli.hh>
#include <runner_utils/ops.hh>
#include <runner_utils/run_loop.hh>
#include <runner_utils/timer.hh>

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>


using json      = nlohmann::json;
using MeshSet   = carve::mesh::MeshSet<3>;
using MeshSetPtr = std::unique_ptr<MeshSet>;

using runner_utils::print_progress_op;
using runner_utils::Timer;
using runner_utils::validate_op_boolean_binary;


// ---------------------------------------------------------------------------
// Mesh conversion helpers
// ---------------------------------------------------------------------------

// Build a Carve MeshSet from flat indexed arrays (as produced by loadFromFileIndexed).
// verts: [x0,y0,z0, x1,y1,z1, ...]
// tris:  [i0,i1,i2, i3,i4,i5, ...]
static MeshSetPtr meshset_from_indexed(
    std::vector<double> const& verts,
    std::vector<int>    const& tris)
{
    carve::input::PolyhedronData data;

    std::size_t const nv = verts.size() / 3;
    data.reserveVertices(static_cast<int>(nv));
    for (std::size_t i = 0; i < nv; ++i)
        data.addVertex(carve::geom::VECTOR(verts[i*3], verts[i*3+1], verts[i*3+2]));

    std::size_t const nt = tris.size() / 3;
    data.reserveFaces(static_cast<int>(nt), 3);
    for (std::size_t i = 0; i < nt; ++i)
        data.addFace(tris[i*3], tris[i*3+1], tris[i*3+2]);

    return MeshSetPtr(data.createMesh(carve::input::Options()));
}

// Extract a flat indexed mesh from a Carve MeshSet.
// Faces may be n-gons; fan-triangulate them.
static void meshset_to_indexed(
    MeshSet             const& ms,
    std::vector<double>      & out_verts,
    std::vector<int>         & out_tris)
{
    // Map each vertex pointer to its output index.
    std::unordered_map<carve::mesh::Vertex<3> const*, int> vmap;
    vmap.reserve(ms.vertex_storage.size());

    for (auto const& v : ms.vertex_storage)
    {
        int idx = static_cast<int>(out_verts.size() / 3);
        vmap[&v] = idx;
        out_verts.push_back(v.v[0]);
        out_verts.push_back(v.v[1]);
        out_verts.push_back(v.v[2]);
    }

    for (auto const* mesh : ms.meshes)
    {
        for (auto const* face : mesh->faces)
        {
            // Fan-triangulate: (v0, v_prev, v_cur) for each subsequent vertex.
            auto const* edge = face->edge;
            int const v0 = vmap.at(edge->vert);
            edge = edge->next;
            int vprev = vmap.at(edge->vert);
            edge = edge->next;
            while (edge != face->edge)
            {
                int const vcur = vmap.at(edge->vert);
                out_tris.push_back(v0);
                out_tris.push_back(vprev);
                out_tris.push_back(vcur);
                vprev = vcur;
                edge = edge->next;
            }
        }
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

        // SSA storage: ssa[i] = result of op i.
        std::vector<MeshSetPtr> ssa;
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

                    // io phase — file -> flat double verts + int indices
                    Timer io_timer;
                    auto [verts, tris] = runner_mesh_helpers::loadFromFileIndexed(path);
                    double const io_ms = io_timer.elapsed_ms();

                    // import phase — flat f64 -> carve::mesh::MeshSet<3>
                    Timer import_timer;

                    MeshSetPtr mesh = meshset_from_indexed(verts, tris);
                    if (!mesh)
                        throw std::runtime_error("Carve import failed: createMesh returned null");

                    // Validate: all sub-meshes must be closed.
                    for (auto const* m : mesh->meshes)
                    {
                        if (!m->isClosed())
                            throw std::runtime_error("Carve import failed: mesh is not closed "
                                                     "(open boundary or non-manifold geometry)");
                    }

                    double const import_ms = import_timer.elapsed_ms();

                    // disk write — not timed
                    std::string const file_path = out_dir + "/op_" + std::to_string(i) + ".obj";
                    {
                        std::vector<double> out_verts;
                        std::vector<int>    out_tris;
                        meshset_to_indexed(*mesh, out_verts, out_tris);
                        runner_mesh_helpers::saveToFileIndexed(file_path, out_verts, out_tris);
                    }

                    ssa.push_back(std::move(mesh));

                    op_res["status"]    = "success";
                    op_res["total_ms"]  = op_total_timer.elapsed_ms();
                    op_res["io_ms"]     = io_ms;
                    op_res["import_ms"] = import_ms;
                    op_res["file"]      = file_path;
                }
                else if (op_str == "boolean-union" || op_str == "boolean-intersection" || op_str == "boolean-difference")
                {
                    validate_op_boolean_binary(i, ops, "carve runner");

                    auto const args = op.at("args").get<std::vector<int>>();

                    carve::csg::CSG::OP csg_op;
                    if      (op_str == "boolean-union")        csg_op = carve::csg::CSG::UNION;
                    else if (op_str == "boolean-intersection") csg_op = carve::csg::CSG::INTERSECTION;
                    else                                        csg_op = carve::csg::CSG::A_MINUS_B;

                    MeshSet* lhs = ssa[static_cast<std::size_t>(args[0])].get();
                    MeshSet* rhs = ssa[static_cast<std::size_t>(args[1])].get();

                    // Boolean phase — timed as operation_ms.
                    Timer op_timer;
                    carve::csg::CSG csg;
                    MeshSetPtr result(csg.compute(lhs, rhs, csg_op));
                    double const operation_ms = op_timer.elapsed_ms();

                    if (!result)
                    {
                        op_res["status"]       = "invalid_input";
                        op_res["total_ms"]     = op_total_timer.elapsed_ms();
                        op_res["operation_ms"] = operation_ms;
                        op_res["error"]        = "CSG::compute returned null — inputs may be invalid";
                        ssa.push_back(nullptr);
                        ops_result.push_back(op_res);
                        failed = true;
                        break;
                    }

                    // Export phase — timed as export_ms (conversion only).
                    Timer export_timer;
                    std::vector<double> out_verts;
                    std::vector<int>    out_tris;
                    meshset_to_indexed(*result, out_verts, out_tris);
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
                    ssa.push_back(nullptr);
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
    runner_utils::Config const cfg = runner_utils::parse_args(argc, argv, "carve_runner");
    return runner_utils::run_main_loop(cfg, execute_run);
}
