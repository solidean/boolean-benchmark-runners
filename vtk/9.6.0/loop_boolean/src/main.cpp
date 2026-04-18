// ---------------------------------------------------------------------------
// main.cpp — vtk_loop_runner entry point
//
// Execution contract:
//   vtk_loop_runner --request <request.json> --result <result.json>
//
// Uses vtkLoopBooleanPolyDataFilter for union, intersection, and
// difference on triangulated, closed vtkPolyData meshes.
//
// Requires closed, triangulated, manifold inputs.
// Preprocessing (vtkCleanPolyData + vtkTriangleFilter) is applied during
// load-mesh and timed as import_ms.
//
// This runner is binary-only: boolean ops with 3+ args return 'unsupported'.
// The meta-runner generates sequences of binary ops for iterated booleans.
//
// Timing conventions (per-op fields):
//   load-mesh:
//     total_ms   — start before file open, end when preprocessed vtkPolyData is in SSA
//     io_ms      — file → flat double verts + int indices
//                  (runner_mesh_helpers::loadFromFileIndexed)
//     import_ms  — flat f64 verts+indices → vtkPolyData → vtkCleanPolyData → vtkTriangleFilter
//   boolean ops:
//     total_ms      — start when native operands are ready, end when result is in SSA
//     operation_ms  — vtkLoopBooleanPolyDataFilter::Update() only
//     export_ms     — result vtkPolyData → flat f64 verts+indices (excludes disk write)
// ---------------------------------------------------------------------------

#include <vtkCallbackCommand.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkCleanPolyData.h>
#include <vtkIdList.h>
#include <vtkLoopBooleanPolyDataFilter.h>
#include <vtkNew.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>
#include <vtkTriangleFilter.h>

#include <nlohmann/json.hpp>
#include <runner_mesh_helpers/io.hh>
#include <runner_utils/cli.hh>
#include <runner_utils/ops.hh>
#include <runner_utils/run_loop.hh>
#include <runner_utils/timer.hh>

#include <stdexcept>
#include <string>
#include <vector>


using json = nlohmann::json;
using runner_utils::print_progress_op;
using runner_utils::Timer;
using runner_utils::validate_op_boolean_binary;

// ---------------------------------------------------------------------------
// VTK error observer
// ---------------------------------------------------------------------------

struct VtkErrorFlag
{
    bool triggered = false;
    std::string message;
};

static void vtk_error_callback(vtkObject* /*caller*/, unsigned long /*eid*/,
                                void* client_data, void* /*call_data*/)
{
    auto* flag = static_cast<VtkErrorFlag*>(client_data);
    flag->triggered = true;
    flag->message = "vtkLoopBooleanPolyDataFilter reported an error";
}

// ---------------------------------------------------------------------------
// Mesh conversion helpers
// ---------------------------------------------------------------------------

static vtkSmartPointer<vtkPolyData> build_and_preprocess(
    std::vector<double> const& verts,
    std::vector<int> const& tris)
{
    auto points = vtkSmartPointer<vtkPoints>::New();
    points->SetDataTypeToDouble();
    vtkIdType const n_verts = static_cast<vtkIdType>(verts.size() / 3);
    points->SetNumberOfPoints(n_verts);
    for (vtkIdType i = 0; i < n_verts; ++i)
        points->SetPoint(i, verts[3 * i], verts[3 * i + 1], verts[3 * i + 2]);

    auto cells = vtkSmartPointer<vtkCellArray>::New();
    vtkIdType const n_tris = static_cast<vtkIdType>(tris.size() / 3);
    cells->AllocateEstimate(n_tris, 3);
    for (vtkIdType i = 0; i < n_tris; ++i)
    {
        vtkIdType pts[3] = {
            static_cast<vtkIdType>(tris[3 * i]),
            static_cast<vtkIdType>(tris[3 * i + 1]),
            static_cast<vtkIdType>(tris[3 * i + 2]),
        };
        cells->InsertNextCell(3, pts);
    }

    auto raw = vtkSmartPointer<vtkPolyData>::New();
    raw->SetPoints(points);
    raw->SetPolys(cells);

    auto cleaner = vtkSmartPointer<vtkCleanPolyData>::New();
    cleaner->SetInputData(raw);
    cleaner->Update();

    auto triangulator = vtkSmartPointer<vtkTriangleFilter>::New();
    triangulator->SetInputConnection(cleaner->GetOutputPort());
    triangulator->Update();

    auto result = vtkSmartPointer<vtkPolyData>::New();
    result->DeepCopy(triangulator->GetOutput());
    return result;
}

