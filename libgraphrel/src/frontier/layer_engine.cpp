/**
 * @file layer_engine.cpp
 * @brief Level-by-level expansion of frontier states (serial and parallel).
 */

#include "frontier/layer_engine.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "common/thread_team.hpp"

namespace teddy::graphrel::detail
{

namespace
{

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
            for (state_table& s : shards_)
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

    state_table& shard (std::size_t s)
    {
        return shards_[s];
    }

    std::vector<state_ref>& order ()
    {
        return order_;
    }

    /// Serial insertion (unsharded layer): merge or append. Returns the
    /// state's position in the layer.
    std::size_t insert (
        std::uint8_t const* key,
        std::uint64_t hash,
        double probability
    )
    {
        return shards_[0].add(key, hash, probability);
    }

    /// Call f(key, probability) for the states at positions [begin, end).
    template<class F>
    void for_range (std::size_t begin, std::size_t end, F&& f) const
    {
        if (! sharded_)
        {
            state_table const& table = shards_[0];
            for (std::size_t g = begin; g < end; ++g)
                f(table.key(g), table.probability(g));
            return;
        }
        for (std::size_t g = begin; g < end; ++g)
        {
            state_ref const r        = order_[g];
            state_table const& table = shards_[r >> shard_shift];
            std::size_t const e      = static_cast<std::size_t>(r & entry_mask);
            f(table.key(e), table.probability(e));
        }
    }

private:
    std::size_t key_size_ = 0;
    bool sharded_         = false;
    bool ever_sharded_    = false; ///< shards 1..63 may hold memory
    std::array<state_table, shard_count> shards_;
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

// Link values used only while a parallel layer is being built.
constexpr std::uint32_t pending = 0xFFFFFFFDu; ///< state, index not known yet
constexpr std::uint32_t same_as_high = 0xFFFFFFFCu; ///< single branch

[[noreturn]] void too_many_states (std::size_t n)
{
    throw std::length_error(
        "graphrel: a layer of " + std::to_string(n)
        + " states is too large to record diagram links"
    );
}

} // namespace

layer_engine::layer_engine(
    frontier_plan const& plan,
    engine_options const& options
) :
    plan_(plan),
    options_(options)
{
    if (options_.block_size == 0)
        options_.block_size = 1;
}

engine_result layer_engine::run(
    std::function<double(std::size_t)> const& probability,
    std::vector<std::vector<state_links>>* links
) const
{
    engine_result result;
    engine_statistics& stats = result.statistics;

    unsigned threads         = options_.threads != 0
                                 ? options_.threads
                                 : std::thread::hardware_concurrency();
    if (threads == 0)
        threads = 1;
    std::size_t const block_size = options_.block_size;
    bool const keep_all          = options_.keep_all_branches;
    double const prune           = options_.prune_threshold;

    // Created for the first large layer, so small inputs never start threads.
    std::unique_ptr<thread_team> team;
    std::vector<child_buffer> buffers;
    std::array<std::vector<std::uint64_t>, shard_count>
        created_at; // per shard, per entry: the position that created it
    position_bitmap created;
    // With links, parallel layers also need, per child position, where the
    // child was merged, and per shard entry, its position in the layer.
    std::vector<state_ref> merged_at;
    std::array<std::vector<std::uint32_t>, shard_count> position_of;

    if (links)
        links->assign(plan_.levels(), {});

    // Layer `level` lives in layers[level % 2]; the two are reused, never moved
    // (moving 64 tables per level costs ~2x on long thin graphs).
    std::array<layer, 2> layers;
    layers[0].reset(plan_.key_size(0), 1, false);
    std::vector<std::uint8_t> const initial = plan_.initial_key();
    layers[0].insert(
        initial.data(),
        state_table::hash(initial.data(), initial.size()),
        1.0
    );

    std::vector<block_sums> sums;

    for (std::size_t level = 0; level < plan_.levels(); ++level)
    {
        layer& current       = layers[level % 2];
        layer& next          = layers[(level + 1) % 2];
        std::size_t const n  = current.size();
        stats.max_layer_size = std::max(stats.max_layer_size, n);
        stats.total_states += n;
        bool const parallel = threads > 1 && n >= options_.min_parallel_layer;
        next.reset(plan_.key_size(level + 1), n, parallel);
        std::size_t const child_key_size = next.key_size();
        double const p                   = probability(level);

        std::vector<state_links>* const out
            = links ? &(*links)[level] : nullptr;
        if (out)
            out->assign(n, state_links {});

        // Expand the parents of block b; live children go to
        // emit(key, hash, probability, position), which returns the link
        // value. Unless all branches are kept, a branch with probability
        // exactly 0 is skipped, and with a prune threshold branches at or
        // below it are dropped and their probability summed.
        std::size_t const blocks = (n + block_size - 1) / block_size;
        sums.assign(blocks, block_sums {});
        auto expand_block = [&] (std::size_t b, auto&& emit)
        {
            block_sums& sum = sums[b];
            std::uint8_t low[frontier_plan::max_key_size];
            std::uint8_t high[frontier_plan::max_key_size];
            std::uint64_t position
                = 2 * static_cast<std::uint64_t>(b * block_size);
            auto route = [&] (
                             outcome o,
                             double branch_probability,
                             std::uint8_t const* key,
                             std::uint64_t at
                         ) -> std::uint32_t
            {
                if (o == outcome::failure)
                    return state_links::to_false;
                if (! keep_all)
                {
                    if (branch_probability <= 0.0)
                        return state_links::to_false;
                    if (branch_probability <= prune)
                    {
                        sum.dropped += branch_probability;
                        return state_links::to_false;
                    }
                }
                if (o == outcome::success)
                {
                    sum.success += branch_probability;
                    return state_links::to_true;
                }
                return emit(
                    key,
                    state_table::hash(key, child_key_size),
                    branch_probability,
                    at
                );
            };
            current.for_range(
                b * block_size,
                std::min(n, (b + 1) * block_size),
                [&] (std::uint8_t const* key, double parent)
                {
                    expansion const ex = plan_.expand(level, key, low, high);
                    state_links l;
                    if (ex.single)
                    {
                        l.high = route(ex.high, parent, high, position + 1);
                        l.low  = l.high == pending ? same_as_high : l.high;
                    }
                    else
                    {
                        ++sum.branching;
                        l.low
                            = route(ex.low, parent * (1.0 - p), low, position);
                        l.high = route(ex.high, parent * p, high, position + 1);
                    }
                    if (out)
                        (*out)[static_cast<std::size_t>(position / 2)] = l;
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
                              double branch_probability,
                              std::uint64_t
                          ) -> std::uint32_t
            {
                std::size_t const e = next.insert(key, h, branch_probability);
                if (out && e >= state_links::max_states)
                    too_many_states(e + 1);
                return static_cast<std::uint32_t>(e);
            };
            for (std::size_t b = 0; b < blocks; ++b)
                expand_block(b, insert);
        }
        else
        {
            if (! team)
            {
                team = std::make_unique<thread_team>(threads);
                buffers.resize(team->size());
            }
            std::size_t const t_count = team->size();
            created.prepare(2 * n);
            for (std::vector<std::uint64_t>& c : created_at)
                c.clear();
            if (out)
                merged_at.assign(2 * n, 0);
            std::size_t const round = t_count * blocks_per_thread_per_round;
            for (std::size_t first = 0; first < blocks; first += round)
            {
                std::size_t const last = std::min(blocks, first + round);
                // Phase 1: thread t expands a contiguous run of blocks.
                team->run(
                    [&] (unsigned t)
                    {
                        child_buffer& buffer_out = buffers[t];
                        buffer_out.clear();
                        auto buffer = [&] (
                                          std::uint8_t const* key,
                                          std::uint64_t h,
                                          double branch_probability,
                                          std::uint64_t at
                                      ) -> std::uint32_t
                        {
                            child_buffer::part& part
                                = buffer_out.parts[shard_of(h)];
                            part.keys.insert(
                                part.keys.end(),
                                key,
                                key + child_key_size
                            );
                            part.hashes.push_back(h);
                            part.probs.push_back(branch_probability);
                            part.positions.push_back(at);
                            return pending;
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
                            state_table& table = next.shard(s);
                            for (child_buffer const& in : buffers)
                            {
                                child_buffer::part const& part = in.parts[s];
                                for (std::size_t i = 0; i < part.probs.size();
                                     ++i)
                                {
                                    std::size_t const before = table.size();
                                    std::size_t const e      = table.add(
                                        part.keys.data() + i * child_key_size,
                                        part.hashes[i],
                                        part.probs[i]
                                    );
                                    if (e == before)
                                    {
                                        created_at[s].push_back(
                                            part.positions[i]
                                        );
                                        created.set(part.positions[i]);
                                    }
                                    if (out)
                                        merged_at[part.positions[i]]
                                            = make_ref(s, e);
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
            if (out && total > state_links::max_states)
                too_many_states(total);
            std::vector<state_ref>& order = next.order();
            order.resize(total);
            std::atomic<std::size_t> next_shard {0};
            team->run(
                [&] (unsigned)
                {
                    for (std::size_t s = next_shard++; s < shard_count;
                         s             = next_shard++)
                    {
                        if (out)
                            position_of[s].resize(created_at[s].size());
                        for (std::size_t e = 0; e < created_at[s].size(); ++e)
                        {
                            std::size_t const at
                                = created.rank(created_at[s][e]);
                            order[at] = make_ref(s, e);
                            if (out)
                                position_of[s][e]
                                    = static_cast<std::uint32_t>(at);
                        }
                    }
                }
            );
            // Phase 4 (links only): resolve the children's positions.
            if (out)
            {
                auto resolve = [&] (std::uint64_t at)
                {
                    state_ref const r = merged_at[at];
                    return position_of[r >> shard_shift][static_cast<
                        std::size_t>(r & entry_mask)];
                };
                team->run(
                    [&] (unsigned t)
                    {
                        std::size_t const g0 = n * t / t_count;
                        std::size_t const g1 = n * (t + 1) / t_count;
                        for (std::size_t g = g0; g < g1; ++g)
                        {
                            state_links& l = (*out)[g];
                            if (l.high == pending)
                                l.high = resolve(2 * g + 1);
                            if (l.low == same_as_high)
                                l.low = l.high;
                            else if (l.low == pending)
                                l.low = resolve(2 * g);
                        }
                    }
                );
                scratch_bytes += merged_at.capacity() * sizeof(state_ref);
                for (std::vector<std::uint32_t> const& v : position_of)
                    scratch_bytes += v.capacity() * sizeof(std::uint32_t);
            }
            for (child_buffer const& in : buffers)
                scratch_bytes += in.memory_bytes();
            for (std::vector<std::uint64_t> const& c : created_at)
                scratch_bytes += c.capacity() * 8;
            scratch_bytes += created.memory_bytes();
        }

        std::size_t branching = 0;
        for (block_sums const& sum : sums)
        {
            result.success += sum.success;
            result.dropped += sum.dropped;
            branching += sum.branching;
        }
        stats.max_branching_layer_size
            = std::max(stats.max_branching_layer_size, branching);
        stats.peak_memory_bytes = std::max(
            stats.peak_memory_bytes,
            current.memory_bytes() + next.memory_bytes() + scratch_bytes
        );
    }
    // Every state is resolved by the last layer (the final front is empty).
    return result;
}

} // namespace teddy::graphrel::detail
