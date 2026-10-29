#pragma once
// Point quadtree: a leaf splits into 4 quadrants once it holds more than kCap points, so the
// tree is deep where data is dense and shallow where it is sparse.
#include <utility>
#include "geo.hpp"

struct Quadtree {
    static constexpr const char* name = "Quadtree";
    static constexpr size_t kCap = 32;
    static constexpr int kMaxDepth = 24;
    struct Item { geo::Point p; uint32_t id; };  // coordinates inline: no pointer chase per candidate
    struct Node { geo::BBox box; int32_t child = -1; std::vector<Item> items; };  // children: child..child+3
    std::vector<Node> nodes;

    static int quadrant(const geo::BBox& b, geo::Point p) {
        geo::Point m = b.mid();
        return (p.lat >= m.lat) << 1 | (p.lng >= m.lng);
    }
    void split(int n) {
        geo::BBox b = nodes[n].box;
        geo::Point m = b.mid();
        int32_t first = int32_t(nodes.size());
        for (int q = 0; q < 4; ++q)
            nodes.push_back({{q & 2 ? m.lat : b.minLat, q & 1 ? m.lng : b.minLng,
                              q & 2 ? b.maxLat : m.lat, q & 1 ? b.maxLng : m.lng}});
        nodes[n].child = first;
        for (Item it : std::exchange(nodes[n].items, {}))
            nodes[first + quadrant(b, it.p)].items.push_back(it);
    }
    void insert(geo::Point p, uint32_t id) {
        int n = 0, depth = 0;
        for (; nodes[n].child >= 0; ++depth) n = nodes[n].child + quadrant(nodes[n].box, p);
        nodes[n].items.push_back({p, id});
        if (nodes[n].items.size() > kCap && depth < kMaxDepth) split(n);
    }

    void build(const std::vector<geo::Point>& p) {
        geo::BBox root;
        for (auto& x : p) root.expand(x);
        nodes.assign(1, {root});
        for (uint32_t i = 0; i < p.size(); ++i) insert(p[i], i);
    }

    void radius(geo::Point c, double rKm, std::vector<uint32_t>& out) const {
        out.clear();
        double r2 = rKm * rKm;
        std::vector<int> stack{0};
        while (!stack.empty()) {
            const Node& n = nodes[stack.back()];
            stack.pop_back();
            if (geo::minDist2(c, n.box) > r2) continue;
            if (n.child < 0) {
                for (const Item& it : n.items)
                    if (geo::dist2(c, it.p) <= r2) out.push_back(it.id);
            } else {
                for (int q = 0; q < 4; ++q) stack.push_back(n.child + q);
            }
        }
    }
    // Best-first search: always expand the node closest to c; stop once the nearest unexplored
    // node is farther than the current k-th best point.
    void knn(geo::Point c, size_t k, std::vector<uint32_t>& out) const {
        geo::KnnHeap heap{k};
        std::priority_queue<std::pair<double, int>, std::vector<std::pair<double, int>>, std::greater<>> pq;
        pq.emplace(0.0, 0);
        while (!pq.empty() && pq.top().first <= heap.worst()) {
            const Node& n = nodes[pq.top().second];
            pq.pop();
            if (n.child < 0) {
                for (const Item& it : n.items) heap.push(geo::dist2(c, it.p), it.id);
            } else {
                for (int q = 0; q < 4; ++q) pq.emplace(geo::minDist2(c, nodes[n.child + q].box), n.child + q);
            }
        }
        heap.drain(out);
    }
};

// MX-CIF quadtree for polygons: each zone lives in the deepest node whose quadrant fully
// contains its bbox. Zones crossing a split line stay higher up, so there is no duplication.
struct QuadtreePoly {
    static constexpr const char* name = "Quadtree";
    static constexpr int kMaxDepth = 16;
    struct Node { geo::BBox box; int32_t child = -1; std::vector<uint32_t> items; };
    std::vector<Node> nodes;
    const std::vector<geo::Polygon>* polys{};

    // Quadrant fully containing b, or -1 if b straddles a split line.
    static int quadrant(const geo::BBox& node, const geo::BBox& b) {
        geo::Point m = node.mid();
        int y = b.minLat >= m.lat ? 2 : b.maxLat < m.lat ? 0 : -1;
        int x = b.minLng >= m.lng ? 1 : b.maxLng < m.lng ? 0 : -1;
        return x < 0 || y < 0 ? -1 : y | x;
    }
    void insert(uint32_t id) {
        const geo::BBox& b = (*polys)[id].box;
        int n = 0;
        for (int depth = 0, q; depth < kMaxDepth && (q = quadrant(nodes[n].box, b)) >= 0; ++depth) {
            if (nodes[n].child < 0) {
                geo::BBox nb = nodes[n].box;
                geo::Point m = nb.mid();
                nodes[n].child = int32_t(nodes.size());
                for (int k = 0; k < 4; ++k)
                    nodes.push_back({{k & 2 ? m.lat : nb.minLat, k & 1 ? m.lng : nb.minLng,
                                      k & 2 ? nb.maxLat : m.lat, k & 1 ? nb.maxLng : m.lng}});
            }
            n = nodes[n].child + q;
        }
        nodes[n].items.push_back(id);
    }

    void build(const std::vector<geo::Polygon>& p) {
        polys = &p;
        geo::BBox root;
        for (auto& x : p) root.expand(x.box);
        nodes.assign(1, {root});
        for (uint32_t i = 0; i < p.size(); ++i) insert(i);
    }

    // A point descends a single root-to-leaf path, testing the zones parked along the way.
    void containing(geo::Point p, std::vector<uint32_t>& out) const {
        out.clear();
        if (!nodes[0].box.contains(p)) return;
        for (int n = 0;;) {
            for (uint32_t id : nodes[n].items)
                if (geo::containsPoint((*polys)[id], p)) out.push_back(id);
            if (nodes[n].child < 0) break;
            n = nodes[n].child + Quadtree::quadrant(nodes[n].box, p);
        }
    }
    void overlapping(const geo::Polygon& q, std::vector<uint32_t>& out) const {
        out.clear();
        std::vector<int> stack{0};
        while (!stack.empty()) {
            const Node& n = nodes[stack.back()];
            stack.pop_back();
            if (!n.box.intersects(q.box)) continue;
            for (uint32_t id : n.items)
                if (geo::intersects((*polys)[id], q)) out.push_back(id);
            if (n.child >= 0)
                for (int k = 0; k < 4; ++k) stack.push_back(n.child + k);
        }
    }
};
