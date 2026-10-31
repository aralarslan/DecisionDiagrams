// G^max under BFS vs row-major sweep (vertex priority kept), meshes with p=0.9 everywhere.
#include "lbl/lbl_detail.hpp"
#include "common/bell_cache.hpp"
#include "common/partition.hpp"
#include <teddy/graphrel/graphrel.hpp>
#include <chrono>
#include <cstdio>
#include <functional>
using namespace teddy::graphrel;

// Carlier & Lucet (1996) Algorithm 2, transcribed literally (1-indexed PART).
static std::uint64_t cl_number(const std::vector<int>& part1, const bell_cache& c) {
    std::size_t F = part1.size();
    std::uint64_t num = 1; int j = 1;
    for (std::size_t i = 2; i <= F; ++i) {
        if (part1[i - 1] == j + 1) { ++j; num += c.stirling(i - 1, j) * j; }
        else num = (num - 1) * j + part1[i - 1];
    }
    std::uint64_t base = 0;
    for (int h = 1; h < j; ++h) base += c.stirling(F, h);
    return num + base;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "numbering") {
        bell_cache c(14);
        std::size_t checked = 0, bad = 0;
        for (std::size_t F = 1; F <= 10; ++F) {
            // enumerate all restricted growth strings of length F
            std::vector<int> a(F, 1);
            std::vector<char> seen(c.bell(F) + 1, 0);
            std::function<void(std::size_t, int)> rec = [&](std::size_t i, int mx) {
                if (i == F) {
                    std::uint64_t n1 = partition_to_number(a, mx, c), n2 = cl_number(a, c);
                    ++checked;
                    if (n1 != n2 || n1 < 1 || n1 > c.bell(F) || seen[n1] || number_to_partition(n1, F, c) != a) ++bad;
                    else seen[n1] = 1;
                    return;
                }
                for (int b = 1; b <= mx + 1; ++b) { a[i] = b; rec(i + 1, std::max(mx, b)); }
            };
            rec(1, 1);
        }
        std::printf("numbering: %zu partitions (F<=10) checked against C&L Alg.2, bijective onto [1,B(F)], %zu bad\n", checked, bad);
        return 0;
    }
    std::size_t r = std::strtoul(argv[1], nullptr, 10), c = std::strtoul(argv[2], nullptr, 10);
    graph g(r * c, r * (c - 1) + c * (r - 1));
    for (std::size_t i = 0; i < r; ++i)
        for (std::size_t j = 0; j < c; ++j) {
            if (j + 1 < c) g.add_edge(i * c + j, i * c + j + 1, 0.9);
            if (i + 1 < r) g.add_edge(i * c + j, (i + 1) * c + j, 0.9);
        }
    for (auto& p : g.vertex_probs) p = 0.9;
    g.terminals = {0, r * c - 1};
    // row-major sweep: vertex v, then every edge to an earlier vertex
    std::vector<variable> sweep;
    for (std::size_t v = 0; v < r * c; ++v) {
        sweep.emplace_back(variable_type::vertex, v);
        for (auto e : g.adj[v]) {
            std::size_t o = g.edges[e].from == v ? g.edges[e].to : g.edges[e].from;
            if (o < v) sweep.emplace_back(variable_type::edge, e);
        }
    }
    for (int which = 0; which < 2; ++which) {
        if (which == 0 && argc > 3) continue;  // skip BFS when asked
        auto ord = which == 0 ? generate_variable_ordering(g) : sweep;
        auto t0 = std::chrono::steady_clock::now();
        lbl_calculator calc(g, ord);
        double R = calc.calculate();
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        auto st = calc.get_statistics();
        std::printf("%zux%zu %-9s R=%.6f G^max=%zu Fmax=%zu time=%.1fs\n", r, c, which == 0 ? "BFS" : "row-sweep",
                    R, st.max_layer_size, st.max_boundary_size, s);
    }
}
