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
// Supported input formats:  .obj  .stl  .off
// Supported output formats: .obj  (only — others throw std::runtime_error)
//
// Header-only, C++20 stdlib only, no external dependencies.
// Add  runners/_common/cpp/include  to your include path.
// ---------------------------------------------------------------------------

#include "mesh.hh"
#include "obj.hh"
#include "off.hh"
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
    throw std::runtime_error("loadFromFileIndexed: unsupported format '" + ext
                             + "' (supported: .obj, .stl, .off) — path: " + path);
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
    throw std::runtime_error("loadFromFileUnrolled: unsupported format '" + ext
                             + "' (supported: .obj, .stl, .off) — path: " + path);
}

// ---------------------------------------------------------------------------
// Save  (.obj only)
// ---------------------------------------------------------------------------

inline void saveToFileIndexed(std::string const& path, std::span<double const> verts, std::span<int const> tris)
{
    std::string const ext = detail::lowerExtension(path);
    if (ext == ".obj")
    {
        saveObjIndexed(path, verts, tris);
        return;
    }
    throw std::runtime_error("saveToFileIndexed: only .obj output is supported; got '" + ext + "' — path: " + path);
}

inline void saveToFileUnrolled(std::string const& path, std::span<double const> verts)
{
    std::string const ext = detail::lowerExtension(path);
    if (ext == ".obj")
    {
        saveObjUnrolled(path, verts);
        return;
    }
    throw std::runtime_error("saveToFileUnrolled: only .obj output is supported; got '" + ext + "' — path: " + path);
}

} // namespace runner_mesh_helpers
