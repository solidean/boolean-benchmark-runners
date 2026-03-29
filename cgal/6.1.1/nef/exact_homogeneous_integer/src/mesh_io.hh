#pragma once

// ---------------------------------------------------------------------------
// mesh_io.hh — format-agnostic mesh loader → Nef_polyhedron_3
//
// Supported input formats (detected by file extension, case-insensitive):
//   .off  — read file bytes into an in-memory string, feed to OFF_to_nef_3
//   .obj  — read as polygon soup, serialise to an in-memory OFF string,
//            then feed to OFF_to_nef_3
//   .stl  — same pipeline as OBJ
//
// Output (mesh export) helpers are also declared here.
//
// Raw mesh format (declared for timing purposes):
//   input raw:  indexed-tris-f64 (normalised via in-memory OFF text)
//   output raw: indexed-tris-f64
//
// Timing split:
//   io_ms     — file → in-memory OFF string
//               .off: file bytes read into std::string
//               .obj/.stl: file → polygon soup → OFF text string
//               (all formats produce an OFF text intermediate; this is the
//               compromise that lets us time file I/O separately from parsing)
//   import_ms — OFF string → Nef_polyhedron_3 via OFF_to_nef_3
//               (includes OFF text parsing but excludes file I/O)
//   export_ms — Nef_polyhedron_3 → IndexedMesh
//               (convert_nef_polyhedron_to_polygon_soup + to_double conversion;
//               excludes disk write)
// ---------------------------------------------------------------------------

#include "runner_types.hh"

#include <CGAL/OFF_to_nef_3.h>
#include <CGAL/boost/graph/convert_nef_polyhedron_to_polygon_mesh.h>

// Polygon-soup I/O (CGAL 5.x+)
#include <CGAL/IO/OBJ.h>
#include <CGAL/IO/STL.h>
#include <CGAL/Simple_cartesian.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
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

// Build an in-memory OFF string from a polygon soup.
inline std::string soup_to_off_string(std::vector<SoupPoint> const& pts, std::vector<SoupFace> const& faces)
{
    std::ostringstream oss;
    oss << "OFF\n";
    oss << pts.size() << " " << faces.size() << " 0\n";
    oss.precision(17);
    for (auto const& p : pts)
        oss << p.x() << " " << p.y() << " " << p.z() << "\n";
    for (auto const& f : faces)
    {
        oss << f.size();
        for (auto idx : f)
            oss << " " << idx;
        oss << "\n";
    }
    return oss.str();
}

// Homogeneous Kernel::Point_3 → double Cartesian triple.
inline std::array<double, 3> to_cartesian_d(Kernel::Point_3 const& p)
{
    double hw = CGAL::to_double(p.hw());
    return {
        CGAL::to_double(p.hx()) / hw,
        CGAL::to_double(p.hy()) / hw,
        CGAL::to_double(p.hz()) / hw,
    };
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
// load_raw_off_string — io phase
//
// Reads the source file and produces an in-memory OFF text string.
// This is the io_ms boundary: file I/O and any format conversion up to (but
// not including) OFF_to_nef_3.
//
// For .off files: reads raw bytes into a string (no parsing).
// For .obj/.stl files: parses into polygon soup, serialises to OFF text.
// ---------------------------------------------------------------------------
inline std::string load_raw_off_string(std::string const& path)
{
    namespace d = mesh_io_detail;

    std::string const ext = d::lower_extension(path);

    if (ext == ".off")
    {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open())
            throw std::runtime_error("Cannot open OFF file: " + path);
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }

    if (ext == ".obj")
    {
        std::vector<d::SoupPoint> pts;
        std::vector<d::SoupFace> faces;
        if (!CGAL::IO::read_OBJ(path, pts, faces))
            throw std::runtime_error("Failed to read OBJ file: " + path);
        return d::soup_to_off_string(pts, faces);
    }

    if (ext == ".stl")
    {
        std::vector<d::SoupPoint> pts;
        std::vector<d::SoupFace> faces;
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open())
            throw std::runtime_error("Cannot open STL file: " + path);
        if (!CGAL::IO::read_STL(f, pts, faces))
            throw std::runtime_error("Failed to read STL file: " + path);
        return d::soup_to_off_string(pts, faces);
    }

    throw std::runtime_error("Unsupported input format '" + ext + "' for file: " + path
                             + "  (supported: .off, .obj, .stl)");
}

// ---------------------------------------------------------------------------
// import_nef — import phase
//
// Converts an in-memory OFF text string into a Nef_polyhedron_3.
// This is the import_ms boundary: OFF text parsing via OFF_to_nef_3.
// Includes the text parsing cost but excludes all file I/O.
//
// skipped_facets is set to the number of facets that OFF_to_nef_3 rejected.
// ---------------------------------------------------------------------------
inline Nef import_nef(std::string const& off_str, std::size_t& skipped_facets)
{
    std::istringstream iss(off_str);
    Nef nef;
    skipped_facets = CGAL::OFF_to_nef_3(iss, nef);
    return nef;
}

// ---------------------------------------------------------------------------
// nef_to_indexed — export phase (conversion only)
//
// Converts a Nef_polyhedron_3 to an IndexedMesh (indexed-tris-f64).
// This is the export_ms boundary: topology extraction via
// convert_nef_polyhedron_to_polygon_soup (with triangulation=true) plus
// homogeneous → double Cartesian conversion.
// Does NOT write to disk.
// ---------------------------------------------------------------------------
inline IndexedMesh nef_to_indexed(Nef const& nef)
{
    namespace d = mesh_io_detail;

    // Extract triangulated polygon soup in the Nef kernel's point type.
    std::vector<Kernel::Point_3> nef_pts;
    std::vector<std::vector<std::size_t>> raw_faces;
    CGAL::convert_nef_polyhedron_to_polygon_soup(nef, nef_pts, raw_faces, /*triangulate=*/true);

    IndexedMesh im;
    im.vertices.reserve(nef_pts.size());
    for (auto const& p : nef_pts)
        im.vertices.push_back(d::to_cartesian_d(p));

    im.triangles.reserve(raw_faces.size());
    for (auto const& f : raw_faces)
    {
        if (f.size() < 3)
            continue;
        // triangulate=true should already give triangles, but fan-triangulate
        // defensively in case any polygon slips through.
        for (std::size_t j = 1; j + 1 < f.size(); ++j)
            im.triangles.push_back({f[0], f[j], f[j + 1]});
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

inline Nef load_nef(std::string const& path, std::size_t& skipped_facets)
{
    std::string const off_str = load_raw_off_string(path);
    return import_nef(off_str, skipped_facets);
}

inline void export_nef(Nef const& nef, std::string const& path)
{
    write_indexed(nef_to_indexed(nef), path);
}
