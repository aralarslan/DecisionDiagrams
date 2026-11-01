// Reproduce Wu & Sun (2024) Table 2 rows with the library's LBL.
#include "lbl/lbl_detail.hpp"
#include <teddy/graphrel/graphrel.hpp>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sys/resource.h>
using namespace teddy::graphrel;
static long rss_mb() { rusage u{}; getrusage(RUSAGE_SELF, &u); return u.ru_maxrss / 1024; }
int main(int argc, char** argv) {
    graph g;
    const char* which = argv[1];
    if (!std::strcmp(which, "mesh")) {
        std::size_t r = std::strtoul(argv[2], nullptr, 10), c = std::strtoul(argv[3], nullptr, 10);
        g = graph(r * c, r * (c - 1) + c * (r - 1));
        for (std::size_t i = 0; i < r; ++i)
            for (std::size_t j = 0; j < c; ++j) {
                if (j + 1 < c) g.add_edge(i * c + j, i * c + j + 1, 0.9);
                if (i + 1 < r) g.add_edge(i * c + j, (i + 1) * c + j, 0.9);
            }
        g.set_terminals({0, r * c - 1});
    } else {  // complete graph K_n
        std::size_t n = std::strtoul(argv[2], nullptr, 10);
        g = graph(n, n * (n - 1) / 2);
        for (std::size_t a = 0; a < n; ++a)
            for (std::size_t b = a + 1; b < n; ++b) g.add_edge(a, b, 0.9);
        g.set_terminals({0, n - 1});
    }
    g.set_all_vertex_probs(0.9);
    auto t0 = std::chrono::steady_clock::now();
    lbl_calculator calc(g);
    double r = calc.calculate();
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    auto st = calc.get_statistics();
    std::printf("%s %s %s: R=%.6f  max_layer_size(G^max)=%zu  max_boundary=%zu  layers=%zu  time=%.1fs  RSS=%ldMB\n",
                which, argv[2], argc > 3 ? argv[3] : "", r, st.max_layer_size, st.max_boundary_size,
                st.num_layers, s, rss_mb());
}
