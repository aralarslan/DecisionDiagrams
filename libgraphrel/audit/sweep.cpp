// G^max under BFS vs row-major sweep (vertex priority kept), meshes with p=0.9 everywhere.
#include <teddy/graphrel/graphrel.hpp>
#include <chrono>
#include <cstdio>
#include <string>
using namespace teddy::graphrel;

// The audit-time "numbering" mode, which checked the partition numbering of
// the previous engine against Carlier & Lucet's Algorithm 2 (README.md), was
// removed with that engine: frontier states store block labels directly.
// usage: sweep <rows> <cols> [skip-bfs]

int main(int argc, char** argv) {
    std::size_t r = std::strtoul(argv[1], nullptr, 10), c = std::strtoul(argv[2], nullptr, 10);
    graph g(r * c, r * (c - 1) + c * (r - 1));
    for (std::size_t i = 0; i < r; ++i)
        for (std::size_t j = 0; j < c; ++j) {
            if (j + 1 < c) g.add_edge(i * c + j, i * c + j + 1, 0.9);
            if (i + 1 < r) g.add_edge(i * c + j, (i + 1) * c + j, 0.9);
        }
    g.set_all_vertex_probs(0.9);
    g.set_terminals({0, r * c - 1});
    // row-major sweep: vertex v, then every edge to an earlier vertex
    std::vector<variable> sweep;
    for (std::size_t v = 0; v < r * c; ++v) {
        sweep.emplace_back(variable_type::vertex, v);
        for (auto e : g.get_incident_edges(v)) {
            std::size_t o = g.edges()[e].from == v ? g.edges()[e].to : g.edges()[e].from;
            if (o < v) sweep.emplace_back(variable_type::edge, e);
        }
    }
    for (int which = 0; which < 2; ++which) {
        if (which == 0 && argc > 3) continue;  // skip BFS when asked
        auto ord = which == 0 ? generate_variable_ordering(g, ordering_strategy::bfs) : sweep;
        auto t0 = std::chrono::steady_clock::now();
        const reliability_result res = analyze_reliability(g, {.ordering = ord});
        double R = res.reliability;
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        auto st = res.statistics;
        std::printf("%zux%zu %-9s R=%.6f G^max=%zu Fmax=%zu time=%.1fs\n", r, c, which == 0 ? "BFS" : "row-sweep",
                    R, st.max_layer_size, st.max_front_size, s);
    }
}
