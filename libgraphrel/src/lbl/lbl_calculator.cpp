/**
 * @file lbl_calculator.cpp
 * @brief Layer-by-layer reliability (Wu & Sun 2024) on frontier-local states.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "common/thread_team.hpp"
#include "common/validation.hpp"
#include "lbl/lbl_detail.hpp"
#include "teddy/graphrel/reliability.hpp"

namespace teddy::graphrel
{

namespace
{

constexpr char const* who = "calculate_reliability";

// ── Layers ──────────────────────────────────────────────────────────────────
//
// A layer's states live in shards chosen by the top bits of the key hash, so
// that threads can merge children into different shards at the same time.
// The layer's order is the order in which its states were first produced:
// expanding parents in that order keeps related states together, which makes
// the next layer's lookups cache-friendly (iterating in hash order instead
// costs ~1.5x on the 10x10 mesh).
//
// Children are produced in a fixed sequence: parent g yields position 2g
// (fails) and 2g + 1 (works). Serially, a layer is a single table, whose
// entries appear in that sequence. In parallel, threads expand contiguous
// runs of parents into buffers; each shard then takes its children run by
// run, in sequence, marks the position of every state it creates in a bitmap,
// and the order is rebuilt from the bitmap ranks. Either way every state has
// the same position and every sum is accumulated in the same order, so the
// result is the same for any number of threads.

constexpr std::size_t shard_count = 64;
constexpr unsigned shard_shift    = 58; // top 6 bits of the hash
constexpr std::size_t blocks_per_thread_per_round = 16;

std::size_t shard_of (std::uint64_t hash)
{
    return static_cast<std::size_t>(hash >> shard_shift);
}

/// A state's place: shard in the top 6 bits, entry index below.
using state_ref                = std::uint64_t;
constexpr state_ref entry_mask = (state_ref {1} << shard_shift) - 1;

state_ref make_ref (std::size_t shard, std::size_t entry)
{
    return (static_cast<state_ref>(shard) << shard_shift) | entry;
}

class layer
{
public:
    /// @p sharded: built in parallel (64 shards plus the order); otherwise a
    /// single table whose index order is the order.
    void reset (std::size_t key_size, std::size_t expected, bool sharded)
    {
        key_size_ = key_size;
        sharded_  = sharded;
        if (sharded)
        {
            ever_sharded_ = true;
            for (detail::state_table& s : shards_)
                s.reset(key_size, expected / shard_count + 1);
        }
        else
        {
            // Only shard 0 is read; the others are reset when next sharded.
            // (Resetting all 64 on every level costs ~2x on long thin graphs.)
            shards_[0].reset(key_size, expected);
        }
        order_.clear();
    }

    bool sharded () const
    {
        return sharded_;
    }

    std::size_t key_size () const
    {
        return key_size_;
    }

    std::size_t size () const
    {
        return sharded_ ? order_.size() : shards_[0].size();
    }

    std::size_t memory_bytes () const
    {
        std::size_t n = order_.capacity() * sizeof(state_ref);
        for (std::size_t s = 0; s < (ever_sharded_ ? shard_count : 1); ++s)
            n += shards_[s].memory_bytes();
        return n;
    }

    detail::state_table& shard (std::size_t s)
    {
        return shards_[s];
    }

    std::vector<state_ref>& order ()
    {
        return order_;
    }

    /// Serial insertion (unsharded layer): merge or append.
    void insert (
        std::uint8_t const* key,
        std::uint64_t hash,
        double probability
    )
    {
        shards_[0].add(key, hash, probability);
    }

    /// Call f(key, probability) for the states at positions [begin, end).
    template<class F>
    void for_range (std::size_t begin, std::size_t end, F&& f) const
    {
        if (! sharded_)
        {
            detail::state_table const& table = shards_[0];
            for (std::size_t g = begin; g < end; ++g)
                f(table.key(g), table.probability(g));
            return;
        }
        for (std::size_t g = begin; g < end; ++g)
        {
            state_ref const r                = order_[g];
            detail::state_table const& table = shards_[r >> shard_shift];
            std::size_t const e = static_cast<std::size_t>(r & entry_mask);
            f(table.key(e), table.probability(e));
        }
    }

private:
    std::size_t key_size_ = 0;
    bool sharded_         = false;
    bool ever_sharded_    = false; ///< shards 1..63 may hold memory
    std::array<detail::state_table, shard_count> shards_;
    std::vector<state_ref> order_;
};

/// Children produced by one thread in one round, grouped by shard.
struct child_buffer
{
    struct part
    {
        std::vector<std::uint8_t> keys;
        std::vector<std::uint64_t> hashes;
        std::vector<double> probs;
        std::vector<std::uint64_t> positions;
    };

    std::array<part, shard_count> parts;

    void clear ()
    {
        for (part& p : parts)
        {
            p.keys.clear();
            p.hashes.clear();
            p.probs.clear();
            p.positions.clear();
        }
    }

    std::size_t memory_bytes () const
    {
        std::size_t n = 0;
        for (part const& p : parts)
            n += p.keys.capacity()
               + (p.hashes.capacity() + p.positions.capacity()) * 8
               + p.probs.capacity() * sizeof(double);
        return n;
    }
};

/// Bitmap over child positions with rank queries.
class position_bitmap
{
public:
    void prepare (std::size_t positions)
    {
        words_ = (positions + 63) / 64;
        if (words_ > capacity_)
        {
            bits_     = std::make_unique<std::atomic<std::uint64_t>[]>(words_);
            capacity_ = words_;
        }
        for (std::size_t w = 0; w < words_; ++w)
            bits_[w].store(0, std::memory_order_relaxed);
    }

    void set (std::uint64_t position)
    {
        bits_[position / 64].fetch_or(
            std::uint64_t {1} << (position % 64),
            std::memory_order_relaxed
        );
    }

    /// After all set() calls: prefix counts for rank().
    void index ()
    {
        rank_.resize(words_);
        std::uint64_t total = 0;
        for (std::size_t w = 0; w < words_; ++w)
        {
            rank_[w] = total;
            total += static_cast<std::uint64_t>(
                std::popcount(bits_[w].load(std::memory_order_relaxed))
            );
        }
    }

    /// Number of set positions below @p position.
    std::size_t rank (std::uint64_t position) const
    {
        std::uint64_t const word
            = bits_[position / 64].load(std::memory_order_relaxed);
        std::uint64_t const below
            = word & ((std::uint64_t {1} << (position % 64)) - 1);
        return static_cast<std::size_t>(
            rank_[position / 64]
            + static_cast<std::uint64_t>(std::popcount(below))
        );
    }

    std::size_t memory_bytes () const
    {
        return capacity_ * 8 + rank_.capacity() * 8;
    }

private:
    std::unique_ptr<std::atomic<std::uint64_t>[]> bits_;
    std::size_t capacity_ = 0;
    std::size_t words_    = 0;
    std::vector<std::uint64_t> rank_;
};

/// Sums of one block of parents, combined in block order.
struct block_sums
{
    double success        = 0.0;
    double dropped        = 0.0;
    std::size_t branching = 0;
};

} // namespace

lbl_calculator::lbl_calculator(graph const& g) :
    lbl_calculator(
        g,
        generate_variable_ordering(
            detail::require_valid(g, who),
            ordering_strategy::cutwidth
        )
    )
{
}

// The graph and ordering are validated before anything indexes into them.
lbl_calculator::lbl_calculator(
    graph const& g,
    std::vector<variable> const& ordering
) :
    net_(detail::require_valid(g, who)),
    ordering_(detail::require_valid_ordering(g, ordering, who))
{
}

double lbl_calculator::calculate()
{
    stats_               = statistics {};
    dropped_probability_ = 0.0;

    // |K| = 1: the terminal set is connected iff that vertex works.
    if (net_.terminals().size() == 1)
    {
        return net_.vertex_probs()[net_.terminals().front()];
    }

    detail::frontier_plan const plan(
        net_,
        ordering_,
        /*force_perfect_vertices=*/true
    );
    stats_.num_layers        = plan.levels();
    stats_.max_boundary_size = plan.max_front_size();

    unsigned threads
        = threads_ != 0 ? threads_ : std::thread::hardware_concurrency();
    if (threads == 0)
        threads = 1;
    // Created for the first large layer, so small inputs never start threads.
    std::unique_ptr<detail::thread_team> team;
    std::vector<child_buffer> buffers;
    std::array<std::vector<std::uint64_t>, shard_count>
        created_at; // per shard, per entry
    position_bitmap created;

    // Layer `level` lives in layers[level % 2]; the two are reused, never moved
    // (moving 64 tables per level costs ~2x on long thin graphs).
    std::array<layer, 2> layers;
    layers[0].reset(plan.key_size(0), 1, false);
    std::vector<std::uint8_t> const initial = plan.initial_key();
    layers[0].insert(
        initial.data(),
        detail::state_table::hash(initial.data(), initial.size()),
        1.0
    );

    double success = 0.0;
    std::vector<block_sums> sums;

    for (std::size_t level = 0; level < plan.levels(); ++level)
    {
        layer& current        = layers[level % 2];
        layer& next           = layers[(level + 1) % 2];
        std::size_t const n   = current.size();
        stats_.max_layer_size = std::max(stats_.max_layer_size, n);
        stats_.total_decompositions += n;
        bool const parallel = threads > 1 && n >= min_parallel_layer_;
        next.reset(plan.key_size(level + 1), n, parallel);
        std::size_t const child_key_size = next.key_size();

        variable const& var              = ordering_[level];
        double const p                   = var.type == variable_type::edge
                                             ? net_.edges()[var.id].prob
                                             : net_.vertex_probs()[var.id];

        // Expand the parents of block b; live children go to
        // emit(key, hash, probability, position). A branch with probability
        // exactly 0 is skipped; with a prune threshold, branches at or below
        // it are dropped and their probability recorded, so R lies in
        // [result, result + dropped_probability()].
        std::size_t const blocks = (n + block_size_ - 1) / block_size_;
        sums.assign(blocks, block_sums {});
        auto expand_block = [&] (std::size_t b, auto&& emit)
        {
            block_sums& sum = sums[b];
            std::uint8_t low[detail::frontier_plan::max_key_size];
            std::uint8_t high[detail::frontier_plan::max_key_size];
            std::uint64_t position
                = 2 * static_cast<std::uint64_t>(b * block_size_);
            auto route = [&] (
                             detail::outcome o,
                             double probability,
                             std::uint8_t const* key,
                             std::uint64_t at
                         )
            {
                if (probability <= 0.0 || o == detail::outcome::failure)
                    return;
                if (probability <= prune_threshold_)
                {
                    sum.dropped += probability;
                    return;
                }
                if (o == detail::outcome::success)
                    sum.success += probability;
                else
                    emit(
                        key,
                        detail::state_table::hash(key, child_key_size),
                        probability,
                        at
                    );
            };
            current.for_range(
                b * block_size_,
                std::min(n, (b + 1) * block_size_),
                [&] (std::uint8_t const* key, double parent)
                {
                    detail::expansion const ex
                        = plan.expand(level, key, low, high);
                    if (ex.single)
                    {
                        route(ex.high, parent, high, position + 1);
                    }
                    else
                    {
                        ++sum.branching;
                        route(ex.low, parent * (1.0 - p), low, position);
                        route(ex.high, parent * p, high, position + 1);
                    }
                    position += 2;
                }
            );
        };

        std::size_t scratch_bytes = 0;
        if (! parallel)
        {
            auto insert = [&] (
                              std::uint8_t const* key,
                              std::uint64_t h,
                              double probability,
                              std::uint64_t
                          ) { next.insert(key, h, probability); };
            for (std::size_t b = 0; b < blocks; ++b)
                expand_block(b, insert);
        }
        else
        {
            if (! team)
            {
                team = std::make_unique<detail::thread_team>(threads);
                buffers.resize(team->size());
            }
            std::size_t const t_count = team->size();
            created.prepare(2 * n);
            for (std::vector<std::uint64_t>& c : created_at)
                c.clear();
            std::size_t const round = t_count * blocks_per_thread_per_round;
            for (std::size_t first = 0; first < blocks; first += round)
            {
                std::size_t const last = std::min(blocks, first + round);
                // Phase 1: thread t expands a contiguous run of blocks.
                team->run(
                    [&] (unsigned t)
                    {
                        child_buffer& out = buffers[t];
                        out.clear();
                        auto buffer = [&] (
                                          std::uint8_t const* key,
                                          std::uint64_t h,
                                          double probability,
                                          std::uint64_t at
                                      )
                        {
                            child_buffer::part& part = out.parts[shard_of(h)];
                            part.keys.insert(
                                part.keys.end(),
                                key,
                                key + child_key_size
                            );
                            part.hashes.push_back(h);
                            part.probs.push_back(probability);
                            part.positions.push_back(at);
                        };
                        std::size_t const span = last - first;
                        std::size_t const b0   = first + span * t / t_count;
                        std::size_t const b1 = first + span * (t + 1) / t_count;
                        for (std::size_t b = b0; b < b1; ++b)
                            expand_block(b, buffer);
                    }
                );
                // Phase 2: each shard takes its children run by run.
                std::atomic<std::size_t> next_shard {0};
                team->run(
                    [&] (unsigned)
                    {
                        for (std::size_t s = next_shard++; s < shard_count;
                             s             = next_shard++)
                        {
                            detail::state_table& table = next.shard(s);
                            for (child_buffer const& in : buffers)
                            {
                                child_buffer::part const& part = in.parts[s];
                                for (std::size_t i = 0; i < part.probs.size();
                                     ++i)
                                {
                                    std::size_t const before = table.size();
                                    if (table.add(
                                            part.keys.data()
                                                + i * child_key_size,
                                            part.hashes[i],
                                            part.probs[i]
                                        )
                                        == before)
                                    {
                                        created_at[s].push_back(
                                            part.positions[i]
                                        );
                                        created.set(part.positions[i]);
                                    }
                                }
                            }
                        }
                    }
                );
            }
            // Phase 3: order the new states by the position that created them.
            created.index();
            std::size_t total = 0;
            for (std::size_t s = 0; s < shard_count; ++s)
                total += next.shard(s).size();
            std::vector<state_ref>& order = next.order();
            order.resize(total);
            std::atomic<std::size_t> next_shard {0};
            team->run(
                [&] (unsigned)
                {
                    for (std::size_t s = next_shard++; s < shard_count;
                         s             = next_shard++)
                        for (std::size_t e = 0; e < created_at[s].size(); ++e)
                            order[created.rank(created_at[s][e])]
                                = make_ref(s, e);
                }
            );
            for (child_buffer const& in : buffers)
                scratch_bytes += in.memory_bytes();
            for (std::vector<std::uint64_t> const& c : created_at)
                scratch_bytes += c.capacity() * 8;
            scratch_bytes += created.memory_bytes();
        }

        std::size_t branching = 0;
        for (block_sums const& sum : sums)
        {
            success += sum.success;
            dropped_probability_ += sum.dropped;
            branching += sum.branching;
        }
        stats_.max_branching_layer_size
            = std::max(stats_.max_branching_layer_size, branching);
        stats_.peak_table_bytes = std::max(
            stats_.peak_table_bytes,
            current.memory_bytes() + next.memory_bytes() + scratch_bytes
        );
    }
    // Every state is resolved by the last layer (the final front is empty).
    return success;
}

