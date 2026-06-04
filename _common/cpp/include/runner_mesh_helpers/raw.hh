#pragma once

// ---------------------------------------------------------------------------
// runner_mesh_helpers/raw.hh
//
// Raw binary mesh formats — essentially memcpy to/from disk. The extension
// alone is the schema: there is no header version or metadata. Native byte
// order (little-endian on supported hosts), matching the binary-STL loader.
//
//   .raw-f64      — unrolled. A flat dump of doubles, 9 per triangle
//                   ([x0,y0,z0, x1,y1,z1, x2,y2,z2] × N). No index buffer.
//
//   .raw-f64-i32  — indexed. Layout:
//                     uint32  vertex_count
//                     uint32  triangle_count
//                     double  vertices[3 * vertex_count]
//                     int32   indices [3 * triangle_count]
//
// Header-only, C++20 stdlib only, no external dependencies.
// ---------------------------------------------------------------------------

#include "mesh.hh"

#include <cstdint>
#include <cstring> // memcpy
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace runner_mesh_helpers
{

namespace detail
{

// Open a file for binary reading and return its size in bytes.
inline std::uint64_t openBinaryForRead(std::ifstream& f, std::string const& path)
{
    f.open(path, std::ios::binary);
    if (!f.is_open())
        throw std::runtime_error("raw: cannot open file: " + path);
    f.seekg(0, std::ios::end);
    auto const size = static_cast<std::uint64_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    return size;
}

// Open a file for binary writing, creating parent directories as needed.
inline void openBinaryForWrite(std::ofstream& f, std::string const& path)
{
    if (auto parent = std::filesystem::path(path).parent_path(); !parent.empty())
        std::filesystem::create_directories(parent);
    f.open(path, std::ios::binary);
    if (!f.is_open())
        throw std::runtime_error("raw: cannot open file for writing: " + path);
}

} // namespace detail

// ---------------------------------------------------------------------------
// .raw-f64  (unrolled is the native form)
// ---------------------------------------------------------------------------

inline std::vector<double> loadRawF64Unrolled(std::string const& path)
{
    std::ifstream f;
    std::uint64_t const bytes = detail::openBinaryForRead(f, path);

    if (bytes % sizeof(double) != 0)
        throw std::runtime_error("raw-f64: file size is not a multiple of sizeof(double): " + path);
    std::size_t const count = static_cast<std::size_t>(bytes / sizeof(double));
    if (count % 9 != 0)
        throw std::runtime_error("raw-f64: double count must be a multiple of 9 (9 per triangle): " + path);

    std::vector<double> verts(count);
    if (count > 0)
    {
        f.read(reinterpret_cast<char*>(verts.data()), static_cast<std::streamsize>(bytes));
        if (!f)
            throw std::runtime_error("raw-f64: read error: " + path);
    }
    return verts;
}

inline std::pair<std::vector<double>, std::vector<int>> loadRawF64Indexed(std::string const& path)
{
    return unrolledToIndexed(loadRawF64Unrolled(path));
}

inline void saveRawF64Unrolled(std::string const& path, std::span<double const> verts)
{
    if (verts.size() % 9 != 0)
        throw std::runtime_error("saveRawF64Unrolled: vertex data size must be a multiple of 9");

    std::ofstream out;
    detail::openBinaryForWrite(out, path);
    if (!verts.empty())
        out.write(reinterpret_cast<char const*>(verts.data()),
                  static_cast<std::streamsize>(verts.size() * sizeof(double)));
}

inline void saveRawF64Indexed(std::string const& path, std::span<double const> verts, std::span<int const> tris)
{
    // The format carries no index buffer — expand to unrolled triangles first.
    saveRawF64Unrolled(path, indexedToUnrolled(verts, tris));
}

// ---------------------------------------------------------------------------
// .raw-f64-i32  (indexed is the native form)
// ---------------------------------------------------------------------------

inline std::pair<std::vector<double>, std::vector<int>> loadRawF64I32Indexed(std::string const& path)
{
    static_assert(sizeof(int) == 4, "raw-f64-i32 assumes 32-bit int for the index buffer");

    std::ifstream f;
    std::uint64_t const bytes = detail::openBinaryForRead(f, path);

    if (bytes < 8)
        throw std::runtime_error("raw-f64-i32: file too small for header: " + path);

    std::uint32_t vcnt = 0, tcnt = 0;
    f.read(reinterpret_cast<char*>(&vcnt), 4);
    f.read(reinterpret_cast<char*>(&tcnt), 4);
    if (!f)
        throw std::runtime_error("raw-f64-i32: read error on header: " + path);

    // File size must match the header exactly — guards against truncation/corruption.
    std::uint64_t const expected = 8ULL + 24ULL * vcnt + 12ULL * tcnt;
    if (expected != bytes)
        throw std::runtime_error("raw-f64-i32: file size " + std::to_string(bytes) + " does not match header (expected "
                                 + std::to_string(expected) + "): " + path);

    std::vector<double> verts(static_cast<std::size_t>(vcnt) * 3);
    std::vector<int> tris(static_cast<std::size_t>(tcnt) * 3);
    if (!verts.empty())
        f.read(reinterpret_cast<char*>(verts.data()), static_cast<std::streamsize>(verts.size() * sizeof(double)));
    if (!tris.empty())
        f.read(reinterpret_cast<char*>(tris.data()), static_cast<std::streamsize>(tris.size() * sizeof(int)));
    if (!f)
        throw std::runtime_error("raw-f64-i32: read error on body: " + path);

    return {std::move(verts), std::move(tris)};
}

inline std::vector<double> loadRawF64I32Unrolled(std::string const& path)
{
    auto [verts, tris] = loadRawF64I32Indexed(path);
    return indexedToUnrolled(verts, tris);
}

inline void saveRawF64I32Indexed(std::string const& path, std::span<double const> verts, std::span<int const> tris)
{
    static_assert(sizeof(int) == 4, "raw-f64-i32 assumes 32-bit int for the index buffer");

    if (verts.size() % 3 != 0)
        throw std::runtime_error("saveRawF64I32Indexed: vertex data size must be a multiple of 3");
    if (tris.size() % 3 != 0)
        throw std::runtime_error("saveRawF64I32Indexed: index count must be a multiple of 3");

    std::ofstream out;
    detail::openBinaryForWrite(out, path);

    std::uint32_t const vcnt = static_cast<std::uint32_t>(verts.size() / 3);
    std::uint32_t const tcnt = static_cast<std::uint32_t>(tris.size() / 3);
    out.write(reinterpret_cast<char const*>(&vcnt), 4);
    out.write(reinterpret_cast<char const*>(&tcnt), 4);
    if (!verts.empty())
        out.write(reinterpret_cast<char const*>(verts.data()),
                  static_cast<std::streamsize>(verts.size() * sizeof(double)));
    if (!tris.empty())
        out.write(reinterpret_cast<char const*>(tris.data()),
                  static_cast<std::streamsize>(tris.size() * sizeof(int)));
}

inline void saveRawF64I32Unrolled(std::string const& path, std::span<double const> verts)
{
    auto [v, t] = unrolledToIndexed(verts);
    saveRawF64I32Indexed(path, v, t);
}

} // namespace runner_mesh_helpers
