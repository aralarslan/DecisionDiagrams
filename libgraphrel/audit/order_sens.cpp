// How much does BFS tie-breaking (edge insertion order / labelling) change G^max?
#include "lbl/lbl_detail.hpp"
#include <teddy/graphrel/graphrel.hpp>
#include <algorithm>
#include <cstdio>
#include <random>
using namespace teddy::graphrel;
int main(int argc, char** argv) {
    std::size_t r = std::strtoul(argv[1], nullptr, 10), c = std::strtoul(argv[2], nullptr, 10);
    int trials = argc > 3 ? std::atoi(argv[3]) : 20;
    std::vector<std::pair<std::size_t, std::size_t>> ends;
    for (std::size_t i = 0; i < r; ++i)
        for (std::size_t j = 0; j < c; ++j) {
            if (j + 1 < c) ends.emplace_back(i * c + j, i * c + j + 1);
            if (i + 1 < r) ends.emplace_back(i * c + j, (i + 1) * c + j);
        }
    std::mt19937_64 rng(7);
    std::size_t best = SIZE_MAX, worst = 0;
    for (int t = 0; t < trials; ++t) {
        auto e = ends;
        if (t > 0) std::shuffle(e.begin(), e.end(), rng);
        graph g(r * c, e.size());
        for (auto [a, b] : e) g.add_edge(a, b, 0.9);
        for (auto& p : g.vertex_probs) p = 0.9;
        g.terminals = {0, r * c - 1};
        for (auto strat : {ordering_strategy::bfs, ordering_strategy::dfs}) {
            auto ord = generate_variable_ordering(g, strat);
            lbl_calculator calc(g, ord);
            double R = calc.calculate();
            auto st = calc.get_statistics();
            if (strat == ordering_strategy::bfs) { best = std::min(best, st.max_layer_size); worst = std::max(worst, st.max_layer_size); }
            if (t < 3) std::printf("trial %d %s: R=%.6f G^max=%zu Fmax=%zu\n", t, strat == ordering_strategy::bfs ? "bfs" : "dfs", R, st.max_layer_size, st.max_boundary_size);
        }
    }
    std::printf("%zux%zu BFS over %d edge orders: G^max min=%zu max=%zu\n", r, c, trials, best, worst);
}
