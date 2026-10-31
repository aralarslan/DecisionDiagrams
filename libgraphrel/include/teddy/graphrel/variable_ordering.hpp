#ifndef TEDDY_GRAPHREL_VARIABLE_ORDERING_HPP
#define TEDDY_GRAPHREL_VARIABLE_ORDERING_HPP

#include "teddy/graphrel/graph.hpp"

#include <cstddef>
#include <vector>

/**
 * @file variable_ordering.hpp
 * @brief Variable ordering for the layer-by-layer decomposition (Wu & Sun 2024)
 *
 * Declares the variable/ordering types and the BFS/DFS ordering generators.
 * Include this header when you need a custom ordering for calculate_reliability().
 */

namespace teddy::graphrel {

/**
 * @brief Type of variable in the decomposition ordering
 */
enum class variable_type {
    vertex, ///< Vertex variable (vertex reliability)
    edge    ///< Edge variable (edge reliability)
};

/**
 * @brief A single variable in the decomposition ordering
 */
struct variable {
    variable_type type;
    std::size_t   id;
    variable(variable_type t, std::size_t i) : type(t), id(i) {}
};

/**
 * @brief Variable ordering strategy
 */
enum class ordering_strategy {
    /// Breadth-first from the first terminal, as in Wu & Sun (2024), Fig. 4.
    bfs,
    /// Depth-first from the first terminal. Correct, but can produce far more
    /// states per layer than the other strategies; mostly for experiments.
    dfs,
    /// Greedy edge-cut minimisation: picks, among BFS and several greedy
    /// sweeps from the first terminal, the order whose largest edge cut is
    /// smallest. Usually far fewer states per layer than BFS (e.g. 10x10 mesh
    /// and random geometric graphs). Default for calculate_reliability() and
    /// build_diagram().
    cutwidth
};

/**
 * @brief Generate a BFS variable ordering (Wu & Sun default).
 *
 * Kept as BFS for compatibility. calculate_reliability() and build_diagram()
 * use ordering_strategy::cutwidth by default.
 *
 * @param g Graph with terminals set.
 * @return Ordered list of variables (vertices before incident edges).
 */
std::vector<variable> generate_variable_ordering(const graph& g);

/**
 * @brief Generate a variable ordering using the specified strategy.
 *
 * @param g        Graph with terminals set.
 * @param strategy bfs or dfs.
 * @return Ordered list of variables.
 */
std::vector<variable> generate_variable_ordering(const graph& g,
                                                  ordering_strategy strategy);

/**
 * @brief Estimate the maximum boundary set size for a given ordering.
 *
 * @param g        Graph structure.
 * @param ordering Variable ordering to evaluate.
 * @return Maximum boundary set size F_max.
 */
std::size_t estimate_max_boundary_size(const graph& g,
                                        const std::vector<variable>& ordering);

} // namespace teddy::graphrel

#endif // TEDDY_GRAPHREL_VARIABLE_ORDERING_HPP
