#pragma once

#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <CGAL/Surface_mesh.h>

using Kernel = CGAL::Exact_predicates_exact_constructions_kernel;
using Mesh   = CGAL::Surface_mesh<Kernel::Point_3>;
