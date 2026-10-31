/**
 * @file lbl_calculator.cpp
 * @brief Layer-by-layer reliability orchestrator (Wu & Sun 2024)
 *
 * Owns the calculate() loop skeleton and insert_or_merge().
 * Per-variable decision logic lives in lbl_variable_processing.cpp.
 * Boundary identification lives in boundary.cpp.
 */

#include "lbl_detail.hpp"
#include "teddy/graphrel/reliability.hpp"
#include "common/graph_operations.hpp"
#include "common/validation.hpp"

#include <algorithm>
#include <stdexcept>

namespace teddy::graphrel {

// ── Constructors ────────────────────────────────────────────────────────────

namespace {
constexpr const char* who = "calculate_reliability";
}

lbl_calculator::lbl_calculator(const graph& g,
                               std::size_t pool_size,
                               std::size_t extra_pool_size)
    : lbl_calculator(g,
                     generate_variable_ordering(detail::require_valid(g, who)),
                     pool_size, extra_pool_size)
{}

// The graph and ordering are validated in the initializer list, before any
// member that indexes into them (cache_, boundary_) is constructed.
lbl_calculator::lbl_calculator(const graph& g,
                               const std::vector<variable>& ordering,
                               std::size_t pool_size,
                               std::size_t extra_pool_size)
    : net_(detail::require_valid(g, who))
    , cache_(g.num_vertices + g.num_edges)
    , ordering_(detail::require_valid_ordering(g, ordering, who))
    , boundary_(g, ordering_)
    , state_pool_(pool_size, extra_pool_size)
{
    stats_ = statistics{};
}

// ── Main calculation loop ────────────────────────────────────────────────────

double lbl_calculator::calculate() {
    stats_ = statistics{};
    early_success_probability_ = 0.0;
    dropped_probability_ = 0.0;
    double total_load_factor = 0.0;

    // |K| = 1: the terminal set is connected iff that vertex works. Handled
    // up front because the layer loop treats |K| < 2 as already connected.
    if (net_.terminals.size() == 1) {
        return net_.vertex_probs[net_.terminals.front()];
    }

    // Layer 0: original network, probability 1.0
    decomp_state* initial = state_pool_.create(net_, net_.terminals);

    boundary_partition initial_part = boundary_.compute(*initial, 0, cache_);
    partition_key initial_key{
        initial_part.canonical_number,
        encode_k_bitset(initial_part.K_membership),
        compute_boundary_hash(initial_part.boundary_vertices)
    };

    current_layer_.clear();
    current_layer_.emplace(initial_key, lbl_entry{initial, 1.0});

    // Process one variable per iteration
    for (std::size_t var_idx = 0; var_idx < ordering_.size(); ++var_idx) {
        next_layer_.clear();

        stats_.total_decompositions += current_layer_.size();
        stats_.max_layer_size = std::max(stats_.max_layer_size,
                                         current_layer_.size());
        total_load_factor += current_layer_.load_factor();

        for (auto& entry : current_layer_) {
            lbl_entry& e = entry.value;
            process_variable(*e.sg, e.probability,
                             ordering_[var_idx], var_idx);
        }

        // Recycle current layer states
        for (auto& entry : current_layer_) {
            state_pool_.destroy(entry.value.sg);
        }

        current_layer_.swap(next_layer_);
    }

    // Aggregation
    stats_.num_layers       = ordering_.size();
    stats_.avg_load_factor  = ordering_.empty()
                                ? 0.0
                                : total_load_factor / ordering_.size();
    stats_.max_probe_length = std::max(current_layer_.get_max_probe_length(),
                                       next_layer_.get_max_probe_length());
    stats_.total_map_resizes = current_layer_.get_resize_count()
                             + next_layer_.get_resize_count();
    stats_.max_layer_size = std::max(stats_.max_layer_size,
                                     current_layer_.size());

    double final_reliability = early_success_probability_;
    for (auto& entry : current_layer_) {
        lbl_entry& e = entry.value;
        if (e.get_subgraph().is_success_terminal()) {
            final_reliability += e.probability;
        }
        state_pool_.destroy(e.sg);
    }

    return final_reliability;
}

// ── insert_or_merge ─────────────────────────────────────────────────────────

void lbl_calculator::insert_or_merge(decomp_state* sg, double probability,
                                     std::size_t next_level) {
    // Early success terminal: accumulate and discard
    if (sg->is_success_terminal()) {
        early_success_probability_ += probability;
        state_pool_.destroy(sg);
        return;
    }

    boundary_partition part = boundary_.compute(*sg, next_level, cache_);
    stats_.max_boundary_size = std::max(stats_.max_boundary_size,
                                        part.boundary_vertices.size());

    partition_key key{
        part.canonical_number,
        encode_k_bitset(part.K_membership),
        compute_boundary_hash(part.boundary_vertices)
    };

    lbl_entry* existing = next_layer_.find(key);
    if (existing != nullptr) {
        state_pool_.destroy(sg);
        existing->probability += probability;
    } else {
        next_layer_.emplace(key, lbl_entry{sg, probability});
    }
}

// ── prune_unreachable_edges wrapper ─────────────────────────────────────────

void lbl_calculator::prune_unreachable_edges(decomp_state* state) {
    teddy::graphrel::prune_unreachable_edges(*state, net_);
}

// ── Public free function ─────────────────────────────────────────────────────

double calculate_reliability(const graph& g) {
    lbl_calculator calc(g);
    return calc.calculate();
}

double calculate_reliability(const graph& g,
                             const std::vector<variable>& ordering) {
    lbl_calculator calc(g, ordering);
    return calc.calculate();
}

namespace {
reliability_bounds bounds_from(lbl_calculator& calc, double prune_threshold) {
    calc.set_prune_threshold(prune_threshold);
    const double lower = calc.calculate();
    return {lower, std::min(1.0, lower + calc.dropped_probability())};
}

void check_threshold(double prune_threshold) {
    if (!(prune_threshold >= 0.0 && prune_threshold < 1.0))
        throw std::invalid_argument(
            "calculate_reliability_bounds: prune_threshold must be in [0, 1)");
}
} // namespace

reliability_bounds calculate_reliability_bounds(const graph& g,
                                                double prune_threshold) {
    check_threshold(prune_threshold);
    lbl_calculator calc(g);
    return bounds_from(calc, prune_threshold);
}

reliability_bounds calculate_reliability_bounds(const graph& g,
                                                const std::vector<variable>& ordering,
                                                double prune_threshold) {
    check_threshold(prune_threshold);
    lbl_calculator calc(g, ordering);
    return bounds_from(calc, prune_threshold);
}

} // namespace teddy::graphrel
