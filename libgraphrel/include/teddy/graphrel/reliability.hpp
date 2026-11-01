#ifndef TEDDY_GRAPHREL_RELIABILITY_HPP
#define TEDDY_GRAPHREL_RELIABILITY_HPP

#include <cstddef>
#include <vector>

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
 * @brief Settings of analyze_reliability(). The defaults give the exact
 *        reliability, serially, with the cut-width ordering.
 */
struct reliability_options
{
    /// Variable ordering. Empty (the default): generate_variable_ordering(g),
    /// i.e. cut-width. Use generate_variable_ordering(g,
    /// ordering_strategy::bfs) to reproduce Wu & Sun's setup exactly (same
    /// result, usually more states per layer).
    std::vector<variable> ordering;

    /// Threads used to expand large layers: 1 (the default) runs serially,
    /// 0 uses std::thread::hardware_concurrency(). The result is
    /// bit-identical for every thread count.
    unsigned threads = 1;

    /// In [0, 1). 0 (the default) is exact. Otherwise every branch whose
    /// probability is at most this is dropped, and the result is an interval
    /// [reliability, upper_bound] guaranteed to contain the exact value. On
    /// dense graphs most states carry negligible probability: the complete
    /// graph K12 with p = 0.9 runs about 40x faster with 1e-15 (0.2 s
    /// instead of 9 s, serially), with upper_bound - reliability = 4e-11.
    double prune_threshold = 0.0;
};

/**
 * @brief Size of the computation.
 */
struct reliability_statistics
{
    std::size_t num_layers     = 0; ///< variables, |V| + |E|
    std::size_t max_front_size = 0; ///< largest boundary set
    std::size_t max_layer_size = 0; ///< most states in one layer

    /// Most states in one layer that the layer's variable splits into two
    /// children (it is neither forced nor irrelevant to them). This is Wu &
    /// Sun's G^max (B1 in their Table 2).
    std::size_t max_branching_layer_size = 0;

    std::size_t total_states             = 0; ///< states summed over all layers
    std::size_t peak_memory_bytes = 0; ///< two live layers and scratch space
};

struct reliability_result
{
    /// The reliability; a lower bound when pruning dropped something.
    double reliability = 0.0;
    /// reliability + the probability of the pruned branches (capped at 1).
    double upper_bound = 0.0;
    reliability_statistics statistics;

    /// True if nothing was pruned: reliability is exact.
    bool exact () const
    {
        return upper_bound == reliability;
    }
};

/**
 * @brief Exact network reliability with the layer-by-layer algorithm.
 *
 * Same as analyze_reliability(g).reliability.
 *
 * @param g Graph with edge/vertex reliabilities and terminal set.
 * @return Probability that all terminals work and are connected.
 * @throws std::invalid_argument if @p g has no terminal set
 */
double calculate_reliability (graph const& g);

/**
 * @brief Layer-by-layer reliability with options, bounds and statistics.
 *
 * @param g       Graph with edge/vertex reliabilities and terminal set.
 * @param options Ordering, threads and pruning (see reliability_options).
 * @throws std::invalid_argument if @p g has no terminal set, the ordering
 *         is not a permutation of all variables with every vertex before its
 *         edges, or prune_threshold is not in [0, 1)
 * @throws std::length_error if the ordering has a boundary of more than 64
 *         vertices
 */
reliability_result analyze_reliability (
    graph const& g,
    reliability_options const& options = {}
);

} // namespace teddy::graphrel

#endif // TEDDY_GRAPHREL_RELIABILITY_HPP
