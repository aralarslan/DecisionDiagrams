// Per-level front size, edge cut and layer size, for the ordering model.
#include "lbl/lbl_detail.hpp"
#include "frontier/frontier.hpp"
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
    throw std::runtime_error("unknown case");
}


using namespace teddy::graphrel::detail;
// Per-level data for the ordering model (HANDOFF.md §5.9).
// usage: level_lab <case> <bfs|cutwidth|dfs>   (cases as in edge_order_lab;
// LAB_PV=<p> sets every vertex probability). One row per level:
//   case strategy pv level front_size edge_cut states type(v|e)
// The report's data set: every case of edge_order_lab's in-sample list with
// pv 0.9 and 1.0, each run limited to 60 s and 4 GB; fit with fit_levels.py.
int main(int argc, char** argv) {
    graph g = make(argv[1]);
    const double pv = std::getenv("LAB_PV") ? std::atof(std::getenv("LAB_PV")) : 0.9;
    g.set_all_vertex_probs(pv);
    std::string st = argv[2];
    auto ord = generate_variable_ordering(g, st == "bfs" ? ordering_strategy::bfs : st == "dfs" ? ordering_strategy::dfs : ordering_strategy::cutwidth);
    frontier_plan plan(g, ord, true);
    // cut per level (before processing ordering[l])
    std::vector<char> emitted(g.num_vertices(), 0), done(g.num_edges(), 0);
    std::vector<std::size_t> cut(ord.size() + 1, 0);
    std::size_t c = 0;
    for (std::size_t l = 0; l < ord.size(); ++l) {
        cut[l] = c;
        const variable& v = ord[l];
        if (v.type == variable_type::vertex) {
            emitted[v.id] = 1;
            for (std::size_t e : g.get_incident_edges(v.id)) {
                const edge& ed = g.edges()[e];
                if (ed.from == ed.to || done[e]) continue;
                std::size_t o = ed.from == v.id ? ed.to : ed.from;
                if (!emitted[o]) ++c; // first emitted endpoint
            }
        } else {
            const edge& ed = g.edges()[v.id];
            done[v.id] = 1;
            if (ed.from != ed.to) --c;
        }
    }
    state_table cur, nxt;
    cur.reset(plan.key_size(0), 1);
    auto init = plan.initial_key();
    cur.add(init.data(), 1.0);
    std::uint8_t lo[80], hi[80];
    for (std::size_t l = 0; l < plan.levels(); ++l) {
        nxt.reset(plan.key_size(l + 1), cur.size());
        const variable& v = ord[l];
        double p = v.type == variable_type::edge ? g.edges()[v.id].prob : g.vertex_probs()[v.id];
        std::printf("%s %s %.1f %zu %zu %zu %zu %c\n", argv[1], st.c_str(), pv, l, plan.front_size(l), cut[l], cur.size(), v.type == variable_type::edge ? 'e' : 'v');
        for (std::size_t i = 0; i < cur.size(); ++i) {
            auto ex = plan.expand(l, cur.key(i), lo, hi);
            double q = cur.probability(i);
            if (ex.single) { if (ex.high == outcome::state) nxt.add(hi, q); }
            else { if (ex.low == outcome::state) nxt.add(lo, q * (1 - p)); if (ex.high == outcome::state) nxt.add(hi, q * p); }
        }
        std::swap(cur, nxt);
    }
}
