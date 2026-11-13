# boundary-extract

Extract administrative boundaries from an OSM .pbf into a directory tree.

## Build
Needs libosmium, protozero, GEOS, expat, zlib, bz2, CMake.

    cmake -S . -B build && cmake --build build -j

## Usage

    build/boundary-extract in.pbf outdir [--min-level N] [--max-level N] [--buffer METERS] [--simplify] [--tol F] [--output LIST]

- `--min-level/--max-level`: admin_level range (default: all)
- `--buffer METERS`: grow geometry by this distance (always covers the original); applied before `--simplify`
- `--simplify`: simplify geometry before writing (result always covers the original)
- `--tol F`: simplification tolerance as fraction of bbox diagonal (default 0.001)
- `--output LIST`: comma-separated and/or repeated: `poly`, `geojson`, `linestring` (default: `geojson`)

## Output
    outdir/4-Nordrhein-Westfalen.poly
    outdir/4-Nordrhein-Westfalen.geojson
    outdir/4-Nordrhein-Westfalen.linestring.geojson
    outdir/4-Nordrhein-Westfalen/5-Regierungsbezirk Detmold.geojson
    ...

- Directories mirror the geometric containment hierarchy (levels may be skipped).
- `.geojson`: (Multi)Polygon, OSM tags as properties, plus `@id` (`relation/N`).
- `.linestring.geojson`: outline as (Multi)LineString.

## License
CC0 1.0 (public domain dedication).
