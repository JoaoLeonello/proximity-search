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

// Dynamic R-tree for moving points (Guttman 1984): insert descends to the leaf that needs the
// least enlargement and splits overflowing nodes with the quadratic split; delete finds the
// leaf, removes the entry and reinserts the points of any node that drops below kMin. This is
// the insert/delete path a GiST index takes; the STR tree above can only be rebuilt.
struct DynamicRTree {
    static constexpr const char* name = "R-tree (Guttman)";
    static constexpr size_t kMax = 16, kMin = 6;
    struct Item { geo::Point p; uint32_t id; };
    struct Node {
        geo::BBox box;
        int32_t parent = -1;
        bool leaf = true;
        std::vector<Item> items;     // leaf
        std::vector<int32_t> kids;   // internal
    };
    // shortcut: nodes dropped by condense() stay in the vector unused; add a free list for long runs.
    std::vector<Node> nodes;
    int32_t root = 0;

    static geo::BBox pointBox(geo::Point p) { return {p.lat, p.lng, p.lat, p.lng}; }
    static double area(const geo::BBox& b) { return (b.maxLat - b.minLat) * (b.maxLng - b.minLng); }
    static double enlargement(geo::BBox b, const geo::BBox& add) {
        double before = area(b);
        b.expand(add);
        return area(b) - before;
    }

    void recompute(int32_t n) {
        geo::BBox b;
        for (const Item& it : nodes[n].items) b.expand(it.p);
        for (int32_t k : nodes[n].kids) b.expand(nodes[k].box);
        nodes[n].box = b;
    }

    // Quadratic split: seed the two groups with the pair that would waste the most area together,
    // then hand out the rest, most decided entry first. `a` keeps one group; the other is returned.
    template <class T, class BoxOf>
    static std::vector<T> quadraticSplit(std::vector<T>& a, BoxOf boxOf) {
        std::vector<T> all = std::move(a), b;
        a.clear();
        size_t s1 = 0, s2 = 1;
        double worst = -geo::kInf;
        for (size_t i = 0; i < all.size(); ++i)
            for (size_t j = i + 1; j < all.size(); ++j) {
                geo::BBox u = boxOf(all[i]);
                u.expand(boxOf(all[j]));
                double waste = area(u) - area(boxOf(all[i])) - area(boxOf(all[j]));
                if (waste > worst) worst = waste, s1 = i, s2 = j;
            }
        geo::BBox ba = boxOf(all[s1]), bb = boxOf(all[s2]);
        a.push_back(all[s1]);
        b.push_back(all[s2]);
        std::vector<bool> used(all.size());
        used[s1] = used[s2] = true;
        for (size_t left = all.size() - 2; left > 0; --left) {
            // One group needs every remaining entry to reach kMin: give them all to it.
            if (a.size() + left == kMin || b.size() + left == kMin) {
                auto& g = a.size() + left == kMin ? a : b;
                for (size_t i = 0; i < all.size(); ++i)
                    if (!used[i]) g.push_back(all[i]);
                break;
            }
            size_t pick = 0;
            double best = -1, ea = 0, eb = 0;
            for (size_t i = 0; i < all.size(); ++i) {
                if (used[i]) continue;
                double da = enlargement(ba, boxOf(all[i])), db = enlargement(bb, boxOf(all[i]));
                if (std::abs(da - db) > best) best = std::abs(da - db), pick = i, ea = da, eb = db;
            }
            used[pick] = true;
            bool toA = ea != eb ? ea < eb : area(ba) != area(bb) ? area(ba) < area(bb) : a.size() <= b.size();
            (toA ? a : b).push_back(all[pick]);
            (toA ? ba : bb).expand(boxOf(all[pick]));
        }
        return b;
    }

    // Walk from n to the root, tightening boxes and splitting any node over kMax.
    void adjustUp(int32_t n) {
        for (; n >= 0; n = nodes[n].parent) {
            if ((nodes[n].leaf ? nodes[n].items.size() : nodes[n].kids.size()) <= kMax) {
                recompute(n);
                continue;
            }
            int32_t sib = int32_t(nodes.size());
            nodes.push_back({});
            nodes[sib].leaf = nodes[n].leaf;
            if (nodes[n].leaf) {
                nodes[sib].items = quadraticSplit(nodes[n].items, [](const Item& it) { return pointBox(it.p); });
            } else {
                nodes[sib].kids = quadraticSplit(nodes[n].kids, [&](int32_t k) { return nodes[k].box; });
                for (int32_t k : nodes[sib].kids) nodes[k].parent = sib;
            }
            recompute(n);
            recompute(sib);
            if (nodes[n].parent < 0) {  // root split: the tree grows one level
                int32_t r = int32_t(nodes.size());
                nodes.push_back({});
                nodes[r].leaf = false;
                nodes[r].kids = {n, sib};
                nodes[n].parent = nodes[sib].parent = r;
                recompute(r);
                root = r;
                return;
            }
            nodes[sib].parent = nodes[n].parent;
            nodes[nodes[n].parent].kids.push_back(sib);
        }
    }

