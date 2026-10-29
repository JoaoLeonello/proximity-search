// Runs every scenario against every structure, checks each result against the naive scan,
// and prints Markdown tables (paste-ready for the README).
//
//   ./bench [points=1000000] [queries=2000] [zones=20000]
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <tuple>
#include <utility>

#include "btree_index.hpp"
#include "geohash.hpp"
#include "naive.hpp"
#include "quadtree.hpp"
#include "rtree.hpp"

using Clock = std::chrono::steady_clock;

struct Result { const char* name; double us; uint64_t checksum; size_t hits; };

static bool allCorrect = true;
constexpr double kInfUs = 1e300;

template <class F>
static double elapsedUs(F f) {
    auto t0 = Clock::now();
    f();
    return std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
}

// Best of 3 runs. Order-independent fingerprint of every answer catches any missing/extra id.
template <class Index, class Queries, class Run>
static Result measure(const Index& idx, const Queries& qs, Run run) {
    std::vector<uint32_t> out;
    uint64_t checksum = 0;
    size_t hits = 0;
    double us = kInfUs;
    for (int rep = 0; rep < 3; ++rep) us = std::min(us, elapsedUs([&] {
        checksum = 0, hits = 0;
        for (const auto& q : qs) {
            run(idx, q, out);
            hits += out.size();
            checksum = checksum * 1000003 + out.size();
            for (uint32_t id : out) checksum += uint64_t(id) * 2654435761u;
        }
    }));
    return {Index::name, us / double(qs.size()), checksum, hits};
}

static std::string thousands(double v) {
    std::string s = std::to_string(uint64_t(v + 0.5));
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), ",");
    return s;
}

static void table(const char* title, const std::vector<Result>& rs, size_t queries) {
    const Result& naive = rs[0];
    double best = naive.us;
    for (auto& r : rs) best = std::min(best, r.us);
    std::printf("\n### %s\n\n_avg %.1f results/query_\n\n", title, double(naive.hits) / double(queries));
    std::printf("| Structure | µs / query | queries / s | vs naive | correct |\n|---|---:|---:|---:|:---:|\n");
    for (auto& r : rs) {
        bool ok = r.checksum == naive.checksum && r.hits == naive.hits;
        allCorrect &= ok;
        const char* b = r.us == best ? "**" : "";
        std::printf("| %s%s%s | %s%.2f%s | %s | %.1f× | %s |\n", b, r.name, b, b, r.us, b,
                    thousands(1e6 / r.us).c_str(), naive.us / r.us, ok ? "✅" : "❌");
    }
}

template <class Tuple, class Queries, class Run>
static void scenario(const char* title, const Tuple& idx, const Queries& qs, Run run) {
    std::vector<Result> rs;
    std::apply([&](const auto&... ix) { (rs.push_back(measure(ix, qs, run)), ...); }, idx);
    table(title, rs, qs.size());
}

template <class Tuple, class Data>
static void buildAll(const char* title, Tuple& idx, const Data& data) {
    std::printf("\n### %s\n\n| Structure | build (ms) |\n|---|---:|\n", title);
    std::apply([&](auto&... ix) {
        ((std::printf("| %s | %.1f |\n", ix.name, elapsedUs([&] { ix.build(data); }) / 1000.0)), ...);
    }, idx);
}

// Each index applies the same moves once (they change state, so there is no best of 3).
// Indexes without move(), i.e. the naive scan, just read the updated array and are skipped.
template <class Tuple>
static void moveAll(const char* title, Tuple& idx, const std::vector<geo::Move>& moves) {
    std::printf("\n### %s\n\n_%s location updates_\n\n| Structure | µs / update | updates / s |\n|---|---:|---:|\n",
                title, thousands(double(moves.size())).c_str());
    std::apply([&](auto&... ix) {
        auto one = [&](auto& index) {
            if constexpr (requires { index.move(0u, geo::Point{}, geo::Point{}); }) {
                double us = elapsedUs([&] { for (const auto& m : moves) index.move(m.id, m.from, m.to); });
                us /= double(moves.size());
                std::printf("| %s | %.2f | %s |\n", index.name, us, thousands(1e6 / us).c_str());
            }
        };
        (one(ix), ...);
    }, idx);
}

