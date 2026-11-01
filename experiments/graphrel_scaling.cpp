/**
 * @file graphrel_scaling.cpp
 * @brief Scaling benchmark: LBL vs BDD on mesh networks (libgraphrel)
 *
 * Port of graphrel/tests/benchmarks/test_bsbdd_scaling.cpp, updated for the
 * teddy::graphrel API. analyze_reliability() reports the detailed statistics
 * (G_max, total_states, max_front_size).
 *
 * Usage:
 *   ./graphrel-scaling          # 3x3 through 8x8
 *   ./graphrel-scaling 10       # include 10x10
 */

#include "teddy/graphrel/graphrel.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace gr = teddy::graphrel;

// ---------------------------------------------------------------------------
// Mesh network factory
// ---------------------------------------------------------------------------

/// Build an N×M grid; terminals = {0, N*M-1} (top-left → bottom-right).
static gr::graph make_mesh(std::size_t rows, std::size_t cols,
                            double p_edge, double p_vertex)
{
    std::size_t nv = rows * cols;
    std::size_t ne = rows * (cols - 1) + (rows - 1) * cols;
    gr::graph g(nv, ne);

    g.set_all_vertex_probs(p_vertex);

    // Horizontal edges
    for (std::size_t r = 0; r < rows; ++r)
        for (std::size_t c = 0; c + 1 < cols; ++c)
            g.add_edge(r * cols + c, r * cols + c + 1, p_edge);

    // Vertical edges
    for (std::size_t r = 0; r + 1 < rows; ++r)
        for (std::size_t c = 0; c < cols; ++c)
            g.add_edge(r * cols + c, (r + 1) * cols + c, p_edge);

    g.set_terminals({0, nv - 1});
    return g;
}

// ---------------------------------------------------------------------------
// Result record
// ---------------------------------------------------------------------------

struct ScalingResult {
    std::string name;
    std::size_t vertices = 0;
    std::size_t edges    = 0;

    // LBL
    double      lbl_reliability    = 0.0;
    long        lbl_time_ms        = 0;
    std::size_t lbl_gmax           = 0;
    std::size_t lbl_decompositions = 0;
    std::size_t lbl_max_boundary   = 0;

    // BDD
    double      bdd_reliability = 0.0;
    long        bdd_time_ms     = 0;
    long long   bdd_node_count  = 0;

    bool match = false;
};

// ---------------------------------------------------------------------------
// Benchmark runner
// ---------------------------------------------------------------------------

static ScalingResult run(int rows, int cols,
                          double p_edge = 0.9, double p_vertex = 0.9)
{
    gr::graph g = make_mesh(rows, cols, p_edge, p_vertex);

    ScalingResult res;
    res.name     = std::to_string(rows) + "x" + std::to_string(cols);
    res.vertices = g.num_vertices();
    res.edges    = g.edges().size();

    // 1. LBL — analyze_reliability() reports the statistics
    {
        auto t0 = std::chrono::high_resolution_clock::now();
        const gr::reliability_result lbl = gr::analyze_reliability(g);
        res.lbl_reliability = lbl.reliability;
        auto t1 = std::chrono::high_resolution_clock::now();
        res.lbl_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        auto const& s = lbl.statistics;
        res.lbl_gmax           = s.max_layer_size;
        res.lbl_decompositions = s.total_states;
        res.lbl_max_boundary   = s.max_front_size;
    }

    // 2. BDD — public build_diagram(); pool scaled from LBL decomposition count
    {
        teddy::int64 pool = static_cast<teddy::int64>(
            std::max(100'000LL,
                     static_cast<long long>(res.lbl_decompositions) / 2));

        gr::diagram_options bdd_options;
        bdd_options.pool_size = pool;
        auto t0 = std::chrono::high_resolution_clock::now();
        gr::reliability_diagram bdd = gr::build_diagram(g, bdd_options);
        res.bdd_reliability = bdd.manager.calculate_probability(bdd.probs, bdd.diagram);
        auto t1 = std::chrono::high_resolution_clock::now();
        res.bdd_time_ms   = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        res.bdd_node_count = bdd.manager.get_node_count(bdd.diagram);
    }

    res.match = std::abs(res.lbl_reliability - res.bdd_reliability) < 1e-9;
    return res;
}

// ---------------------------------------------------------------------------
// Output helpers
// ---------------------------------------------------------------------------

static std::string fmt_time(long ms)
{
    if (ms < 1000)
        return std::to_string(ms) + "ms";
    if (ms < 60'000)
        return std::to_string(ms / 1000) + "." + std::to_string((ms % 1000) / 100) + "s";
    return std::to_string(ms / 60'000) + "m" + std::to_string((ms % 60'000) / 1000) + "s";
}

