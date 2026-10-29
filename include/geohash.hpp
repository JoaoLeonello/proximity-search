#pragma once
// Geohash as a 64-bit integer: lng/lat cell indices Morton-interleaved (lng bit first).
// Base32 geohash strings are this same bit stream, 5 bits per character; the integer form
// just skips string compares. A geohash prefix is a cell, and a cell is a contiguous range
// of the sorted codes, so a lookup is a handful of binary searches (Redis GEO works this way).
#include <unordered_map>
#include "geo.hpp"

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
            rows.push_back({gh::interleave(gh::cellX(p[i].lng, gh::kBits), gh::cellY(p[i].lat, gh::kBits)),
                            p[i].lat, p[i].lng, i});
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.code < b.code; });
    }

    // Classic geohash search: pick the finest precision whose cell is still >= r, then scan
    // the query's cell plus its 8 neighbours. The circle can never leave that 3x3 block.
    void radius(geo::Point c, double rKm, std::vector<uint32_t>& out) const {
        out.clear();
        double r2 = rKm * rKm;
        double coarsest = std::min(360.0 * geo::kKmPerDegLng, 180.0 * geo::kKmPerDegLat);
        int bits = std::clamp(int(std::floor(std::log2(coarsest / rKm))), 1, gh::kBits);
        int shift = 2 * (gh::kBits - bits);
        int64_t cx = gh::cellX(c.lng, bits), cy = gh::cellY(c.lat, bits);

        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                uint64_t prefix = gh::interleave(uint32_t(cx + dx), uint32_t(cy + dy));
                uint64_t lo = prefix << shift, hi = (prefix + 1) << shift;
                auto it = std::lower_bound(rows.begin(), rows.end(), lo,
                                           [](const Row& r, uint64_t v) { return r.code < v; });
                for (; it != rows.end() && it->code < hi; ++it)
                    if (geo::dist2(c, {it->lat, it->lng}) <= r2) out.push_back(it->id);
            }
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
