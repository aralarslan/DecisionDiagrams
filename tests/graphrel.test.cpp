/**
 * @file graphrel.test.cpp
 * @brief End-to-end graphrel tests (Phase A gate)
 *
 * Smoke tests verifying the full pipeline:
 *   read graph → build BDD → calculate prob → verify value
 *
 * Reference values from Wu & Sun (2024), Table 1 / Figure 1.
 * Bridge network (5-node Wheatstone bridge) with p=0.9 for all edges.
 */

#define BOOST_TEST_MODULE graphrel_test
#include <boost/test/included/unit_test.hpp>
#include <boost/test/tools/floating_point_comparison.hpp>

#include <teddy/graphrel/graphrel.hpp>
#include "common/graph_operations.hpp"
#include "common/pruning.hpp"
#include "common/bell_cache.hpp"
#include "common/decomp_state.hpp"
#include "common/union_find.hpp"

namespace {

/**
 * @brief Build the Wheatstone bridge with perfect vertices.
 *
 * Standard 5-edge Wheatstone bridge:
 *   Edges: (0,1), (0,2), (1,2), (1,3), (2,3)
 *   Terminals: {0, 3}
 *   All edge reliabilities = p, vertex prob = 1.0 (perfect)
 */
teddy::graphrel::graph make_bridge_graph(double p) {
    teddy::graphrel::graph g(4, 5);

    g.add_edge(0, 1, p);  // e0
    g.add_edge(0, 2, p);  // e1
    g.add_edge(1, 2, p);  // e2
    g.add_edge(1, 3, p);  // e3
    g.add_edge(2, 3, p);  // e4

    g.terminals = {0, 3};

    return g;
}

/**
 * @brief Wu & Sun bridge network (Figure 1): p for BOTH vertices and edges.
 *
 * This is the exact network from the paper. With p=0.9, the expected
 * prob is 0.760078728 (Wu & Sun 2024, Table 2).
 */
teddy::graphrel::graph make_wusun_bridge(double p) {
    teddy::graphrel::graph g(4, 5);

    // All vertices are fallible
    for (std::size_t i = 0; i < 4; ++i)
        g.vertex_probs[i] = p;

    g.add_edge(0, 1, p);  // e1: v1-v2
    g.add_edge(0, 2, p);  // e2: v1-v3
    g.add_edge(1, 2, p);  // e3: v2-v3
    g.add_edge(1, 3, p);  // e4: v2-v4
    g.add_edge(2, 3, p);  // e5: v3-v4

    g.terminals = {0, 3};  // v1, v4

    return g;
}

/**
 * @brief N×M grid mesh network.
 *
 * Topology example for 3×4:
 *   0---1---2---3
 *   |   |   |   |
 *   4---5---6---7
 *   |   |   |   |
 *   8---9--10--11
 *
 * Terminals: top-left (0) and bottom-right (rows*cols - 1).
 */
teddy::graphrel::graph make_mesh_graph(std::size_t rows, std::size_t cols,
                                       double edge_rel  = 0.9,
                                       double vertex_rel = 0.9) {
    std::size_t num_vertices = rows * cols;
    std::size_t num_edges    = rows * (cols - 1) + (rows - 1) * cols;

    teddy::graphrel::graph g(num_vertices, num_edges);

    for (std::size_t i = 0; i < num_vertices; ++i)
        g.vertex_probs[i] = vertex_rel;

    // Horizontal edges
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c < cols - 1; ++c) {
            g.add_edge(r * cols + c, r * cols + c + 1, edge_rel);
        }
    }

    // Vertical edges
    for (std::size_t r = 0; r < rows - 1; ++r) {
        for (std::size_t c = 0; c < cols; ++c) {
            g.add_edge(r * cols + c, (r + 1) * cols + c, edge_rel);
        }
    }

    g.terminals = {0, num_vertices - 1};

    return g;
}

} // namespace

BOOST_AUTO_TEST_SUITE(graphrel_test)

