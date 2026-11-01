// Wu & Sun (2024) Table 2 reproduction with the frontier engine.
// usage: table2 mesh <rows> <cols> <bfs|cutwidth|paper>
//        table2 complete <n> <bfs|cutwidth|paper> [second terminal, default n-1]
// "paper": vertices by id (row by row on these meshes), each vertex's edges by
// id. B1 = states per layer that the layer's variable splits (the paper's
// G^max); Gmax = all states per layer. THREADS=<n> in the environment sets the
// number of threads (default 1; 0 = all hardware threads).
#include <teddy/graphrel/graphrel.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/resource.h>
using namespace teddy::graphrel;

static std::vector<variable> by_id(const graph& g) {
    std::vector<variable> ord;
    std::vector<char> placed(g.num_vertices(), 0), done(g.num_edges(), 0);
    for (std::size_t v = 0; v < g.num_vertices(); ++v) {
        ord.emplace_back(variable_type::vertex, v);
        placed[v] = 1;
        for (std::size_t e : g.get_incident_edges(v)) {
            const std::size_t o = g.edges()[e].from == v ? g.edges()[e].to : g.edges()[e].from;
            if (!done[e] && placed[o]) { done[e] = 1; ord.emplace_back(variable_type::edge, e); }
        }
    }
    return ord;
}

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
        const std::size_t t2 = argc > 4 ? std::strtoul(argv[4], nullptr, 10) : n - 1;
        name = "K" + std::to_string(n) + " K={0," + std::to_string(t2) + "}";
        g = graph(n, n * (n - 1) / 2);
        for (std::size_t a = 0; a < n; ++a)
            for (std::size_t b = a + 1; b < n; ++b) g.add_edge(a, b, 0.9);
        g.set_terminals({0, t2});
    }
    g.set_all_vertex_probs(0.9);
    auto ord = strat == "paper" ? by_id(g)
             : generate_variable_ordering(g, strat == "bfs" ? ordering_strategy::bfs : ordering_strategy::cutwidth);
    auto t0 = std::chrono::steady_clock::now();
    const unsigned threads = std::getenv("THREADS") ? static_cast<unsigned>(std::atoi(std::getenv("THREADS"))) : 1u;
    const reliability_result res = analyze_reliability(g, {.ordering = ord, .threads = threads});
    double R = res.reliability;
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    auto st = res.statistics;
    rusage u{}; getrusage(RUSAGE_SELF, &u);
    std::printf("%-16s %-8s T=%u V=%zu E=%zu L=%zu  R=%.6f  B1=%zu  Gmax=%zu  F=%zu  work=%zu  time=%.1fs  peakRSS=%ldMB  tables=%.1fMB\n",
                name.c_str(), strat.c_str(), threads, g.num_vertices(), g.num_edges(), st.num_layers, R,
                st.max_branching_layer_size, st.max_layer_size, st.max_front_size,
                st.total_states, s, u.ru_maxrss / 1024, st.peak_memory_bytes / 1048576.0);
}
