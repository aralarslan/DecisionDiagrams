// Edge order within each vertex group: which rule minimises states?
// (HANDOFF.md §5.6, §5.9)
// usage: edge_order_lab <case> <bfs|cutwidth> <variant...>
//   case: meshRxC, Kn, geo40_<seed>, geo60_<seed>, geo80_<seed>
//   variants, applied to the edges emitted after each vertex of the library's
//   vertex sequence: id (edge id), rev (reverse id), new (most recently
//   emitted endpoint first), rem_new / rem_old (fewest unemitted edges of the
//   other endpoint first, then newest / oldest endpoint; rem_new is the
//   library's departing-first rule)
// LAB_PV=<p> sets every vertex probability (default 0.9).
// Prints, per variant: widest layer / total states (time, R).
#include <teddy/graphrel/graphrel.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <numeric>
#include <random>
#include <string>
using namespace teddy::graphrel;

static graph mesh(std::size_t r, std::size_t c) {
    graph g(r * c, r * (c - 1) + c * (r - 1));
    for (std::size_t i = 0; i < r; ++i)
        for (std::size_t j = 0; j < c; ++j) {
            if (j + 1 < c) g.add_edge(i * c + j, i * c + j + 1, 0.9);
            if (i + 1 < r) g.add_edge(i * c + j, (i + 1) * c + j, 0.9);
        }
    g.set_terminals({0, r * c - 1});
    return g;
}
static graph complete(std::size_t n) {
    graph g(n, n * (n - 1) / 2);
    for (std::size_t a = 0; a < n; ++a)
        for (std::size_t b = a + 1; b < n; ++b) g.add_edge(a, b, 0.9);
    g.set_terminals({0, n - 1});
    return g;
}
static graph geometric(std::size_t n, double radius, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> U(0, 1);
    std::vector<double> x(n), y(n);
    for (std::size_t i = 0; i < n; ++i) { x[i] = U(rng); y[i] = U(rng); }
    std::vector<std::pair<std::size_t, std::size_t>> es;
    for (std::size_t a = 0; a < n; ++a)
        for (std::size_t b = a + 1; b < n; ++b)
            if (std::hypot(x[a] - x[b], y[a] - y[b]) < radius) es.emplace_back(a, b);
    std::vector<std::size_t> comp(n); std::iota(comp.begin(), comp.end(), 0);
    std::function<std::size_t(std::size_t)> f = [&](std::size_t a) { return comp[a] == a ? a : comp[a] = f(comp[a]); };
    for (auto [a, b] : es) comp[f(a)] = f(b);
    for (std::size_t a = 1; a < n; ++a) if (f(a) != f(0)) {
        std::size_t best = 0; double bd = 9;
        for (std::size_t b = 0; b < n; ++b) if (f(b) == f(0)) { double d = std::hypot(x[a]-x[b], y[a]-y[b]); if (d < bd) { bd = d; best = b; } }
        es.emplace_back(best, a); comp[f(a)] = f(0);
    }
    graph g(n, es.size());
    for (auto [a, b] : es) g.add_edge(a, b, 0.9);
    std::size_t s = 0, t = 0; double bd = -1;
    for (std::size_t a = 0; a < n; ++a) for (std::size_t b = a + 1; b < n; ++b) {
        double d = std::hypot(x[a]-x[b], y[a]-y[b]); if (d > bd) { bd = d; s = a; t = b; } }
    g.set_terminals({s, t});
    return g;
}

static graph make(const std::string& name) {
    if (name.rfind("mesh", 0) == 0) {
        auto x = name.find('x');
        return mesh(std::stoul(name.substr(4, x - 4)), std::stoul(name.substr(x + 1)));
    }
    if (name[0] == 'K') return complete(std::stoul(name.substr(1)));
    if (name.rfind("geo40_", 0) == 0) return geometric(40, 0.25, std::stoul(name.substr(6)));
    if (name.rfind("geo60_", 0) == 0) return geometric(60, 0.2, 100 + std::stoul(name.substr(6)));
    if (name.rfind("geo80_", 0) == 0) return geometric(80, 0.177, 200 + std::stoul(name.substr(6)));
    throw std::runtime_error("unknown case");
}

// Re-sort the edges emitted after each vertex.
static std::vector<variable> reorder(const graph& g, std::vector<variable> ord, const std::string& variant) {
    std::vector<std::size_t> remaining(g.num_vertices(), 0), emitted_at(g.num_vertices(), 0);
    for (auto& e : g.edges()) if (e.from != e.to) { ++remaining[e.from]; ++remaining[e.to]; }
    std::size_t clock = 0;
    std::size_t i = 0;
    while (i < ord.size()) {
        const std::size_t v = ord[i].id;
        emitted_at[v] = ++clock;
        std::size_t j = i + 1;
        while (j < ord.size() && ord[j].type == variable_type::edge) ++j;
        auto other = [&](const variable& var) {
            const edge& e = g.edges()[var.id];
            return e.from == v ? e.to : e.from;
        };
        auto first = ord.begin() + i + 1, last = ord.begin() + j;
        // Start from edge-id order, whatever order the library emitted.
        std::sort(first, last, [](const variable& a, const variable& b) { return a.id < b.id; });
        if (variant == "rev") std::reverse(first, last);
        else if (variant == "new")
            std::stable_sort(first, last, [&](auto& a, auto& b) { return emitted_at[other(a)] > emitted_at[other(b)]; });
        else if (variant == "rem_new")
            std::stable_sort(first, last, [&](auto& a, auto& b) {
                auto ra = remaining[other(a)], rb = remaining[other(b)];
                if (ra != rb) return ra < rb;
                return emitted_at[other(a)] > emitted_at[other(b)]; });
        else if (variant == "rem_old")
            std::stable_sort(first, last, [&](auto& a, auto& b) {
                auto ra = remaining[other(a)], rb = remaining[other(b)];
                if (ra != rb) return ra < rb;
                return emitted_at[other(a)] < emitted_at[other(b)]; });
        for (auto it = first; it != last; ++it) {
            const edge& e = g.edges()[it->id];
            if (e.from != e.to) { --remaining[e.from]; --remaining[e.to]; }
        }
        i = j;
    }
    return ord;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    graph g = make(argv[1]);
    const double pv = std::getenv("LAB_PV") ? std::atof(std::getenv("LAB_PV")) : 0.9;
    g.set_all_vertex_probs(pv);
    const std::string strat = argv[2];
    auto base = generate_variable_ordering(g, strat == "bfs" ? ordering_strategy::bfs : ordering_strategy::cutwidth);
    std::printf("%-8s %-8s pv=%.1f", argv[1], strat.c_str(), pv);
    for (int a = 3; a < argc; ++a) {
        auto ord = reorder(g, base, argv[a]);
        auto t0 = std::chrono::steady_clock::now();
        const reliability_result res = analyze_reliability(g, {.ordering = ord});
        double R = res.reliability;
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        auto st = res.statistics;
        std::printf("  %s:%zu/%zu(%.2fs,R=%.6f)", argv[a], st.max_layer_size, st.total_states, s, R);
    }
    std::printf("\n");
}
