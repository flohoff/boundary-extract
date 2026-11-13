// boundary-extract: OSM admin boundaries -> hierarchical geojson/poly
#include <osmium/area/assembler.hpp>
#include <osmium/area/multipolygon_manager.hpp>
#include <osmium/geom/wkb.hpp>
#include <osmium/handler/node_locations_for_ways.hpp>
#include <osmium/index/map/flex_mem.hpp>
#include <osmium/io/any_input.hpp>
#include <osmium/visitor.hpp>

#include <geos_c.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static GEOSContextHandle_t ctx;

struct GDel {
    void operator()(GEOSGeometry* g) const { if (g) GEOSGeom_destroy_r(ctx, g); }
};
using Geom = std::unique_ptr<GEOSGeometry, GDel>;

struct Boundary {
    long long id = 0;
    int level = 0;
    std::string name;
    std::vector<std::pair<std::string, std::string>> tags;
    Geom geom;
    double area = 0;
    int parent = -1;
    std::vector<int> children;
};

// ---------- geometry helpers ----------

static void collect_polys(const GEOSGeometry* g, std::vector<GEOSGeometry*>& out) {
    int t = GEOSGeomTypeId_r(ctx, g);
    if (t == GEOS_POLYGON) {
        out.push_back(GEOSGeom_clone_r(ctx, g));
    } else if (t == GEOS_MULTIPOLYGON || t == GEOS_GEOMETRYCOLLECTION) {
        int n = GEOSGetNumGeometries_r(ctx, g);
        for (int i = 0; i < n; ++i) collect_polys(GEOSGetGeometryN_r(ctx, g, i), out);
    }
}

// valid (multi)polygon only
static Geom clean_polygons(Geom g) {
    if (!g) return g;
    if (!GEOSisValid_r(ctx, g.get())) {
        Geom v{GEOSMakeValid_r(ctx, g.get())};
        if (!v) return v;
        g = std::move(v);
    }
    int t = GEOSGeomTypeId_r(ctx, g.get());
    if (t == GEOS_POLYGON || t == GEOS_MULTIPOLYGON) return g;
    std::vector<GEOSGeometry*> polys;
    collect_polys(g.get(), polys);
    if (polys.empty()) return Geom{};
    return Geom{GEOSGeom_createCollection_r(ctx, GEOS_MULTIPOLYGON, polys.data(), polys.size())};
}

static bool covers(const GEOSGeometry* a, const GEOSGeometry* b) {
    return GEOSCovers_r(ctx, a, b) == 1;
}

static Geom clone(const GEOSGeometry* g) { return Geom{GEOSGeom_clone_r(ctx, g)}; }

// Simplified geometry that is guaranteed to cover g (falls back to g).
static Geom simplify_cover(const GEOSGeometry* g, double frac) {
    double x0, y0, x1, y1;
    GEOSGeom_getXMin_r(ctx, g, &x0);
    GEOSGeom_getYMin_r(ctx, g, &y0);
    GEOSGeom_getXMax_r(ctx, g, &x1);
    GEOSGeom_getYMax_r(ctx, g, &y1);
    double tol = frac * std::hypot(x1 - x0, y1 - y0);
    if (tol <= 0) return clone(g);

    Geom s{GEOSTopologyPreserveSimplify_r(ctx, g, tol)};
    if (s && covers(s.get(), g)) return s;
    if (!s) return clone(g);

    GEOSBufferParams* bp = GEOSBufferParams_create_r(ctx);
    GEOSBufferParams_setJoinStyle_r(ctx, bp, GEOSBUF_JOIN_MITRE);
    GEOSBufferParams_setMitreLimit_r(ctx, bp, 3.0);
    GEOSBufferParams_setQuadrantSegments_r(ctx, bp, 2);
    double d = tol * 1.05;
    Geom result;
    for (int i = 0; i < 6 && !result; ++i, d *= 1.5) {
        Geom b{GEOSBufferWithParams_r(ctx, s.get(), bp, d)};
        if (!b || !covers(b.get(), g)) continue;
        Geom t{GEOSTopologyPreserveSimplify_r(ctx, b.get(), tol * 0.25)};
        result = (t && covers(t.get(), g)) ? std::move(t) : std::move(b);
    }
    GEOSBufferParams_destroy_r(ctx, bp);
    return result ? std::move(result) : clone(g);
}

// Buffer by meters (local equirectangular scaling); result always covers g.
static int scale_x(double* x, double* y, void* ud) {
    (void)y;
    *x *= *static_cast<double*>(ud);
    return 1;
}

