// ---------------------------------------------------------------------------
// main.cpp — quickcsg_runner entry point
//
// Execution contract:
//   quickcsg_runner --request <request.json> --result <result.json>
//
// Uses QuickCSG (mCSG) for union, intersection, and difference on
// polyhedral meshes.
//
// Library: QuickCSG by Douze, Franco, Raffin (INRIA 2013–2015)
// Commit:  caff91954daf864da9d738d1d3b9ff2e8902568e
// Paper:   https://arxiv.org/abs/1706.01558
//
// This runner is binary-only: boolean ops with 3+ args return 'unsupported'.
// The meta-runner generates sequences of binary ops for iterated booleans.
// (QuickCSG itself supports N-ary booleans; this restriction is a runner choice.)
//
// mCSG API (verified against mCSG.hpp, bitvectors.hpp, mesh_csg.cpp):
//
//   Class:       mCSG::CSG (inherits mCSG::CSGParameters for settings)
//   Constructor: CSG()  OR  CSG(const CSGParameters &params)
//   Load mesh:   csg.addMeshFromVFArray(npt, pts, facets, facetsSize)
//                  pts:       const real (*)[3]  — array of [x,y,z] triples
//                  facets:    int array, chunks: [3 p0 p1 p2  3 p0 p1 p2 ...]
//                  facetsSize: total ints in facets array
//   Topology:    csg.afterLoad()
//   Operations:  new mCSG::CSGUnion(nmesh)        — union
//                new mCSG::CSGIntersection(nmesh)  — intersection
//                new mCSG::CSGDiff(nmesh, npos)    — difference
//                  npos = number of "positive" (first) meshes
//                  CSGDiff(2,1) → mesh0 minus mesh1
//   Computation: csg.initKDTree()
//                csg.exploreKDTree(csgop)
//                csg.makeFacets(csgop)
//                  with CSGParameters::tesselate = 3 → produces triangles
//   Output:      csg.facets[i].outputVertexNos, csg.facets[i].loops
//                  loops[i] = end index of loop i in outputVertexNos
//                csg.vertices[j].coords.{x,y,z}
//
// Timing conventions (per-op fields):
//   load-mesh:
//     total_ms   — start before file open, end when mesh data is in SSA
//     io_ms      — file → flat double verts + int indices
//     import_ms  — (near-zero) data is kept in SSA as flat arrays; actual
//                  conversion to mCSG format happens per-op in apply_op
//   boolean ops:
//     total_ms      — start when operands are ready, end when result is in SSA
//     operation_ms  — full mCSG pipeline (addMeshFromVFArray + afterLoad +
//                     initKDTree + exploreKDTree + makeFacets with tesselate=3)
//     export_ms     — result extraction from csg.facets/csg.vertices into
//                     flat f64 verts + int tris; excludes disk write
// ---------------------------------------------------------------------------

#include "mCSG.hpp"

#include <nlohmann/json.hpp>
#include <runner_mesh_helpers/io.hh>
#include <runner_utils/cli.hh>
#include <runner_utils/ops.hh>
#include <runner_utils/run_loop.hh>
#include <runner_utils/timer.hh>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>


using json = nlohmann::json;
using runner_utils::print_progress_op;
using runner_utils::Timer;
using runner_utils::validate_op_boolean_binary;
using namespace mCSG;


// ---------------------------------------------------------------------------
// Mesh data storage
//
// We store flat double/int arrays.  mCSG::CSG is created fresh per boolean
// operation — it wraps both input meshes for a single computation —
// so there is no persistent native handle to keep in SSA.
// ---------------------------------------------------------------------------

struct MeshData
{
    std::vector<double> verts; // flat f64: [x0,y0,z0, x1,y1,z1, …], length = V*3
    std::vector<int>    tris;  // flat int:  [i0,j0,k0, i1,j1,k1, …], length = T*3
};


// ---------------------------------------------------------------------------
// Operation kind
// ---------------------------------------------------------------------------

enum class OpKind { Union, Intersection, Difference };


// ---------------------------------------------------------------------------
// apply_op
//
// Creates a fresh mCSG::CSG for a single binary boolean operation.
//
// Timing:
//   operation_ms — full mCSG pipeline through makeFacets (with tesselate=3)
//   export_ms    — result extraction from facets/vertices into flat arrays
//
// Returns {out_verts, out_tris, operation_ms, export_ms, ok}.
// ---------------------------------------------------------------------------

struct ApplyResult
{
    std::vector<double> verts;
    std::vector<int>    tris;
    double              operation_ms = 0.0;
    double              export_ms    = 0.0;
    bool                ok           = false;
};

