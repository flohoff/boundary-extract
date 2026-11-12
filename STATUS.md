# Status

- [x] PLAN.md, README.md
- [x] Implementation (src/main.cpp, single file)
- [x] Tested on /scratch/local/nrw.pbf: 2969 boundaries, ~37 s, 83 MB output

## Notes
- 2 relations (6521834, 6521836) fail osmium assembly (invalid area) -> skipped.
- Neighbouring-state boundaries (no level-2/parent in extract) land in the output root.
- Simplified geometry is verified with `covers()` at runtime; falls back to the original if not achievable.

## TODO
- Optional: parallel simplification, RFC 7946 ring orientation.
