#pragma once

// ---------------------------------------------------------------------------
// mesh_io.hh — format-agnostic mesh loader/exporter for Surface_mesh<EPEC>
//
// Supported input formats (detected by file extension, case-insensitive):
//   .off  — read into double polygon soup, convert coords to EPEC, then
//            repair → orient → polygon_soup_to_polygon_mesh
//   .obj  — same pipeline as .off
//   .stl  — same pipeline as .off
//
// After loading, the mesh is validated (is_closed + is_valid_polygon_mesh).
// A std::runtime_error is thrown on validation failure; callers map that to
// "invalid_input" status.
//
// Raw mesh format (declared for timing purposes):
//   input raw:  indexed-tris-f64
//   output raw: indexed-tris-f64
//
// Timing split:
//   io_ms     — file → IndexedMesh (file I/O + format parsing + fan triangulation)
//   import_ms — IndexedMesh → Surface_mesh<EPEC>
//               (double→EPEC promotion, repair, orient, polygon_soup_to_polygon_mesh,
//                validation; excludes disk I/O)
//   export_ms — Surface_mesh<EPEC> → IndexedMesh
//               (EPEC→double conversion, face traversal, fan triangulation;
//                excludes disk write)
// ---------------------------------------------------------------------------

#include "runner_types.hh"

#include <CGAL/IO/OBJ.h>
#include <CGAL/IO/OFF.h>
#include <CGAL/IO/STL.h>
#include <CGAL/Polygon_mesh_processing/orient_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/polygon_soup_to_polygon_mesh.h>
#include <CGAL/Polygon_mesh_processing/repair_polygon_soup.h>
#include <CGAL/Simple_cartesian.h>
#include <CGAL/boost/graph/graph_traits_Surface_mesh.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// IndexedMesh — the benchmark raw mesh format (indexed-tris-f64)
//
// All triangles are stored as index triples into the vertex array.
// All coordinates are double-precision Cartesian.
// ---------------------------------------------------------------------------

struct IndexedMesh
{
    std::vector<std::array<double, 3>> vertices;
    std::vector<std::array<std::size_t, 3>> triangles;
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace mesh_io_detail
{

using SoupKernel = CGAL::Simple_cartesian<double>;
using SoupPoint = SoupKernel::Point_3;
using SoupFace = std::vector<std::size_t>;

inline std::string lower_extension(std::string const& path)
{
    auto ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

// Fan-triangulate a polygon soup into an IndexedMesh.
// Each polygon (v0, v1, v2, ..., vN-1) is split into triangles
// (v0, v1, v2), (v0, v2, v3), ..., (v0, vN-2, vN-1).
inline IndexedMesh soup_to_indexed(std::vector<SoupPoint> const& pts, std::vector<SoupFace> const& faces)
{
    IndexedMesh im;
    im.vertices.reserve(pts.size());
    for (auto const& p : pts)
        im.vertices.push_back({p.x(), p.y(), p.z()});

    for (auto const& f : faces)
    {
        if (f.size() < 3)
            continue;
        for (std::size_t j = 1; j + 1 < f.size(); ++j)
            im.triangles.push_back({f[0], f[j], f[j + 1]});
    }
    return im;
}

// Write helpers (IndexedMesh → file stream)
inline void write_off(std::ostream& out, IndexedMesh const& im)
{
    out.precision(17);
    out << "OFF\n" << im.vertices.size() << " " << im.triangles.size() << " 0\n";
    for (auto const& v : im.vertices)
        out << v[0] << " " << v[1] << " " << v[2] << "\n";
    for (auto const& t : im.triangles)
        out << "3 " << t[0] << " " << t[1] << " " << t[2] << "\n";
}

inline void write_obj(std::ostream& out, IndexedMesh const& im)
{
    out.precision(17);
    for (auto const& v : im.vertices)
        out << "v " << v[0] << " " << v[1] << " " << v[2] << "\n";
    for (auto const& t : im.triangles)
        out << "f " << (t[0] + 1) << " " << (t[1] + 1) << " " << (t[2] + 1) << "\n";
}

inline void write_stl_ascii(std::ostream& out, IndexedMesh const& im)
{
    out.precision(10);
    out << "solid result\n";
    for (auto const& t : im.triangles)
    {
        auto const& a = im.vertices[t[0]];
        auto const& b = im.vertices[t[1]];
        auto const& c = im.vertices[t[2]];
        double nx = (b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]);
        double ny = (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]);
        double nz = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
        double len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 0)
        {
            nx /= len;
            ny /= len;
            nz /= len;
        }
        out << "  facet normal " << nx << " " << ny << " " << nz << "\n";
        out << "    outer loop\n";
        out << "      vertex " << a[0] << " " << a[1] << " " << a[2] << "\n";
        out << "      vertex " << b[0] << " " << b[1] << " " << b[2] << "\n";
        out << "      vertex " << c[0] << " " << c[1] << " " << c[2] << "\n";
        out << "    endloop\n";
        out << "  endfacet\n";
    }
    out << "endsolid result\n";
}

} // namespace mesh_io_detail

