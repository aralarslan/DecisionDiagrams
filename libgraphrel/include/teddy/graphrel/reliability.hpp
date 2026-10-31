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

namespace teddy::graphrel {

/**
 * @brief Calculate network reliability using the layer-by-layer algorithm.
 *
 * @param g Graph with edge/vertex reliabilities and terminal set.
 * @return Probability that all terminals are connected.
 */
double calculate_reliability(const graph& g);

/**
 * @brief Calculate network reliability with a custom variable ordering.
 *
 * @param g        Graph with edge/vertex reliabilities and terminal set.
 * @param ordering Variable ordering (e.g. from generate_variable_ordering).
 * @return Probability that all terminals are connected.
 */
double calculate_reliability(const graph& g,
                             const std::vector<variable>& ordering);

/**
 * @brief Interval guaranteed to contain the network reliability.
 */
struct reliability_bounds {
    double lower; ///< probability of the explored states that connect K
    double upper; ///< lower + total probability of the pruned branches
};

/**
 * @brief Faster, approximate layer-by-layer reliability with a rigorous bound.
 *
 * calculate_reliability() is exact. On dense graphs, most of its states carry
 * negligible probability; this variant drops every branch whose probability is
 * at most @p prune_threshold and returns an interval that contains the exact
 * reliability. For example, the complete graph K12 with p = 0.9 runs about 35x
 * faster with prune_threshold = 1e-15, with upper - lower far below 1e-9.
 *
 * @param prune_threshold In [0, 1). 0 gives lower == upper == exact result.
 * @throws std::invalid_argument for an invalid graph, ordering or threshold.
 */
reliability_bounds calculate_reliability_bounds(const graph& g,
                                                double prune_threshold);

reliability_bounds calculate_reliability_bounds(const graph& g,
                                                const std::vector<variable>& ordering,
                                                double prune_threshold);

} // namespace teddy::graphrel

#endif // TEDDY_GRAPHREL_RELIABILITY_HPP
