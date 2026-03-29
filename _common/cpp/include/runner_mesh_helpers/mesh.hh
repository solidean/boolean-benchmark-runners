#pragma once

// ---------------------------------------------------------------------------
// runner_mesh_helpers/mesh.hh
//
// Shared utilities and conversions for triangle mesh data.
// Meshes are represented as plain flat arrays:
//
//   Unrolled: std::vector<double>
//     Layout: [x0,y0,z0, x1,y1,z1, ...], length = N*9 (N triangles)
//
//   Indexed:  std::pair<std::vector<double>, std::vector<int>>
//     First:  vertex positions, length = V*3
//     Second: triangle indices,  length = T*3
//
// Header-only, C++20 stdlib only, no external dependencies.
// ---------------------------------------------------------------------------

#include <bit>
#include <cctype>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace runner_mesh_helpers
{

// ---------------------------------------------------------------------------
// detail helpers
// ---------------------------------------------------------------------------

namespace detail
{

// Strip a trailing \r from a string (handles CRLF line endings).
inline void stripCr(std::string& s)
{
    if (!s.empty() && s.back() == '\r')
        s.pop_back();
}

struct Vec3Key
{
    uint64_t bx, by, bz;
    bool operator==(Vec3Key const& o) const noexcept { return bx == o.bx && by == o.by && bz == o.bz; }
};

struct Vec3KeyHash
{
    std::size_t operator()(Vec3Key const& k) const noexcept
    {
        // Shift-xor mix of three 64-bit values.
        uint64_t h = k.bx;
        h ^= (k.by << 31) | (k.by >> 33);
        h ^= (k.bz << 17) | (k.bz >> 47);
        h ^= h >> 30;
        h *= UINT64_C(0xbf58476d1ce4e5b9);
        h ^= h >> 27;
        h *= UINT64_C(0x94d049bb133111eb);
        h ^= h >> 31;
        return static_cast<std::size_t>(h);
    }
};

inline Vec3Key makeKey(double x, double y, double z)
{
    return {std::bit_cast<uint64_t>(x), std::bit_cast<uint64_t>(y), std::bit_cast<uint64_t>(z)};
}

// Extension helpers (no std::filesystem needed).
inline std::string lowerExtension(std::string const& path)
{
    // Find the last dot after the last directory separator.
    auto lastSep = path.find_last_of("/\\");
    auto searchFrom = (lastSep == std::string::npos) ? 0 : lastSep + 1;
    auto dot = path.rfind('.');
    if (dot == std::string::npos || dot < searchFrom)
        return {};
    std::string ext = path.substr(dot);
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Conversions
// ---------------------------------------------------------------------------

// Deduplicate flat unrolled triangle vertices into indexed form.
// verts.size() must be a multiple of 9 (3 coords × 3 vertices per triangle).
inline std::pair<std::vector<double>, std::vector<int>> unrolledToIndexed(std::span<double const> verts)
{
    if (verts.size() % 9 != 0)
        throw std::runtime_error("unrolledToIndexed: vertex data size must be a multiple of 9");

    std::vector<double> outVerts;
    std::vector<int> outTris;
    std::unordered_map<detail::Vec3Key, int, detail::Vec3KeyHash> seen;

    outTris.reserve(verts.size() / 3);

    for (std::size_t i = 0; i < verts.size(); i += 3)
    {
        double x = verts[i], y = verts[i + 1], z = verts[i + 2];
        auto key = detail::makeKey(x, y, z);
        auto [it, inserted] = seen.emplace(key, static_cast<int>(outVerts.size() / 3));
        if (inserted)
        {
            outVerts.push_back(x);
            outVerts.push_back(y);
            outVerts.push_back(z);
        }
        outTris.push_back(it->second);
    }

    return {std::move(outVerts), std::move(outTris)};
}

// Expand indexed triangles into flat unrolled form.
// tris.size() must be a multiple of 3.
inline std::vector<double> indexedToUnrolled(std::span<double const> verts, std::span<int const> tris)
{
    if (tris.size() % 3 != 0)
        throw std::runtime_error("indexedToUnrolled: index count must be a multiple of 3");

    std::vector<double> result;
    result.reserve(tris.size() * 3);
    for (int idx : tris)
    {
        result.push_back(verts[static_cast<std::size_t>(idx) * 3]);
        result.push_back(verts[static_cast<std::size_t>(idx) * 3 + 1]);
        result.push_back(verts[static_cast<std::size_t>(idx) * 3 + 2]);
    }
    return result;
}

} // namespace runner_mesh_helpers
