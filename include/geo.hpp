#pragma once
// Shared geometry: points, boxes, convex polygons, distance, k-NN heap, data generators.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <queue>
#include <random>
#include <vector>

namespace geo {

constexpr double kInf = std::numeric_limits<double>::infinity();

struct Point { double lat, lng; };

struct BBox {
    double minLat = kInf, minLng = kInf, maxLat = -kInf, maxLng = -kInf;

    bool contains(Point p) const {
        return p.lat >= minLat && p.lat <= maxLat && p.lng >= minLng && p.lng <= maxLng;
    }
    bool contains(const BBox& o) const {
        return o.minLat >= minLat && o.maxLat <= maxLat && o.minLng >= minLng && o.maxLng <= maxLng;
    }
    bool intersects(const BBox& o) const {
        return o.minLat <= maxLat && o.maxLat >= minLat && o.minLng <= maxLng && o.maxLng >= minLng;
    }
    void expand(Point p) {
        minLat = std::min(minLat, p.lat); maxLat = std::max(maxLat, p.lat);
        minLng = std::min(minLng, p.lng); maxLng = std::max(maxLng, p.lng);
    }
    void expand(const BBox& o) {
        minLat = std::min(minLat, o.minLat); maxLat = std::max(maxLat, o.maxLat);
        minLng = std::min(minLng, o.minLng); maxLng = std::max(maxLng, o.maxLng);
    }
    Point mid() const { return {(minLat + maxLat) / 2, (minLng + maxLng) / 2}; }
};

// Benchmark region: greater Sao Paulo (~50 x 50 km).
inline const BBox kRegion{-23.80, -46.85, -23.35, -46.35};

// Equirectangular projection at the region's latitude: a true Euclidean metric on the
// projected plane, within ~0.3% of haversine at city scale. Being exact in its own plane
// lets every index be checked bit-for-bit against the naive scan.
constexpr double kKmPerDegLat = 110.574;
inline const double kKmPerDegLng = 111.320 * std::cos(-23.575 * std::numbers::pi / 180.0);

inline double dist2(Point a, Point b) {
    double dy = (a.lat - b.lat) * kKmPerDegLat, dx = (a.lng - b.lng) * kKmPerDegLng;
    return dx * dx + dy * dy;
}
// Squared distance from p to the closest point of b (0 if inside). Lower bound used for pruning.
inline double minDist2(Point p, const BBox& b) {
    return dist2(p, {std::clamp(p.lat, b.minLat, b.maxLat), std::clamp(p.lng, b.minLng, b.maxLng)});
}
inline BBox radiusBox(Point c, double rKm) {
    double dLat = rKm / kKmPerDegLat, dLng = rKm / kKmPerDegLng;
    return {c.lat - dLat, c.lng - dLng, c.lat + dLat, c.lng + dLng};
}

// ---- Convex polygons (x = lng, y = lat, counter-clockwise) ----

struct Polygon { std::vector<Point> v; BBox box; };

inline double cross(Point o, Point a, Point b) {
    return (a.lng - o.lng) * (b.lat - o.lat) - (a.lat - o.lat) * (b.lng - o.lng);
}
inline bool containsPoint(const Polygon& poly, Point p) {
    if (!poly.box.contains(p)) return false;
    for (size_t i = 0, n = poly.v.size(); i < n; ++i)
        if (cross(poly.v[i], poly.v[(i + 1) % n], p) < 0) return false;
    return true;
}
// Separating axis theorem: two convex polygons are disjoint iff some edge of one has the
// whole other polygon strictly on its outer side.
inline bool hasSeparatingEdge(const Polygon& a, const Polygon& b) {
    for (size_t i = 0, n = a.v.size(); i < n; ++i) {
        Point p = a.v[i], q = a.v[(i + 1) % n];
        if (std::all_of(b.v.begin(), b.v.end(), [&](Point r) { return cross(p, q, r) < 0; })) return true;
    }
    return false;
}
inline bool intersects(const Polygon& a, const Polygon& b) {
    return a.box.intersects(b.box) && !hasSeparatingEdge(a, b) && !hasSeparatingEdge(b, a);
}

// ---- k nearest neighbours ----

// Max-heap of the best k (dist2, id) seen so far. Ties broken by id so every index agrees.
struct KnnHeap {
    size_t k;
    std::priority_queue<std::pair<double, uint32_t>> h;