static std::pair<std::vector<double>, std::vector<int>>
extract_polydata(vtkPolyData* poly)
{
    vtkIdType const n_pts = poly->GetNumberOfPoints();
    std::vector<double> verts;
    verts.resize(static_cast<std::size_t>(n_pts) * 3);
    for (vtkIdType i = 0; i < n_pts; ++i)
    {
        double p[3];
        poly->GetPoint(i, p);
        verts[3 * i]     = p[0];
        verts[3 * i + 1] = p[1];
        verts[3 * i + 2] = p[2];
    }

    vtkIdType const n_cells = poly->GetNumberOfCells();
    std::vector<int> tris;
    tris.reserve(static_cast<std::size_t>(n_cells) * 3);

    auto id_list = vtkSmartPointer<vtkIdList>::New();
    for (vtkIdType i = 0; i < n_cells; ++i)
    {
        poly->GetCellPoints(i, id_list);
        if (id_list->GetNumberOfIds() != 3)
            throw std::runtime_error("output mesh contains non-triangle cell");
        tris.push_back(static_cast<int>(id_list->GetId(0)));
        tris.push_back(static_cast<int>(id_list->GetId(1)));
        tris.push_back(static_cast<int>(id_list->GetId(2)));
    }

    return {std::move(verts), std::move(tris)};
}

// ---------------------------------------------------------------------------
// Operation helpers
// ---------------------------------------------------------------------------

enum class OpKind
{
    Union,
    Intersection,
    Difference
};

static std::pair<vtkSmartPointer<vtkPolyData>, bool>
apply_op(vtkPolyData* lhs, vtkPolyData* rhs, OpKind kind)
{
    auto filter = vtkSmartPointer<vtkLoopBooleanPolyDataFilter>::New();

    switch (kind)
    {
    case OpKind::Union:
        filter->SetOperationToUnion();
        break;
    case OpKind::Intersection:
        filter->SetOperationToIntersection();
        break;
    case OpKind::Difference:
        filter->SetOperationToDifference();
        break;
    }

    filter->SetInputData(0, lhs);
    filter->SetInputData(1, rhs);

    VtkErrorFlag err_flag;
    auto observer = vtkSmartPointer<vtkCallbackCommand>::New();
    observer->SetCallback(vtk_error_callback);
    observer->SetClientData(&err_flag);
    filter->AddObserver(vtkCommand::ErrorEvent, observer);

    filter->Update();

    auto result = vtkSmartPointer<vtkPolyData>::New();
    result->DeepCopy(filter->GetOutput(0));
    return {result, !err_flag.triggered};
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

        std::vector<vtkSmartPointer<vtkPolyData>> ssa;
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

                    Timer io_timer;
                    auto [verts, tris] = runner_mesh_helpers::loadFromFileIndexed(path);
                    double const io_ms = io_timer.elapsed_ms();

                    Timer import_timer;
                    auto poly = build_and_preprocess(verts, tris);
                    if (poly->GetNumberOfPoints() == 0)
                        throw std::runtime_error("mesh has 0 points after cleaning — degenerate input");
                    double const import_ms = import_timer.elapsed_ms();

                    std::string const file_path = out_dir + "/op_" + std::to_string(i) + ".obj";
                    {
                        auto [out_verts, out_tris] = extract_polydata(poly);
                        runner_mesh_helpers::saveToFileIndexed(file_path, out_verts, out_tris);
                    }

                    ssa.push_back(std::move(poly));

                    op_res["status"]    = "success";
                    op_res["total_ms"]  = op_total_timer.elapsed_ms();
                    op_res["io_ms"]     = io_ms;
                    op_res["import_ms"] = import_ms;
                    op_res["file"]      = file_path;
                }
                else if (op_str == "boolean-union" || op_str == "boolean-intersection" || op_str == "boolean-difference")
                {
                    validate_op_boolean_binary(i, ops, "vtk_loop_runner");

                    auto const args = op.at("args").get<std::vector<int>>();

                    OpKind const kind = (op_str == "boolean-union")        ? OpKind::Union
                                      : (op_str == "boolean-intersection") ? OpKind::Intersection
                                                                           : OpKind::Difference;

                    vtkPolyData* lhs = ssa[static_cast<std::size_t>(args[0])];
                    vtkPolyData* rhs = ssa[static_cast<std::size_t>(args[1])];

                    Timer op_timer;
                    auto [result, ok] = apply_op(lhs, rhs, kind);
                    double const operation_ms = op_timer.elapsed_ms();

                    if (!ok)
                    {
                        op_res["status"]       = "crash";
                        op_res["total_ms"]     = op_total_timer.elapsed_ms();
                        op_res["operation_ms"] = operation_ms;
                        op_res["error"]        = "vtkLoopBooleanPolyDataFilter reported an error";
                        ssa.push_back(vtkSmartPointer<vtkPolyData>::New());
                        ops_result.push_back(op_res);
                        failed = true;
                        break;
                    }

                    Timer export_timer;
                    auto [out_verts, out_tris] = extract_polydata(result);
                    double const export_ms = export_timer.elapsed_ms();

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
                    ssa.push_back(vtkSmartPointer<vtkPolyData>::New());
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
                break;
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
    runner_utils::Config const cfg = runner_utils::parse_args(argc, argv, "vtk_loop_runner");
    return runner_utils::run_main_loop(cfg, execute_run);
}
