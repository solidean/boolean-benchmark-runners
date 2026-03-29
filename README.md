# Boolean Benchmark Runner Repository

This repo contains small standalone runners that all support the same request/result protocol to run standardized benchmarks.

| Project                                                                                                       | Type     | Variant                        | Version | Capabilities | License     |
| ------------------------------------------------------------------------------------------------------------- | -------- | ------------------------------ | ------- | ------------ | ----------- |
| [Blender](https://www.blender.org/)                                                                           | Tool     | Exact                          | 5.1     |              | GPL         |
| [Blender](https://www.blender.org/)                                                                           | Tool     | Fast                           | 5.1     |              | GPL         |
| [Carve](https://github.com/VTREEM/Carve)                                                                      | Library  | Default                        | 2014-9  | R            | GPL         |
| [CGAL](https://www.cgal.org/)                                                                                 | Library  | Corefine (EPEC)                | 6.1.1   | E R I        | GPL / Commercial |
| [CGAL](https://www.cgal.org/)                                                                                 | Library  | Nef (EH-EI, Non-reg)           | 6.1.1   | E R I S      | GPL / Commercial |
| [CGAL](https://www.cgal.org/)                                                                                 | Library  | Nef (EH-EI, Reg)               | 6.1.1   | E R I S      | GPL / Commercial |
| [Geogram](https://github.com/BrunoLevy/geogram)                                                               | Library  | Boolean                        | 1.9.8   | E R S        | BSD-3       |
| [Interactive and Robust Mesh Booleans](https://github.com/gcherchi/InteractiveAndRobustMeshBooleans)          | Research | Default                        | 2024-6  | R            | MIT         |
| [Manifold](https://github.com/elalish/manifold)                                                               | Library  | Default                        | 3.4.0   | R I          | Apache-2.0  |
| [mcut](https://github.com/cutdigital/mcut)                                                                    | Library  | Default                        | 1.3.0   | R            | LGPL / Commercial |
| [Mesh Arrangements for Solid Geometry](https://github.com/wjakob/mesh-arrangements)                           | Research | Default                        | 2.6.0   | E R S        | GPL / Commercial (via CGAL) |
| [QuickCSG](https://gitlab.inria.fr/quickcsg/quickcsg)                                                         | Research | Default                        | 2022-10 |              | Free (non-commercial) / Commercial |
| [Solidean](https://solidean.com)                                                                              | Library  | Default                        | 2026.1  | E R I S      | Proprietary (Free non-commercial) |
| [Trueform](https://github.com/polydera/trueform)                                                              | Library  | Default                        | 0.7.0   | R I S        | Free (non-commercial) / Commercial |
| [VTK](https://vtk.org/)                                                                                       | Library  | BooleanOperationPolyDataFilter | 9.6.0   |              | BSD-3        |
| [VTK](https://vtk.org/)                                                                                       | Library  | LoopBooleanPolyDataFilter      | 9.6.0   |              | BSD-3        |

## Capability Legend

Capabilities are self-reported by the respective projects. They reflect what a method claims within its stated preconditions, not independent verification. A later benchmark may show issues even if a method claims robustness.

- **E** -- Exact arithmetic: exact constructions, not just predicates. Operations like (A \ B) u (A n B) == A hold exactly.
- **R** -- Robust: the method claims robustness within its stated preconditions.
- **I** -- Stable under repeated/iterative operations: the result is usable for additional booleans without loss of information. Methods that output indexed float/double triangle meshes generally cannot claim this unless specifically designed for it.
- **S** -- Supports self-intersections: the method must support some self-intersecting input meshes properly and by design.

## TODOs

- Blender: needs an exact + self-intersection variant (the current Exact variant does not support self-intersecting input).
