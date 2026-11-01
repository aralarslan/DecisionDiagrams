// Ordering lab (HANDOFF.md §3.16): BFS vs greedy cut-width candidates.
// Which cheap metric predicts G^max, and which heuristic minimises it?
//   ordering_lab list                          metrics of every candidate, selection marked
//   ordering_lab run <case> <cand|selected>    exact LBL for one candidate (G^max, total work)
// LAB_PV=<p> sets every vertex probability (default 0.9; 1.0 = perfect vertices).
#include <teddy/graphrel/graphrel.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <numeric>
#include <queue>
#include <random>
#include <string>
using namespace teddy::graphrel;

// vertex sequence -> variable ordering (vertex, then its newly ready edges)
static std::vector<variable> to_ordering(const graph& g, const std::vector<std::size_t>& seq) {
    std::vector<char> done_v(g.num_vertices(), 0), done_e(g.num_edges(), 0);
    std::vector<variable> ord;
    for (std::size_t v : seq) {
        ord.emplace_back(variable_type::vertex, v);
        done_v[v] = 1;
        for (std::size_t e : g.get_incident_edges(v)) {
            if (done_e[e]) continue;
            std::size_t o = g.edges()[e].from == v ? g.edges()[e].to : g.edges()[e].from;
            if (done_v[o]) { ord.emplace_back(variable_type::edge, e); done_e[e] = 1; }
        }
    }
    return ord;
}
static std::vector<std::size_t> vertex_seq(const std::vector<variable>& ord) {
    std::vector<std::size_t> s;
    for (auto& v : ord) if (v.type == variable_type::vertex) s.push_back(v.id);
    return s;
}

struct metrics { std::size_t fmax, cutmax; double cutsum; };
static metrics measure(const graph& g, const std::vector<std::size_t>& seq) {
    std::vector<char> done(g.num_vertices(), 0);
    std::vector<std::size_t> unproc(g.num_vertices(), 0);
    for (std::size_t v = 0; v < g.num_vertices(); ++v)
        for (std::size_t e : g.get_incident_edges(v)) if (g.edges()[e].from != g.edges()[e].to) ++unproc[v];
    std::size_t cut = 0, front = 0; metrics m{0, 0, 0};
    for (std::size_t v : seq) {
        done[v] = 1;
        for (std::size_t e : g.get_incident_edges(v)) {
            std::size_t o = g.edges()[e].from == v ? g.edges()[e].to : g.edges()[e].from;
            if (o == v) continue;
            if (done[o]) { --cut; if (--unproc[o] == 0) --front; --unproc[v]; }
            else ++cut;
        }
        if (unproc[v] > 0) ++front;
        m.fmax = std::max(m.fmax, front); m.cutmax = std::max(m.cutmax, cut); m.cutsum += cut;
    }
    return m;
}

// Greedy: pick the reached vertex that grows the edge cut least (deg - 2*processed),
// tie-break on the most recently touched (continues the current line), then id.
static std::vector<std::size_t> greedy_cut(const graph& g, std::size_t seed, std::size_t first = SIZE_MAX) {
    const std::size_t n = g.num_vertices();
    std::vector<long> deg(n, 0), proc(n, 0), touch(n, -1);
    for (std::size_t v = 0; v < n; ++v)
        for (std::size_t e : g.get_incident_edges(v)) if (g.edges()[e].from != g.edges()[e].to) ++deg[v];
    std::vector<char> done(n, 0);
    std::vector<std::size_t> seq;
    long clock = 0;
    auto take = [&](std::size_t v) {
        done[v] = 1; seq.push_back(v); ++clock;
        for (std::size_t e : g.get_incident_edges(v)) {
            std::size_t o = g.edges()[e].from == v ? g.edges()[e].to : g.edges()[e].from;
            if (o != v && !done[o]) { ++proc[o]; touch[o] = clock; }
        }
    };
    take(seed);
    if (first != SIZE_MAX && !done[first]) take(first);
    while (seq.size() < n) {
        std::size_t best = SIZE_MAX;
        for (std::size_t v = 0; v < n; ++v) {
            if (done[v] || proc[v] == 0) continue;
            if (best == SIZE_MAX) { best = v; continue; }
            long dv = deg[v] - 2 * proc[v], db = deg[best] - 2 * proc[best];
            if (dv < db || (dv == db && touch[v] > touch[best])) best = v;
        }
        if (best == SIZE_MAX) for (std::size_t v = 0; v < n; ++v) if (!done[v]) { best = v; break; }
        take(best);
    }
    return seq;
}

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
// random geometric graph (planar-ish "near neighbour"), connected via MST-ish fallback
static graph geometric(std::size_t n, double radius, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> U(0, 1);
    std::vector<double> x(n), y(n);
    for (std::size_t i = 0; i < n; ++i) { x[i] = U(rng); y[i] = U(rng); }
    std::vector<std::pair<std::size_t, std::size_t>> es;
    for (std::size_t a = 0; a < n; ++a)
        for (std::size_t b = a + 1; b < n; ++b)
            if (std::hypot(x[a] - x[b], y[a] - y[b]) < radius) es.emplace_back(a, b);
    // connect components: link each vertex to its nearest earlier vertex if isolated chain
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
    // terminals: farthest pair
    std::size_t s = 0, t = 0; double bd = -1;
    for (std::size_t a = 0; a < n; ++a) for (std::size_t b = a + 1; b < n; ++b) {
        double d = std::hypot(x[a]-x[b], y[a]-y[b]); if (d > bd) { bd = d; s = a; t = b; } }
    g.set_terminals({s, t});
    return g;
}

