#pragma once

#include <CGAL/Exact_integer.h>
#include <CGAL/Homogeneous.h>
#include <CGAL/Nef_polyhedron_3.h>

using Kernel = CGAL::Homogeneous<CGAL::Exact_integer>;
using Nef = CGAL::Nef_polyhedron_3<Kernel>;
