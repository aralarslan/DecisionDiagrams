/**
 * @file reliability_diagram.cpp
 * @brief BS-BDD builder using composition (Wu & Sun 2024)
 *
 * build_diagram_impl() builds the BDD recursively, holding a bss_manager& ref
 * and calling mgr.make_ite() / mgr.constant() instead of inheriting.
 *
 * IMPORTANT: The boundary_hash XORs var_idx * 0x9E3779B97F4A7C15ULL for
 * level pinning. This is critical for DFS+memoization (unlike LBL where level
 * is implicit in the ping-pong layer). Do not remove.
 */

#include "teddy/graphrel/diagram.hpp"
#include "common/boundary.hpp"
#include "common/graph_operations.hpp"
#include "common/partition.hpp"
#include "common/partition_key.hpp"
#include "common/pruning.hpp"
#include "common/validation.hpp"
#include "common/bell_cache.hpp"
#include "common/decomp_state.hpp"
#include "teddy/graphrel/variable_ordering.hpp"

#include <algorithm>
#include <cassert>
#include <set>
#include <unordered_map>
#include <vector>

namespace teddy::graphrel {

// ── reliability_diagram implementation ────────────────────────────────────────────────

reliability_diagram::reliability_diagram(teddy::bss_manager mgr,
                     teddy::bss_manager::diagram_t diag,
                     std::vector<double> edge_probs,
                     std::vector<teddy::int32> edge_to_var,
                     std::vector<teddy::int32> vertex_to_var)
    : manager(std::move(mgr))
    , diagram(std::move(diag))
    , probs(std::move(edge_probs))
    , edge_to_var_(std::move(edge_to_var))
    , vertex_to_var_(std::move(vertex_to_var))
{}

teddy::int32 reliability_diagram::edge_var(std::size_t edge_id) const {
    return edge_to_var_[edge_id];
}

teddy::int32 reliability_diagram::vertex_var(std::size_t vertex_id) const {
    return vertex_to_var_[vertex_id];
}

teddy::var_change reliability_diagram::edge_change(std::size_t edge_id,
                                          teddy::int32 from,
                                          teddy::int32 to) const {
    return teddy::var_change{edge_to_var_[edge_id], from, to};
}

// ── Internal BDD builder ─────────────────────────────────────────────────────

namespace {

using bdd_t = teddy::bss_manager::diagram_t;

static bool has_imperfect_vertices(const graph& g) {
    for (double rel : g.vertex_probs) {
        if (rel < 1.0)
            return true;
    }
    return false;
}

static teddy::int32 count_bdd_variables(const std::vector<variable>& ordering,
                                        bool has_imperfect) {
    teddy::int32 count = 0;
    for (const auto& var : ordering) {
        if (var.type == variable_type::edge)
            ++count;
        else if (var.type == variable_type::vertex && has_imperfect)
            ++count;
    }
    return count;
}

class diagram_builder {
public:
    diagram_builder(const graph& g,
                const std::vector<variable>& ordering,
                teddy::bss_manager& mgr)
        : net_(g)
        , ordering_(ordering)
        , mgr_(mgr)
        , edge_to_var_(g.num_edges, -1)
        , vertex_to_var_(g.num_vertices, -1)
        , boundary_(g, ordering_)
        , cache_(bell_cache::size_for_boundary(boundary_.max_static_boundary()))
    {
        init_variable_index_mappings();
    }

    bdd_t build() {
        // |K| = 1: the function is just "that vertex works". Handled up front
        // because build_recursive treats |K| < 2 as already connected.
        if (net_.terminals.size() == 1) {
            const teddy::int32 var = vertex_to_var_[net_.terminals.front()];
            return var >= 0 ? mgr_.make_ite(var, mgr_.constant(1), mgr_.constant(0))
                            : mgr_.constant(1);
        }
        decomp_state initial(net_, net_.terminals);
        // The only connectivity check on entry; see build_recursive().
        if (initial.is_failure_terminal(net_))
            return mgr_.constant(0);
        return build_recursive(std::move(initial), 0);
    }

