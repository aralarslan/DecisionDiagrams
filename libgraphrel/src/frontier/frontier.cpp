/**
 * @file frontier.cpp
 * @brief Frontier plan, state transitions and the exact-key state table.
 */

#include "frontier/frontier.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <string>

namespace teddy::graphrel::detail
{

namespace
{

constexpr std::uint64_t bit (std::uint8_t label)
{
    return std::uint64_t {1} << (label - 1);
}

/// Key layout (frontier.hpp): K-mask in the first kmask_bytes bytes, then
/// one label per front position.
std::uint64_t load_kmask (std::uint8_t const* key)
{
    std::uint64_t kmask = 0;
    std::memcpy(&kmask, key, frontier_plan::kmask_bytes);
    return kmask;
}

void store_key (
    std::uint8_t* out,
    std::uint64_t kmask,
    std::uint8_t const* codes,
    std::size_t n
)
{
    std::memcpy(out, &kmask, frontier_plan::kmask_bytes);
    std::memcpy(out + frontier_plan::kmask_bytes, codes, n);
}

} // namespace

// ── frontier_plan ───────────────────────────────────────────────────────────

frontier_plan::frontier_plan(
    graph const& g,
    std::vector<variable> const& ordering,
    bool force_perfect_vertices
)
{
    std::size_t const n = g.num_vertices();

    // Remaining non-loop edges per vertex. Self-loops never connect anything,
    // so they do not keep a vertex on the front.
    // (A valid ordering lists every edge, so these are the loop-free degrees.)
    std::vector<std::size_t> const degree = g.loop_free_degrees();
    std::vector<std::uint32_t> remaining(degree.begin(), degree.end());
    std::vector<char> is_terminal(n, 0);
    for (std::size_t t : g.terminals())
        is_terminal[t] = 1;
    auto unreached = static_cast<std::uint32_t>(g.terminals().size());

    constexpr std::size_t unprocessed = static_cast<std::size_t>(-1);
    std::vector<std::size_t> edge_level(g.num_edges(), unprocessed);
    for (std::size_t l = 0; l < ordering.size(); ++l)
        if (ordering[l].type == variable_type::edge)
            edge_level[ordering[l].id] = l;

    std::vector<std::uint32_t> front;          // vertex ids, in position order
    std::vector<std::uint32_t> position(n, 0); // valid while on the front
    std::vector<char> on_front(n, 0);
    steps_.reserve(ordering.size());
    front_size_.reserve(ordering.size() + 1);

    auto remove_position = [&] (std::uint32_t p)
    {
        on_front[front[p]] = 0;
        front.erase(front.begin() + static_cast<std::ptrdiff_t>(p));
        for (std::size_t i = p; i < front.size(); ++i)
            position[front[i]] = static_cast<std::uint32_t>(i);
    };

    for (variable const& var : ordering)
    {
        front_size_.push_back(front.size());
        max_front_size_ = std::max(max_front_size_, front.size());
        step s;
        if (var.type == variable_type::vertex)
        {
            std::size_t const v = var.id;
            s.is_vertex         = true;
            s.terminal          = is_terminal[v] != 0;
            s.forced = force_perfect_vertices && g.vertex_probs()[v] >= 1.0;
            if (s.terminal)
                --unreached;
            s.enters = remaining[v] > 0;
            if (s.enters)
            {
                position[v] = static_cast<std::uint32_t>(front.size());
                front.push_back(static_cast<std::uint32_t>(v));
                on_front[v] = 1;
            }
        }
        else
        {
            edge const& e = g.edges()[var.id];
            s.forced      = e.prob >= 1.0;
            if (e.from == e.to)
            {
                s.loop = true;
            }
            else
            {
                // Vertex priority guarantees both endpoints are on the front.
                s.pos_u    = position[e.from];
                s.pos_v    = position[e.to];
                s.depart_u = --remaining[e.from] == 0;
                s.depart_v = --remaining[e.to] == 0;
                // Remove the higher position first so the lower one stays
                // valid.
                std::uint32_t const hi = std::max(s.pos_u, s.pos_v);
                std::uint32_t const lo = std::min(s.pos_u, s.pos_v);
                bool const depart_hi
                    = (hi == s.pos_u) ? s.depart_u : s.depart_v;
                bool const depart_lo
                    = (lo == s.pos_u) ? s.depart_u : s.depart_v;
                if (depart_hi)
                    remove_position(hi);
                if (depart_lo)
                    remove_position(lo);
            }
        }
        s.unreached_after = unreached;
        s.exits_offset    = exits_.size();
        for (std::uint32_t v : front)
        {
            exits_.push_back(remaining[v]);
            front_after_.push_back(v);
        }
        // Unprocessed edges between two front vertices (each listed once).
        std::size_t const level = steps_.size();
        s.inner_offset          = inner_edges_.size();
        for (std::size_t i = 0; i < front.size(); ++i)
        {
            std::uint32_t const v = front[i];
            for (std::size_t eid : g.get_incident_edges(v))
            {
                edge const& e = g.edges()[eid];
                if (e.from == e.to || edge_level[eid] <= level)
                    continue; // self-loop or already processed
                std::size_t const w = e.other(v);
                if (on_front[w] && position[w] > i)
                    inner_edges_.emplace_back(
                        static_cast<std::uint8_t>(i),
                        static_cast<std::uint8_t>(position[w])
                    );
            }
        }
        s.inner_count = inner_edges_.size() - s.inner_offset;
        steps_.push_back(s);
    }
    front_size_.push_back(front.size());
    max_front_size_ = std::max(max_front_size_, front.size());

    if (max_front_size_ > max_front)
    {
        throw std::length_error(
            "graphrel: the variable ordering has a front of "
            + std::to_string(max_front_size_)
            + " vertices; the supported maximum is " + std::to_string(max_front)
        );
    }
    compute_lookahead(g, ordering);
}

// Reverse sweep over the levels with a union-find on the "future graph"
// (edges not processed yet). After step l, the future edges are those at
// levels > l and the unreached terminals those whose vertex variable is at a
// level > l. Going from l to l-1 adds the edge at level l (if any) and the
// terminal at level l (if any). Each level records, for the next front, which
// positions share a future component, which components hold unreached
// terminals, and how many such components touch no front vertex.
void frontier_plan::compute_lookahead(
    graph const& g,
    std::vector<variable> const& ordering
)
{
    std::size_t const n = g.num_vertices();
    std::vector<std::uint32_t> parent(n), terminals_in(n, 0);
    for (std::size_t v = 0; v < n; ++v)
        parent[v] = static_cast<std::uint32_t>(v);
    auto find = [&] (std::uint32_t x)
    {
        while (parent[x] != x)
        {
            parent[x] = parent[parent[x]];
            x         = parent[x];
        }
        return x;
    };
    std::size_t components_with_terminals = 0;
    std::vector<char> is_terminal(n, 0);
    for (std::size_t t : g.terminals())
        is_terminal[t] = 1;

    component_.assign(front_after_.size(), 0);
    std::uint32_t roots[max_front];
    for (std::size_t l = steps_.size(); l-- > 0;)
    {
        step& s             = steps_[l];
        std::size_t const f = front_size_[l + 1];
        std::size_t touched
            = 0; // distinct roots on the front holding terminals
        for (std::size_t i = 0; i < f; ++i)
        {
            roots[i]          = find(front_after_[s.exits_offset + i]);
            std::size_t first = i;
            for (std::size_t j = 0; j < i; ++j)
                if (roots[j] == roots[i])
                {
                    first = j;
                    break;
                }
            component_[s.exits_offset + i] = static_cast<std::uint8_t>(first);
            if (terminals_in[roots[i]] > 0)
            {
                s.unreached_component_mask |= std::uint64_t {1} << i;
                if (first == i)
                    ++touched;
            }
        }
        s.floating_components
            = static_cast<std::uint32_t>(components_with_terminals - touched);
        bool several_components = false;
        for (std::size_t i = 0; i < f; ++i)
            several_components
                = several_components || component_[s.exits_offset + i] != 0;
        s.lookahead         = several_components || s.floating_components > 0;

        variable const& var = ordering[l];
        if (var.type == variable_type::edge)
        {
            edge const& e   = g.edges()[var.id];
            std::uint32_t a = find(static_cast<std::uint32_t>(e.from));
            std::uint32_t b = find(static_cast<std::uint32_t>(e.to));
            if (a != b)
            {
                if (terminals_in[a] > 0 && terminals_in[b] > 0)
                    --components_with_terminals;
                parent[b] = a;
                terminals_in[a] += terminals_in[b];
            }
        }
        else if (is_terminal[var.id])
        {
            std::uint32_t const r = find(static_cast<std::uint32_t>(var.id));
            if (terminals_in[r]++ == 0)
                ++components_with_terminals;
        }
    }
}

bool frontier_plan::can_still_connect(
    step const& s,
    std::uint8_t const* codes,
    std::size_t n,
    std::uint64_t kmask
) const
{
    if (s.floating_components >= 2)
        return false; // two parts that only the processed region could join
    // Tiny union-find over blocks (0..B-1) and future components (B +
    // position).
    std::uint8_t blocks = 0;
    for (std::size_t i = 0; i < n; ++i)
        blocks = std::max(blocks, codes[i]);
    std::uint8_t parent[2 * max_front];
    std::size_t const nodes = blocks + n;
    for (std::size_t i = 0; i < nodes; ++i)
        parent[i] = static_cast<std::uint8_t>(i);
    auto find = [&] (std::uint8_t x)
    {
        while (parent[x] != x)
            x = parent[x] = parent[parent[x]];
        return x;
    };
    std::uint8_t const* component = component_.data() + s.exits_offset;
    for (std::size_t i = 0; i < n; ++i)
        if (codes[i] != 0)
            parent[find(static_cast<std::uint8_t>(codes[i] - 1))]
                = find(static_cast<std::uint8_t>(blocks + component[i]));

    // Everything that must end up connected: terminal blocks and future
    // components holding unreached terminals.
    int representative = -1;
    auto require       = [&] (std::uint8_t node)
    {
        int const r = find(node);
        if (representative == -1)
            representative = r;
        return r == representative;
    };
    for (std::uint8_t b = 0; b < blocks; ++b)
        if ((kmask >> b) & 1u)
            if (! require(b))
                return false;
    for (std::size_t i = 0; i < n; ++i)
        if ((s.unreached_component_mask >> i) & 1u)
            if (! require(static_cast<std::uint8_t>(blocks + component[i])))
                return false;
    // A floating component can only be connected if nothing else is required.
    return s.floating_components == 0 || representative == -1;
}

outcome frontier_plan::finish(
    step const& s,
    std::uint64_t kmask,
    std::uint8_t* codes,
    std::size_t f,
    std::uint8_t* out
) const
{
    std::size_t n = f;

    // 1. Departures: a vertex whose last edge was just processed leaves. If it
    //    was the last front vertex of its block, the block is closed for good.
    if (! s.is_vertex && ! s.loop && (s.depart_u || s.depart_v))
    {
        std::uint8_t departing[2];
        std::size_t num_departing = 0;
        std::uint32_t remove[2];
        std::size_t num_remove = 0;
        if (s.depart_u)
            remove[num_remove++] = s.pos_u;
        if (s.depart_v)
            remove[num_remove++] = s.pos_v;
        if (num_remove == 2 && remove[0] < remove[1])
            std::swap(remove[0], remove[1]); // higher position first
        for (std::size_t r = 0; r < num_remove; ++r)
        {
            std::uint8_t const label = codes[remove[r]];
            if (label != 0 && (num_departing == 0 || departing[0] != label))
                departing[num_departing++] = label;
            std::memmove(
                codes + remove[r],
                codes + remove[r] + 1,
                n - remove[r] - 1
            );
            --n;
        }
        for (std::size_t d = 0; d < num_departing; ++d)
        {
            std::uint8_t const label = departing[d];
            if (std::find(codes, codes + n, label) != codes + n)
                continue; // the block still has a vertex on the front
            if (kmask & bit(label))
            {
                // A terminal block closed: success only if it is the only
                // terminal block and every terminal has been reached.
                return (std::popcount(kmask) == 1 && s.unreached_after == 0)
                         ? outcome::success
                         : outcome::failure;
            }
        }
    }

    // 2. Usable exits. A remaining edge is an exit of its front vertex unless
    //    its other end is an absent front vertex (failed or pruned: the edge
    //    is unusable) or in the same block (internal). Then, until nothing
    //    changes:
    //    - a terminal block without exits is isolated for good: success if it
    //      is the only terminal block and every terminal is reached, else
    //      failure;
    //    - a block without terminals and with at most one exit cannot lie on
    //      a path between terminals: drop it (dead end);
    //    - a vertex without exits in a block that has others: drop the vertex
    //      (the block keeps its other members and its terminal mark).
    std::uint32_t const* exits = exits_.data() + s.exits_offset;
    auto const* inner          = inner_edges_.data() + s.inner_offset;
    for (bool changed = true; changed;)
    {
        changed = false;
        std::uint32_t usable[max_front];
        for (std::size_t i = 0; i < n; ++i)
            usable[i] = exits[i];
        for (std::size_t k = 0; k < s.inner_count; ++k)
        {
            auto const [a, b] = inner[k];
            if (codes[a] == 0 || codes[b] == 0 || codes[a] == codes[b])
            {
                --usable[a];
                --usable[b];
            }
        }
        std::uint32_t total[max_front + 2] = {};
        for (std::size_t i = 0; i < n; ++i)
            if (codes[i] != 0)
                total[codes[i]] += usable[i];
        for (std::size_t i = 0; i < n; ++i)
        {
            std::uint8_t const label = codes[i];
            if (label == 0)
                continue;
            if (kmask & bit(label))
            {
                if (total[label] == 0)
                    return (std::popcount(kmask) == 1 && s.unreached_after == 0)
                             ? outcome::success
                             : outcome::failure;
            }
            else if (total[label] <= 1)
            {
                codes[i] = 0;
                changed  = true;
                continue;
            }
            if (usable[i] == 0)
            {
                codes[i] = 0; // no exit left; the block keeps its other members
                changed  = true;
            }
        }
    }

    // 3. Canonical labels: numbered by first appearance.
    std::uint8_t remap[max_front + 2] = {};
    std::uint8_t next_label           = 0;
    std::uint64_t canonical_kmask     = 0;
    for (std::size_t i = 0; i < n; ++i)
    {
        std::uint8_t const label = codes[i];
        if (label == 0)
            continue;
        if (remap[label] == 0)
        {
            remap[label] = ++next_label;
            if (kmask & bit(label))
                canonical_kmask |= bit(next_label);
        }
        codes[i] = remap[label];
    }

    // 4. Look-ahead: drop states whose terminals can no longer be joined.
    if (s.lookahead && ! can_still_connect(s, codes, n, canonical_kmask))
        return outcome::failure;

    // 5. Success: every terminal reached and all of them in one block.
    //    (Several terminal blocks stay live: they may still merge.)
    int const terminal_blocks = std::popcount(canonical_kmask);
    if (s.unreached_after == 0 && terminal_blocks <= 1)
    {
        // terminal_blocks == 0 cannot happen for a live state with |K| >= 2
        // (a closing terminal block is resolved in step 1); treat as failure.
        return terminal_blocks == 1 ? outcome::success : outcome::failure;
    }

    store_key(out, canonical_kmask, codes, n);
    return outcome::state;
}

expansion frontier_plan::expand(
    std::size_t level,
    std::uint8_t const* key,
    std::uint8_t* low_key,
    std::uint8_t* high_key
) const
{
    step const& s             = steps_[level];
    std::size_t const f       = front_size_[level];
    std::uint64_t const kmask = load_kmask(key);
    std::uint8_t const* codes = key + kmask_bytes;
    std::uint8_t buf[max_front + 1];
    expansion r;

    if (s.is_vertex)
    {
        if (! s.enters)
        {
            // A vertex without edges can never connect anything.
            if (s.terminal)
            { // isolated terminal (|K| >= 2): fails either way
                r.single = s.forced;
                return r;
            }
            r.single = true;
            std::memcpy(buf, codes, f);
            r.high = finish(s, kmask, buf, f, high_key);
            return r;
        }
        std::uint8_t const blocks
            = f == 0 ? 0 : *std::max_element(codes, codes + f);
        // Works: the vertex joins the front as a new block.
        std::memcpy(buf, codes, f);
        buf[f] = static_cast<std::uint8_t>(blocks + 1);
        std::uint64_t const kmask_high
            = s.terminal ? (kmask | bit(static_cast<std::uint8_t>(blocks + 1)))
                         : kmask;
        r.high = finish(s, kmask_high, buf, f + 1, high_key);
        if (s.forced)
        {
            r.single = true;
            return r;
        }
        // Fails: a terminal failing is a system failure; otherwise the vertex
        // is absent and its edges are unusable (Kawahara et al. 2019, §3.1).
        if (s.terminal)
        {
            r.low = outcome::failure;
        }
        else
        {
            std::memcpy(buf, codes, f);
            buf[f] = 0;
            r.low  = finish(s, kmask, buf, f + 1, low_key);
        }
        return r;
    }

    // Edge step.
    std::uint8_t const cu = s.loop ? 0 : codes[s.pos_u];
    std::uint8_t const cv = s.loop ? 0 : codes[s.pos_v];
    if (s.loop || cu == 0 || cv == 0 || cu == cv)
    {
        // The edge cannot change connectivity: an endpoint is absent, it is a
        // self-loop, or its endpoints are already connected (a loop in the
        // sense of Imai et al. 1999, Lemma 1).
        r.single = true;
        std::memcpy(buf, codes, f);
        r.high = finish(s, kmask, buf, f, high_key);
        return r;
    }
    // Works: merge the two blocks.
    std::memcpy(buf, codes, f);
    for (std::size_t i = 0; i < f; ++i)
        if (buf[i] == cv)
            buf[i] = cu;
    std::uint64_t kmask_high = kmask & ~bit(cv);
    if (kmask & bit(cv))
        kmask_high |= bit(cu);
    r.high = finish(s, kmask_high, buf, f, high_key);
    if (s.forced)
    {
        r.single = true;
        return r;
    }
    // Fails: nothing merges.
    std::memcpy(buf, codes, f);
    r.low = finish(s, kmask, buf, f, low_key);
    return r;
}

// ── state_table ─────────────────────────────────────────────────────────────

std::uint64_t state_table::hash(std::uint8_t const* p, std::size_t n)
{
    std::uint64_t h = 0x9E3779B97F4A7C15ULL ^ n;
    std::size_t i   = 0;
    for (; i + 8 <= n; i += 8)
    {
        std::uint64_t w;
        std::memcpy(&w, p + i, 8);
        h ^= w;
        h *= 0xBF58476D1CE4E5B9ULL;
        h ^= h >> 31;
    }
    std::uint64_t tail = 0;
    std::memcpy(&tail, p + i, n - i);
    h ^= tail;
    h *= 0x94D049BB133111EBULL;
    h ^= h >> 29;
    return h;
}

void state_table::reset(std::size_t key_size, std::size_t expected)
{
    key_size_ = key_size;
    stride_   = 2 + (key_size + 7) / 8;
    size_     = 0;
    arena_.clear();
    arena_.reserve(expected * stride_);
    std::size_t capacity = 16;
    while (capacity < 2 * expected)
        capacity *= 2;
    slots_.assign(capacity, 0);
}

void state_table::grow()
{
    std::vector<std::uint32_t> bigger(slots_.size() * 2, 0);
    std::size_t const mask = bigger.size() - 1;
    for (std::size_t e = 0; e < size_; ++e)
    {
        std::size_t idx = arena_[e * stride_] & mask;
        while (bigger[idx] != 0)
            idx = (idx + 1) & mask;
        bigger[idx] = static_cast<std::uint32_t>(e + 1);
    }
    slots_.swap(bigger);
}

std::size_t state_table::add(
    std::uint8_t const* key,
    std::uint64_t h,
    double probability
)
{
    if (2 * (size_ + 1) > slots_.size())
        grow();
    std::size_t const mask = slots_.size() - 1;
    std::size_t idx        = h & mask;
    while (slots_[idx] != 0)
    {
        std::size_t const e   = slots_[idx] - 1;
        std::uint64_t* record = arena_.data() + e * stride_;
        if (record[0] == h && std::memcmp(record + 2, key, key_size_) == 0)
        {
            record[1] = std::bit_cast<std::uint64_t>(
                std::bit_cast<double>(record[1]) + probability
            );
            return e;
        }
        idx = (idx + 1) & mask;
    }
    std::size_t const e = size_;
    if (e >= 0xFFFFFFFFu)
        throw std::length_error("graphrel: more than 2^32 states in one layer");
    slots_[idx] = static_cast<std::uint32_t>(e + 1);
    arena_.resize(arena_.size() + stride_, 0); // zero padding after the key
    std::uint64_t* record = arena_.data() + e * stride_;
    record[0]             = h;
    record[1]             = std::bit_cast<std::uint64_t>(probability);
    std::memcpy(record + 2, key, key_size_);
    ++size_;
    return e;
}

std::size_t state_table::memory_bytes() const
{
    return arena_.capacity() * sizeof(std::uint64_t)
         + slots_.capacity() * sizeof(std::uint32_t);
}

} // namespace teddy::graphrel::detail
