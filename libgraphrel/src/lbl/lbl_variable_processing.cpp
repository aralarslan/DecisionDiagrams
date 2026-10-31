/**
 * @file lbl_variable_processing.cpp
 * @brief Per-variable Shannon decomposition — the decision cascade
 *
 * Decision cascade order for each (state, variable):
 *   1. Ghost edge detection     — edge already removed by vertex failure
 *   2. Redundant edge detection — endpoints already connected (Imai Lemma 1)
 *   3. Coloop detection         — bridge; failure = system failure
 *   4. Standard Shannon         — full delete + contract branching
 */

#include "lbl_detail.hpp"
#include "common/graph_operations.hpp"

#include <algorithm>
#include <cassert>

namespace teddy::graphrel {

// A branch is skipped when its probability is exactly zero (a component with
// p = 0 or p = 1). By default nothing else is skipped, so the result is exact
// as in Wu & Sun (2024). With a positive prune threshold, branches at or below
// it are dropped and their probability is accumulated in dropped_probability_,
// which bounds the error: R lies in [result, result + dropped_probability_].
bool lbl_calculator::keep_branch(double probability) {
    if (probability <= 0.0)
        return false;
    if (probability <= prune_threshold_) {
        dropped_probability_ += probability;
        return false;
    }
    return true;
}

void lbl_calculator::process_variable(const decomp_state& sg, double parent_prob,
                                      const variable& var,
                                      std::size_t var_idx) {
    if (!sg.is_valid)
        return;
    if (parent_prob <= 0.0)
        return;

    // ── Early ghost edge check ────────────────────────────────────────────
    if (var.type == variable_type::edge) {
        if (!std::binary_search(sg.remaining_edges.begin(),
                                sg.remaining_edges.end(), var.id)) {
            decomp_state* cloned = state_pool_.create(sg);
            insert_or_merge(cloned, parent_prob, var_idx + 1);
            return;
        }
    }

    // No per-state connectivity check here: states are only inserted when
    // their terminals can still be connected (see calculate()).
    assert(!sg.is_failure_terminal(net_));

    double component_reliability = (var.type == variable_type::edge)
                                   ? net_.edges[var.id].prob
                                   : net_.vertex_probs[var.id];

    // ── Edge variable ─────────────────────────────────────────────────────
    if (var.type == variable_type::edge) {
        std::size_t edge_id = var.id;
        const auto& e = net_.edges[edge_id];

        // Redundant edge: endpoints already connected
        if (sg.uf_.find_root(e.from) == sg.uf_.find_root(e.to)) {
            decomp_state* child = state_pool_.create(sg);
            contract_edge_in_place(*child, edge_id, net_);
            insert_or_merge(child, parent_prob, var_idx + 1);
            return;
        }

        // Coloop: bridge edge — failure disconnects terminals
        if (is_coloop(sg, edge_id, net_)) {
            double success_prob = parent_prob * component_reliability;
            if (keep_branch(success_prob)) {
                decomp_state* child = state_pool_.create(sg);
                contract_edge_in_place(*child, edge_id, net_);
                prune_unreachable_edges(child);
                insert_or_merge(child, success_prob, var_idx + 1);
            }
            return;
        }

        // Standard Shannon: R = p·R(contract) + (1-p)·R(delete)
        // Neither child needs a connectivity check: e is not a coloop, so
        // deleting it keeps the terminals connectable, and contracting an
        // edge never disconnects anything (Imai et al. 1999, Lemma 1).
        double fail_prob = parent_prob * (1.0 - component_reliability);
        if (keep_branch(fail_prob)) {
            decomp_state* child_fail = state_pool_.create(sg);
            delete_edge_in_place(*child_fail, edge_id);
            prune_unreachable_edges(child_fail);
            insert_or_merge(child_fail, fail_prob, var_idx + 1);
        }

        double success_prob = parent_prob * component_reliability;
        if (keep_branch(success_prob)) {
            decomp_state* child_success = state_pool_.create(sg);
            contract_edge_in_place(*child_success, edge_id, net_);
            prune_unreachable_edges(child_success);
            insert_or_merge(child_success, success_prob, var_idx + 1);
        }
        return;
    }

    // ── Vertex variable ────────────────────────────────────────────────────
    std::size_t vertex_id = var.id;
    bool is_terminal = std::binary_search(net_.terminals.begin(),
                                          net_.terminals.end(), vertex_id);

    if (is_terminal) {
        // Terminal must succeed — failure = system failure
        double success_prob = parent_prob * component_reliability;
        if (keep_branch(success_prob)) {
            decomp_state* cloned = state_pool_.create(sg);
            insert_or_merge(cloned, success_prob, var_idx + 1);
        }
    } else {
        // Non-terminal: standard Shannon on vertex
        double fail_prob = parent_prob * (1.0 - component_reliability);
        if (keep_branch(fail_prob)) {
            decomp_state* child_fail = state_pool_.create(sg);
            delete_vertex_in_place(*child_fail, vertex_id, net_);
            if (child_fail->is_valid &&
                !child_fail->is_failure_terminal(net_)) {
                prune_unreachable_edges(child_fail);
                insert_or_merge(child_fail, fail_prob, var_idx + 1);
            } else {
                state_pool_.destroy(child_fail);
            }
        }

        double success_prob = parent_prob * component_reliability;
        if (keep_branch(success_prob)) {
            decomp_state* cloned = state_pool_.create(sg);
            insert_or_merge(cloned, success_prob, var_idx + 1);
        }
    }
}

} // namespace teddy::graphrel
