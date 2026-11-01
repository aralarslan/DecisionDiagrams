#ifndef GRAPHREL_SRC_GRAPH_OPERATIONS_HPP
#define GRAPHREL_SRC_GRAPH_OPERATIONS_HPP

#include "common/decomp_state.hpp"
#include "teddy/graphrel/graph.hpp"

/**
 * @file graph_operations.hpp
 * @brief Edge deletion/contraction and vertex deletion on decomposition states,
 *        plus loop/coloop detection (Imai et al. 1999, Lemma 1).
 */

namespace teddy::graphrel {

// ── In-place operations (used by LBL and BDD) ───────────────────────────────

/// Edge fails: remove it from the remaining edges. No connectivity check.
void delete_edge_in_place(decomp_state& sg, std::size_t edge_id);

/// Edge works: merge its endpoints and update the terminal representatives.
void contract_edge_in_place(decomp_state& sg, std::size_t edge_id,
                            const graph& net);

/// Vertex fails: remove it from K and drop its incident edges. Marks the
/// state invalid if K becomes empty. No connectivity check.
void delete_vertex_in_place(decomp_state& sg, std::size_t vertex_id,
                            const graph& net);

// ── Queries ─────────────────────────────────────────────────────────────────

/// True if deleting the (remaining) edge would disconnect the terminals.
bool is_coloop(const decomp_state& sg, std::size_t edge_id, const graph& net);

/// True if the edge's endpoints are already merged.
bool is_loop(const decomp_state& sg, std::size_t edge_id, const graph& net);

// ── Copying wrappers (tests and experiments) ────────────────────────────────

/// Copy + delete_edge_in_place; marks the result invalid if the terminals
/// can no longer be connected.
decomp_state delete_edge(const decomp_state& sg, std::size_t edge_id,
                         const graph& net);

/// Copy + contract_edge_in_place.
decomp_state contract_edge(const decomp_state& sg, std::size_t edge_id,
                           const graph& net);

/// Copy + delete_vertex_in_place; marks the result invalid if K becomes
/// empty or the terminals can no longer be connected.
decomp_state delete_vertex(const decomp_state& sg, std::size_t vertex_id,
                           const graph& net);

} // namespace teddy::graphrel

#endif // GRAPHREL_SRC_GRAPH_OPERATIONS_HPP
