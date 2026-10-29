#pragma once
// A two-level B+tree for the moving-point indexes: sorted pages of up to kPage rows plus a
// routing array holding each page's first row. Rows stay contiguous inside a page, like a
// database leaf page, so a range scan reads memory sequentially; an insert or erase shifts at
// most one page. A node-per-row tree (std::set) would turn every step of a scan into a cache miss.
#include <algorithm>
#include <vector>

template <class Row>
struct PagedSet {
    static constexpr size_t kPage = 256;
    std::vector<std::vector<Row>> pages;
    std::vector<Row> firsts;  // shortcut: flat routing array, O(pages) shift on a split; fine to ~10^7 rows

    void build(std::vector<Row> rows) {
        std::sort(rows.begin(), rows.end());
        pages.clear();
        firsts.clear();
        for (size_t i = 0; i < rows.size(); i += kPage * 3 / 4) {  // leave room so early inserts don't split
            pages.emplace_back(rows.begin() + i, rows.begin() + std::min(rows.size(), i + kPage * 3 / 4));
            firsts.push_back(pages.back().front());
        }
        if (pages.empty()) pages.emplace_back(), firsts.push_back({});
    }

    size_t pageFor(const Row& r) const {
        size_t p = size_t(std::upper_bound(firsts.begin(), firsts.end(), r) - firsts.begin());
        return p ? p - 1 : 0;
    }

    void insert(const Row& r) {
        size_t p = pageFor(r);
        auto& v = pages[p];
        v.insert(std::lower_bound(v.begin(), v.end(), r), r);
        firsts[p] = v.front();
        if (v.size() > kPage) {
            std::vector<Row> right(v.begin() + kPage / 2, v.end());
            v.resize(kPage / 2);
            firsts.insert(firsts.begin() + p + 1, right.front());
            pages.insert(pages.begin() + p + 1, std::move(right));
        }
    }

    void erase(const Row& r) {
        size_t p = pageFor(r);
        auto& v = pages[p];
        v.erase(std::lower_bound(v.begin(), v.end(), r));
        if (!v.empty()) firsts[p] = v.front();
        else if (pages.size() > 1) pages.erase(pages.begin() + p), firsts.erase(firsts.begin() + p);
    }

    // Calls f on every row >= lo in order until f returns false.
    template <class F>
    void scanFrom(const Row& lo, F f) const {
        size_t p = pageFor(lo);
        auto it = std::lower_bound(pages[p].begin(), pages[p].end(), lo);
        for (;;) {
            for (; it != pages[p].end(); ++it)
                if (!f(*it)) return;
            if (++p == pages.size()) return;
            it = pages[p].begin();
        }
    }
};
