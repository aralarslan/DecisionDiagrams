// Private implementation header — not installed.
// Include only in: lbl_calculator.cpp, boundary.cpp, lbl_variable_processing.cpp

#ifndef GRAPHREL_SRC_LBL_DETAIL_HPP
#define GRAPHREL_SRC_LBL_DETAIL_HPP

#include "common/boundary.hpp"
#include "lbl/flat_hash_map.hpp"
#include "common/partition.hpp"
#include "common/partition_key.hpp"
#include "common/pruning.hpp"
#include "common/bell_cache.hpp"
#include "common/decomp_state.hpp"
#include "lbl/state_pool.hpp"
#include "teddy/graphrel/variable_ordering.hpp"
#include "teddy/graphrel/graph.hpp"

#include <cstddef>
#include <limits>
#include <ostream>
#include <unordered_map>
#include <vector>

namespace teddy::graphrel {

class lbl_calculator {
public:
    explicit lbl_calculator(const graph& g,
                            std::size_t pool_size = 10000,
                            std::size_t extra_pool_size = 5000);

    lbl_calculator(const graph& g,
                   const std::vector<variable>& ordering,
                   std::size_t pool_size = 10000,
                   std::size_t extra_pool_size = 5000);

    // boundary_ keeps a reference to ordering_, so copies/moves would dangle.
    lbl_calculator(const lbl_calculator&) = delete;
    lbl_calculator& operator=(const lbl_calculator&) = delete;

    double calculate();

    void set_log_stream(std::ostream* out) { log_stream_ = out; }

    /// Drop branches with probability <= @p threshold (0 = exact, the default).
    void set_prune_threshold(double threshold) { prune_threshold_ = threshold; }

    /// Total probability of branches dropped by the last calculate() call.
    double dropped_probability() const { return dropped_probability_; }

    struct statistics {
        std::size_t max_layer_size = 0;
        std::size_t total_decompositions = 0;
        std::size_t max_boundary_size = 0;
        std::size_t num_layers = 0;
        std::size_t max_probe_length = 0;
        double avg_load_factor = 0.0;
        std::size_t total_map_resizes = 0;
    };

    statistics get_statistics() const { return stats_; }

private:
    struct lbl_entry {
        decomp_state* sg = nullptr;
        double probability = 0.0;

        lbl_entry() = default;
        lbl_entry(decomp_state* s, double p) : sg(s), probability(p) {}

        const decomp_state& get_subgraph() const { return *sg; }
        decomp_state& get_subgraph() { return *sg; }
    };

    using layer_map = flat_hash_map<partition_key, lbl_entry>;

    bool keep_branch(double probability);
    void insert_or_merge(decomp_state* sg, double probability,
                         std::size_t next_level);
    void process_variable(const decomp_state& sg, double parent_prob,
                          const variable& var, std::size_t var_idx);
    void prune_unreachable_edges(decomp_state* state);

    // Declaration order matters: boundary_ needs ordering_, cache_ is sized
    // from boundary_.
    const graph& net_;
    std::vector<variable> ordering_;
    boundary_computer boundary_;
    bell_cache cache_;
    state_pool state_pool_;
    layer_map current_layer_;
    layer_map next_layer_;
    mutable statistics stats_;
    std::ostream* log_stream_ = nullptr;
    double early_success_probability_ = 0.0;
    double prune_threshold_ = 0.0;
    double dropped_probability_ = 0.0;
};

// Encode K-membership as a bitmask (max 64 blocks)
inline std::uint64_t encode_k_bitset(const std::vector<bool>& K_membership) {
    std::uint64_t bits = 0;
    for (std::size_t i = 0; i < K_membership.size() && i < 64; ++i) {
        if (K_membership[i])
            bits |= (1ULL << i);
    }
    return bits;
}

} // namespace teddy::graphrel

#endif // GRAPHREL_SRC_LBL_DETAIL_HPP