static std::string cpuName() {
    std::ifstream f("/proc/cpuinfo");
    for (std::string line; std::getline(f, line);)
        if (line.rfind("model name", 0) == 0) return line.substr(line.find(':') + 2);
    return "unknown CPU";
}

int main(int argc, char** argv) {
    size_t nPoints = argc > 1 ? std::stoul(argv[1]) : 1'000'000;
    size_t nQueries = argc > 2 ? std::stoul(argv[2]) : 2'000;
    size_t nZones = argc > 3 ? std::stoul(argv[3]) : 20'000;

    auto points = geo::randomPoints(nPoints, 1);
    auto queryPoints = geo::randomPoints(nQueries, 2);
    auto zones = geo::randomPolygons(nZones, 3, 0.2, 1.5);
    auto queryZones = geo::randomPolygons(nQueries, 4, 0.5, 3.0);

    std::printf("## Results\n\n`%s points · %s zones · %s queries per scenario · %s`\n",
                thousands(double(nPoints)).c_str(), thousands(double(nZones)).c_str(),
                thousands(double(nQueries)).c_str(), cpuName().c_str());

    std::tuple<Naive, BTreeIndex, Geohash, Quadtree, RTreeIndex> pointIdx;
    std::tuple<NaivePoly, GeohashPoly, QuadtreePoly, RTreePoly> zoneIdx;
    buildAll("Build: points", pointIdx, points);
    buildAll("Build: zones", zoneIdx, zones);

    auto radius = [](double r) {
        return [r](const auto& ix, geo::Point q, auto& out) { ix.radius(q, r, out); };
    };
    scenario("1. Radius search: 500 m (\"drivers near me\")", pointIdx, queryPoints, radius(0.5));
    scenario("2. Radius search: 5 km (\"restaurants in my area\")", pointIdx, queryPoints, radius(5.0));
    scenario("3. k-nearest: k = 1 (\"closest driver\")", pointIdx, queryPoints,
             [](const auto& ix, geo::Point q, auto& out) { ix.knn(q, 1, out); });
    scenario("4. k-nearest: k = 20 (\"20 closest stores\")", pointIdx, queryPoints,
             [](const auto& ix, geo::Point q, auto& out) { ix.knn(q, 20, out); });
    scenario("5. Point in polygon (\"which delivery zones cover me?\")", zoneIdx, queryPoints,
             [](const auto& ix, geo::Point q, auto& out) { ix.containing(q, out); });
    scenario("6. Polygon overlap (\"which zones overlap this new zone?\")", zoneIdx, queryZones,
             [](const auto& ix, const geo::Polygon& q, auto& out) { ix.overlapping(q, out); });

    // 7. Drivers move. Dynamic versions of each index are built from the same points, absorb the
    // same GPS pings, and must still match the naive scan on the moved data afterwards.
    std::tuple<Naive, MovingBTree, MovingGeohash, MovingQuadtree, DynamicRTree> movingIdx;
    buildAll("Build: moving drivers (dynamic indexes)", movingIdx, points);
    auto moves = geo::randomMoves(points, 10 * nQueries, 5);
    moveAll("7. Moving drivers: location updates", movingIdx, moves);
    scenario("7a. After the moves: radius 500 m", movingIdx, queryPoints, radius(0.5));
    scenario("7b. After the moves: k = 1", movingIdx, queryPoints,
             [](const auto& ix, geo::Point q, auto& out) { ix.knn(q, 1, out); });

    std::printf("\n%s\n", allCorrect ? "All structures match the naive scan. ✅" : "MISMATCH against naive scan. ❌");
    return allCorrect ? 0 : 1;
}
