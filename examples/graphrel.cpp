/**
 * @file graphrel.cpp
 * @brief Network prob examples using teddy::graphrel
 *
 * Demonstrates three workflows:
 *   1. Programmatic graph construction + LBL scalar calculation
 *   2. BDD construction + full TeDDy analysis API (DPLD, importance)
 *   3. File I/O round-trip: write_graph → read_graph → calculate
 *
 * Reference values from Wu & Sun (2024), Table 2.
 * All networks use p = 0.9 for both vertices and edges (paper default).
 *
 * Sample data files (same networks in .rel format):
 *   data/graphrel/bridge.rel
 *   data/graphrel/mesh3x3.rel
 */

#include <teddy/graphrel/graphrel.hpp>
#include <libteddy/details/dplds.hpp>

#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {

// ── Network builders ─────────────────────────────────────────────────────────

/**
 * @brief Wu & Sun bridge network (Figure 1).
 *
 * Topology:
 *        v1(1)
 *       /  |  \
 *     e0   e2  e3
 *     /    |    \
 *  v0(0)        v3(3)   <- terminals
 *     \    |    /
 *     e1        e4
 *       \  |  /
 *        v2(2)
 *
 * All components p = 0.9.  Expected R = 0.760078728
 */
teddy::graphrel::graph make_bridge(double p)
{
    teddy::graphrel::graph g(4, 5);
    for (std::size_t i = 0; i < 4; ++i)
        g.set_vertex_prob(i, p);
    g.add_edge(0, 1, p);
    g.add_edge(0, 2, p);
    g.add_edge(1, 2, p);
    g.add_edge(1, 3, p);
    g.add_edge(2, 3, p);
    g.set_terminals({0, 3});
    return g;
}

/**
 * @brief N×M grid mesh network.
 *
 * Terminals: top-left (0) and bottom-right (rows*cols - 1).
 * Reference values (p=0.9): 3×3→0.72096303, 4×4→0.71325877, 5×5→0.71198829
 */
teddy::graphrel::graph make_mesh(std::size_t rows, std::size_t cols, double p)
{
    std::size_t nv = rows * cols;
    std::size_t ne = rows * (cols - 1) + (rows - 1) * cols;
    teddy::graphrel::graph g(nv, ne);
    for (std::size_t i = 0; i < nv; ++i)
        g.set_vertex_prob(i, p);
    for (std::size_t r = 0; r < rows; ++r)
        for (std::size_t c = 0; c < cols - 1; ++c)
            g.add_edge(r * cols + c, r * cols + c + 1, p);
    for (std::size_t r = 0; r < rows - 1; ++r)
        for (std::size_t c = 0; c < cols; ++c)
            g.add_edge(r * cols + c, (r + 1) * cols + c, p);
    g.set_terminals({0, nv - 1});
    return g;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

void print_header(const char* title)
{
    std::cout << "\n=== " << title << " ===\n";
}

void print_result(const char* label, double actual, double expected, double tol)
{
    bool ok = std::abs(actual - expected) < tol;
    std::cout << std::fixed << std::setprecision(9)
              << "  " << std::left << std::setw(28) << label
              << "  R = " << actual
              << "  (expected " << expected << ")"
              << "  " << (ok ? "PASS" : "FAIL") << "\n";
}

} // namespace

// ── main ─────────────────────────────────────────────────────────────────────