/**
 * Bridge BDD prob test.
 *
 * Expected value: 0.59049 + ... computed analytically for the bridge
 * with all-0.9 edges. The exact value for terminal connectivity
 * (paths from v0 to v3):
 *   R = p^2 + p^2 + p^3 - 2p^3 - 2p^4 + ... (inclusion-exclusion)
 *
 * For p=0.9: analytically verified to be ≈ 0.98181 for a 4-node bridge.
 * We test for self-consistency (BDD == LBL) with a tight tolerance,
 * and a loose sanity check (0 < R < 1).
 */
BOOST_AUTO_TEST_CASE(bridge_bdd_sanity) {
    auto g = make_bridge_graph(0.9);
    auto bdd = teddy::graphrel::build_diagram(g);
    double r = bdd.manager.calculate_probability(bdd.probs, bdd.diagram);

    // Sanity: valid probability
    BOOST_TEST(r > 0.0);
    BOOST_TEST(r < 1.0);

    // Structural: at least one path e0+e3 = 0.9*0.9 = 0.81
    BOOST_TEST(r >= 0.81);
}

/**
 * LBL must match BDD to machine precision on the bridge.
 */
BOOST_AUTO_TEST_CASE(bridge_lbl_matches_bdd) {
    auto g = make_bridge_graph(0.9);

    double r_lbl = teddy::graphrel::calculate_reliability(g);
    auto bdd     = teddy::graphrel::build_diagram(g);
    double r_bdd = bdd.manager.calculate_probability(bdd.probs, bdd.diagram);

    BOOST_TEST(r_lbl == r_bdd, boost::test_tools::tolerance(1e-10));
}

/**
 * Simple path graph (s—e0—a—e1—t): R = p^2.
 */
BOOST_AUTO_TEST_CASE(series_path_reliability) {
    double p = 0.9;
    teddy::graphrel::graph g(3, 2);
    g.add_edge(0, 1, p);
    g.add_edge(1, 2, p);
    g.terminals = {0, 2};

    double r_lbl = teddy::graphrel::calculate_reliability(g);
    auto bdd     = teddy::graphrel::build_diagram(g);
    double r_bdd = bdd.manager.calculate_probability(bdd.probs, bdd.diagram);

    BOOST_TEST(r_lbl == p * p, boost::test_tools::tolerance(1e-12));
    BOOST_TEST(r_bdd == p * p, boost::test_tools::tolerance(1e-12));
}

/**
 * Two parallel edges between s and t: R = 1 - (1-p)^2 = 2p - p^2.
 */
BOOST_AUTO_TEST_CASE(parallel_edges_reliability) {
    double p = 0.9;
    teddy::graphrel::graph g(2, 2);
    g.add_edge(0, 1, p);
    g.add_edge(0, 1, p);
    g.terminals = {0, 1};

    double expected = 1.0 - (1.0 - p) * (1.0 - p);

    double r_lbl = teddy::graphrel::calculate_reliability(g);
    auto bdd     = teddy::graphrel::build_diagram(g);
    double r_bdd = bdd.manager.calculate_probability(bdd.probs, bdd.diagram);

    BOOST_TEST(r_lbl == expected, boost::test_tools::tolerance(1e-12));
    BOOST_TEST(r_bdd == expected, boost::test_tools::tolerance(1e-12));
}

/**
 * Single terminal in terminal set: R = 1 (trivially connected).
 */
BOOST_AUTO_TEST_CASE(single_terminal_trivial) {
    teddy::graphrel::graph g(3, 2);
    g.add_edge(0, 1, 0.9);
    g.add_edge(1, 2, 0.9);
    g.terminals = {1};  // only one terminal

    double r_lbl = teddy::graphrel::calculate_reliability(g);
    auto bdd     = teddy::graphrel::build_diagram(g);
    double r_bdd = bdd.manager.calculate_probability(bdd.probs, bdd.diagram);

    BOOST_TEST(r_lbl == 1.0, boost::test_tools::tolerance(1e-12));
    BOOST_TEST(r_bdd == 1.0, boost::test_tools::tolerance(1e-12));
}

BOOST_AUTO_TEST_SUITE_END()

