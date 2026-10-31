// Differential fuzz: brute force vs LBL vs BDD on small random graphs.
#include <teddy/graphrel/graphrel.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <numeric>
#include <random>
#include <sstream>

using namespace teddy::graphrel;

static double brute(const graph& g) {
    const std::size_t V = g.num_vertices, E = g.edges.size();
    const std::size_t n = V + E;
    double total = 0.0;
    for (std::uint64_t m = 0; m < (1ULL << n); ++m) {
        double p = 1.0;
        for (std::size_t i = 0; i < V; ++i)
            p *= (m >> i & 1) ? g.vertex_probs[i] : 1.0 - g.vertex_probs[i];
        for (std::size_t j = 0; j < E; ++j)
            p *= (m >> (V + j) & 1) ? g.edges[j].prob : 1.0 - g.edges[j].prob;
        if (p == 0.0) continue;
        bool ok = true;
        for (auto t : g.terminals) ok &= (m >> t & 1) != 0;
        if (!ok) continue;
        std::vector<std::size_t> par(V);
        std::iota(par.begin(), par.end(), 0);
        auto find = [&](std::size_t x) { while (par[x] != x) x = par[x] = par[par[x]]; return x; };
        for (std::size_t j = 0; j < E; ++j) {
            const auto& e = g.edges[j];
            if ((m >> (V + j) & 1) && (m >> e.from & 1) && (m >> e.to & 1))
                par[find(e.from)] = find(e.to);
        }
        auto r = find(g.terminals.front());
        for (auto t : g.terminals) ok &= find(t) == r;
        if (ok) total += p;
    }
    return total;
}