auto main() -> int
{
    // ── 1. Bridge: LBL scalar + BDD ──────────────────────────────────────────
    print_header("1. Bridge network — LBL and BDD paths");
    {
        auto g = make_bridge(0.9);
        std::cout << "  V=" << g.num_vertices()
                  << "  E=" << g.num_edges()
                  << "  |K|=" << g.terminals().size() << "\n";

        // Layer-by-layer scalar (memory-efficient, no BDD allocated)
        double r_lbl = teddy::graphrel::calculate_reliability(g);
        print_result("bridge LBL", r_lbl, 0.760078728, 1e-6);

        // BDD path: builds a full BDD, then queries probability
        auto bdd     = teddy::graphrel::build_diagram(g);
        double r_bdd = bdd.manager.calculate_probability(bdd.probs, bdd.diagram);
        print_result("bridge BDD", r_bdd, 0.760078728, 1e-6);
    }

    // ── 2. Bridge: BDD + TeDDy analysis API ──────────────────────────────────
    print_header("2. Bridge BDD — structural importance via TeDDy DPLD API");
    {
        auto bdd = teddy::graphrel::build_diagram(make_bridge(0.9));

        // Compute DPLD for each edge (0→1 change on component, 0→1 on system)
        auto fch = teddy::dpld::basic(0, 1);
        for (std::size_t eid = 0; eid < 5; ++eid) {
            teddy::int32 var = bdd.edge_var(eid);
            if (var < 0) continue;  // perfect edge — no variable
            auto ch   = bdd.edge_change(eid, 0, 1);
            auto dpld = bdd.manager.dpld(ch, fch, bdd.diagram);
            double si = bdd.manager.structural_importance(dpld);
            std::cout << std::fixed << std::setprecision(6)
                      << "  edge " << eid << "  SI = " << si << "\n";
        }
    }

    // ── 3. Mesh networks: LBL (Wu & Sun Table 2) ─────────────────────────────
    print_header("3. Mesh networks — LBL  (Wu & Sun 2024, Table 2)");
    {
        struct Case { std::size_t r, c; double expected; };
        for (auto [r, c, exp] : std::initializer_list<Case>{
                {3, 3, 0.72096303},
                {4, 4, 0.71325877},
                {5, 5, 0.71198829},
            })
        {
            auto g = make_mesh(r, c, 0.9);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%zux%zu mesh LBL", r, c);
            std::cout << "  V=" << g.num_vertices() << "  E=" << g.num_edges() << "\n";
            double rl = teddy::graphrel::calculate_reliability(g);
            print_result(buf, rl, exp, 1e-6);
        }
    }

    // ── 4. File I/O round-trip ────────────────────────────────────────────────
    //
    // .rel file format (see data/graphrel/ for full examples):
    //
    //   c  comment line
    //   p  prob <V> <E> [default_edge_rel [default_vertex_rel]]
    //   t  <terminal1> <terminal2> ...
    //   e  <from> <to> [prob]   (uses default if omitted)
    //   v  <vertex> <prob>      (only needed to override default)
    //
    print_header("4. File I/O round-trip  (write_graph → read_graph → compute)");
    {
        // 4a. Build bridge network and serialize to stream
        auto g_out = make_bridge(0.9);

        std::ostringstream buf;
        teddy::graphrel::write_graph(buf, g_out,
            "Wu & Sun bridge network\nExpected R = 0.760078728");

        std::cout << "  Serialized .rel content:\n";
        {
            std::istringstream preview(buf.str());
            for (std::string line; std::getline(preview, line); )
                std::cout << "    " << line << "\n";
        }

        // 4b. Deserialize and compute
        std::istringstream in(buf.str());
        auto g_in = teddy::graphrel::read_graph(in);

        double r_lbl = teddy::graphrel::calculate_reliability(g_in);
        print_result("bridge from stream LBL", r_lbl, 0.760078728, 1e-6);

        auto bdd = teddy::graphrel::build_diagram(g_in);
        print_result("bridge from stream BDD", bdd.manager.calculate_probability(bdd.probs, bdd.diagram),
                     0.760078728, 1e-6);

        // 4c. Write to file (data/graphrel/bridge.rel and mesh3x3.rel
        //     in the repository are generated the same way)
        teddy::graphrel::write_graph("bridge.rel", g_out,
            "Wu & Sun bridge network\nExpected R = 0.760078728");

        auto g_mesh = make_mesh(3, 3, 0.9);
        teddy::graphrel::write_graph("mesh3x3.rel", g_mesh,
            "Wu & Sun 3x3 mesh\nExpected R = 0.72096303");

        std::cout << "  Files written: bridge.rel, mesh3x3.rel\n";

        // 4d. Read back from file and compute via both paths
        auto g_bridge_file = teddy::graphrel::read_graph("bridge.rel");
        auto g_mesh_file   = teddy::graphrel::read_graph("mesh3x3.rel");

        print_result("bridge.rel LBL",
                     teddy::graphrel::calculate_reliability(g_bridge_file),
                     0.760078728, 1e-6);

        {
            auto bdd_bridge = teddy::graphrel::build_diagram(g_bridge_file);
            print_result("bridge.rel BDD",
                         bdd_bridge.manager.calculate_probability(bdd_bridge.probs, bdd_bridge.diagram),
                         0.760078728, 1e-6);
        }

        print_result("mesh3x3.rel LBL",
                     teddy::graphrel::calculate_reliability(g_mesh_file),
                     0.72096303, 1e-6);

        {
            auto bdd_mesh = teddy::graphrel::build_diagram(g_mesh_file);
            print_result("mesh3x3.rel BDD",
                         bdd_mesh.manager.calculate_probability(bdd_mesh.probs, bdd_mesh.diagram),
                         0.72096303, 1e-6);
        }
    }

    std::cout << "\nDone.\n";
    return 0;
}
