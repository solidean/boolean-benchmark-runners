// ---------------------------------------------------------------------------
// main.cpp — meshlib_runner entry point
//
// Execution contract:
//   meshlib_runner --request <request.json> --result <result.json>
//
// Uses MR::boolean() for union, intersection, and difference on MR::Mesh.
//
// MeshLib uses float coordinates internally; the runner converts between
// double (benchmark I/O format) and float (MeshLib native) during
// import and export.
//
// This runner is binary-only: boolean ops with 3+ args return 'unsupported'.
// The meta-runner generates sequences of binary ops for iterated booleans.
//
// Timing conventions (per-op fields):
//   load-mesh:
//     total_ms   — start before file open, end when MR::Mesh is ready
//     io_ms      — file -> flat double verts + int indices
//                  (runner_mesh_helpers::loadFromFileIndexed)
//     import_ms  — flat f64 verts+indices -> MR::Mesh::fromTriangles
//                  (includes double->float conversion)
//   boolean ops:
//     total_ms      — start when native operands are ready, end when result is ready
//     operation_ms  — MR::boolean() call only
//     export_ms     — MR::Mesh -> flat double verts + int tris
//                     (includes float->double conversion; excludes disk write)
// ---------------------------------------------------------------------------

#include <MRMesh/MRMesh.h>
#include <MRMesh/MRMeshBoolean.h>
#include <MRMesh/MRId.h>
#include <MRMesh/MRVector3.h>
#include <MRMesh/MRMeshBuilder.h>

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
// Operation helpers
// ---------------------------------------------------------------------------

enum class OpKind
{
    Union,
    Intersection,
    Difference
};

// Build an MR::Mesh from flat double vertices and int triangle indices.
// Converts double -> float for MeshLib's internal representation.
static MR::Mesh build_mesh(std::vector<double> const& verts, std::vector<int> const& tris)
{
    std::size_t const num_verts = verts.size() / 3;
    std::size_t const num_tris  = tris.size() / 3;

    MR::VertCoords points;
    points.reserve(num_verts);
    for (std::size_t i = 0; i < num_verts; ++i)
        points.emplace_back(MR::Vector3f{
            static_cast<float>(verts[i * 3 + 0]),
            static_cast<float>(verts[i * 3 + 1]),
            static_cast<float>(verts[i * 3 + 2])});

    MR::Triangulation triangles;
    triangles.reserve(num_tris);
    for (std::size_t i = 0; i < num_tris; ++i)
        triangles.emplace_back(MR::ThreeVertIds{
            MR::VertId(tris[i * 3 + 0]),
            MR::VertId(tris[i * 3 + 1]),
            MR::VertId(tris[i * 3 + 2])});

    return MR::Mesh::fromTriangles(std::move(points), triangles);
}

// Extract flat double vertices and int triangle indices from an MR::Mesh.
// Converts float -> double. Handles potential vertex ID gaps by compacting.
static std::pair<std::vector<double>, std::vector<int>> extract_mesh(MR::Mesh const& mesh)
{
    // Build a mapping from potentially sparse VertIds to dense indices.
    auto const& topology = mesh.topology;
    auto const valid_verts = topology.getValidVerts();
    std::size_t const num_valid = valid_verts.count();

    // Map: VertId -> dense index
    std::vector<int> vert_remap(mesh.points.size(), -1);
    std::vector<double> out_verts;
    out_verts.reserve(num_valid * 3);

    int dense_idx = 0;
    for (auto vid = valid_verts.find_first(); vid.valid(); vid = valid_verts.find_next(vid))
    {
        vert_remap[vid] = dense_idx++;
        auto const& p = mesh.points[vid];
        out_verts.push_back(static_cast<double>(p.x));
        out_verts.push_back(static_cast<double>(p.y));
        out_verts.push_back(static_cast<double>(p.z));
    }

    // Extract triangles with remapped vertex indices.
    auto const valid_faces = topology.getValidFaces();
    std::vector<int> out_tris;
    out_tris.reserve(valid_faces.count() * 3);

    for (auto fid = valid_faces.find_first(); fid.valid(); fid = valid_faces.find_next(fid))
    {
        auto tri = topology.getTriVerts(fid);
        out_tris.push_back(vert_remap[tri[0]]);
        out_tris.push_back(vert_remap[tri[1]]);
        out_tris.push_back(vert_remap[tri[2]]);
    }

    return {std::move(out_verts), std::move(out_tris)};
}

