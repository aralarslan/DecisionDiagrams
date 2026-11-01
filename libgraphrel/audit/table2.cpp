// Wu & Sun (2024) Table 2 reproduction with the frontier engine.
// usage: table2 mesh <rows> <cols> <bfs|cutwidth> | table2 complete <n> <bfs|cutwidth>
#include "lbl/lbl_detail.hpp"
#include <teddy/graphrel/graphrel.hpp>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/resource.h>
using namespace teddy::graphrel;
int main(int argc, char** argv) {
    std::string kind = argv[1];
    graph g;
    std::string name;
    std::string strat;
    if (kind == "mesh") {
        std::size_t r = std::strtoul(argv[2], nullptr, 10), c = std::strtoul(argv[3], nullptr, 10);
        strat = argv[4];
        name = "mesh " + std::to_string(r) + "x" + std::to_string(c);
        g = graph(r * c, r * (c - 1) + c * (r - 1));
        for (std::size_t i = 0; i < r; ++i)
            for (std::size_t j = 0; j < c; ++j) {
                if (j + 1 < c) g.add_edge(i * c + j, i * c + j + 1, 0.9);
                if (i + 1 < r) g.add_edge(i * c + j, (i + 1) * c + j, 0.9);
            }
        g.set_terminals({0, r * c - 1});
    } else {
        std::size_t n = std::strtoul(argv[2], nullptr, 10);
        strat = argv[3];
        name = "K" + std::to_string(n);
        g = graph(n, n * (n - 1) / 2);
        for (std::size_t a = 0; a < n; ++a)
            for (std::size_t b = a + 1; b < n; ++b) g.add_edge(a, b, 0.9);
        g.set_terminals({0, n - 1});
    }
    for (auto& p : g.vertex_probs) p = 0.9;
    auto ord = generate_variable_ordering(g, strat == "bfs" ? ordering_strategy::bfs : ordering_strategy::cutwidth);
    auto t0 = std::chrono::steady_clock::now();
    lbl_calculator calc(g, ord);
    double R = calc.calculate();
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    auto st = calc.get_statistics();
    rusage u{}; getrusage(RUSAGE_SELF, &u);
    std::printf("%-14s %-8s V=%zu E=%zu L=%zu  R=%.6f  Gmax=%zu  F=%zu  work=%zu  time=%.1fs  peakRSS=%ldMB  tables=%.1fMB\n",
                name.c_str(), strat.c_str(), g.num_vertices, g.num_edges, st.num_layers, R, st.max_layer_size,
                st.max_boundary_size, st.total_decompositions, s, u.ru_maxrss / 1024, st.peak_table_bytes / 1048576.0);
}