static void print_summary(const std::vector<ScalingResult>& results)
{
    std::cout << "\n";
    std::cout << "==========================================================================\n";
    std::cout << " SCALING BENCHMARK: LBL vs build_diagram  (p_edge=0.9, p_vertex=0.9)\n";
    std::cout << "==========================================================================\n";
    std::cout << std::left
              << std::setw(8)  << "Mesh"
              << std::setw(14) << "Reliability"
              << std::setw(12) << "LBL Time"
              << std::setw(12) << "BDD Time"
              << std::setw(12) << "LBL G_max"
              << std::setw(14) << "BDD Nodes"
              << std::setw(8)  << "Match"
              << "\n";
    std::cout << "--------------------------------------------------------------------------\n";

    for (const auto& r : results) {
        std::cout << std::left
                  << std::setw(8)  << r.name
                  << std::fixed << std::setprecision(8)
                  << std::setw(14) << r.lbl_reliability
                  << std::setw(12) << fmt_time(r.lbl_time_ms)
                  << std::setw(12) << fmt_time(r.bdd_time_ms)
                  << std::setw(12) << r.lbl_gmax
                  << std::setw(14) << r.bdd_node_count
                  << std::setw(8)  << (r.match ? "OK" : "FAIL")
                  << "\n";
    }
    std::cout << "--------------------------------------------------------------------------\n";
}

static void print_detailed(const ScalingResult& r)
{
    std::cout << "\n--- " << r.name << " mesh (" << r.vertices << "V, " << r.edges << "E) ---\n";

    std::cout << "  LBL:\n"
              << "    Reliability:     " << std::fixed << std::setprecision(10) << r.lbl_reliability << "\n"
              << "    Time:            " << r.lbl_time_ms << " ms\n"
              << "    G_max:           " << r.lbl_gmax << "\n"
              << "    Decompositions:  " << r.lbl_decompositions << "\n"
              << "    Max boundary:    " << r.lbl_max_boundary << "\n";

    std::cout << "  BDD:\n"
              << "    Reliability:     " << std::fixed << std::setprecision(10) << r.bdd_reliability << "\n"
              << "    Time:            " << r.bdd_time_ms << " ms\n"
              << "    Node count:      " << r.bdd_node_count << "\n";

    double diff = std::abs(r.lbl_reliability - r.bdd_reliability);
    std::cout << "  Match: " << (r.match ? "OK" : "FAIL")
              << "  (diff=" << std::scientific << std::setprecision(3) << diff << ")\n";
}

static void print_scaling_ratios(const std::vector<ScalingResult>& results)
{
    if (results.size() < 2) return;

    std::cout << "\n\nSCALING RATIOS\n"
              << "==============\n";
    for (std::size_t i = 1; i < results.size(); ++i) {
        const auto& prev = results[i - 1];
        const auto& cur  = results[i];

        auto ratio = [](auto num, auto den) -> double {
            return den > 0 ? static_cast<double>(num) / static_cast<double>(den) : 0.0;
        };

        std::cout << "  " << prev.name << " -> " << cur.name << ": "
                  << "LBL time " << std::fixed << std::setprecision(1) << ratio(cur.lbl_time_ms, prev.lbl_time_ms) << "x, "
                  << "BDD time " << ratio(cur.bdd_time_ms, prev.bdd_time_ms) << "x, "
                  << "G_max "    << ratio(cur.lbl_gmax, prev.lbl_gmax) << "x, "
                  << "BDD nodes " << ratio(cur.bdd_node_count, prev.bdd_node_count) << "x\n";
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    int max_size = 8;
    if (argc > 1) {
        max_size = std::atoi(argv[1]);
        if (max_size < 3)  max_size = 3;
        if (max_size > 12) max_size = 12;
    }

    std::cout << "\ngraphrel Scaling Benchmark\n"
              << "Parameters: p_edge=0.9, p_vertex=0.9, K=2 (top-left → bottom-right)\n"
              << "Max mesh size: " << max_size << "x" << max_size << "\n";

    if (max_size >= 10)
        std::cout << "\nNote: 10x10 BDD may use significant memory (>4 GB). "
                     "Monitor accordingly.\n";

    std::vector<std::pair<int,int>> sizes = {{3,3},{4,4},{5,5},{6,6},{8,8}};
    if (max_size >= 10) sizes.push_back({10,10});

    std::vector<ScalingResult> results;
    for (auto [r, c] : sizes) {
        if (r > max_size) continue;
        std::cout << "\nRunning " << r << "x" << c << " mesh..." << std::flush;
        auto res = run(r, c);
        results.push_back(res);
        std::cout << " done  LBL=" << fmt_time(res.lbl_time_ms)
                  << "  BDD=" << fmt_time(res.bdd_time_ms) << std::flush;
    }

    print_summary(results);

    std::cout << "\n\nDETAILED RESULTS\n"
              << "================\n";
    for (const auto& r : results)
        print_detailed(r);

    print_scaling_ratios(results);

    bool all_match = true;
    for (const auto& r : results)
        if (!r.match) { all_match = false; break; }

    std::cout << "\nAll results match: " << (all_match ? "YES" : "NO") << "\n";
    return all_match ? 0 : 1;
}
