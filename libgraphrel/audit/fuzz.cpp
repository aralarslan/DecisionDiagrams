// Differential fuzz: brute force vs LBL vs BDD on small random graphs.
#include <teddy/graphrel/graphrel.hpp>
#include <cmath>
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

static std::string dump(const graph& g) {
    std::ostringstream os;
    write_graph(os, g);
    return os.str();
}

int main(int argc, char** argv) {
    int iters = argc > 1 ? std::atoi(argv[1]) : 2000;
    std::mt19937_64 rng(12345);
    int fails_lbl = 0, fails_bdd = 0;
    for (int it = 0; it < iters; ++it) {
        std::size_t V = 2 + rng() % 6;              // 2..7
        std::size_t E = 1 + rng() % 9;              // 1..9
        bool vert = rng() % 2;
        bool perfect_edges = rng() % 3 == 0;
        graph g(V, E);
        auto pick = [&] { const double ps[] = {0.0, 0.3, 0.5, 0.9, 1.0}; return ps[rng() % 5]; };
        for (std::size_t j = 0; j < E; ++j) {
            std::size_t a = rng() % V, b = rng() % V;
            double p = perfect_edges && rng() % 2 ? 1.0 : 0.1 + 0.8 * (double)(rng() % 1000) / 1000.0;
            g.add_edge(a, b, p);
        }
        if (vert) for (auto& p : g.vertex_probs) p = rng() % 3 ? pick() : 1.0;
        std::size_t k = (getenv("KMIN2") ? 2 : 1) + rng() % std::min<std::size_t>(V - (getenv("KMIN2") ? 1 : 0), 3);
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
    }
    std::printf("iters=%d  lbl_mismatches=%d  bdd_mismatches=%d\n", iters, fails_lbl, fails_bdd);
}