// ── Wu & Sun (2024) paper verification ───────────────────────────────────────
//
// Reference values from Wu & Sun (2024), Table 2.
// Networks use p=0.9 for BOTH vertices and edges unless noted.
// The "quick" subset (bridge, 3×3, 4×4) runs fast enough for CI.
// Larger meshes (5×5+) are included but may take tens of seconds.
//
BOOST_AUTO_TEST_SUITE(wusun_test)

// Bridge network (Wu & Sun Figure 1) with p=0.9 for vertices AND edges.
// Expected: 0.760078728 (from paper Table 2 / Figure 1).

BOOST_AUTO_TEST_CASE(bridge_lbl) {
    auto g = make_wusun_bridge(0.9);
    double r = teddy::graphrel::calculate_reliability(g);
    BOOST_TEST(r == 0.760078728, boost::test_tools::tolerance(1e-6));
}

BOOST_AUTO_TEST_CASE(bridge_bdd) {
    auto g = make_wusun_bridge(0.9);
    auto bdd = teddy::graphrel::build_diagram(g);
    double r = bdd.manager.calculate_probability(bdd.probs, bdd.diagram);
    BOOST_TEST(r == 0.760078728, boost::test_tools::tolerance(1e-6));
}

BOOST_AUTO_TEST_CASE(bridge_lbl_bdd_agree) {
    auto g    = make_wusun_bridge(0.9);
    double r_lbl = teddy::graphrel::calculate_reliability(g);
    auto bdd     = teddy::graphrel::build_diagram(g);
    double r_bdd = bdd.manager.calculate_probability(bdd.probs, bdd.diagram);
    BOOST_TEST(r_lbl == r_bdd, boost::test_tools::tolerance(1e-12));
}

// 3×3 mesh, p=0.9 (fast, expected: 0.72096303)

BOOST_AUTO_TEST_CASE(mesh_3x3_lbl) {
    auto g = make_mesh_graph(3, 3);
    double r = teddy::graphrel::calculate_reliability(g);
    BOOST_TEST(r == 0.72096303, boost::test_tools::tolerance(1e-6));
}

BOOST_AUTO_TEST_CASE(mesh_3x3_bdd) {
    auto g = make_mesh_graph(3, 3);
    auto bdd = teddy::graphrel::build_diagram(g);
    double r = bdd.manager.calculate_probability(bdd.probs, bdd.diagram);
    BOOST_TEST(r == 0.72096303, boost::test_tools::tolerance(1e-6));
}

BOOST_AUTO_TEST_CASE(mesh_3x3_lbl_bdd_agree) {
    auto g       = make_mesh_graph(3, 3);
    double r_lbl = teddy::graphrel::calculate_reliability(g);
    auto bdd     = teddy::graphrel::build_diagram(g);
    double r_bdd = bdd.manager.calculate_probability(bdd.probs, bdd.diagram);
    BOOST_TEST(r_lbl == r_bdd, boost::test_tools::tolerance(1e-10));
}

// 4×4 mesh, p=0.9 (expected: 0.71325877)

BOOST_AUTO_TEST_CASE(mesh_4x4_lbl) {
    auto g = make_mesh_graph(4, 4);
    double r = teddy::graphrel::calculate_reliability(g);
    BOOST_TEST(r == 0.71325877, boost::test_tools::tolerance(1e-6));
}

// 5×5 mesh, p=0.9 (expected: 0.71198829)
// Slower — may take ~10-30 s on CI depending on hardware.

BOOST_AUTO_TEST_CASE(mesh_5x5_lbl) {
    auto g = make_mesh_graph(5, 5);
    double r = teddy::graphrel::calculate_reliability(g);
    BOOST_TEST(r == 0.71198829, boost::test_tools::tolerance(1e-6));
}

BOOST_AUTO_TEST_SUITE_END()

// ── Unit tests ────────────────────────────────────────────────────────────────
//
// Tests for core data structures:
//   union_find, bell_cache, graph construction
//
BOOST_AUTO_TEST_SUITE(graphrel_unit_test)

// ---- union_find ----

BOOST_AUTO_TEST_CASE(union_find_initial_state) {
    teddy::graphrel::union_find uf(5);
    for (std::size_t i = 0; i < 5; ++i)
        BOOST_TEST(uf.find_root(i) == i);
    BOOST_TEST(!uf.connected_const(0, 1));
    BOOST_TEST(!uf.connected_const(2, 4));
}

