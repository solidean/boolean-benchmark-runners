#pragma once

// ---------------------------------------------------------------------------
// runner_mesh_helpers/off.hh
//
// OFF load (unrolled + indexed).
// Handles plain OFF, NOFF (normals ignored), COFF (colors ignored), NCOFF.
// Triangle meshes only — faces with != 3 vertices cause a runtime_error.
// Header-only, C++20 stdlib only, no external dependencies.
//
// Format reference: http://www.geomview.org/docs/html/OFF.html
// ---------------------------------------------------------------------------

#include "mesh.hh"

#include <cstdlib> // strtod, strtoul
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace runner_mesh_helpers
{

namespace detail
{

// Strip everything from '#' onwards, then trim trailing whitespace.
inline void stripOffComment(std::string& s)
{
    auto pos = s.find('#');
    if (pos != std::string::npos)
        s.resize(pos);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.pop_back();
}

// Read the next non-blank, non-comment line from the stream.
// Returns false when the stream is exhausted.
inline bool nextOffLine(std::ifstream& f, std::string& out, int& lineNum)
{
    while (std::getline(f, out))
    {
        ++lineNum;
        stripCr(out);
        stripOffComment(out);
        // Trim leading whitespace.
        std::size_t start = 0;
        while (start < out.size() && (out[start] == ' ' || out[start] == '\t'))
            ++start;
        if (start > 0)
            out.erase(0, start);
        if (!out.empty())
            return true;
    }
    return false;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

inline std::pair<std::vector<double>, std::vector<int>> loadOffIndexed(std::string const& path)
{
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("OFF: cannot open file: " + path);

    std::string line;
    int lineNum = 0;

    // --- Header line ---
    if (!detail::nextOffLine(f, line, lineNum))
        throw std::runtime_error("OFF: empty file: " + path);

    // Accept any variant ending in "OFF": OFF, NOFF, COFF, NCOFF, CNOFF.
    // Reject 4OFF (homogeneous coordinates — not supported).
    if (line.find("4OFF") != std::string::npos)
        throw std::runtime_error("OFF: 4OFF (homogeneous coordinates) is not supported: " + path);
    if (line.rfind("OFF") == std::string::npos)
        throw std::runtime_error("OFF: unrecognized header '" + line + "': " + path);

    // --- Counts line: nV nF nE ---
    if (!detail::nextOffLine(f, line, lineNum))
        throw std::runtime_error("OFF: missing counts line: " + path);

    char const* p = line.c_str();
    char* end;
    errno = 0;
    unsigned long nV = std::strtoul(p, &end, 10);
    if (end == p)
        throw std::runtime_error("OFF: bad nVertices at line " + std::to_string(lineNum) + ": " + path);
    p = end;
    unsigned long nF = std::strtoul(p, &end, 10);
    if (end == p)
        throw std::runtime_error("OFF: bad nFaces at line " + std::to_string(lineNum) + ": " + path);
    // nEdges is ignored.

    std::vector<double> verts;
    std::vector<int> tris;
    verts.reserve(nV * 3);
    tris.reserve(nF * 3);

    // --- Vertices ---
    for (unsigned long i = 0; i < nV; ++i)
    {
        if (!detail::nextOffLine(f, line, lineNum))
            throw std::runtime_error("OFF: expected " + std::to_string(nV) + " vertices but file ended at vertex "
                                     + std::to_string(i) + ": " + path);

        char const* s = line.c_str();
        char* e;
        double xyz[3];
        bool ok = true;
        for (int j = 0; j < 3 && ok; ++j)
        {
            xyz[j] = std::strtod(s, &e);
            ok = (e != s);
            s = e;
        }
        if (!ok)
            throw std::runtime_error("OFF: malformed vertex at line " + std::to_string(lineNum) + ": " + path);
        verts.push_back(xyz[0]);
        verts.push_back(xyz[1]);
        verts.push_back(xyz[2]);
        // Any remaining data on the line (COFF color channels, NOFF normals) is ignored.
    }

    // --- Faces ---
    for (unsigned long i = 0; i < nF; ++i)
    {
        if (!detail::nextOffLine(f, line, lineNum))
            throw std::runtime_error("OFF: expected " + std::to_string(nF) + " faces but file ended at face "
                                     + std::to_string(i) + ": " + path);

        char const* s = line.c_str();
        char* e;
        errno = 0;
        unsigned long n = std::strtoul(s, &e, 10);
        if (e == s)
            throw std::runtime_error("OFF: malformed face count at line " + std::to_string(lineNum) + ": " + path);
        s = e;

        if (n != 3)
            throw std::runtime_error("OFF: face must have exactly 3 vertices (got " + std::to_string(n) + ") at line "
                                     + std::to_string(lineNum) + ": " + path);

        for (unsigned long j = 0; j < 3; ++j)
        {
            errno = 0;
            unsigned long idx = std::strtoul(s, &e, 10);
            if (e == s)
                throw std::runtime_error("OFF: malformed face index at line " + std::to_string(lineNum) + ": " + path);
            s = e;
            if (idx >= verts.size() / 3)
                throw std::runtime_error("OFF: face index " + std::to_string(idx) + " out of range at line "
                                         + std::to_string(lineNum) + ": " + path);
            tris.push_back(static_cast<int>(idx));
        }
        // Trailing COFF face color data (remainder of line) is silently ignored.
    }

    return {std::move(verts), std::move(tris)};
}

inline std::vector<double> loadOffUnrolled(std::string const& path)
{
    auto [v, t] = loadOffIndexed(path);
    return indexedToUnrolled(v, t);
}

} // namespace runner_mesh_helpers
