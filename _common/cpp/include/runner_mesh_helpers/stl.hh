#pragma once

// ---------------------------------------------------------------------------
// runner_mesh_helpers/stl.hh
//
// STL load (ASCII and binary auto-detected), unrolled + indexed variants.
// Header-only, C++20 stdlib only, no external dependencies.
//
// Binary detection: read 80-byte header + 4-byte triangle count, check if
// expected file size (80+4+count*50) matches actual size.  If yes → binary,
// else → ASCII.
// ---------------------------------------------------------------------------

#include "mesh.hh"

#include <cstdint>
#include <cstdlib> // strtod
#include <cstring> // memcpy, strncmp
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace runner_mesh_helpers
{

namespace detail
{

enum class StlFormat
{
    Binary,
    Ascii
};

// Determine whether an STL file is binary or ASCII.
// The stream must be open in binary mode and positioned at the beginning.
inline StlFormat detectStlFormat(std::ifstream& f, std::string const& path)
{
    // Seek to end to get file size.
    f.seekg(0, std::ios::end);
    auto fileSize = static_cast<std::uint64_t>(f.tellg());
    f.seekg(0, std::ios::beg);

    if (fileSize < 84) // Too small even for an empty binary STL.
        return StlFormat::Ascii;

    // Read 80-byte header + 4-byte triangle count.
    char header[80];
    f.read(header, 80);
    if (!f)
        throw std::runtime_error("STL: read error on header: " + path);

    std::uint32_t count = 0;
    f.read(reinterpret_cast<char*>(&count), 4);
    if (!f)
        throw std::runtime_error("STL: read error on triangle count: " + path);

    // Each binary triangle record is exactly 50 bytes.
    std::uint64_t expected = 80ULL + 4ULL + static_cast<std::uint64_t>(count) * 50ULL;
    f.seekg(0, std::ios::beg);

    return (expected == fileSize) ? StlFormat::Binary : StlFormat::Ascii;
}

// Read a little-endian float from a char buffer.
inline float readLeFloat(char const* buf)
{
    float v;
    std::memcpy(&v, buf, 4);
    return v;
}

inline std::vector<double> loadStlBinary(std::ifstream& f, std::string const& path)
{
    // Skip 80-byte header.
    f.seekg(80, std::ios::beg);

    std::uint32_t count = 0;
    f.read(reinterpret_cast<char*>(&count), 4);
    if (!f)
        throw std::runtime_error("STL: read error on triangle count: " + path);

    std::vector<double> verts;
    verts.reserve(static_cast<std::size_t>(count) * 9);

    char record[50];
    for (std::uint32_t i = 0; i < count; ++i)
    {
        f.read(record, 50);
        if (!f)
            throw std::runtime_error("STL: unexpected end of file at triangle " + std::to_string(i) + ": " + path);

        // Bytes 0-11: normal (ignored).
        // Bytes 12-47: 3 × (x, y, z) float.
        // Bytes 48-49: attribute byte count (ignored).
        for (int v = 0; v < 3; ++v)
        {
            verts.push_back(static_cast<double>(readLeFloat(record + 12 + v * 12 + 0)));
            verts.push_back(static_cast<double>(readLeFloat(record + 12 + v * 12 + 4)));
            verts.push_back(static_cast<double>(readLeFloat(record + 12 + v * 12 + 8)));
        }
    }

    return verts;
}

inline std::vector<double> loadStlAscii(std::ifstream& f, std::string const& path)
{
    std::vector<double> verts;
    std::string line;
    int lineNum = 0;
    int vertsInFacet = 0;

    while (std::getline(f, line))
    {
        ++lineNum;
        stripCr(line);

        // Trim leading whitespace.
        char const* p = line.c_str();
        while (*p == ' ' || *p == '\t')
            ++p;

        if (std::strncmp(p, "vertex", 6) == 0 && (p[6] == ' ' || p[6] == '\t'))
        {
            char const* s = p + 7;
            char* e;
            double x = std::strtod(s, &e);
            if (e == s)
                throw std::runtime_error("STL ASCII: malformed vertex at line " + std::to_string(lineNum) + ": " + path);
            s = e;
            double y = std::strtod(s, &e);
            if (e == s)
                throw std::runtime_error("STL ASCII: malformed vertex at line " + std::to_string(lineNum) + ": " + path);
            s = e;
            double z = std::strtod(s, &e);
            if (e == s)
                throw std::runtime_error("STL ASCII: malformed vertex at line " + std::to_string(lineNum) + ": " + path);
            verts.push_back(x);
            verts.push_back(y);
            verts.push_back(z);
            ++vertsInFacet;
        }
        else if (std::strncmp(p, "endfacet", 8) == 0)
        {
            if (vertsInFacet != 3)
                throw std::runtime_error("STL ASCII: facet must have exactly 3 vertices (got "
                                         + std::to_string(vertsInFacet) + ") before line " + std::to_string(lineNum)
                                         + ": " + path);
            vertsInFacet = 0;
        }
        // "solid", "endsolid", "facet normal", "outer loop", "endloop" → silently skip.
    }

    return verts;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

inline std::vector<double> loadStlUnrolled(std::string const& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open())
        throw std::runtime_error("STL: cannot open file: " + path);

    auto fmt = detail::detectStlFormat(f, path);
    f.seekg(0, std::ios::beg);

    if (fmt == detail::StlFormat::Binary)
        return detail::loadStlBinary(f, path);
    else
        return detail::loadStlAscii(f, path);
}

inline std::pair<std::vector<double>, std::vector<int>> loadStlIndexed(std::string const& path)
{
    return unrolledToIndexed(loadStlUnrolled(path));
}

} // namespace runner_mesh_helpers