    void insert(geo::Point p, uint32_t id) {
        int32_t n = root;
        while (!nodes[n].leaf) {
            int32_t best = -1;
            double bestGrow = geo::kInf, bestArea = geo::kInf;
            for (int32_t k : nodes[n].kids) {
                double g = enlargement(nodes[k].box, pointBox(p)), a = area(nodes[k].box);
                if (g < bestGrow || (g == bestGrow && a < bestArea)) best = k, bestGrow = g, bestArea = a;
            }
            n = best;
        }
        nodes[n].items.push_back({p, id});
        adjustUp(n);
    }

    int32_t findLeaf(geo::Point p, uint32_t id) const {
        std::vector<int32_t> stack{root};
        while (!stack.empty()) {
            int32_t n = stack.back();
            stack.pop_back();
            if (!nodes[n].box.contains(p)) continue;
            if (!nodes[n].leaf) {
                stack.insert(stack.end(), nodes[n].kids.begin(), nodes[n].kids.end());
                continue;
            }
            for (const Item& it : nodes[n].items)
                if (it.id == id) return n;
        }
        return -1;
    }

    void collect(int32_t n, std::vector<Item>& out) const {
        out.insert(out.end(), nodes[n].items.begin(), nodes[n].items.end());
        for (int32_t k : nodes[n].kids) collect(k, out);
    }

    void remove(geo::Point p, uint32_t id) {
        int32_t n = findLeaf(p, id);
        auto& v = nodes[n].items;
        *std::find_if(v.begin(), v.end(), [&](const Item& it) { return it.id == id; }) = v.back();
        v.pop_back();

        // Condense: unlink underfull nodes on the way up and reinsert their points afterwards.
        std::vector<Item> orphans;
        for (; nodes[n].parent >= 0; n = nodes[n].parent) {
            size_t size = nodes[n].leaf ? nodes[n].items.size() : nodes[n].kids.size();
            if (size < kMin) {
                auto& kids = nodes[nodes[n].parent].kids;
                kids.erase(std::find(kids.begin(), kids.end(), n));
                collect(n, orphans);
            } else {
                recompute(n);
            }
        }
        recompute(root);
        while (!nodes[root].leaf && nodes[root].kids.size() == 1) {
            root = nodes[root].kids[0];
            nodes[root].parent = -1;
        }
        if (!nodes[root].leaf && nodes[root].kids.empty()) nodes[root].leaf = true;
        for (const Item& it : orphans) insert(it.p, it.id);
    }

    void build(const std::vector<geo::Point>& p) {
        nodes.assign(1, {});
        root = 0;
        for (uint32_t i = 0; i < p.size(); ++i) insert(p[i], i);
    }
    void move(uint32_t id, geo::Point from, geo::Point to) {
        remove(from, id);
        insert(to, id);
    }

    void radius(geo::Point c, double rKm, std::vector<uint32_t>& out) const {
        out.clear();
        double r2 = rKm * rKm;
        std::vector<int32_t> stack{root};
        while (!stack.empty()) {
            const Node& n = nodes[stack.back()];
            stack.pop_back();
            if (n.items.empty() && n.kids.empty()) continue;
            if (geo::minDist2(c, n.box) > r2) continue;
            for (const Item& it : n.items)
                if (geo::dist2(c, it.p) <= r2) out.push_back(it.id);
            stack.insert(stack.end(), n.kids.begin(), n.kids.end());
        }
    }
    void knn(geo::Point c, size_t k, std::vector<uint32_t>& out) const {
        geo::KnnHeap heap{k};
        std::priority_queue<std::pair<double, int32_t>, std::vector<std::pair<double, int32_t>>, std::greater<>> pq;
        pq.emplace(0.0, root);
        while (!pq.empty() && pq.top().first <= heap.worst()) {
            const Node& n = nodes[pq.top().second];
            pq.pop();
            for (const Item& it : n.items) heap.push(geo::dist2(c, it.p), it.id);
            for (int32_t kid : n.kids) pq.emplace(geo::minDist2(c, nodes[kid].box), kid);
        }
        heap.drain(out);
    }
};
