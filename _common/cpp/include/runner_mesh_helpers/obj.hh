#pragma once

// ---------------------------------------------------------------------------
// runner_mesh_helpers/obj.hh
//
// OBJ load (unrolled + indexed) and save (unrolled + indexed).
// Triangle meshes only — faces with != 3 vertices cause a runtime_error.
// Header-only, C++20 stdlib only, no external dependencies.
// ---------------------------------------------------------------------------

#include "mesh.hh"

#include <cerrno>
#include <cstdlib> // strtod, strtol
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace runner_mesh_helpers
{

namespace detail
{

// Parse the vertex index from an OBJ face token ("v", "v/t", "v/t/n", "v//n").
// Returns the 0-based index.  Throws on parse failure.
inline int parseObjFaceVertex(char const* token, std::size_t totalVertices, int lineNum)
{
    char* end;
    errno = 0;
    long idx = std::strtol(token, &end, 10);
    if (end == token || errno != 0)
        throw std::runtime_error("OBJ: bad face index '" + std::string(token) + "' at line " + std::to_string(lineNum));

    if (idx == 0)
        throw std::runtime_error("OBJ: face index 0 is invalid (1-based) at line " + std::to_string(lineNum));

    // Convert to 0-based; handle negative relative indices.
    int64_t idx0;
    if (idx > 0)
        idx0 = static_cast<int64_t>(idx) - 1;
    else
        idx0 = static_cast<int64_t>(totalVertices) + idx; // idx is negative

    if (idx0 < 0 || static_cast<std::size_t>(idx0) >= totalVertices)
        throw std::runtime_error("OBJ: face index out of range (" + std::to_string(idx) + ") at line "
                                 + std::to_string(lineNum));

    return static_cast<int>(idx0);
}

} // namespace detail

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

inline std::vector<double> loadObjUnrolled(std::string const& path)
{
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("OBJ: cannot open file: " + path);

    // rawVerts: flat [x,y,z, x,y,z, ...] of all 'v' lines
    std::vector<double> rawVerts;
    std::vector<double> result;
    std::string line;
    int lineNum = 0;

    while (std::getline(f, line))
    {
        ++lineNum;
        detail::stripCr(line);

        char const* p = line.c_str();
        while (*p == ' ' || *p == '\t')
            ++p;
        if (*p == '\0' || *p == '#')
            continue;

        if (p[0] == 'v' && (p[1] == ' ' || p[1] == '\t'))
        {
            char const* s = p + 2;
            char* end;
            double xyz[3];
            bool ok = true;
            for (int i = 0; i < 3 && ok; ++i)
            {
                xyz[i] = std::strtod(s, &end);
                ok = (end != s);
                s = end;
            }
            if (!ok)
                throw std::runtime_error("OBJ: malformed vertex at line " + std::to_string(lineNum));
            rawVerts.push_back(xyz[0]);
            rawVerts.push_back(xyz[1]);
            rawVerts.push_back(xyz[2]);
            continue;
        }

        if (p[0] == 'f' && (p[1] == ' ' || p[1] == '\t'))
        {
            char const* s = p + 2;
            int indices[3];
            int count = 0;
            while (*s)
            {
                while (*s == ' ' || *s == '\t')
                    ++s;
                if (*s == '\0' || *s == '#')
                    break;
                if (count == 3)
                    throw std::runtime_error("OBJ: face with more than 3 vertices at line " + std::to_string(lineNum));
                char const* tokStart = s;
                while (*s && *s != ' ' && *s != '\t')
                    ++s;
                std::string tok(tokStart, s);
                auto slash = tok.find('/');
                if (slash != std::string::npos)
                    tok.resize(slash);
                indices[count++] = detail::parseObjFaceVertex(tok.c_str(), rawVerts.size() / 3, lineNum);
            }
            if (count != 3)
                throw std::runtime_error("OBJ: face must have exactly 3 vertices at line " + std::to_string(lineNum));
            for (int i = 0; i < 3; ++i)
            {
                std::size_t base = static_cast<std::size_t>(indices[i]) * 3;
                result.push_back(rawVerts[base]);
                result.push_back(rawVerts[base + 1]);
                result.push_back(rawVerts[base + 2]);
            }
            continue;
        }

        // All other directives (vt, vn, vp, usemtl, mtllib, o, g, s, ...) are silently skipped.
    }

    return result;
}

inline std::pair<std::vector<double>, std::vector<int>> loadObjIndexed(std::string const& path)
{
    return unrolledToIndexed(loadObjUnrolled(path));
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

inline void saveObjIndexed(std::string const& path, std::span<double const> verts, std::span<int const> tris)
{
    if (verts.size() % 3 != 0)
        throw std::runtime_error("saveObjIndexed: vertex data size must be a multiple of 3");
    if (tris.size() % 3 != 0)
        throw std::runtime_error("saveObjIndexed: index count must be a multiple of 3");

    if (auto parent = std::filesystem::path(path).parent_path(); !parent.empty())
        std::filesystem::create_directories(parent);

    std::ofstream out(path);
    if (!out.is_open())
        throw std::runtime_error("OBJ: cannot open file for writing: " + path);

    out.precision(17);
    for (std::size_t i = 0; i < verts.size(); i += 3)
        out << "v " << verts[i] << ' ' << verts[i + 1] << ' ' << verts[i + 2] << '\n';
    for (std::size_t i = 0; i < tris.size(); i += 3)
        out << "f " << (tris[i] + 1) << ' ' << (tris[i + 1] + 1) << ' ' << (tris[i + 2] + 1) << '\n';
}

inline void saveObjUnrolled(std::string const& path, std::span<double const> verts)
{
    auto [v, t] = unrolledToIndexed(verts);
    saveObjIndexed(path, v, t);
}

} // namespace runner_mesh_helpers
