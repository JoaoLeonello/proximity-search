#pragma once
// Baseline: no index, test everything. Every other structure must return exactly what this returns.
#include "geo.hpp"

struct Naive {
    static constexpr const char* name = "Naive scan";
    const std::vector<geo::Point>* pts{};

    void build(const std::vector<geo::Point>& p) { pts = &p; }

    void radius(geo::Point c, double rKm, std::vector<uint32_t>& out) const {
        out.clear();
        double r2 = rKm * rKm;
        for (uint32_t i = 0; i < pts->size(); ++i)
            if (geo::dist2(c, (*pts)[i]) <= r2) out.push_back(i);
    }
    void knn(geo::Point c, size_t k, std::vector<uint32_t>& out) const {
        geo::KnnHeap heap{k};
        for (uint32_t i = 0; i < pts->size(); ++i) heap.push(geo::dist2(c, (*pts)[i]), i);
        heap.drain(out);
    }
};

struct NaivePoly {
    static constexpr const char* name = "Naive scan";
    const std::vector<geo::Polygon>* polys{};

    void build(const std::vector<geo::Polygon>& p) { polys = &p; }

    void containing(geo::Point p, std::vector<uint32_t>& out) const {
        out.clear();
        for (uint32_t i = 0; i < polys->size(); ++i)
            if (geo::containsPoint((*polys)[i], p)) out.push_back(i);
    }
    void overlapping(const geo::Polygon& q, std::vector<uint32_t>& out) const {
        out.clear();
        for (uint32_t i = 0; i < polys->size(); ++i)
            if (geo::intersects((*polys)[i], q)) out.push_back(i);
    }
};