// ── Public free functions ────────────────────────────────────────────────────

double calculate_reliability (graph const& g, unsigned threads)
{
    lbl_calculator calc(g);
    calc.set_threads(threads);
    return calc.calculate();
}

double calculate_reliability (
    graph const& g,
    std::vector<variable> const& ordering,
    unsigned threads
)
{
    lbl_calculator calc(g, ordering);
    calc.set_threads(threads);
    return calc.calculate();
}

namespace
{
reliability_bounds bounds_from (
    lbl_calculator& calc,
    double prune_threshold,
    unsigned threads
)
{
    calc.set_prune_threshold(prune_threshold);
    calc.set_threads(threads);
    double const lower = calc.calculate();
    return {lower, std::min(1.0, lower + calc.dropped_probability())};
}

void check_threshold (double prune_threshold)
{
    if (! (prune_threshold >= 0.0 && prune_threshold < 1.0))
        throw std::invalid_argument(
            "calculate_reliability_bounds: prune_threshold must be in [0, 1)"
        );
}
} // namespace

reliability_bounds calculate_reliability_bounds (
    graph const& g,
    double prune_threshold,
    unsigned threads
)
{
    check_threshold(prune_threshold);
    lbl_calculator calc(g);
    return bounds_from(calc, prune_threshold, threads);
}

reliability_bounds calculate_reliability_bounds (
    graph const& g,
    std::vector<variable> const& ordering,
    double prune_threshold,
    unsigned threads
)
{
    check_threshold(prune_threshold);
    lbl_calculator calc(g, ordering);
    return bounds_from(calc, prune_threshold, threads);
}

} // namespace teddy::graphrel
