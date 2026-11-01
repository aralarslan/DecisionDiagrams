#ifndef GRAPHREL_SRC_DECOMP_STATE_HPP
#define GRAPHREL_SRC_DECOMP_STATE_HPP

#include "teddy/graphrel/graph.hpp"
#include "common/union_find.hpp"

#include <cstdint>
#include <string>
#include <vector>

/**
 * @file decomp_state.hpp
 * @brief One state (subgraph) of the layer-by-layer / BDD decomposition.
 */

namespace teddy::graphrel {

/**
 * @brief A decomposition state: which edges remain, how vertices are merged,
 *        and which (representative) vertices carry the terminals.
 */
struct decomp_state {
  /// Remaining (not yet deleted or contracted) edge ids, sorted ascending.
  std::vector<std::size_t> remaining_edges;
  /// Vertices merged by contracted edges.
  union_find uf_;
  /// One representative vertex per terminal component, sorted ascending.
  std::vector<std::size_t> current_K;
  /// False once the state can no longer succeed (e.g. K became empty).
  bool is_valid = true;

  /// Free-list link used by state_pool.
  decomp_state *next_ = nullptr;

  decomp_state() : uf_(0) {}

  /// Initial state: every edge remains, no vertex merged, K = @p K (sorted).
  decomp_state(const graph &net, const std::vector<std::size_t> &K);

  /// Deep copy (free-list link reset).
  decomp_state clone() const;

  /// True if all terminals are already merged into one component.
  bool is_success_terminal() const;

  /// True if the terminals cannot be connected even using every remaining
  /// edge (optionally ignoring @p ignored_edge_id, for coloop detection).
  /// O(V + E): copies the union-find and merges along all remaining edges.
  bool is_failure_terminal(const graph &net,
                           std::size_t ignored_edge_id = SIZE_MAX) const;

  bool operator==(const decomp_state &other) const;

  /// Human-readable dump for debugging.
  std::string to_string(const graph &net) const;
};

} // namespace teddy::graphrel

#endif // GRAPHREL_SRC_DECOMP_STATE_HPP
