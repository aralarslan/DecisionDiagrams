/**
 * @file graph_operations.cpp
 * @brief Edge/vertex operations for the decomposition (Shannon expansion).
 *
 * The in-place operations are the implementation. The copying variants
 * (delete_edge, contract_edge, delete_vertex) are thin wrappers kept for
 * tests and experiments; they also flag states whose terminals can no longer
 * be connected.
 */

#include "common/graph_operations.hpp"

#include <algorithm>

namespace teddy::graphrel {

namespace {

/// Remove @p x from a sorted vector, if present.
void erase_sorted(std::vector<std::size_t>& values, std::size_t x) {
    auto it = std::lower_bound(values.begin(), values.end(), x);
    if (it != values.end() && *it == x)
        values.erase(it);
}

/// Insert @p x into a sorted vector, unless already present.
void insert_sorted(std::vector<std::size_t>& values, std::size_t x) {
    auto it = std::lower_bound(values.begin(), values.end(), x);
    if (it == values.end() || *it != x)
        values.insert(it, x);
}

bool contains_sorted(const std::vector<std::size_t>& values, std::size_t x) {
    return std::binary_search(values.begin(), values.end(), x);
}

} // namespace

// ── In-place operations (used by LBL and BDD) ───────────────────────────────

void delete_edge_in_place(decomp_state& sg, std::size_t edge_id) {
    erase_sorted(sg.remaining_edges, edge_id);
}

void contract_edge_in_place(decomp_state& sg, std::size_t edge_id,
                            const graph& net) {
    const edge& e = net.edges[edge_id];
    const std::size_t u = e.from;
    const std::size_t v = e.to;

    sg.uf_.unite(u, v);
    erase_sorted(sg.remaining_edges, edge_id);

    // current_K holds one representative per terminal component (sorted).
    // After merging u and v, replace their entries by the new root.
    const bool u_in_K = contains_sorted(sg.current_K, u);
    const bool v_in_K = contains_sorted(sg.current_K, v);
    if (!u_in_K && !v_in_K)
        return;

    const std::size_t rep = sg.uf_.find(u);
    if (u_in_K && v_in_K) {
        erase_sorted(sg.current_K, u);
        erase_sorted(sg.current_K, v);
        insert_sorted(sg.current_K, rep);
    } else if (v_in_K) {
        erase_sorted(sg.current_K, v);
        insert_sorted(sg.current_K, rep);
    } else if (rep != u) {
        erase_sorted(sg.current_K, u);
        insert_sorted(sg.current_K, rep);
    }
}

void delete_vertex_in_place(decomp_state& sg, std::size_t vertex_id,
                            const graph& net) {
    erase_sorted(sg.current_K, vertex_id);
    for (std::size_t eid : net.adj[vertex_id])
        erase_sorted(sg.remaining_edges, eid);
    if (sg.current_K.empty())
        sg.is_valid = false;
}

// ── Queries ─────────────────────────────────────────────────────────────────

bool is_coloop(const decomp_state& sg, std::size_t edge_id, const graph& net) {
    const edge& e = net.edges[edge_id];
    if (sg.uf_.connected_const(e.from, e.to))
        return false; // a loop, not a coloop
    return contains_sorted(sg.remaining_edges, edge_id) &&
           sg.is_failure_terminal(net, edge_id);
}

bool is_loop(const decomp_state& sg, std::size_t edge_id, const graph& net) {
    const edge& e = net.edges[edge_id];
    return sg.uf_.connected_const(e.from, e.to);
}

// ── Copying wrappers ────────────────────────────────────────────────────────

decomp_state delete_edge(const decomp_state& sg, std::size_t edge_id,
                         const graph& net) {
    decomp_state result = sg.clone();
    delete_edge_in_place(result, edge_id);
    if (result.is_failure_terminal(net))
        result.is_valid = false;
    return result;
}

decomp_state contract_edge(const decomp_state& sg, std::size_t edge_id,
                           const graph& net) {
    decomp_state result = sg.clone();
    contract_edge_in_place(result, edge_id, net);
    return result;
}

decomp_state delete_vertex(const decomp_state& sg, std::size_t vertex_id,
                           const graph& net) {
    decomp_state result = sg.clone();
    delete_vertex_in_place(result, vertex_id, net);
    if (result.is_valid && result.is_failure_terminal(net))
        result.is_valid = false;
    return result;
}

} // namespace teddy::graphrel