// Perform a boolean operation. Returns {result_mesh, ok}.
static std::pair<MR::Mesh, bool> apply_op(MR::Mesh const& lhs, MR::Mesh const& rhs, OpKind kind)
{
    MR::BooleanOperation mr_op;
    switch (kind)
    {
    case OpKind::Union:        mr_op = MR::BooleanOperation::Union;        break;
    case OpKind::Intersection: mr_op = MR::BooleanOperation::Intersection; break;
    case OpKind::Difference:   mr_op = MR::BooleanOperation::DifferenceAB; break;
    }

    auto result = MR::boolean(lhs, rhs, mr_op);

    if (!result.valid())
        return {MR::Mesh(), false};

    return {std::move(*result), true};
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
        std::vector<MR::Mesh> ssa;
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

                    // import phase — flat f64 -> MR::Mesh (double->float conversion)
                    Timer import_timer;
                    MR::Mesh loaded = build_mesh(verts, tris);
                    double const import_ms = import_timer.elapsed_ms();

                    // disk write — not timed
                    std::string const file_path = out_dir + "/op_" + std::to_string(i) + ".obj";
                    {
                        auto [out_verts, out_tris] = extract_mesh(loaded);
                        runner_mesh_helpers::saveToFileIndexed(file_path, out_verts, out_tris);
                    }

                    ssa.push_back(std::move(loaded));

                    op_res["status"]    = "success";
                    op_res["total_ms"]  = op_total_timer.elapsed_ms();
                    op_res["io_ms"]     = io_ms;
                    op_res["import_ms"] = import_ms;
                    op_res["file"]      = file_path;
                }
                else if (op_str == "boolean-union" || op_str == "boolean-intersection" || op_str == "boolean-difference")
                {
                    validate_op_boolean_binary(i, ops, "meshlib runner");

                    auto const args = op.at("args").get<std::vector<int>>();

                    OpKind const kind = (op_str == "boolean-union")        ? OpKind::Union
                                      : (op_str == "boolean-intersection") ? OpKind::Intersection
                                                                           : OpKind::Difference;

                    MR::Mesh const& lhs = ssa[static_cast<std::size_t>(args[0])];
                    MR::Mesh const& rhs = ssa[static_cast<std::size_t>(args[1])];

                    // Boolean phase — timed as operation_ms.
                    Timer op_timer;
                    auto [result, ok] = apply_op(lhs, rhs, kind);
                    double const operation_ms = op_timer.elapsed_ms();

                    if (!ok)
                    {
                        op_res["status"]       = "invalid_input";
                        op_res["total_ms"]     = op_total_timer.elapsed_ms();
                        op_res["operation_ms"] = operation_ms;
                        op_res["error"]        = "MR::boolean returned an error — "
                                                 "inputs may be non-manifold, open, or otherwise invalid";
                        ssa.push_back(MR::Mesh());
                        ops_result.push_back(op_res);
                        failed = true;
                        break;
                    }

                    // Export phase — timed as export_ms (conversion only).
                    Timer export_timer;
                    auto [out_verts, out_tris] = extract_mesh(result);
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
                    ssa.push_back(MR::Mesh());
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
    runner_utils::Config const cfg = runner_utils::parse_args(argc, argv, "meshlib_runner");
    return runner_utils::run_main_loop(cfg, execute_run);
}