// Build the QuickCSG VF-array format from flat triangle indices.
// Format: [3 p0 p1 p2  3 p0 p1 p2  ...], one chunk per triangle.
static std::vector<int> make_vf_array(std::vector<int> const& tris)
{
    int const ntri = static_cast<int>(tris.size() / 3);
    std::vector<int> arr;
    arr.reserve(ntri * 4);
    for (int i = 0; i < ntri; i++)
    {
        arr.push_back(3);
        arr.push_back(tris[i*3 + 0]);
        arr.push_back(tris[i*3 + 1]);
        arr.push_back(tris[i*3 + 2]);
    }
    return arr;
}

static ApplyResult apply_op(MeshData const& lhs, MeshData const& rhs, OpKind kind)
{
    ApplyResult res;

    int const nv0 = static_cast<int>(lhs.verts.size() / 3);
    int const nv1 = static_cast<int>(rhs.verts.size() / 3);

    // -----------------------------------------------------------------------
    // Boolean computation (operation_ms)
    // -----------------------------------------------------------------------
    Timer op_timer;

    // CSGParameters controls algorithm settings.
    // tesselate=3: triangulate output polygons inside makeFacets().
    CSGParameters params;
    params.verbose   = 0;
    params.tesselate = 3; // output triangles (not raw polygons)

    CSG csg(params);

    // Load both meshes.
    // addMeshFromVFArray expects vertices as const real(*)[3] and facets in
    // [count p0 p1 ...] chunk format.  We reinterpret the contiguous flat
    // double* storage as double(*)[3] — same layout, safe for POD.
    {
        auto const* pts0  = reinterpret_cast<const real(*)[3]>(lhs.verts.data());
        auto        facs0 = make_vf_array(lhs.tris);
        csg.addMeshFromVFArray(nv0, pts0, facs0.data(), static_cast<int>(facs0.size()));
    }
    {
        auto const* pts1  = reinterpret_cast<const real(*)[3]>(rhs.verts.data());
        auto        facs1 = make_vf_array(rhs.tris);
        csg.addMeshFromVFArray(nv1, pts1, facs1.data(), static_cast<int>(facs1.size()));
    }

    // Topology phase: reorder facet–edge links.
    csg.afterLoad();

    // Build CSGOperation for the requested boolean.
    // CSGDiff(nmesh, npos): union of first npos meshes minus union of the rest.
    // For A-B: CSGDiff(2, 1) → mesh 0 (A) minus mesh 1 (B).
    std::unique_ptr<CSGOperation> csgop;
    switch (kind)
    {
    case OpKind::Union:
        csgop = std::make_unique<CSGUnion>(csg.nmesh);
        break;
    case OpKind::Intersection:
        csgop = std::make_unique<CSGIntersection>(csg.nmesh);
        break;
    case OpKind::Difference:
        csgop = std::make_unique<CSGDiff>(csg.nmesh, /*npos=*/1);
        break;
    }

    // KD-tree exploration (produces all intersection vertices).
    csg.initKDTree();
    csg.exploreKDTree(csgop.get());

    // CSG facet construction.  With tesselate=3, makeFacets() also calls
    // tesselateFacet() on each output facet → each loop becomes a triangle.
    csg.makeFacets(csgop.get());

    res.operation_ms = op_timer.elapsed_ms();

    // -----------------------------------------------------------------------
    // Export phase (export_ms): extract result from csg.facets / csg.vertices
    //
    // After makeFacets() with tesselate=3 each facet's loops[] stores end
    // indices into outputVertexNos[], with each loop being exactly 3 vertices.
    // -----------------------------------------------------------------------
    Timer export_timer;

    // Build a vertex-index map: csg.vertices index → output index.
    // Only vertices that appear in outputVertexNos are emitted.
    std::vector<int> vmap(csg.vertices.size(), -1);
    int nv_out = 0;

    for (int fi = 0; fi < static_cast<int>(csg.facets.size()); fi++)
    {
        Facet const& facet = csg.facets[fi];
        int l0 = 0;
        for (int li = 0; li < static_cast<int>(facet.loops.size()); li++)
        {
            int l1 = facet.loops[li];
            for (int j = l0; j < l1; j++)
            {
                int vi = facet.outputVertexNos[j];
                if (vmap[vi] < 0) vmap[vi] = nv_out++;
            }
            l0 = l1;
        }
    }

    // Fill flat vertex array.
    res.verts.resize(static_cast<std::size_t>(nv_out) * 3);
    for (int i = 0; i < static_cast<int>(csg.vertices.size()); i++)
    {
        if (vmap[i] >= 0)
        {
            int dst = vmap[i];
            res.verts[dst*3 + 0] = csg.vertices[i].coords.x;
            res.verts[dst*3 + 1] = csg.vertices[i].coords.y;
            res.verts[dst*3 + 2] = csg.vertices[i].coords.z;
        }
    }

    // Fill flat triangle array.
    // With tesselate=3 each loop has exactly 3 vertices; skip malformed ones.
    for (int fi = 0; fi < static_cast<int>(csg.facets.size()); fi++)
    {
        Facet const& facet = csg.facets[fi];
        int l0 = 0;
        for (int li = 0; li < static_cast<int>(facet.loops.size()); li++)
        {
            int l1 = facet.loops[li];
            if (l1 - l0 == 3)
            {
                res.tris.push_back(vmap[facet.outputVertexNos[l0 + 0]]);
                res.tris.push_back(vmap[facet.outputVertexNos[l0 + 1]]);
                res.tris.push_back(vmap[facet.outputVertexNos[l0 + 2]]);
            }
            l0 = l1;
        }
    }

    res.export_ms = export_timer.elapsed_ms();
    res.ok        = true;
    return res;
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

        // SSA storage: ssa[i] = mesh data produced by op i.
        std::vector<MeshData> ssa;
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

                    // import phase — data is stored in SSA as flat arrays.
                    // Actual conversion to mCSG format happens inside apply_op()
                    // at operation time, so import_ms is negligible here.
                    Timer import_timer;
                    MeshData mesh;
                    mesh.verts = std::move(verts);
                    mesh.tris  = std::move(tris);
                    double const import_ms = import_timer.elapsed_ms();

                    // disk write — not timed
                    std::string const file_path = out_dir + "/op_" + std::to_string(i) + ".obj";
                    runner_mesh_helpers::saveToFileIndexed(file_path, mesh.verts, mesh.tris);

                    ssa.push_back(std::move(mesh));

                    op_res["status"]    = "success";
                    op_res["total_ms"]  = op_total_timer.elapsed_ms();
                    op_res["io_ms"]     = io_ms;
                    op_res["import_ms"] = import_ms;
                    op_res["file"]      = file_path;
                }
                else if (op_str == "boolean-union" || op_str == "boolean-intersection"
                         || op_str == "boolean-difference")
                {
                    validate_op_boolean_binary(i, ops, "quickcsg runner");

                    auto const args = op.at("args").get<std::vector<int>>();

                    OpKind const kind = (op_str == "boolean-union")        ? OpKind::Union
                                      : (op_str == "boolean-intersection") ? OpKind::Intersection
                                                                           : OpKind::Difference;

                    MeshData const& lhs = ssa[static_cast<std::size_t>(args[0])];
                    MeshData const& rhs = ssa[static_cast<std::size_t>(args[1])];

                    auto result = apply_op(lhs, rhs, kind);

                    if (!result.ok)
                    {
                        op_res["status"]       = "invalid_input";
                        op_res["total_ms"]     = op_total_timer.elapsed_ms();
                        op_res["operation_ms"] = result.operation_ms;
                        op_res["error"]        = "mCSG boolean operation returned no valid output";
                        ssa.push_back(MeshData{});
                        ops_result.push_back(op_res);
                        failed = true;
                        break;
                    }

                    // disk write — not timed
                    std::string const file_path = out_dir + "/op_" + std::to_string(i) + ".obj";
                    runner_mesh_helpers::saveToFileIndexed(file_path, result.verts, result.tris);

                    MeshData result_mesh;
                    result_mesh.verts = std::move(result.verts);
                    result_mesh.tris  = std::move(result.tris);
                    ssa.push_back(std::move(result_mesh));

                    op_res["status"]       = "success";
                    op_res["total_ms"]     = op_total_timer.elapsed_ms();
                    op_res["operation_ms"] = result.operation_ms;
                    op_res["export_ms"]    = result.export_ms;
                    op_res["file"]         = file_path;
                }
                else
                {
                    op_res["status"]   = "unsupported";
                    op_res["total_ms"] = 0.0;
                    op_res["error"]    = "unknown op: " + op_str;
                    ssa.push_back(MeshData{});
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
    runner_utils::Config const cfg = runner_utils::parse_args(argc, argv, "quickcsg_runner");
    return runner_utils::run_main_loop(cfg, execute_run);
}
