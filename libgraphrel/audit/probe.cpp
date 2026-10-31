// Targeted probes for individual audit findings.
#include <teddy/graphrel/graphrel.hpp>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <sys/resource.h>

using namespace teddy::graphrel;
using clk = std::chrono::steady_clock;

static double bdd_r(const graph& g) {
    auto d = build_diagram(g);
    return d.manager.calculate_probability(d.probs, d.diagram);
}
static long rss_mb() { rusage u{}; getrusage(RUSAGE_SELF, &u); return u.ru_maxrss / 1024; }
static double ms(clk::time_point a) { return std::chrono::duration<double, std::milli>(clk::now() - a).count(); }

static graph grid(std::size_t r, std::size_t c, double p) {
    graph g(r * c, r * (c - 1) + c * (r - 1));
    for (std::size_t i = 0; i < r; ++i)
        for (std::size_t j = 0; j < c; ++j) {
            if (j + 1 < c) g.add_edge(i * c + j, i * c + j + 1, p);
            if (i + 1 < r) g.add_edge(i * c + j, (i + 1) * c + j, p);
        }
    g.terminals = {0, r * c - 1};
    return g;
}

int main(int argc, char** argv) {
    std::string what = argc > 1 ? argv[1] : "all";
    bool all = what == "all";

    if (all || what == "ghost") {
        // s=0, v=1 (p_v=0.5), t=2; perfect edges 0-1, 1-2; edge 0-2 with p=0.5
        graph g(3, 3);
        g.vertex_probs[1] = 0.5;
        g.add_edge(0, 1, 1.0);
        g.add_edge(1, 2, 1.0);
        g.add_edge(0, 2, 0.5);
        g.terminals = {0, 2};
        std::printf("[ghost] exact=0.75 lbl=%.6f bdd=%.6f\n", calculate_reliability(g), bdd_r(g));
    }
    if (all || what == "single") {
        graph g(2, 1);
        g.vertex_probs[0] = 0.3;
        g.add_edge(0, 1, 0.5);
        g.terminals = {0};
        std::printf("[single-terminal] exact=0.3 lbl=%.6f bdd=%.6f\n", calculate_reliability(g), bdd_r(g));
    }
    if (all || what == "unsorted") {
        // path 0-1-2, terminals {2,0} unsorted, terminal 0 unreliable
        graph g(3, 2);
        g.vertex_probs[0] = 0.5;
        g.add_edge(0, 1, 0.9);
        g.add_edge(1, 2, 0.9);
        g.terminals = {2, 0};
        std::printf("[unsorted terminals] exact=%.6f validate=%d lbl=%.6f bdd=%.6f\n",
                    0.5 * 0.81, g.validate(), calculate_reliability(g), bdd_r(g));
    }
    if (all || what == "ordering") {
        // path 0-1-2 with unreliable middle vertex, edges ordered before vertices
        graph g(3, 2);
        g.vertex_probs[1] = 0.5;
        g.add_edge(0, 1, 0.9);
        g.add_edge(1, 2, 0.9);
        g.terminals = {0, 2};
        std::vector<variable> ord{{variable_type::edge, 0}, {variable_type::edge, 1},
                                  {variable_type::vertex, 0}, {variable_type::vertex, 1},
                                  {variable_type::vertex, 2}};
        std::vector<variable> partial{{variable_type::vertex, 0}, {variable_type::edge, 0}};
        std::printf("[custom ordering] exact=%.6f edges-first=%.6f missing-vars=%.6f\n",
                    0.5 * 0.81, calculate_reliability(g, ord), calculate_reliability(g, partial));
    }
    if (all || what == "parse") {
        std::istringstream in("p reliability 2 1\nt 0 1\ne 0 1 O.9\n");  // letter O typo
        graph g = read_graph(in);
        std::printf("[parser] 'e 0 1 O.9' parsed prob=%g (silently)\n", g.edges[0].prob);
        std::istringstream in2("p reliability 2 1 0.9x\nt 0 1\ne 0 1\n");
        graph g2 = read_graph(in2);
        std::printf("[parser] 'p ... 0.9x' default edge prob=%g, trailing junk accepted\n", g2.edges[0].prob);
    }
    if (all || what == "roundtrip") {
        graph g(2, 1);
        g.add_edge(0, 1, 0.1 + 0.2);  // 0.30000000000000004
        g.terminals = {0, 1};
        std::stringstream ss;
        write_graph(ss, g);
        graph h = read_graph(ss);
        std::printf("[roundtrip] exact bits preserved: %s\n", h.edges[0].prob == g.edges[0].prob ? "yes" : "NO");
    }
    if (all || what == "api") {
        graph g(3, 0);
        g.add_edge(0, 1, 0.9);
        g.terminals = {0, 1};
        std::printf("[api] graph(3,0)+add_edge: num_edges=%zu edges.size()=%zu validate=%d\n",
                    g.num_edges, g.edges.size(), g.validate());
        graph h(2, 2);
        h.add_edge(0, 1, 1.0);
        h.add_edge(0, 1, 0.5);
        h.terminals = {0, 1};
        auto d = build_diagram(h);
        std::printf("[api] perfect edge 0 -> edge_var=%d (header says -1), var_count=%d\n",
                    d.edge_var(0), d.manager.get_var_count());
    }
    if (what == "ladder") {
        // 2 x N ladder: boundary is 2, but bell_cache is sized V+E
        std::size_t n = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 4000;
        graph g = grid(2, n, 0.9);
        auto t0 = clk::now();
        double r = calculate_reliability(g);
        std::printf("[ladder 2x%zu] V+E=%zu R=%.6f time=%.0f ms peak RSS=%ld MB\n",
                    n, g.num_vertices + g.num_edges, r, ms(t0), rss_mb());
    }
    if (what == "order") {
        std::size_t n = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 40;
        graph g = grid(n, n, 0.9);
        auto t0 = clk::now();
        auto ord = generate_variable_ordering(g);
        std::printf("[ordering %zux%zu] generate_variable_ordering: %.0f ms\n", n, n, ms(t0));
        t0 = clk::now();
        auto f = estimate_max_boundary_size(g, ord);
        std::printf("[ordering %zux%zu] estimate_max_boundary_size=%zu: %.0f ms\n", n, n, f, ms(t0));
    }
    if (what == "grid") {
        std::size_t r = std::strtoul(argv[2], nullptr, 10), c = std::strtoul(argv[3], nullptr, 10);
        graph g = grid(r, c, 0.9);
        auto t0 = clk::now();
        double rl = calculate_reliability(g);
        double tl = ms(t0);
        t0 = clk::now();
        double rd = bdd_r(g);
        std::printf("[grid %zux%zu] lbl=%.9f (%.0f ms)  bdd=%.9f (%.0f ms) RSS=%ld MB\n", r, c, rl, tl, rd, ms(t0), rss_mb());
    }
    if (what == "eps") {
        // ultra-reliable: 6x6 grid, p=0.99999 -> unreliability tiny; compare 1-R
        graph g = grid(5, 5, 0.9999);
        double rl = calculate_reliability(g);
        double rd = bdd_r(g);
        std::printf("[epsilon] 1-R  lbl=%.6e  bdd=%.6e  rel.diff=%.2e\n", 1 - rl, 1 - rd, std::fabs((1 - rl) - (1 - rd)) / (1 - rd));
    }
}