// ---------------------------------------------------------------------------
// load_raw — io phase
//
// Reads the source file into an IndexedMesh (indexed-tris-f64).
// This is the io_ms boundary: file I/O, format parsing, fan triangulation.
// skipped_facets is always 0 for this loader.
// ---------------------------------------------------------------------------
inline IndexedMesh load_raw(std::string const& path, std::size_t& skipped_facets)
{
    namespace d = mesh_io_detail;
    skipped_facets = 0;

    std::string const ext = d::lower_extension(path);

    std::vector<d::SoupPoint> soup_pts;
    std::vector<d::SoupFace> soup_faces;

    if (ext == ".off")
    {
        if (!CGAL::IO::read_OFF(path, soup_pts, soup_faces))
            throw std::runtime_error("Failed to read OFF file: " + path);
    }
    else if (ext == ".obj")
    {
        if (!CGAL::IO::read_OBJ(path, soup_pts, soup_faces))
            throw std::runtime_error("Failed to read OBJ file: " + path);
    }
    else if (ext == ".stl")
    {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open())
            throw std::runtime_error("Cannot open STL file: " + path);
        if (!CGAL::IO::read_STL(f, soup_pts, soup_faces))
            throw std::runtime_error("Failed to read STL file: " + path);
    }
    else
    {
        throw std::runtime_error("Unsupported input format '" + ext + "' for file: " + path
                                 + "  (supported: .off, .obj, .stl)");
    }

    return d::soup_to_indexed(soup_pts, soup_faces);
}

// ---------------------------------------------------------------------------
// import_mesh — import phase
//
// Converts an IndexedMesh (indexed-tris-f64) into a validated Surface_mesh<EPEC>.
// This is the import_ms boundary: double→EPEC promotion, repair, orient,
// polygon_soup_to_polygon_mesh, and closed/valid checks.
// Throws std::runtime_error if the mesh is not closed or not valid.
// ---------------------------------------------------------------------------
inline Mesh import_mesh(IndexedMesh const& raw)
{
    namespace PMP = CGAL::Polygon_mesh_processing;

    // Promote double coordinates to EPEC.
    std::vector<Kernel::Point_3> pts;
    pts.reserve(raw.vertices.size());
    for (auto const& v : raw.vertices)
        pts.emplace_back(v[0], v[1], v[2]);

    // Convert triangles back to polygon soup for PMP functions.
    std::vector<std::vector<std::size_t>> faces;
    faces.reserve(raw.triangles.size());
    for (auto const& t : raw.triangles)
        faces.push_back({t[0], t[1], t[2]});

    PMP::repair_polygon_soup(pts, faces);
    PMP::orient_polygon_soup(pts, faces);

    Mesh m;
    PMP::polygon_soup_to_polygon_mesh(pts, faces, m);

    if (!CGAL::is_closed(m))
        throw std::runtime_error("Loaded mesh is not closed (open boundary edges present)");

    if (!CGAL::is_valid_polygon_mesh(m))
        throw std::runtime_error("Loaded mesh is not a valid polygon mesh");

    return m;
}

// ---------------------------------------------------------------------------
// mesh_to_indexed — export phase (conversion only)
//
// Converts a Surface_mesh<EPEC> to an IndexedMesh (indexed-tris-f64).
// This is the export_ms boundary: EPEC→double conversion, face traversal,
// fan triangulation of any non-triangle faces.
// Does NOT write to disk.
// ---------------------------------------------------------------------------
inline IndexedMesh mesh_to_indexed(Mesh const& mesh)
{
    IndexedMesh im;
    im.vertices.reserve(mesh.num_vertices());

    for (auto v : mesh.vertices())
    {
        Kernel::Point_3 const& p = mesh.point(v);
        im.vertices.push_back({
            CGAL::to_double(p.x()),
            CGAL::to_double(p.y()),
            CGAL::to_double(p.z()),
        });
    }

    im.triangles.reserve(mesh.num_faces());
    for (auto f : mesh.faces())
    {
        // Collect face vertex indices.
        std::vector<std::size_t> fv;
        auto h = mesh.halfedge(f);
        auto h_end = h;
        do
        {
            fv.push_back(static_cast<std::size_t>(mesh.target(h)));
            h = mesh.next(h);
        } while (h != h_end);

        // Fan triangulate.
        for (std::size_t j = 1; j + 1 < fv.size(); ++j)
            im.triangles.push_back({fv[0], fv[j], fv[j + 1]});
    }

    return im;
}

// ---------------------------------------------------------------------------
// write_indexed — disk write (not timed)
//
// Writes an IndexedMesh to file.
// Supported output formats: .off, .obj, .stl (ASCII).
// ---------------------------------------------------------------------------
inline void write_indexed(IndexedMesh const& im, std::string const& path)
{
    namespace d = mesh_io_detail;

    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent);

    std::string const ext = d::lower_extension(path);
    std::ofstream out(path);
    if (!out.is_open())
        throw std::runtime_error("Cannot open output file for writing: " + path);

    if (ext == ".off")
        d::write_off(out, im);
    else if (ext == ".obj")
        d::write_obj(out, im);
    else if (ext == ".stl")
        d::write_stl_ascii(out, im);
    else
        throw std::runtime_error("Unsupported output format '" + ext + "' (supported: .off, .obj, .stl)");
}

// ---------------------------------------------------------------------------
// Convenience wrappers (used where timing breakdown is not needed)
// ---------------------------------------------------------------------------

inline Mesh load_mesh(std::string const& path, std::size_t& skipped_facets)
{
    IndexedMesh raw = load_raw(path, skipped_facets);
    return import_mesh(raw);
}

inline void export_mesh(Mesh const& mesh, std::string const& path)
{
    write_indexed(mesh_to_indexed(mesh), path);
}
