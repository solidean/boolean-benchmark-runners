#pragma once

// ---------------------------------------------------------------------------
// runner_mesh_helpers/io.hh
//
// Auto-dispatching mesh I/O umbrella.
//
// Usage:
//   #include <runner_mesh_helpers/io.hh>
//   auto [verts, tris] = runner_mesh_helpers::loadFromFileIndexed("model.obj");
//   runner_mesh_helpers::saveToFileIndexed("result.obj", verts, tris);
//
// Supported input formats:  .obj  .stl  .off  .raw-f64  .raw-f64-i32
// Supported output formats: .obj  .raw-f64  .raw-f64-i32
//                           (others throw std::runtime_error)
//
// The .raw-f64 / .raw-f64-i32 formats are raw binary dumps for high-throughput
// benchmarks — see raw.hh for their layout.
//
// Header-only, C++20 stdlib only, no external dependencies.
// Add  runners/_common/cpp/include  to your include path.
// ---------------------------------------------------------------------------

#include "mesh.hh"
#include "obj.hh"
#include "off.hh"
#include "raw.hh"
#include "stl.hh"

#include <stdexcept>
#include <string>


namespace runner_mesh_helpers
{

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

inline std::pair<std::vector<double>, std::vector<int>> loadFromFileIndexed(std::string const& path)
{
    std::string const ext = detail::lowerExtension(path);
    if (ext == ".obj")
        return loadObjIndexed(path);
    if (ext == ".stl")
        return loadStlIndexed(path);
    if (ext == ".off")
        return loadOffIndexed(path);
    if (ext == ".raw-f64")
        return loadRawF64Indexed(path);
    if (ext == ".raw-f64-i32")
        return loadRawF64I32Indexed(path);
    throw std::runtime_error("loadFromFileIndexed: unsupported format '" + ext
                             + "' (supported: .obj, .stl, .off, .raw-f64, .raw-f64-i32) — path: " + path);
}

inline std::vector<double> loadFromFileUnrolled(std::string const& path)
{
    std::string const ext = detail::lowerExtension(path);
    if (ext == ".obj")
        return loadObjUnrolled(path);
    if (ext == ".stl")
        return loadStlUnrolled(path);
    if (ext == ".off")
        return loadOffUnrolled(path);
    if (ext == ".raw-f64")
        return loadRawF64Unrolled(path);
    if (ext == ".raw-f64-i32")
        return loadRawF64I32Unrolled(path);
    throw std::runtime_error("loadFromFileUnrolled: unsupported format '" + ext
                             + "' (supported: .obj, .stl, .off, .raw-f64, .raw-f64-i32) — path: " + path);
}

// ---------------------------------------------------------------------------
// Save  (.obj  .raw-f64  .raw-f64-i32)
// ---------------------------------------------------------------------------

inline void saveToFileIndexed(std::string const& path, std::span<double const> verts, std::span<int const> tris)
{
    std::string const ext = detail::lowerExtension(path);
    if (ext == ".obj")
        return saveObjIndexed(path, verts, tris);
    if (ext == ".raw-f64")
        return saveRawF64Indexed(path, verts, tris);
    if (ext == ".raw-f64-i32")
        return saveRawF64I32Indexed(path, verts, tris);
    throw std::runtime_error("saveToFileIndexed: unsupported output format '" + ext
                             + "' (supported: .obj, .raw-f64, .raw-f64-i32) — path: " + path);
}

inline void saveToFileUnrolled(std::string const& path, std::span<double const> verts)
{
    std::string const ext = detail::lowerExtension(path);
    if (ext == ".obj")
        return saveObjUnrolled(path, verts);
    if (ext == ".raw-f64")
        return saveRawF64Unrolled(path, verts);
    if (ext == ".raw-f64-i32")
        return saveRawF64I32Unrolled(path, verts);
    throw std::runtime_error("saveToFileUnrolled: unsupported output format '" + ext
                             + "' (supported: .obj, .raw-f64, .raw-f64-i32) — path: " + path);
}

} // namespace runner_mesh_helpers
