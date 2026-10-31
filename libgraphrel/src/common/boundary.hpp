#ifndef GRAPHREL_SRC_BOUNDARY_HPP
#define GRAPHREL_SRC_BOUNDARY_HPP

#include "common/partition.hpp"
#include "common/bell_cache.hpp"
#include "common/decomp_state.hpp"
#include "teddy/graphrel/variable_ordering.hpp"
#include "teddy/graphrel/graph.hpp"

#include <cstddef>
#include <vector>

/**
 * @file boundary.hpp
 * @brief Boundary-set partition computation shared by LBL and BDD.
 */

namespace teddy::graphrel {

/**
 * @class boundary_computer
 * @brief Identifies a state's boundary vertices and canonicalises their partition.
 *
 * At level l (before processing ordering[l]), a vertex belongs to a state's
 * boundary set when it has been reached (its vertex variable or one of its
 * edges is among the first l variables) and it still has an unprocessed
 * remaining edge in that state. This is the elimination front of Imai et al.
 * (1999), restricted to the edges the state still has.
 *
 * Construction is O(V + E). Thread-safety: compute() uses thread_local
 * scratch buffers — safe from different threads, not re-entrant in one.
 */
class boundary_computer {
public:
    boundary_computer(const graph& g, const std::vector<variable>& ordering);

    boundary_partition compute(const decomp_state& sg, std::size_t level,
                               const bell_cache& cache) const;

    /// Largest boundary any state can have at any level (the static front).
    std::size_t max_static_boundary() const { return max_static_boundary_; }

private:
    const graph& net_;
    /// Position of each edge in the ordering (not_in_ordering if absent).
    std::vector<std::size_t> edge_level_;
    /// First level at which each vertex is reached (vertex variable or edge).
    std::vector<std::size_t> vertex_first_level_;
    std::size_t max_static_boundary_ = 0;
};

} // namespace teddy::graphrel

#endif // GRAPHREL_SRC_BOUNDARY_HPP