static Geom buffer_meters(const GEOSGeometry* g, double meters) {
    double y0, y1;
    GEOSGeom_getYMin_r(ctx, g, &y0);
    GEOSGeom_getYMax_r(ctx, g, &y1);
    double k = std::max(std::cos((y0 + y1) / 2 * M_PI / 180), 0.01);
    double inv = 1 / k;
    Geom p{GEOSGeom_transformXY_r(ctx, g, scale_x, &k)};
    if (!p) return clone(g);
    Geom b{GEOSBuffer_r(ctx, p.get(), meters / 111320.0, 8)};
    if (!b) return clone(g);
    Geom back{GEOSGeom_transformXY_r(ctx, b.get(), scale_x, &inv)};
    if (!back) return clone(g);
    Geom u{GEOSUnion_r(ctx, back.get(), g)};
    return clean_polygons(u ? std::move(u) : std::move(back));
}

// ---------- output ----------

static std::string json_esc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += char(c);
        }
    }
    return o;
}

static void write_geojson(const fs::path& p, const Boundary& b, const GEOSGeometry* g) {
    GEOSGeoJSONWriter* w = GEOSGeoJSONWriter_create_r(ctx);
    char* js = GEOSGeoJSONWriter_writeGeometry_r(ctx, w, g, -1);
    std::ofstream f(p);
    f << "{\"type\":\"Feature\",\"properties\":{\"@id\":\"relation/" << b.id << "\"";
    for (auto& [k, v] : b.tags) f << ",\"" << json_esc(k) << "\":\"" << json_esc(v) << "\"";
    f << "},\"geometry\":" << js << "}\n";
    GEOSFree_r(ctx, js);
    GEOSGeoJSONWriter_destroy_r(ctx, w);
}

static void write_ring(std::ofstream& f, const GEOSGeometry* ring) {
    const GEOSCoordSequence* cs = GEOSGeom_getCoordSeq_r(ctx, ring);
    unsigned n = 0;
    GEOSCoordSeq_getSize_r(ctx, cs, &n);
    for (unsigned i = 0; i < n; ++i) {
        double x, y;
        GEOSCoordSeq_getXY_r(ctx, cs, i, &x, &y);
        char buf[80];
        std::snprintf(buf, sizeof buf, "   %.7f   %.7f\n", x, y);
        f << buf;
    }
}

static void write_poly(const fs::path& p, const Boundary& b, const GEOSGeometry* g) {
    std::ofstream f(p);
    f << b.name << "\n";
    int sec = 0;
    int np = GEOSGetNumGeometries_r(ctx, g);
    for (int i = 0; i < np; ++i) {
        const GEOSGeometry* poly = GEOSGetGeometryN_r(ctx, g, i);
        f << ++sec << "\n";
        write_ring(f, GEOSGetExteriorRing_r(ctx, poly));
        f << "END\n";
        int nh = GEOSGetNumInteriorRings_r(ctx, poly);
        for (int h = 0; h < nh; ++h) {
            f << "!" << ++sec << "\n";
            write_ring(f, GEOSGetInteriorRingN_r(ctx, poly, h));
            f << "END\n";
        }
    }
    f << "END\n";
}

static std::string safe_name(const std::string& s) {
    std::string o;
    for (unsigned char c : s) o += (c < 0x20 || c == '/' || c == '\\' || c == 0x7f) ? '_' : char(c);
    if (o.size() > 150) {
        size_t n = 150;
        while (n > 0 && (static_cast<unsigned char>(o[n]) & 0xC0) == 0x80) --n;
        o.resize(n);
    }
    while (!o.empty() && (o[0] == '.' || o[0] == ' ')) o.erase(0, 1);
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

enum : unsigned { OUT_POLY = 1, OUT_GEOJSON = 2, OUT_LINESTRING = 4 };

static bool parse_outputs(const std::string& list, unsigned& mask) {
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t e = list.find(',', pos);
        if (e == std::string::npos) e = list.size();
        std::string t = list.substr(pos, e - pos);
        pos = e + 1;
        if (t.empty()) continue;
        unsigned bit;
        if (t == "poly") bit = OUT_POLY;
        else if (t == "geojson") bit = OUT_GEOJSON;
        else if (t == "linestring") bit = OUT_LINESTRING;
        else return false;
        mask |= bit;
    }
    return true;
}

struct Writer {
    std::vector<Boundary>& bs;
    double tol;
    unsigned outputs;
    bool simplify;
    double buffer;
    size_t done = 0, total = 0, fallback = 0;

    void write_all(const fs::path& stem, const Boundary& b, const GEOSGeometry* g) {
        if (outputs & OUT_POLY) write_poly(stem.string() + ".poly", b, g);
        if (outputs & OUT_GEOJSON) write_geojson(stem.string() + ".geojson", b, g);
        if (outputs & OUT_LINESTRING) {
            Geom ls{GEOSBoundary_r(ctx, g)};
            if (ls) write_geojson(stem.string() + ".linestring.geojson", b, ls.get());
        }
    }

