# Plan

Goal: read a .pbf, assemble all administrative boundaries, write hierarchical
GeoJSON + simplified GeoJSON + .poly.

Stack: C++17, CMake, libosmium, GEOS (C API).

## Decisions
- CLI: `boundary-extract in.pbf outdir [--min-level N] [--max-level N] [--tol F]`; levels default all.
- Pass 1: relations with `boundary=administrative` + numeric `admin_level` only.
- Pass 2: osmium MultipolygonManager -> areas -> WKB -> GEOS (made valid).
- Hierarchy: geometric. Parent = nearest lower-level boundary (highest level, then
  smallest area) that contains the child's point-on-surface. Levels may be skipped.
- Layout: `<lvl>-<name>.geojson` next to dir `<lvl>-<name>/` holding children.
  Name collisions in a dir get `_<relation id>`.
- Per boundary: `.geojson` (all tags as properties + `@id`), `.simple.geojson`, `.poly`.
- Simplification only expands: topology-preserving DP (tol = F * bbox diagonal, default 0.001),
  if not covering the original: mitre-buffer by growing distance until `covers()`.
  Fallback: original geometry.
- `.poly`: Osmosis format, one section per polygon, `!` prefix for holes.

## Test data
/scratch/local/nrw.pbf