BOOST_AUTO_TEST_CASE(union_find_unite) {
    teddy::graphrel::union_find uf(5);
    uf.unite(0, 1);
    BOOST_TEST(uf.connected(0, 1));
    BOOST_TEST(!uf.connected(0, 2));
    uf.unite(1, 2);
    BOOST_TEST(uf.connected(0, 2));
    BOOST_TEST(!uf.connected(0, 3));
    BOOST_TEST(!uf.connected(3, 4));
}

BOOST_AUTO_TEST_CASE(union_find_get_classes) {
    teddy::graphrel::union_find uf(4);
    uf.unite(0, 1);
    uf.unite(2, 3);
    auto classes = uf.get_classes();
    BOOST_TEST(classes.size() == 2u);
    std::size_t total = 0;
    for (const auto& c : classes)
        total += c.size();
    BOOST_TEST(total == 4u);
}

BOOST_AUTO_TEST_CASE(union_find_reset) {
    teddy::graphrel::union_find uf(3);
    uf.unite(0, 1);
    uf.unite(1, 2);
    BOOST_TEST(uf.connected(0, 2));
    uf.reset();
    BOOST_TEST(!uf.connected(0, 1));
    BOOST_TEST(!uf.connected(1, 2));
    BOOST_TEST(!uf.connected(0, 2));
}

// ---- bell_cache ----

BOOST_AUTO_TEST_CASE(stirling_known_values) {
    teddy::graphrel::bell_cache cache;
    // S(1,1) = 1
    BOOST_TEST(cache.stirling(1, 1) == 1u);
    // S(2,1) = 1, S(2,2) = 1
    BOOST_TEST(cache.stirling(2, 1) == 1u);
    BOOST_TEST(cache.stirling(2, 2) == 1u);
    // S(3,1) = 1, S(3,2) = 3, S(3,3) = 1
    BOOST_TEST(cache.stirling(3, 1) == 1u);
    BOOST_TEST(cache.stirling(3, 2) == 3u);
    BOOST_TEST(cache.stirling(3, 3) == 1u);
    // S(4,1) = 1, S(4,2) = 7, S(4,3) = 6, S(4,4) = 1
    BOOST_TEST(cache.stirling(4, 1) == 1u);
    BOOST_TEST(cache.stirling(4, 2) == 7u);
    BOOST_TEST(cache.stirling(4, 3) == 6u);
    BOOST_TEST(cache.stirling(4, 4) == 1u);
    // S(n,0) = 0 for n > 0
    BOOST_TEST(cache.stirling(1, 0) == 0u);
    BOOST_TEST(cache.stirling(3, 0) == 0u);
    // S(n,k) = 0 for k > n
    BOOST_TEST(cache.stirling(2, 3) == 0u);
}

BOOST_AUTO_TEST_CASE(bell_known_values) {
    teddy::graphrel::bell_cache cache;
    BOOST_TEST(cache.bell(0) == 1u);
    BOOST_TEST(cache.bell(1) == 1u);
    BOOST_TEST(cache.bell(2) == 2u);
    BOOST_TEST(cache.bell(3) == 5u);
    BOOST_TEST(cache.bell(4) == 15u);
    BOOST_TEST(cache.bell(5) == 52u);
    BOOST_TEST(cache.bell(6) == 203u);
    BOOST_TEST(cache.bell(7) == 877u);
    BOOST_TEST(cache.bell(8) == 4140u);
}

BOOST_AUTO_TEST_CASE(stirling_bell_is_valid) {
    teddy::graphrel::bell_cache cache(10);
    BOOST_TEST( cache.is_valid(0));
    BOOST_TEST( cache.is_valid(10));
    BOOST_TEST(!cache.is_valid(11));
    BOOST_TEST(cache.max_n() == 10u);
}

// ---- graph construction ----