    void emit(int idx, const fs::path& dir, const std::string& base) {
        Boundary& b = bs[idx];
        fs::path stem = dir / base;
        Geom buf;
        const GEOSGeometry* g = b.geom.get();
        if (buffer > 0) {
            buf = buffer_meters(g, buffer);
            g = buf.get();
        }
        if (simplify) {
            Geom s = simplify_cover(g, tol);
            write_all(stem, b, s.get());
        } else {
            write_all(stem, b, g);
        }
        if (++done % 1000 == 0) std::fprintf(stderr, "  written %zu/%zu\n", done, total);
        b.geom.reset();

        if (b.children.empty()) return;
        fs::create_directories(stem);
        std::set<std::string> used;
        for (int c : b.children) {
            Boundary& cb = bs[c];
            std::string n = safe_name(std::to_string(cb.level) + "-" +
                                      (cb.name.empty() ? "relation " + std::to_string(cb.id) : cb.name));
            if (!used.insert(n).second) {
                n += "_" + std::to_string(cb.id);
                used.insert(n);
            }
            emit(c, stem, n);
        }
    }
};

// ---------- osmium ----------

static const char* tag(const osmium::TagList& t, const char* k) { return t.get_value_by_key(k); }

static bool parse_level(const char* v, int& out) {
    if (!v || !*v) return false;
    char* e;
    long l = std::strtol(v, &e, 10);
    if (*e || l < 0 || l > 99) return false;
    out = int(l);
    return true;
}

struct Pass1 : osmium::handler::Handler {
    osmium::area::MultipolygonManager<osmium::area::Assembler>& mgr;
    int lo, hi;
    Pass1(osmium::area::MultipolygonManager<osmium::area::Assembler>& m, int l, int h) : mgr(m), lo(l), hi(h) {}
    void relation(const osmium::Relation& r) {
        const char* b = tag(r.tags(), "boundary");
        int lvl;
        if (b && std::string(b) == "administrative" && parse_level(tag(r.tags(), "admin_level"), lvl) &&
            lvl >= lo && lvl <= hi)
            mgr.relation(r);
    }
};

struct AreaCollector : osmium::handler::Handler {
    std::vector<Boundary>& out;
    int lo, hi;
    osmium::geom::WKBFactory<> wkb{osmium::geom::wkb_type::wkb, osmium::geom::out_type::binary};
    AreaCollector(std::vector<Boundary>& o, int l, int h) : out(o), lo(l), hi(h) {}

    void area(const osmium::Area& a) {
        if (a.from_way()) return;
        const char* b = tag(a.tags(), "boundary");
        int lvl;
        if (!b || std::string(b) != "administrative" || !parse_level(tag(a.tags(), "admin_level"), lvl) ||
            lvl < lo || lvl > hi)
            return;
        std::string w;
        try {
            w = wkb.create_multipolygon(a);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "relation %lld: %s\n", (long long)a.orig_id(), e.what());
            return;
        }
        Geom g{GEOSGeomFromWKB_buf_r(ctx, reinterpret_cast<const unsigned char*>(w.data()), w.size())};
        g = clean_polygons(std::move(g));
        if (!g || GEOSisEmpty_r(ctx, g.get())) return;
        Boundary bd;
        bd.id = a.orig_id();
        bd.level = lvl;
        const char* n = tag(a.tags(), "name");
        bd.name = n ? n : "";
        for (auto& t : a.tags()) bd.tags.emplace_back(t.key(), t.value());
        GEOSArea_r(ctx, g.get(), &bd.area);
        bd.geom = std::move(g);
        out.push_back(std::move(bd));
    }
};

// ---------- hierarchy ----------

struct QueryCtx {
    std::vector<int> hits;
};

static void collect_hit(void* item, void* ud) {
    static_cast<QueryCtx*>(ud)->hits.push_back(int(reinterpret_cast<intptr_t>(item)));
}

