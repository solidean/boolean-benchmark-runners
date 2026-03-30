# Boolean Benchmark Runner Repository

This repo contains small standalone runners that all support the same request/result protocol to run standardized benchmarks.

| Project                                                                                                       | Type     | Variant                        | Version | Capabilities | License     |
| ------------------------------------------------------------------------------------------------------------- | -------- | ------------------------------ | ------- | ------------ | ----------- |
| [Blender](https://www.blender.org/)                                                                           | Tool     | [Exact](blender/5.1/runner.yaml)                          | 5.1     |              | GPL         |
| [Blender](https://www.blender.org/)                                                                           | Tool     | [Fast](blender/5.1/runner.yaml)                           | 5.1     |              | GPL         |
| [Carve](https://github.com/VTREEM/Carve)                                                                      | Library  | [Default](carve/2014-9/runner.yaml)                        | 2014-9  | R            | GPL         |
| [CGAL](https://www.cgal.org/)                                                                                 | Library  | [Corefine (EPEC)](cgal/6.1.1/corefine/epec/runner.yaml)                | 6.1.1   | E R I        | GPL / Commercial |
| [CGAL](https://www.cgal.org/)                                                                                 | Library  | [Nef (EH-EI)](cgal/6.1.1/nef/exact_homogeneous_integer/runner.yaml)           | 6.1.1   | E R I S      | GPL / Commercial |
| [CGAL](https://www.cgal.org/)                                                                                 | Library  | [Nef (EH-EI, Reg)](cgal/6.1.1/nef/exact_homogeneous_integer/runner.yaml)               | 6.1.1   | E R I S      | GPL / Commercial |
| [Geogram](https://github.com/BrunoLevy/geogram)                                                               | Library  | [Boolean](geogram/1.9.8/boolean/runner.yaml)                        | 1.9.8   | E R S        | BSD-3       |
| [Interactive and Robust Mesh Booleans](https://github.com/gcherchi/InteractiveAndRobustMeshBooleans)          | Research | [Default](interactive-and-robust-mesh-booleans/2024-6/runner.yaml)                        | 2024-6  | R            | MIT         |
| [Manifold](https://github.com/elalish/manifold)                                                               | Library  | [Default](manifold/3.4.0/runner.yaml)                        | 3.4.0   | R I          | Apache-2.0  |
| [mcut](https://github.com/cutdigital/mcut)                                                                    | Library  | [Default](mcut/1.3.0/runner.yaml)                        | 1.3.0   | R            | LGPL / Commercial |
| [Mesh Arrangements for Solid Geometry](https://github.com/wjakob/mesh-arrangements)                           | Research | [Default](mesh-arrangements-for-solid-geometry/2.6.0/runner.yaml)                        | 2.6.0   | E R S        | GPL / Commercial (via CGAL) |
| [QuickCSG](https://gitlab.inria.fr/quickcsg/quickcsg)                                                         | Research | [Default](quickcsg/2022-10/runner.yaml)                        | 2022-10 |              | Free (non-commercial) / Commercial |
| [Solidean](https://solidean.com)                                                                              | Library  | [Default](solidean/2026.1/runner.yaml)                        | 2026.1  | E R I S      | Proprietary (Free non-commercial) |
| [Trueform](https://github.com/polydera/trueform)                                                              | Library  | [Default](trueform/0.7.0/runner.yaml)                        | 0.7.0   | R I S        | Free (non-commercial) / Commercial |
| [VTK](https://vtk.org/)                                                                                       | Library  | [BoolOp](vtk/9.6.0/boolean_polydata/runner.yaml) | 9.6.0   |              | BSD-3        |
| [VTK](https://vtk.org/)                                                                                       | Library  | [LoopBool](vtk/9.6.0/loop_boolean/runner.yaml)      | 9.6.0   |              | BSD-3        |

## Capability Legend

Capabilities are self-reported by the respective projects. They reflect what a method claims within its stated preconditions, not independent verification. A later benchmark may show issues even if a method claims robustness.

- [**E**] Exact arithmetic: exact constructions, not just predicates. Operations like (A \ B) u (A n B) == A hold exactly.
- [**R**] Robust: the method claims robustness within its stated preconditions.
- [**I**] Stable under repeated/iterative operations: the result is usable for additional booleans without loss of information. Methods that output indexed float/double triangle meshes generally cannot claim this unless specifically designed for it.
- [**S**] Supports self-intersections: the method must support some self-intersecting input meshes properly and by design.

## Runner Folder Structure

Each runner lives under `<family>/<version>/` (or `<family>/<version>/<variant>/`):

```
runner.yaml          # runner identity and runtime config (mandatory)
build.py             # build script — run with: uv run build.py
CMakeLists.txt       # CMake project (C++ runners)
vcpkg.json           # vcpkg manifest declaring C++ dependencies
src/main.cc          # adapter source code
.gitignore           # ignores auto-created bin/ and download/
bin/                 # auto-created: compiled executables, build-info.json
download/            # auto-created: downloaded/cloned upstream sources
```

Only the `runner.yaml` is mandatory, the rest depends on the runner type. A common C++ setup is shown.

## Quickstart

* `uv run build-all.py -y` builds all runners, accepting all licenses (can take 30+ min cold)
  (`-y` means you accept all licenses, including the free non-commercial licenses of each runner;
  omit `-y` to be prompted interactively where required)
* `uv run bootstrap-vcpkg.py` (optional, pre-warms vcpkg packages for all runners, 60+ min cold)

## TODOs

- Blender: needs an exact + self-intersection variant (the current Exact variant does not support self-intersecting input).