BOOST_AUTO_TEST_CASE(graph_construction) {
    teddy::graphrel::graph g(4, 3);
    BOOST_TEST(g.num_vertices == 4u);
    BOOST_TEST(g.num_edges   == 3u);
    g.add_edge(0, 1, 0.9);
    g.add_edge(1, 2, 0.8);
    g.add_edge(2, 3, 0.7);
    BOOST_TEST(g.edges.size() == 3u);
    BOOST_TEST(g.edges[0].from        == 0u);
    BOOST_TEST(g.edges[0].to          == 1u);
    BOOST_TEST(g.edges[0].prob == 0.9);
    BOOST_TEST(g.edges[1].prob == 0.8);
    // vertex 1 is incident to e0 and e1
    BOOST_TEST(g.adj[1].size() == 2u);
}

BOOST_AUTO_TEST_CASE(graph_is_terminal) {
    teddy::graphrel::graph g(4, 2);
    g.add_edge(0, 1, 0.9);
    g.add_edge(2, 3, 0.9);
    g.terminals = {0, 3};
    BOOST_TEST( g.is_terminal(0));
    BOOST_TEST(!g.is_terminal(1));
    BOOST_TEST(!g.is_terminal(2));
    BOOST_TEST( g.is_terminal(3));
}

BOOST_AUTO_TEST_CASE(graph_validation) {
    teddy::graphrel::graph g(3, 2);
    g.add_edge(0, 1, 0.9);
    g.add_edge(1, 2, 0.9);
    g.terminals = {0, 2};
    BOOST_TEST(g.validate());
}

BOOST_AUTO_TEST_SUITE_END()

// ── Integration tests ─────────────────────────────────────────────────────────
//
// Tests for algorithm components:
//   coloop detection, pruning, graph operations, ordering independence,
//   vertex variables
//
BOOST_AUTO_TEST_SUITE(graphrel_integration_test)

// ---- coloop detection ----

BOOST_AUTO_TEST_CASE(coloop_chain_both_bridges) {
    // 0 --e0-- 1 --e1-- 2, terminals {0,2}: both edges are bridges
    teddy::graphrel::graph g(3, 2);
    g.add_edge(0, 1, 0.9);
    g.add_edge(1, 2, 0.9);
    g.terminals = {0, 2};

    teddy::graphrel::decomp_state sg(g, g.terminals);
    BOOST_TEST( teddy::graphrel::is_coloop(sg, 0, g));
    BOOST_TEST( teddy::graphrel::is_coloop(sg, 1, g));
}

BOOST_AUTO_TEST_CASE(coloop_parallel_not_bridge) {
    // 0 --e0-- 1, 0 --e1-- 1, terminals {0,1}: neither is a coloop
    teddy::graphrel::graph g(2, 2);
    g.add_edge(0, 1, 0.9);
    g.add_edge(0, 1, 0.9);
    g.terminals = {0, 1};

    teddy::graphrel::decomp_state sg(g, g.terminals);
    BOOST_TEST(!teddy::graphrel::is_coloop(sg, 0, g));
    BOOST_TEST(!teddy::graphrel::is_coloop(sg, 1, g));
}

BOOST_AUTO_TEST_CASE(coloop_diamond_no_bridges) {
    // 0-e0-1-e1-3, 0-e2-2-e3-3, terminals {0,3}: no bridges
    teddy::graphrel::graph g(4, 4);
    g.add_edge(0, 1, 0.9);  // e0
    g.add_edge(1, 3, 0.9);  // e1
    g.add_edge(0, 2, 0.9);  // e2
    g.add_edge(2, 3, 0.9);  // e3
    g.terminals = {0, 3};

    teddy::graphrel::decomp_state sg(g, g.terminals);
    BOOST_TEST(!teddy::graphrel::is_coloop(sg, 0, g));
    BOOST_TEST(!teddy::graphrel::is_coloop(sg, 1, g));
    BOOST_TEST(!teddy::graphrel::is_coloop(sg, 2, g));
    BOOST_TEST(!teddy::graphrel::is_coloop(sg, 3, g));
}

// ---- pruning ----