static void build_hierarchy(std::vector<Boundary>& bs) {
    GEOSSTRtree* tree = GEOSSTRtree_create_r(ctx, 10);
    for (size_t i = 0; i < bs.size(); ++i)
        GEOSSTRtree_insert_r(ctx, tree, bs[i].geom.get(), reinterpret_cast<void*>(intptr_t(i)));
    std::vector<const GEOSPreparedGeometry*> prep(bs.size(), nullptr);

    for (size_t i = 0; i < bs.size(); ++i) {
        Boundary& b = bs[i];
        Geom pt{GEOSPointOnSurface_r(ctx, b.geom.get())};
        if (!pt) continue;
        QueryCtx q;
        GEOSSTRtree_query_r(ctx, tree, b.geom.get(), collect_hit, &q);
        int best = -1;
        for (int c : q.hits) {
            if (c == int(i) || bs[c].level >= b.level) continue;
            if (best >= 0 && (bs[c].level < bs[best].level ||
                              (bs[c].level == bs[best].level && bs[c].area >= bs[best].area)))
                continue;
            if (!prep[c]) prep[c] = GEOSPrepare_r(ctx, bs[c].geom.get());
            if (GEOSPreparedContains_r(ctx, prep[c], pt.get()) == 1) best = c;
        }
        b.parent = best;
    }
    for (auto p : prep) if (p) GEOSPreparedGeom_destroy_r(ctx, p);
    GEOSSTRtree_destroy_r(ctx, tree);

    for (size_t i = 0; i < bs.size(); ++i)
        if (bs[i].parent >= 0) bs[bs[i].parent].children.push_back(int(i));
    for (auto& b : bs)
        std::sort(b.children.begin(), b.children.end(), [&](int x, int y) {
            if (bs[x].level != bs[y].level) return bs[x].level < bs[y].level;
            return bs[x].name < bs[y].name;
        });
}

// ---------- main ----------

static void usage() {
    std::fprintf(stderr, "usage: boundary-extract in.pbf outdir [--min-level N] [--max-level N] [--buffer METERS] [--simplify] [--tol F] [--output LIST]\n"
                         "  LIST: comma-separated (or repeated) of poly,geojson,linestring (default geojson)\n");
    std::exit(2);
}

int main(int argc, char** argv) {
    std::vector<std::string> pos;
    int lo = 0, hi = 99;
    double tol = 0.001;
    unsigned outputs = 0;
    bool simplify = false;
    double buffer = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--buffer" && i + 1 < argc) { buffer = std::atof(argv[++i]); continue; }
        if (a == "--simplify") { simplify = true; continue; }
        if (a == "--output" && i + 1 < argc) {
            if (!parse_outputs(argv[++i], outputs)) usage();
            continue;
        }
        if (a == "--min-level" && i + 1 < argc) lo = std::atoi(argv[++i]);
        else if (a == "--max-level" && i + 1 < argc) hi = std::atoi(argv[++i]);
        else if (a == "--tol" && i + 1 < argc) tol = std::atof(argv[++i]);
        else if (a.rfind("--", 0) == 0) usage();
        else pos.push_back(a);
    }
    if (pos.size() != 2) usage();
    if (!outputs) outputs = OUT_GEOJSON;

    ctx = GEOS_init_r();
    try {
        osmium::io::File file{pos[0]};
        using index_type = osmium::index::map::FlexMem<osmium::unsigned_object_id_type, osmium::Location>;
        using loc_handler = osmium::handler::NodeLocationsForWays<index_type>;

        osmium::area::Assembler::config_type acfg;
        osmium::area::MultipolygonManager<osmium::area::Assembler> mgr{acfg};

        std::fprintf(stderr, "pass 1: relations\n");
        {
            osmium::io::Reader r{file, osmium::osm_entity_bits::relation};
            Pass1 p1{mgr, lo, hi};
            osmium::apply(r, p1);
            r.close();
            mgr.prepare_for_lookup();
        }

        std::fprintf(stderr, "pass 2: assembling\n");
        std::vector<Boundary> bs;
        {
            index_type index;
            loc_handler locs{index};
            locs.ignore_errors();
            AreaCollector col{bs, lo, hi};
            osmium::io::Reader r{file};
            osmium::apply(r, locs, mgr.handler([&](osmium::memory::Buffer&& buf) { osmium::apply(buf, col); }));
            r.close();
        }
        std::fprintf(stderr, "assembled %zu boundaries\n", bs.size());

        std::fprintf(stderr, "building hierarchy\n");
        build_hierarchy(bs);

        std::fprintf(stderr, "writing\n");
        fs::create_directories(pos[1]);
        Writer w{bs, tol, outputs, simplify, buffer};
        w.total = bs.size();
        std::vector<int> roots;
        for (size_t i = 0; i < bs.size(); ++i) if (bs[i].parent < 0) roots.push_back(int(i));
        std::sort(roots.begin(), roots.end(), [&](int x, int y) {
            if (bs[x].level != bs[y].level) return bs[x].level < bs[y].level;
            return bs[x].name < bs[y].name;
        });
        std::set<std::string> used;
        for (int r : roots) {
            Boundary& b = bs[r];
            std::string n = safe_name(std::to_string(b.level) + "-" +
                                      (b.name.empty() ? "relation " + std::to_string(b.id) : b.name));
            if (!used.insert(n).second) {
                n += "_" + std::to_string(b.id);
                used.insert(n);
            }
            w.emit(r, pos[1], n);
        }
        std::fprintf(stderr, "done: %zu boundaries\n", w.done);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    GEOS_finish_r(ctx);
    return 0;
}
