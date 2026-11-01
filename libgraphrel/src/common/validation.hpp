#ifndef GRAPHREL_SRC_VALIDATION_HPP
#define GRAPHREL_SRC_VALIDATION_HPP

#include <vector>

#include "teddy/graphrel/graph.hpp"
#include "teddy/graphrel/variable_ordering.hpp"

/**
 * @file validation.hpp
 * @brief Precondition checks shared by the public entry points.
 *
 * The algorithms assume (Wu & Sun 2024 §2.1; Carlier & Lucet 1996 §2;
 * Kawahara et al. 2019 §3.1):
 *   - a well-formed graph with probabilities in [0, 1] (guaranteed by the
 *     graph class),
 *   - a non-empty, sorted, duplicate-free terminal set,
 *   - for the layer-by-layer algorithm, an ordering that lists every vertex and
 *     edge exactly once and places each vertex before its incident edges
 *     ("vertex priority").
 */

namespace teddy::graphrel::detail
{

/// Returns nullptr if @p g can be analysed (it has a terminal set),
/// otherwise a description of the problem.
char const* find_graph_problem (graph const& g);

/// Throws std::invalid_argument("<who>: <problem>") unless @p g can be
/// analysed. Returns @p g so it can be used in constructor initializer lists.
graph const& require_valid (graph const& g, char const* who);

/// Throws std::invalid_argument unless @p ordering is a permutation of all
/// vertex and edge variables of @p g that respects vertex priority.
/// Returns @p ordering so it can be used in constructor initializer lists.
std::vector<variable> const& require_valid_ordering (
    graph const& g,
    std::vector<variable> const& ordering,
    char const* who
);

} // namespace teddy::graphrel::detail

#endif // GRAPHREL_SRC_VALIDATION_HPP