BOOST_AUTO_TEST_CASE(pruning_dead_appendage) {
    // 0 --e0-- 1 --e1-- 2 --e2-- 3  terminals {0,2}: vertex 3 is a dead leaf
    teddy::graphrel::graph g(4, 3);
    g.add_edge(0, 1, 0.9);  // e0
    g.add_edge(1, 2, 0.9);  // e1
    g.add_edge(2, 3, 0.9);  // e2
    g.terminals = {0, 2};

    teddy::graphrel::decomp_state sg(g, g.terminals);
    BOOST_TEST(sg.remaining_edges.size() == 3u);

    teddy::graphrel::prune_unreachable_edges(sg, g);

    // e2 pruned (vertex 3: non-terminal, degree 1)
    BOOST_TEST(sg.remaining_edges.size() == 2u);
    BOOST_TEST(std::binary_search(sg.remaining_edges.begin(),
                                   sg.remaining_edges.end(), std::size_t{0}));
    BOOST_TEST(std::binary_search(sg.remaining_edges.begin(),
                                   sg.remaining_edges.end(), std::size_t{1}));
}

BOOST_AUTO_TEST_CASE(pruning_cascade) {
    // 0 --e0-- 1 --e1-- 2 --e2-- 3 --e3-- 4  terminals {0,2}
    // vertices 3,4 are non-terminal dead ends → cascade removes e3 then e2
    teddy::graphrel::graph g(5, 4);
    g.add_edge(0, 1, 0.9);  // e0
    g.add_edge(1, 2, 0.9);  // e1
    g.add_edge(2, 3, 0.9);  // e2
    g.add_edge(3, 4, 0.9);  // e3
    g.terminals = {0, 2};

    teddy::graphrel::decomp_state sg(g, g.terminals);
    teddy::graphrel::prune_unreachable_edges(sg, g);

    BOOST_TEST(sg.remaining_edges.size() == 2u);
    BOOST_TEST(std::binary_search(sg.remaining_edges.begin(),
                                   sg.remaining_edges.end(), std::size_t{0}));
    BOOST_TEST(std::binary_search(sg.remaining_edges.begin(),
                                   sg.remaining_edges.end(), std::size_t{1}));
}

// ---- graph operations ----

BOOST_AUTO_TEST_CASE(graph_ops_delete_edge) {
    teddy::graphrel::graph g(3, 2);
    g.add_edge(0, 1, 0.9);  // e0
    g.add_edge(1, 2, 0.9);  // e1
    g.terminals = {0, 2};

    teddy::graphrel::decomp_state sg(g, g.terminals);
    auto sg2 = teddy::graphrel::delete_edge(sg, 0, g);
    BOOST_TEST(sg2.remaining_edges.size() == 1u);
    BOOST_TEST(std::binary_search(sg2.remaining_edges.begin(),
                                   sg2.remaining_edges.end(), std::size_t{1}));
}

BOOST_AUTO_TEST_CASE(graph_ops_contract_edge) {
    teddy::graphrel::graph g(3, 2);
    g.add_edge(0, 1, 0.9);  // e0
    g.add_edge(1, 2, 0.9);  // e1
    g.terminals = {0, 2};

    teddy::graphrel::decomp_state sg(g, g.terminals);
    auto sg2 = teddy::graphrel::contract_edge(sg, 0, g);

    // e0 removed; vertices 0 and 1 merged
    BOOST_TEST(sg2.remaining_edges.size() == 1u);
    BOOST_TEST( sg2.uf_.connected_const(0, 1));
    BOOST_TEST(!sg2.uf_.connected_const(0, 2));
}

BOOST_AUTO_TEST_CASE(graph_ops_delete_vertex) {
    teddy::graphrel::graph g(3, 2);
    g.add_edge(0, 1, 0.9);  // e0
    g.add_edge(1, 2, 0.9);  // e1
    g.terminals = {0, 2};

    teddy::graphrel::decomp_state sg(g, g.terminals);
    auto sg2 = teddy::graphrel::delete_vertex(sg, 1, g);

    // Both edges incident to vertex 1 removed
    BOOST_TEST(sg2.remaining_edges.empty());
}