// Random ordering that respects the vertex-priority principle (every edge comes
// after both of its endpoints) but is otherwise arbitrary -- not BFS/DFS-shaped.
static std::vector<variable> random_vertex_priority(const graph& g, std::mt19937_64& rng) {
    std::vector<std::size_t> verts(g.num_vertices);
    std::iota(verts.begin(), verts.end(), 0);
    std::shuffle(verts.begin(), verts.end(), rng);
    std::vector<char> placed(g.num_vertices, 0), done(g.edges.size(), 0);
    std::vector<variable> ord;
    std::vector<std::size_t> ready;
    std::size_t next_v = 0;
    while (ord.size() < g.num_vertices + g.edges.size()) {
        bool take_vertex = next_v < verts.size() && (ready.empty() || rng() % 2);
        if (take_vertex) {
            std::size_t v = verts[next_v++];
            placed[v] = 1;
            ord.emplace_back(variable_type::vertex, v);
            for (std::size_t e = 0; e < g.edges.size(); ++e)
                if (!done[e] && placed[g.edges[e].from] && placed[g.edges[e].to]) {
                    done[e] = 1;
                    ready.push_back(e);
                }
        } else {
            std::size_t i = rng() % ready.size();
            ord.emplace_back(variable_type::edge, ready[i]);
            ready.erase(ready.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    return ord;
}

// Completely random permutation of all variables (violates vertex priority).
static std::vector<variable> random_any(const graph& g, std::mt19937_64& rng) {
    std::vector<variable> ord;
    for (std::size_t v = 0; v < g.num_vertices; ++v) ord.emplace_back(variable_type::vertex, v);
    for (std::size_t e = 0; e < g.edges.size(); ++e) ord.emplace_back(variable_type::edge, e);
    std::shuffle(ord.begin(), ord.end(), rng);
    return ord;
}

static std::string dump(const graph& g) {
    std::ostringstream os;
    write_graph(os, g);
    return os.str();
}

int main(int argc, char** argv) {
    // Modes (environment variables):
    //   default  : anything goes (disconnected graphs, loops, multi-edges, |K| >= 1)
    //   KMIN2=1  : as default but |K| >= 2
    //   PAPER=1  : Wu & Sun (2024) assumptions only -- connected simple graph,
    //              2 <= |K| <= |V|, every edge/vertex probability strictly in (0,1),
    //              default BFS vertex-priority ordering
    //   PAPER=1 PERFECT=1 : as PAPER, but some edges may have p = 1
    int iters = argc > 1 ? std::atoi(argv[1]) : 2000;
    const bool paper = std::getenv("PAPER") != nullptr;
    const bool perfect_ok = std::getenv("PERFECT") != nullptr;
    const bool kmin2 = paper || std::getenv("KMIN2") != nullptr;
    std::mt19937_64 rng(12345);
    int fails_lbl = 0, fails_bdd = 0, fails_dfs = 0, fails_vp = 0, fails_any = 0;
    for (int it = 0; it < iters; ++it) {
        std::size_t V = (paper ? 3 : 2) + rng() % (paper ? 5 : 6);   // 2..7 (paper: 3..7)
        std::size_t E = 1 + rng() % 9;                                 // 1..9
        bool vert = paper || rng() % 2;
        bool perfect_edges = paper ? perfect_ok : rng() % 3 == 0;
        auto rand_p = [&] { return 0.1 + 0.8 * (double)(rng() % 1000) / 1000.0; };
        std::vector<std::pair<std::size_t, std::size_t>> ends;
        if (paper) {
            // random spanning tree (connected), then extra distinct non-loop edges
            for (std::size_t v = 1; v < V; ++v) ends.emplace_back(rng() % v, v);
            std::size_t max_e = V * (V - 1) / 2;
            E = std::min(std::max(E, V - 1), max_e);
            while (ends.size() < E) {
                std::size_t a = rng() % V, b = rng() % V;
                if (a == b) continue;
                if (a > b) std::swap(a, b);
                if (std::find(ends.begin(), ends.end(), std::make_pair(a, b)) == ends.end())
                    ends.emplace_back(a, b);
            }
        } else {
            for (std::size_t j = 0; j < E; ++j) ends.emplace_back(rng() % V, rng() % V);
        }
        graph g(V, E);
        auto pick = [&] { const double ps[] = {0.0, 0.3, 0.5, 0.9, 1.0}; return ps[rng() % 5]; };
        for (auto [a, b] : ends) {
            double p = perfect_edges && rng() % 2 ? 1.0 : rand_p();
            g.add_edge(a, b, p);
        }
        if (vert) for (auto& p : g.vertex_probs) p = paper ? rand_p() : (rng() % 3 ? pick() : 1.0);
        std::size_t k = (kmin2 ? 2 : 1) + rng() % std::min<std::size_t>(V - (kmin2 ? 1 : 0), 3);
        std::vector<std::size_t> ids(V);
        std::iota(ids.begin(), ids.end(), 0);
        std::shuffle(ids.begin(), ids.end(), rng);
        g.terminals.assign(ids.begin(), ids.begin() + k);
        std::sort(g.terminals.begin(), g.terminals.end());

        double rb = brute(g);
        double rl = calculate_reliability(g);
        auto d = build_diagram(g);
        double rd = d.manager.calculate_probability(d.probs, d.diagram);
        bool bl = std::fabs(rb - rl) > 1e-9, bd = std::fabs(rb - rd) > 1e-9;
        fails_lbl += bl; fails_bdd += bd;
        if ((bl || bd) && (fails_lbl + fails_bdd) <= 6)
            std::printf("MISMATCH it=%d brute=%.9f lbl=%.9f bdd=%.9f\n%s\n", it, rb, rl, rd, dump(g).c_str());

        // LBL under other orderings
        auto off = [&](double r) { return std::fabs(rb - r) > 1e-9; };
        fails_dfs += off(calculate_reliability(g, generate_variable_ordering(g, ordering_strategy::dfs)));
        fails_vp  += off(calculate_reliability(g, random_vertex_priority(g, rng)));
        fails_any += off(calculate_reliability(g, random_any(g, rng)));
    }
    std::printf("iters=%d  lbl_mismatches=%d  bdd_mismatches=%d\n", iters, fails_lbl, fails_bdd);
    std::printf("LBL with other orderings: dfs=%d  random_vertex_priority=%d  random_unconstrained=%d\n",
                fails_dfs, fails_vp, fails_any);
}
