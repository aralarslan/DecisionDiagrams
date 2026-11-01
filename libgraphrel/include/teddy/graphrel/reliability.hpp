#ifndef TEDDY_GRAPHREL_RELIABILITY_HPP
#define TEDDY_GRAPHREL_RELIABILITY_HPP

#include "teddy/graphrel/graph.hpp"
#include "teddy/graphrel/variable_ordering.hpp"

/**
 * @file reliability.hpp
 * @brief Layer-by-layer network reliability (Wu & Sun 2024)
 *
 * Computes reliability directly as a probability (no BDD allocated).
 * Use build_diagram() instead when DPLD / importance / MCS analysis is needed.
 */

namespace teddy::graphrel
{

/**
 * @brief Calculate network reliability using the layer-by-layer algorithm.
 *
 * Exact. Uses ordering_strategy::cutwidth; pass an ordering from
 * generate_variable_ordering(g, ordering_strategy::bfs) to reproduce
 * Wu & Sun's setup exactly (same result, usually more states per layer).
 *
 * @param g       Graph with edge/vertex reliabilities and terminal set.
 * @param threads Threads used to expand large layers: 1 (the default) runs
 *                serially, 0 uses std::thread::hardware_concurrency(). The
 *                result is bit-identical for every thread count.
 * @return Probability that all terminals are connected.
 */
double calculate_reliability (graph const& g, unsigned threads = 1);

/**
 * @brief Calculate network reliability with a custom variable ordering.
 *
 * @param g        Graph with edge/vertex reliabilities and terminal set.
 * @param ordering Variable ordering (e.g. from generate_variable_ordering).
 * @param threads  As for calculate_reliability(const graph&, unsigned).
 * @return Probability that all terminals are connected.
 */
double calculate_reliability (
    graph const& g,
    std::vector<variable> const& ordering,
    unsigned threads = 1
);

/**
 * @brief Interval guaranteed to contain the network reliability.
 */
struct reliability_bounds
{
    double lower; ///< probability of the explored states that connect K
    double upper; ///< lower + total probability of the pruned branches
};

/**
 * @brief Faster, approximate layer-by-layer reliability with a rigorous bound.
 *
 * calculate_reliability() is exact. On dense graphs, most of its states carry
 * negligible probability; this variant drops every branch whose probability is
 * at most @p prune_threshold and returns an interval that contains the exact
 * reliability. For example, the complete graph K12 with p = 0.9 runs about 40x
 * faster with prune_threshold = 1e-15 (0.2 s instead of 9 s, serially), with
 * upper - lower = 4e-11.
 *
 * @param prune_threshold In [0, 1). 0 gives lower == upper == exact result.
 * @param threads As for calculate_reliability(const graph&, unsigned).
 * @throws std::invalid_argument for an invalid graph, ordering or threshold.
 */
reliability_bounds calculate_reliability_bounds (
    graph const& g,
    double prune_threshold,
    unsigned threads = 1
);

reliability_bounds calculate_reliability_bounds (
    graph const& g,
    std::vector<variable> const& ordering,
    double prune_threshold,
    unsigned threads = 1
);

} // namespace teddy::graphrel

#endif // TEDDY_GRAPHREL_RELIABILITY_HPP
