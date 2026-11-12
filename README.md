# boundary-extract

Extract administrative boundaries from an OSM .pbf into a directory tree.

## Build
Needs libosmium, protozero, GEOS, expat, zlib, bz2, CMake.

    cmake -S . -B build && cmake --build build -j

## Usage

    build/boundary-extract in.pbf outdir [--min-level N] [--max-level N] [--tol F]

- `--min-level/--max-level`: admin_level range (default: all)
- `--tol F`: simplification tolerance as fraction of bbox diagonal (default 0.001)

## Output
    outdir/4-Nordrhein-Westfalen.geojson
    outdir/4-Nordrhein-Westfalen.simple.geojson
    outdir/4-Nordrhein-Westfalen.simple.poly
    outdir/4-Nordrhein-Westfalen/5-Regierungsbezirk Detmold.geojson
    ...

- Directories mirror the geometric containment hierarchy (levels may be skipped).
- `.geojson`: full geometry, OSM tags as properties, plus `@id` (`relation/N`).
- `.simple.geojson` / `.simple.poly`: simplified geometry that always covers the original.
- `.simple.linestring.geojson`: outline of the simplified geometry as (Multi)LineString.