    const std::vector<teddy::int32>& edge_to_var() const { return edge_to_var_; }
    const std::vector<teddy::int32>& vertex_to_var() const { return vertex_to_var_; }

private:
    void init_variable_index_mappings() {
        bool has_imperfect = has_imperfect_vertices(net_);
        teddy::int32 idx = 0;
        for (const auto& var : ordering_) {
            if (var.type == variable_type::edge) {
                edge_to_var_[var.id] = idx++;
            } else if (var.type == variable_type::vertex && has_imperfect) {
                vertex_to_var_[var.id] = idx++;
            }
        }
    }

    partition_key make_key(const decomp_state& sg, std::size_t var_idx) {
        boundary_partition part = boundary_.compute(sg, var_idx, cache_);
        // XOR var_idx into boundary_hash for level pinning (required for DFS)
        return partition_key{
            part.canonical_number,
            [&]() -> std::uint64_t {
                std::uint64_t bits = 0;
                for (std::size_t i = 0; i < part.K_membership.size() && i < 64; ++i)
                    if (part.K_membership[i]) bits |= (1ULL << i);
                return bits;
            }(),
            compute_boundary_hash(part.boundary_vertices)
                ^ (var_idx * 0x9E3779B97F4A7C15ULL)
        };
    }

    bdd_t build_recursive(decomp_state state, std::size_t var_idx) {
        if (var_idx >= ordering_.size()) {
            return state.is_success_terminal()
                ? mgr_.constant(1) : mgr_.constant(0);
        }

        if (state.is_success_terminal())
            return mgr_.constant(1);

        // States reach here only if their terminals can still be connected:
        // vertex deletions are checked where they happen, deleting a
        // non-coloop edge cannot disconnect, contraction never does.
        assert(!state.is_failure_terminal(net_));

        partition_key key = make_key(state, var_idx);
        auto cache_it = partition_cache_.find(key);
        if (cache_it != partition_cache_.end()) {
            return cache_it->second;
        }

        const variable& var = ordering_[var_idx];
        bdd_t result;

        if (var.type == variable_type::edge) {
            result = process_edge_variable(std::move(state), var.id, var_idx);
        } else {
            if (vertex_to_var_[var.id] >= 0) {
                result = process_vertex_variable(std::move(state), var.id,
                                                  var_idx);
            } else {
                // Perfect vertex — skip
                result = build_recursive(std::move(state), var_idx + 1);
            }
        }

        partition_cache_[key] = result;
        return result;
    }

    bdd_t process_edge_variable(decomp_state state, std::size_t edge_id,
                                 std::size_t var_idx) {
        // Ghost edge: already removed by vertex failure. Must come before the
        // perfect-edge shortcut: an edge incident to a failed vertex is
        // unusable even if p(e) = 1 (Kawahara et al. 2019, §3.1).
        if (!std::binary_search(state.remaining_edges.begin(),
                                state.remaining_edges.end(), edge_id)) {
            return build_recursive(std::move(state), var_idx + 1);
        }

        // Perfect edge: always succeeds, no BDD node
        if (net_.edges[edge_id].prob >= 1.0) {
            contract_edge_in_place(state, edge_id, net_);
            return build_recursive(std::move(state), var_idx + 1);
        }

        const auto& e = net_.edges[edge_id];

        // Redundant edge: endpoints already connected
        if (state.uf_.find_root(e.from) ==
            state.uf_.find_root(e.to)) {
            contract_edge_in_place(state, edge_id, net_);
            return build_recursive(std::move(state), var_idx + 1);
        }

        // Coloop: bridge — make_ite with false low
        if (is_coloop(state, edge_id, net_)) {
            contract_edge_in_place(state, edge_id, net_);
            prune_unreachable_edges(state, net_);
            bdd_t high = build_recursive(std::move(state), var_idx + 1);
            return mgr_.make_ite(edge_to_var_[edge_id], high, mgr_.constant(0));
        }

        // Standard Shannon: 1 clone for failure branch
        decomp_state fail_state = state;
        contract_edge_in_place(state, edge_id, net_);
        prune_unreachable_edges(state, net_);
        bdd_t high = build_recursive(std::move(state), var_idx + 1);

        // e is not a coloop, so deleting it keeps the terminals connectable.
        delete_edge_in_place(fail_state, edge_id);
        prune_unreachable_edges(fail_state, net_);
        bdd_t low = build_recursive(std::move(fail_state), var_idx + 1);

        return mgr_.make_ite(edge_to_var_[edge_id], high, low);
    }