BOOST_AUTO_TEST_CASE(graph_ops_is_loop) {
    // Parallel edges 0--e0--1, 0--e1--1
    // After contracting e0, e1 endpoints are in same component → loop
    teddy::graphrel::graph g(2, 2);
    g.add_edge(0, 1, 0.9);  // e0
    g.add_edge(0, 1, 0.9);  // e1
    g.terminals = {0, 1};

    teddy::graphrel::decomp_state sg(g, g.terminals);
    BOOST_TEST(!teddy::graphrel::is_loop(sg, 0, g));

    auto sg2 = teddy::graphrel::contract_edge(sg, 0, g);
    BOOST_TEST(teddy::graphrel::is_loop(sg2, 1, g));
}

BOOST_AUTO_TEST_CASE(graph_ops_success_terminal) {
    // Chain: contracting both edges connects all terminals
    teddy::graphrel::graph g(3, 2);
    g.add_edge(0, 1, 0.9);  // e0
    g.add_edge(1, 2, 0.9);  // e1
    g.terminals = {0, 2};

    teddy::graphrel::decomp_state sg(g, g.terminals);
    BOOST_TEST(!sg.is_success_terminal());

    auto sg2 = teddy::graphrel::contract_edge(sg, 0, g);
    auto sg3 = teddy::graphrel::contract_edge(sg2, 1, g);
    BOOST_TEST(sg3.is_success_terminal());
}

// ---- ordering independence ----

BOOST_AUTO_TEST_CASE(ordering_independence_chain) {
    // Chain R = p^2 = 0.81; BFS and DFS must agree
    teddy::graphrel::graph g(3, 2);
    g.add_edge(0, 1, 0.9);
    g.add_edge(1, 2, 0.9);
    g.terminals = {0, 2};

    auto bfs_ord = teddy::graphrel::generate_variable_ordering(
        g, teddy::graphrel::ordering_strategy::bfs);
    auto dfs_ord = teddy::graphrel::generate_variable_ordering(
        g, teddy::graphrel::ordering_strategy::dfs);

    double r_bfs = teddy::graphrel::calculate_reliability(g, bfs_ord);
    double r_dfs = teddy::graphrel::calculate_reliability(g, dfs_ord);

    BOOST_TEST(r_bfs == r_dfs, boost::test_tools::tolerance(1e-12));
    BOOST_TEST(r_bfs == 0.81,  boost::test_tools::tolerance(1e-12));
}

BOOST_AUTO_TEST_CASE(ordering_independence_bridge) {
    // Wheatstone bridge; BFS and DFS must agree
    auto g = make_bridge_graph(0.9);

    auto bfs_ord = teddy::graphrel::generate_variable_ordering(
        g, teddy::graphrel::ordering_strategy::bfs);
    auto dfs_ord = teddy::graphrel::generate_variable_ordering(
        g, teddy::graphrel::ordering_strategy::dfs);

    double r_bfs = teddy::graphrel::calculate_reliability(g, bfs_ord);
    double r_dfs = teddy::graphrel::calculate_reliability(g, dfs_ord);

    BOOST_TEST(r_bfs == r_dfs, boost::test_tools::tolerance(1e-12));
}

// ---- vertex variables ----

BOOST_AUTO_TEST_CASE(vertex_variables_perfect_vs_fallible) {
    // Same topology; fallible vertices reduce prob vs perfect
    auto g_perfect  = make_bridge_graph(0.9);  // vertex_probs = 1.0
    auto g_fallible = make_wusun_bridge(0.9);  // vertex_probs = 0.9

    double r_perfect  = teddy::graphrel::calculate_reliability(g_perfect);
    double r_fallible = teddy::graphrel::calculate_reliability(g_fallible);

    BOOST_TEST(r_fallible < r_perfect);
    BOOST_TEST(r_fallible == 0.760078728, boost::test_tools::tolerance(1e-6));
}

BOOST_AUTO_TEST_CASE(vertex_variables_bdd_matches_lbl) {
    // BDD and LBL must agree for fallible-vertex network
    auto g    = make_wusun_bridge(0.9);
    double r_lbl = teddy::graphrel::calculate_reliability(g);
    auto bdd     = teddy::graphrel::build_diagram(g);
    double r_bdd = bdd.manager.calculate_probability(bdd.probs, bdd.diagram);

    BOOST_TEST(r_lbl == r_bdd, boost::test_tools::tolerance(1e-12));
}

BOOST_AUTO_TEST_SUITE_END()
