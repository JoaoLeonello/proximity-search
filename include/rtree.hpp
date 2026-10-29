#pragma once
// R-tree, bulk-loaded with Sort-Tile-Recursive (STR): sort by x, cut into vertical slices,
// sort each slice by y, pack runs of kFan into nodes, repeat one level up. Nodes are
// bounding boxes of their children, so the tree stores rectangles natively (PostGIS GiST
// indexes are R-tree-like).
#include <utility>
#include "geo.hpp"

struct RTree {
    static constexpr uint32_t kFan = 16;
    struct Entry { geo::BBox box; uint32_t id; };
    struct Node { geo::BBox box; uint32_t first, count; bool leaf; };  // children: [first, first+count)
    std::vector<Entry> entries;
    std::vector<Node> nodes;
    uint32_t root = 0;

    // Reorders v in STR order and returns the (first, count) of each group of <= kFan.
    template <class T>
    static std::vector<std::pair<uint32_t, uint32_t>> strPack(std::vector<T>& v) {
        size_t n = v.size(), groups = (n + kFan - 1) / kFan;
        size_t slice = size_t(std::ceil(std::sqrt(double(groups)))) * kFan;
        auto byX = [](const T& a, const T& b) { return a.box.minLng + a.box.maxLng < b.box.minLng + b.box.maxLng; };
        auto byY = [](const T& a, const T& b) { return a.box.minLat + a.box.maxLat < b.box.minLat + b.box.maxLat; };
        std::sort(v.begin(), v.end(), byX);
        std::vector<std::pair<uint32_t, uint32_t>> out;
        for (size_t s = 0; s < n; s += slice) {
            size_t e = std::min(n, s + slice);
            std::sort(v.begin() + s, v.begin() + e, byY);
            for (size_t g = s; g < e; g += kFan) out.emplace_back(uint32_t(g), uint32_t(std::min<size_t>(kFan, e - g)));
        }
        return out;
    }

    void build(std::vector<Entry> e) {
        entries = std::move(e);
        nodes.clear();
        if (entries.empty()) return;
        std::vector<Node> level;
        for (auto [f, c] : strPack(entries)) {
            Node leaf{{}, f, c, true};
            for (uint32_t i = f; i < f + c; ++i) leaf.box.expand(entries[i].box);
            level.push_back(leaf);
        }
        while (level.size() > 1) {
            auto groups = strPack(level);
            uint32_t base = uint32_t(nodes.size());
            nodes.insert(nodes.end(), level.begin(), level.end());
            std::vector<Node> up;
            for (auto [f, c] : groups) {
                Node parent{{}, base + f, c, false};
                for (uint32_t i = base + f; i < base + f + c; ++i) parent.box.expand(nodes[i].box);
                up.push_back(parent);
            }
            level = std::move(up);
        }
        nodes.push_back(level[0]);
        root = uint32_t(nodes.size() - 1);
    }

    // Depth-first walk into every node (and entry) whose box passes `keep`.
    template <class Keep, class Visit>
    void search(Keep keep, Visit visit) const {
        if (nodes.empty()) return;
        std::vector<uint32_t> stack{root};
        while (!stack.empty()) {
            const Node& n = nodes[stack.back()];
            stack.pop_back();
            if (!keep(n.box)) continue;
            for (uint32_t i = n.first; i < n.first + n.count; ++i) {
                if (!n.leaf) stack.push_back(i);
                else if (keep(entries[i].box)) visit(entries[i].id);
            }
        }
    }

    // Best-first k-NN over entry boxes (for points the box *is* the point, so distance is exact).
    void knn(geo::Point c, size_t k, std::vector<uint32_t>& out) const {
        geo::KnnHeap heap{k};
        std::priority_queue<std::pair<double, uint32_t>, std::vector<std::pair<double, uint32_t>>, std::greater<>> pq;
        if (!nodes.empty()) pq.emplace(0.0, root);
        while (!pq.empty() && pq.top().first <= heap.worst()) {
            const Node& n = nodes[pq.top().second];
            pq.pop();
            for (uint32_t i = n.first; i < n.first + n.count; ++i) {
                if (n.leaf) heap.push(geo::minDist2(c, entries[i].box), entries[i].id);
                else pq.emplace(geo::minDist2(c, nodes[i].box), i);
            }
        }
        heap.drain(out);
    }
};

struct RTreeIndex {
    static constexpr const char* name = "R-tree (STR)";
    RTree tree;

    void build(const std::vector<geo::Point>& p) {
        std::vector<RTree::Entry> e(p.size());
        for (uint32_t i = 0; i < p.size(); ++i) e[i] = {{p[i].lat, p[i].lng, p[i].lat, p[i].lng}, i};
        tree.build(std::move(e));
    }
    void radius(geo::Point c, double rKm, std::vector<uint32_t>& out) const {
        out.clear();
        double r2 = rKm * rKm;
        tree.search([&](const geo::BBox& b) { return geo::minDist2(c, b) <= r2; },
                    [&](uint32_t id) { out.push_back(id); });
    }
    void knn(geo::Point c, size_t k, std::vector<uint32_t>& out) const { tree.knn(c, k, out); }
};

struct RTreePoly {
    static constexpr const char* name = "R-tree (STR)";
    RTree tree;
    const std::vector<geo::Polygon>* polys{};

    void build(const std::vector<geo::Polygon>& p) {
        polys = &p;
        std::vector<RTree::Entry> e(p.size());
        for (uint32_t i = 0; i < p.size(); ++i) e[i] = {p[i].box, i};
        tree.build(std::move(e));
    }
    void containing(geo::Point p, std::vector<uint32_t>& out) const {
        out.clear();
        tree.search([&](const geo::BBox& b) { return b.contains(p); },
                    [&](uint32_t id) { if (geo::containsPoint((*polys)[id], p)) out.push_back(id); });
    }
    void overlapping(const geo::Polygon& q, std::vector<uint32_t>& out) const {
        out.clear();
        tree.search([&](const geo::BBox& b) { return b.intersects(q.box); },
                    [&](uint32_t id) { if (geo::intersects((*polys)[id], q)) out.push_back(id); });
    }
};