    bdd_t process_vertex_variable(decomp_state state, std::size_t vertex_id,
                                   std::size_t var_idx) {
        teddy::int32 tidx = vertex_to_var_[vertex_id];
        bool is_terminal = std::binary_search(net_.terminals.begin(),
                                              net_.terminals.end(), vertex_id);

        if (is_terminal) {
            bdd_t high = build_recursive(std::move(state), var_idx + 1);
            return mgr_.make_ite(tidx, high, mgr_.constant(0));
        }

        // Non-terminal: Shannon on vertex
        decomp_state fail_state = state;
        bdd_t high = build_recursive(std::move(state), var_idx + 1);

        delete_vertex_in_place(fail_state, vertex_id, net_);
        prune_unreachable_edges(fail_state, net_);
        bdd_t low;
        if (fail_state.is_valid && !fail_state.is_failure_terminal(net_)) {
            low = build_recursive(std::move(fail_state), var_idx + 1);
        } else {
            low = mgr_.constant(0);
        }

        return mgr_.make_ite(tidx, high, low);
    }

    // Declaration order matters: boundary_ needs ordering_, cache_ is sized
    // from boundary_.
    const graph& net_;
    const std::vector<variable>& ordering_;
    teddy::bss_manager& mgr_;
    std::vector<teddy::int32> edge_to_var_;
    std::vector<teddy::int32> vertex_to_var_;
    boundary_computer boundary_;
    bell_cache cache_;
    std::unordered_map<partition_key, bdd_t> partition_cache_;
};

} // anonymous namespace

// ── Public build_diagram ─────────────────────────────────────────────────────────

reliability_diagram build_diagram(const graph& g, teddy::int64 pool_size) {
    detail::require_valid(g, "build_diagram");
    auto ordering = generate_variable_ordering(g, ordering_strategy::cutwidth);
    bool has_imperfect = has_imperfect_vertices(g);
    teddy::int32 var_count = count_bdd_variables(ordering, has_imperfect);

    teddy::bss_manager mgr(var_count, pool_size);
    diagram_builder builder(g, ordering, mgr);
    bdd_t diagram = builder.build();

    // Build probs vector indexed by TeDDy variable
    std::vector<double> probs(static_cast<std::size_t>(var_count), 0.0);
    const auto& e2v = builder.edge_to_var();
    const auto& v2v = builder.vertex_to_var();

    for (std::size_t edge_id = 0; edge_id < g.num_edges; ++edge_id) {
        teddy::int32 idx = e2v[edge_id];
        if (idx >= 0)
            probs[static_cast<std::size_t>(idx)] = g.edges[edge_id].prob;
    }
    if (has_imperfect) {
        for (std::size_t v = 0; v < g.num_vertices; ++v) {
            teddy::int32 idx = v2v[v];
            if (idx >= 0)
                probs[static_cast<std::size_t>(idx)] = g.vertex_probs[v];
        }
    }

    return reliability_diagram(std::move(mgr), std::move(diagram), std::move(probs),
                     std::vector<teddy::int32>(e2v),
                     std::vector<teddy::int32>(v2v));
}

} // namespace teddy::graphrel
