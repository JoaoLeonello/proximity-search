#pragma once
// What a plain B-tree index on `lat` buys you in a relational DB: one range scan over a
// latitude band, then every row in that band is filtered by longitude and distance.
// The sorted vector is the B+tree leaf level, flattened; lower_bound is the root-to-leaf descent.
#include "geo.hpp"

struct BTreeIndex {
    static constexpr const char* name = "B-tree (lat)";
    struct Row { double lat, lng; uint32_t id; };
    std::vector<Row> rows;
    const std::vector<geo::Point>* pts{};

    void build(const std::vector<geo::Point>& p) {
        pts = &p;
        rows.clear();
        rows.reserve(p.size());
        for (uint32_t i = 0; i < p.size(); ++i) rows.push_back({p[i].lat, p[i].lng, i});
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.lat < b.lat; });
    }

    void radius(geo::Point c, double rKm, std::vector<uint32_t>& out) const {
        out.clear();
        geo::BBox b = geo::radiusBox(c, rKm);
        double r2 = rKm * rKm;
        auto it = std::lower_bound(rows.begin(), rows.end(), b.minLat,
                                   [](const Row& r, double v) { return r.lat < v; });
        for (; it != rows.end() && it->lat <= b.maxLat; ++it)
            if (it->lng >= b.minLng && it->lng <= b.maxLng && geo::dist2(c, {it->lat, it->lng}) <= r2)
                out.push_back(it->id);
    }
    void knn(geo::Point c, size_t k, std::vector<uint32_t>& out) const {
        geo::knnByExpandingRadius(*this, *pts, c, k, out);
    }
};