static std::size_t total_work = 0;
static std::size_t gmax(graph g, const std::vector<variable>& ord, double& secs) {
    const double pv = std::getenv("LAB_PV") ? std::atof(std::getenv("LAB_PV")) : 0.9;
    g.set_all_vertex_probs(pv);
    auto t0 = std::chrono::steady_clock::now();
    const reliability_result res = analyze_reliability(g, {.ordering = ord});
    secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    total_work = res.statistics.total_states;
    return res.statistics.max_layer_size;
}

// usage: order_lab list                 -> metrics for every (case, candidate), plus selection
//        order_lab run <case> <cand>     -> run exact LBL for one candidate
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::vector<std::pair<std::string, graph>> cases;
    for (auto [r, c] : std::vector<std::pair<int,int>>{{5,5},{6,6},{7,7},{8,8},{3,10},{10,3},{4,12},{12,4},{2,20},{20,2},{6,10},{10,10}})
        cases.emplace_back("mesh" + std::to_string(r) + "x" + std::to_string(c), mesh(r, c));
    for (int s = 1; s <= 8; ++s) cases.emplace_back("geo40_" + std::to_string(s), geometric(40, 0.25, s));
    for (int s = 1; s <= 4; ++s) cases.emplace_back("geo60_" + std::to_string(s), geometric(60, 0.2, 100 + s));
    const std::string mode = argv[1];
    for (std::size_t ci = 0; ci < cases.size(); ++ci) {
        auto& [name, g] = cases[ci];
        if (mode == "run" && name != argv[2]) continue;
        std::vector<std::pair<std::string, std::vector<std::size_t>>> cands;
        cands.emplace_back("bfs", vertex_seq(generate_variable_ordering(g, ordering_strategy::bfs)));
        const std::size_t seed = g.terminals().front();
        cands.emplace_back("greedy", greedy_cut(g, seed));
        for (std::size_t e : g.get_incident_edges(seed)) {
            std::size_t o = g.edges()[e].from == seed ? g.edges()[e].to : g.edges()[e].from;
            cands.emplace_back("greedy>" + std::to_string(o), greedy_cut(g, seed, o));
        }
        // selection: lexicographic (cutmax, Fmax, cutsum), first wins ties
        std::size_t best = 0; metrics bm = measure(g, cands[0].second);
        for (std::size_t k = 1; k < cands.size(); ++k) {
            metrics m = measure(g, cands[k].second);
            if (std::tie(m.cutmax, m.fmax, m.cutsum) < std::tie(bm.cutmax, bm.fmax, bm.cutsum)) { best = k; bm = m; }
        }
        for (std::size_t k = 0; k < cands.size(); ++k) {
            auto& [cn, seq] = cands[k];
            metrics m = measure(g, seq);
            if (mode == "list") {
                std::printf("%s %s %zu %zu %.0f %s\n", name.c_str(), cn.c_str(), m.fmax, m.cutmax, m.cutsum, k == best ? "SELECTED" : "-");
            } else if (cn == argv[3] || (std::string(argv[3]) == "selected" && k == best)) {
                double secs = 0;
                std::size_t gm = gmax(g, to_ordering(g, seq), secs);
                std::printf("%s %s Gmax=%zu work=%zu time=%.2fs\n", name.c_str(), cn.c_str(), gm, total_work, secs);
                return 0;
            }
        }
    }
}
