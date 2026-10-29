#pragma once
// Geohash as a 64-bit integer: lng/lat cell indices Morton-interleaved (lng bit first).
// Base32 geohash strings are this same bit stream, 5 bits per character; the integer form
// just skips string compares. A geohash prefix is a cell, and a cell is a contiguous range
// of the sorted codes, so a lookup is a handful of binary searches (Redis GEO works this way).
#include <unordered_map>
#include "geo.hpp"
#include "paged.hpp"

namespace gh {

constexpr int kBits = 26;  // per axis -> 52-bit codes, ~0.6 m cells

inline uint64_t spread(uint32_t x) {
    uint64_t v = x;
    v = (v | v << 16) & 0x0000FFFF0000FFFFull;
    v = (v | v << 8) & 0x00FF00FF00FF00FFull;
    v = (v | v << 4) & 0x0F0F0F0F0F0F0F0Full;
    v = (v | v << 2) & 0x3333333333333333ull;
    v = (v | v << 1) & 0x5555555555555555ull;
    return v;
}
inline uint64_t interleave(uint32_t x, uint32_t y) { return spread(x) << 1 | spread(y); }
inline uint32_t cellX(double lng, int bits) { return uint32_t((lng + 180.0) / 360.0 * double(1u << bits)); }
inline uint32_t cellY(double lat, int bits) { return uint32_t((lat + 90.0) / 180.0 * double(1u << bits)); }

// Classic geohash search: pick the finest precision whose cell is still >= r, then visit the
// code range [lo, hi) of the query's cell and of its 8 neighbours. The circle can never leave
// that 3x3 block.
template <class F>
void forEachNeighbourRange(geo::Point c, double rKm, F f) {
    double coarsest = std::min(360.0 * geo::kKmPerDegLng, 180.0 * geo::kKmPerDegLat);
    int bits = std::clamp(int(std::floor(std::log2(coarsest / rKm))), 1, kBits);
    int shift = 2 * (kBits - bits);
    int64_t cx = cellX(c.lng, bits), cy = cellY(c.lat, bits);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            uint64_t prefix = interleave(uint32_t(cx + dx), uint32_t(cy + dy));
            f(prefix << shift, (prefix + 1) << shift);
        }
}

inline uint64_t code(geo::Point p) { return interleave(cellX(p.lng, kBits), cellY(p.lat, kBits)); }

}  // namespace gh

struct Geohash {
    static constexpr const char* name = "Geohash";
    struct Row { uint64_t code; double lat, lng; uint32_t id; };
    std::vector<Row> rows;  // sorted by code
    const std::vector<geo::Point>* pts{};

    void build(const std::vector<geo::Point>& p) {
        pts = &p;
        rows.clear();
        rows.reserve(p.size());
        for (uint32_t i = 0; i < p.size(); ++i)
            rows.push_back({gh::code(p[i]), p[i].lat, p[i].lng, i});
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.code < b.code; });
    }

    void radius(geo::Point c, double rKm, std::vector<uint32_t>& out) const {
        out.clear();
        double r2 = rKm * rKm;
        gh::forEachNeighbourRange(c, rKm, [&](uint64_t lo, uint64_t hi) {
            auto it = std::lower_bound(rows.begin(), rows.end(), lo,
                                       [](const Row& r, uint64_t v) { return r.code < v; });
            for (; it != rows.end() && it->code < hi; ++it)
                if (geo::dist2(c, {it->lat, it->lng}) <= r2) out.push_back(it->id);
        });
    }
    void knn(geo::Point c, size_t k, std::vector<uint32_t>& out) const {
        geo::knnByExpandingRadius(*this, *pts, c, k, out);
    }
};

// Geohash for moving points. A sorted vector would shift the whole array on every move, so the
// codes live in a paged B+tree instead: a move is erase(old code) + insert(new code), touching
// one page. Redis GEOADD does the same over a sorted set.
struct MovingGeohash {
    static constexpr const char* name = "Geohash";
    struct Row {
        uint64_t code; double lat, lng; uint32_t id;
        bool operator<(const Row& o) const { return code != o.code ? code < o.code : id < o.id; }
    };
    PagedSet<Row> rows;
    const std::vector<geo::Point>* pts{};

    static Row row(geo::Point p, uint32_t id) { return {gh::code(p), p.lat, p.lng, id}; }

    void build(const std::vector<geo::Point>& p) {
        pts = &p;
        std::vector<Row> r(p.size());
        for (uint32_t i = 0; i < p.size(); ++i) r[i] = row(p[i], i);
        rows.build(std::move(r));
    }
    void move(uint32_t id, geo::Point from, geo::Point to) {
        rows.erase(row(from, id));
        rows.insert(row(to, id));
    }
    void radius(geo::Point c, double rKm, std::vector<uint32_t>& out) const {
        out.clear();
        double r2 = rKm * rKm;
        gh::forEachNeighbourRange(c, rKm, [&](uint64_t lo, uint64_t hi) {
            rows.scanFrom({lo, 0, 0, 0}, [&](const Row& r) {
                if (r.code >= hi) return false;
                if (geo::dist2(c, {r.lat, r.lng}) <= r2) out.push_back(r.id);
                return true;
            });
        });
    }
    void knn(geo::Point c, size_t k, std::vector<uint32_t>& out) const {
        geo::knnByExpandingRadius(*this, *pts, c, k, out);
    }
};

// Polygons in a geohash grid: each zone is registered in every fixed-precision cell its bbox
// touches. Lookups hash the query's cell(s) and test the candidates exactly.
struct GeohashPoly {
    static constexpr const char* name = "Geohash";
    static constexpr int kCellBits = 14;  // ~2.2 x 1.2 km cells (geohash length ~5.6)
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells;
    const std::vector<geo::Polygon>* polys{};
    mutable std::vector<uint32_t> seen;  // per-query dedupe stamps
    mutable uint32_t epoch = 0;

    template <class F>
    static void forEachCell(const geo::BBox& b, F f) {
        for (uint32_t y = gh::cellY(b.minLat, kCellBits); y <= gh::cellY(b.maxLat, kCellBits); ++y)
            for (uint32_t x = gh::cellX(b.minLng, kCellBits); x <= gh::cellX(b.maxLng, kCellBits); ++x)
                f(gh::interleave(x, y));
    }

    void build(const std::vector<geo::Polygon>& p) {
        polys = &p;
        cells.clear();
        seen.assign(p.size(), 0);
        for (uint32_t i = 0; i < p.size(); ++i) forEachCell(p[i].box, [&](uint64_t c) { cells[c].push_back(i); });
    }

    void containing(geo::Point p, std::vector<uint32_t>& out) const {
        out.clear();
        auto it = cells.find(gh::interleave(gh::cellX(p.lng, kCellBits), gh::cellY(p.lat, kCellBits)));
        if (it == cells.end()) return;
        for (uint32_t id : it->second)
            if (geo::containsPoint((*polys)[id], p)) out.push_back(id);
    }
    void overlapping(const geo::Polygon& q, std::vector<uint32_t>& out) const {
        out.clear();
        ++epoch;
        forEachCell(q.box, [&](uint64_t c) {
            auto it = cells.find(c);
            if (it == cells.end()) return;
            for (uint32_t id : it->second)
                if (seen[id] != epoch) {
                    seen[id] = epoch;
                    if (geo::intersects((*polys)[id], q)) out.push_back(id);
                }
        });
    }
};