    void push(double d2, uint32_t id) {
        if (h.size() < k) h.emplace(d2, id);
        else if (std::pair{d2, id} < h.top()) { h.pop(); h.emplace(d2, id); }
    }
    double worst() const { return h.size() < k ? kInf : h.top().first; }
    void drain(std::vector<uint32_t>& out) {
        out.resize(h.size());
        for (size_t i = out.size(); i-- > 0; h.pop()) out[i] = h.top().second;
    }
};

// For indexes with no native nearest-neighbour walk (B-tree, geohash): grow the circle until it
// holds k points. Correct because once k points lie within r, the k nearest all lie within r.
template <class Index>
void knnByExpandingRadius(const Index& idx, const std::vector<Point>& pts, Point c, size_t k,
                          std::vector<uint32_t>& out) {
    for (double r = 0.25; ; r *= 2) {
        idx.radius(c, r, out);
        if (out.size() >= k || r > 200) break;
    }
    KnnHeap heap{k};
    for (uint32_t id : out) heap.push(dist2(c, pts[id]), id);
    heap.drain(out);
}

// ---- Data generators ----

// 70% of points around 12 fixed "downtown" hotspots (sigma ~2 km), 30% uniform. Real
// location data is clustered, and that is exactly what separates adaptive from fixed grids.
inline std::vector<Point> randomPoints(size_t n, uint64_t seed) {
    std::mt19937_64 rng(seed), spotRng(42);
    std::uniform_real_distribution<double> lat(kRegion.minLat, kRegion.maxLat), lng(kRegion.minLng, kRegion.maxLng);
    std::vector<Point> spots(12);
    for (auto& s : spots) s = {lat(spotRng), lng(spotRng)};
    std::normal_distribution<double> g(0.0, 0.02);
    std::uniform_real_distribution<double> u01(0, 1);
    std::uniform_int_distribution<size_t> pick(0, spots.size() - 1);

    std::vector<Point> pts(n);
    for (auto& p : pts) {
        if (u01(rng) < 0.7) {
            Point s = spots[pick(rng)];
            p = {std::clamp(s.lat + g(rng), kRegion.minLat, kRegion.maxLat),
                 std::clamp(s.lng + g(rng), kRegion.minLng, kRegion.maxLng)};
        } else {
            p = {lat(rng), lng(rng)};
        }
    }
    return pts;
}

// Convex "zones" (delivery areas, geofences): 5-8 vertices on a circle of random radius.
inline std::vector<Polygon> randomPolygons(size_t n, uint64_t seed, double rMinKm, double rMaxKm) {
    auto centers = randomPoints(n, seed);
    std::mt19937_64 rng(seed * 7919);
    std::uniform_real_distribution<double> radius(rMinKm, rMaxKm), angle(0, 2 * std::numbers::pi);
    std::uniform_int_distribution<int> sides(5, 8);

    std::vector<Polygon> polys(n);
    for (size_t i = 0; i < n; ++i) {
        double r = radius(rng);
        std::vector<double> a(sides(rng));
        for (auto& x : a) x = angle(rng);
        std::sort(a.begin(), a.end());
        for (double t : a) {
            Point p{centers[i].lat + r * std::sin(t) / kKmPerDegLat, centers[i].lng + r * std::cos(t) / kKmPerDegLng};
            polys[i].v.push_back(p);
            polys[i].box.expand(p);
        }
    }
    return polys;
}

}  // namespace geo
