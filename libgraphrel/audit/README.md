# libgraphrel audit

Scope: everything under `libgraphrel/` (about 4,800 lines), plus how `tests/graphrel.test.cpp`
and CI use it. Audited at commit `39fe87d`.

## How this was checked

- I read every source and header.
- I compiled with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wmissing-declarations`.
  There were only 15 warnings, mostly `int`/`size_t` mixing in `partition.cpp`.
- **Differential fuzzing** (`fuzz.cpp` in this folder): I generated 10,000 random small graphs
  (2–7 vertices, 1–9 edges, multi-edges, self-loops, perfect and imperfect edges and vertices,
  1–4 terminals). On each one, `calculate_reliability` (LBL) and `build_diagram` (BDD) were
  compared against brute-force enumeration of all states.
- I ran the same fuzz under ASan and UBSan. It found **no memory errors and no undefined behaviour**.
- **Targeted repros** (`probe.cpp`) for each finding below, plus timing and peak-memory scaling runs.
- Profiled with callgrind on a 7×7 grid.

**Headline:** the LBL path matched brute force on every fuzz case. The **BDD path is wrong on
about 10 % of random graphs** (296 of 3,000 with any number of terminals; 9 of 5,000 with at least two
terminals). There are two root causes, both one-line fixes, described below. With both fixes applied
locally, BDD had 0 mismatches over 10,000 cases. Most other findings are about performance, which
is dominated by a design choice: each decomposition state stores the whole graph.

```
# compile all libgraphrel/src/*/*.cpp into libgraphrel.a first, then:
g++ -std=c++20 -O2 -I<repo> -I<repo>/libgraphrel/include -I<repo>/libgraphrel/src \
    libgraphrel/audit/fuzz.cpp libgraphrel.a -o fuzz && ./fuzz 5000 && KMIN2=1 ./fuzz 5000
```

---

## 1. Correctness

### 1.1 BDD connects through a failed vertex using perfect edges *(high)*
`src/diagram/diagram.cpp:177-187`. The perfect-edge shortcut (`prob >= 1.0` → contract) runs
**before** the ghost-edge check. So an edge already removed by `delete_vertex_in_place` gets
contracted anyway, which joins its endpoints through the failed vertex.

```
s=0, v=1 (p_v=0.5), t=2;  edges 0-1 (p=1), 1-2 (p=1), 0-2 (p=0.5)
exact = 0.75    LBL = 0.75    BDD = 1.0
```
**Fix:** swap the two `if` blocks so the ghost check comes first. Verified: this fix together with
the one in 1.2 gives 0 fuzz mismatches.

### 1.2 BDD ignores terminal vertex reliability when |K| = 1 *(high)*
`diagram.cpp:143`. `is_success_terminal()` is trivially true when there are fewer than 2 terminals,
so the BDD returns `1` before the terminal's own vertex variable is ever processed.
```
2 vertices, terminal {0}, p_v0 = 0.3      exact = 0.3    LBL = 0.3    BDD = 1.0
```
This accounts for most of the 296 mismatches. **Fix:** don't short-circuit on success while an
imperfect terminal vertex variable is still pending. Simpler alternative: put terminal vertex
variables first in the ordering and multiply them in up front.

### 1.3 Unsorted `graph::terminals` silently gives wrong answers *(high, public API)*
`graph.hpp:94`, `lbl_variable_processing.cpp:103` and `diagram.cpp:227` all use
`std::binary_search` on `terminals`. `graph::validate()` never checks that the list is sorted.
Only `read_graph` sorts it. When a terminal isn't recognised, its vertex failure is treated as an
ordinary vertex deletion, which removes it from K. The remaining terminals are then "connected",
so the failure counts as **success**.
```
path 0-1-2, terminals = {2, 0}, p_v0 = 0.5      exact = 0.405    LBL = BDD = 0.855
```
**Fix:** have `validate()` require the list to be sorted, unique and non-empty. Better: make
`terminals` private behind a setter that normalises it (see 3.1).

### 1.4 `calculate_reliability(g, ordering)` trusts arbitrary orderings *(medium, public API)*
`reliability.hpp:32`. The algorithm needs every variable to appear exactly once, with each vertex
before all of its incident edges. None of this is checked:
```
path 0-1-2, p_v1 = 0.5      exact = 0.405
  edges-first ordering      → 0.81   (middle vertex never matters: early success)
  ordering missing vars     → 0.0
```
**Fix:** check that the ordering is a permutation and respects vertex priority (O(V+E)), and
throw `std::invalid_argument` if not.

### 1.5 Probability cutoff `PROB_EPSILON = 1e-15` biases small unreliabilities *(medium)*
`lbl_variable_processing.cpp:19`. Branches with probability ≤ 1e-15 are dropped. On a 5×5 grid
with p = 0.9999 the LBL value of `1-R` is off by **1.2e-6 relative**. With the cutoff set to `0` the
gap to BDD falls to 5.5e-9, which is floating-point noise. This matters to anyone computing
unreliability of highly reliable networks. **Fix:** skip only exact zeros, or make the cutoff an
opt-in parameter.

### 1.6 State identity is hash-based and can overflow *(medium, soundness)*
- `partition_key.boundary_hash` is the **only** record of which boundary vertices a state has.
  The comment in `partition.hpp:77` admits "Hash collision = Identity collision = Data loss".
  The chance of a collision is tiny, but when one happens the answer is silently wrong.
- `bell_cache` stores S(n,k) as `uint64_t` with no overflow check (`bell_cache.cpp:61`). B(26) is
  already larger than 2⁶⁴. `stirling()` also returns `0` for `n > max_n` (`bell_cache.cpp:76`).
  For boundaries of 26 or more vertices, partition numbers wrap around and can collide. The comment
  `partition_key::MAX_BOUNDARY_SIZE = 64` (`partition_key.hpp:30`) is therefore wrong.

**Fix:** throw if the boundary size F exceeds 25. Make keys exact, either by comparing the
boundary vertex list on a hash hit or by using the frontier-local representation in 2.3, where the
boundary is implied by the layer.

### 1.7 Parser and writer *(medium)*
`graph_io.cpp`
- `e 0 1 O.9` (letter O) parses as **prob = 0** with no error. A failed `operator>>` stores 0
  (`:149`). The same applies to the optional defaults on the `p` line (`:92`).
- Trailing junk is accepted (`p reliability 2 1 0.9x`).
- Negative counts (`p reliability -3 2`) wrap to a huge `size_t`, so the user gets a `bad_alloc`
  instead of a `ParseError`.
- `write_graph` uses `setprecision(15)` (`:256`), which doesn't round-trip doubles. For example
  `0.1+0.2` comes back changed. Use `max_digits10` or `std::to_chars`.

**Fix:** parse tokens with `std::from_chars`, require the whole token to be consumed, and reject
any leftover input on the line.

### 1.8 Lifetime and exception-safety nits *(low)*
- `lbl_calculator` (`lbl_detail.hpp:26`) can be copied implicitly. A copy's `boundary_` would
  still point at the *original* object's `ordering_`, which dangles once the original is destroyed.
  Delete the copy and move operations.
- `lbl_calculator` and `diagram_builder` build the ordering and run `boundary_computer::precompute()`
  **before** checking `g.validate()`, so an invalid graph indexes out of bounds before the check
  can report it. `build_diagram` never calls `validate()` at all.
- `state_pool::create` (`state_pool.hpp:188-207`) destroys a recycled node and then
  placement-news a new one. If the copy constructor throws, the node is in neither the live set
  nor the free list, and the pool's destructor destroys it a second time. Also, the public `grow()`
  can leave a partly used pool whose unconstructed tail the destructor later "destroys".

### 1.9 API docs don't match behaviour *(low)*
`diagram.hpp:33-43` says `edge_var()` returns -1 for perfect edges, but every edge gets a variable.
`vertex_var()` returns -1 only if *all* vertices are perfect: when one vertex is imperfect, every
vertex gets a variable. The result is wasted BDD variables and a misleading API.

---

## 2. Efficiency

### Measured scaling
A 2×N ladder has a boundary of size 2, so cost should grow **linearly** with N:

| N (2×N ladder) | V+E    | time   | peak RSS |
|---------------:|-------:|-------:|---------:|
| 500            | 2,498  | 0.75 s | 46 MB    |
| 1,000          | 4,998  | 3.8 s  | 176 MB   |
| 2,000          | 9,998  | 17.9 s | 692 MB   |
| 4,000          | 19,998 | 69.9 s | **2.76 GB** |

Both time and memory grow quadratically. Grids: 8×8 takes 1.1 s, 10×10 takes 16 s for LBL and
30 s for BDD (400 MB). A frontier-based implementation should handle 10×10 in milliseconds.

### 2.1 `bell_cache(V + E)` is O((V+E)²) memory
`lbl_calculator.cpp:32` and `diagram.cpp:90`. The table only needs to cover the largest boundary,
and because of 1.6 it can't usefully go past about 25 anyway. Size it at `min(F_max, 25) + 1`.
That is roughly 1.6 GB of the ladder's 2.76 GB.

### 2.2 `boundary_computer::precompute` is O(L·(V+E)) time and O(L·V) memory
`boundary.cpp:47-84`. It rebuilds two `std::set`s from scratch at every one of the L = V+E levels
and stores a vertex list per level. `vertices_with_unprocessed_edges` is computed and never read.
A single incremental sweep is O(V+E). Also replace `edge_level_map_`, an `unordered_map` keyed by
dense edge IDs, with a `std::vector`: it's looked up for every remaining edge of every state in
`identify_vertices`.

### 2.3 Per-state work is Θ(V+E) instead of Θ(F)
The main cost. `decomp_state` holds a union-find over **all** V vertices and a `remaining_edges`
list over **all** E edges, so every clone, connectivity check and pruning pass touches the whole graph.

Callgrind on a 7×7 grid: boundary computation ≈35 %, union-find unite/find ≈35 %,
pruning ≈19 %, malloc/free ≈15 % (these overlap; they're inclusive costs).

Work you can remove today without redesigning:
- `process_variable` calls `build_full_connectivity` + `is_failure_terminal_with` on **every**
  state (`lbl_variable_processing.cpp:40`). Children are already checked when they're created, so
  only the initial state can arrive in a failed state.
- After the coloop test has passed, `delete_edge` can't disconnect the terminals, and `contract`
  never can. So both `is_failure_terminal(net_)` calls in the standard-Shannon branch
  (`:78`, `:91`; `diagram.cpp:215`) are always false. That's two full O(V+E) union-find builds per
  branching.
- `is_valid` is always true after edge-only in-place operations.
- `is_coloop` repeats checks the caller already made (loop test, membership `binary_search`) and
  builds yet another full union-find.

Long term: switch to the standard frontier-local state used by Wu & Sun and by Hardy, Lucet &
Limnios: a canonical partition of the F boundary vertices plus terminal bits. Per-state cost
becomes O(F), clones become a few bytes, and the state *is* the exact hash key, which also fixes 1.6.

### 2.4 Ordering helpers are polynomially slow
`variable_ordering.cpp`
- BFS scans **all** E edges, using `std::set` lookups, for every dequeued vertex (`:190`). That's
  O(V·E·log V). Checking only the incident edges is enough.
- `estimate_max_boundary_size` is O(L²·E): it runs a `find_if` over the whole ordering for every
  incident edge at every level (`:285`). It took **938 ms on 20×20 and 73 s on 40×40**. Precompute
  each edge's position once and it becomes O(V+E).
- DFS recurses through `std::function` (`:74`), so a long path will overflow the stack.
- Use `std::vector<char>` instead of `std::set<std::size_t>` for dense IDs throughout.

### 2.5 `state_pool` doesn't pool the expensive part
A recycled node is destroyed and then copy-constructed, so each state still makes 4 heap
allocations (`remaining_edges`, `parent_`, `rank_`, `current_K`). The pool only saves the 80-byte
header. Copy-*assign* into recycled nodes to reuse vector capacity, or drop the pool.

The ghost, redundant-edge, coloop and terminal-vertex paths also clone the parent and then
destroy the parent at the end of the layer. Moving the parent into the next layer saves a clone.

### 2.6 Smaller allocation churn
- `identify_vertices` returns a fresh vector. `canonicalize_partition` copies it into
  `ws.sorted_boundary` and again into `partition.boundary_vertices`. It also builds an
  `unordered_set` and a `vector<bool>` per call, and binary-searches for an index it could have
  carried along.
- `prune_unreachable_edges` allocates and fills two V-sized vectors on every call and every pass.
- `graph::get_incident_edges` returns a copy, and `get_neighbors` allocates. Return
  `std::span<const std::size_t>` instead.

### 2.7 BDD recursion depth is V+E
`build_recursive` takes `decomp_state` by value and recurses once per variable. Large graphs will
overflow the stack, especially on MSVC, where the default stack is 1 MB and CI builds MSVC.
Use an explicit stack, or build the BDD layer by layer like LBL.

---

## 3. C++ usage, design and code quality

### 3.1 `graph` invariants aren't enforced
- `graph` is a public aggregate with redundant state: `num_vertices`, `num_edges` and `edge::id`
  must all agree with the vectors.
- `graph g(3, 0); g.add_edge(...)` leaves `num_edges == 0`, so `validate()` fails with a confusing
  "invalid graph".
- `terminals` must be sorted (see 1.3) but nothing enforces it.

Suggested shape: a `class graph` with private storage, `num_edges()` computed from `edges.size()`,
`set_terminals()` that sorts and deduplicates, and `vertex_count`/`edge_count` accessors.
`reliability_diagram` has the same problem: its public `manager`, `diagram` and `probs` can be
changed independently, and its "internal" constructor is public.

### 3.2 Duplication
`graph_operations.cpp` has three versions of each operation (`X`, `X_no_check`, `X_in_place`), and
the terminal-set update in `contract_edge` is pasted twice. Only the `_in_place` versions are used
by the library; the others exist only for tests. Keep the in-place versions and write the others
as `copy + in_place` in the tests.

`make_key` in `diagram.cpp` re-implements `encode_k_bitset` from `lbl_detail.hpp`.

### 3.3 Dead or conflicting code
- `src/common/variable_ordering.hpp` duplicates the public header under a **different include
  guard**. Including both is a hard error (`redefinition of 'struct variable'`). Nothing includes
  it, so delete it.
- Unused: `pool_guard`; `flat_hash_map::find_or_insert`, the `DELETED` tombstone state and the
  collision counters; `boundary_partition::is_success_terminal` / `is_failure_terminal`;
  `level_boundary_data::vertices_with_unprocessed_edges`; `processed_vars` in
  `estimate_max_boundary_size`.
- `lbl_calculator::statistics` and `set_log_stream` can't be reached through the public API, and
  `log_stream_` is never written to.
- Used only by tests: `number_to_partition`, `union_find::get_classes` / `reset` / `connected`, `is_loop`.
- `generate_variable_ordering_dfs` has external linkage and no declaration. It belongs in an
  anonymous namespace (`-Wmissing-declarations` flags it).

### 3.4 Docs and comments
- A search-and-replace of "reliability" → "prob" garbled the docs: "Edge prob probability",
  "BDD-based network prob", "prob analysis".
- Stale names from earlier versions: `CompactPartitionKey`, `StirlingBellCache`,
  `ReliabilityCalculator`, `bdd_builder`, `reliability_calculator.cpp`, "renamed … in C.3".
- `diagram.cpp`'s file header says `reliability_diagram.cpp`.
- `lbl_detail.hpp` says it's included by `boundary.cpp`, which it isn't.
- Doxygen blocks are placed **inside function bodies** (`graph.cpp`, `decomp_state.cpp`,
  `partition.cpp`, `union_find.cpp`, `variable_ordering.cpp`). Doxygen ignores them, and they
  repeat the header docs.

### 3.5 Style and consistency
- Naming conventions are mixed: `ParseError` (CamelCase); `mainPoolSize_` / `nextPoolNode_`
  (camelCase); a public member with a trailing underscore (`decomp_state::uf_`); `current_K`.
  The rest of the codebase uses snake_case.
- `decomp_state` mixes domain data with allocator plumbing (`next_`) and a validity flag that's
  always true except after vertex deletion.
- `union_find` caps graphs at **65,535 vertices** (`uint16_t` parents). That's a reasonable size
  trade-off, but it isn't documented in the public API, and it surfaces as an `invalid_argument`
  thrown from deep inside the code. Its `find` is recursive. It also spells out all four defaulted
  special members, which the rule of zero makes unnecessary.
- Headers include more than they use: `partition.hpp` pulls in `<bitset>`, `<set>`, `<sstream>`,
  `<ostream>` and `<string>`. `decomp_state.hpp` includes `<climits>` for `SIZE_MAX`, which lives
  in `<cstdint>`. `flat_hash_map.hpp` uses `std::hash` without including `<functional>`.
- `static` inside an anonymous namespace is redundant (`diagram.cpp:64,72`). The `default:` in the
  `switch` over an enum class (`variable_ordering.cpp:242`) disables `-Wswitch`.
- `flat_hash_map::iterator` has no `operator==` or `const_iterator`, so you can't range-for over a
  `const` map. Also, `find_slot() const` updates `mutable` statistics, so concurrent `const` lookups
  are a data race.

### 3.6 What's good
- Clear split between the public `include/` headers and private `src/` code; an umbrella header;
  a CMake `ALIAS` target.
- Reusing the TeDDy `bss_manager` gives users DPLD, importance measures and the rest of TeDDy's
  analysis for free.
- The LBL algorithm itself is correct on every graph I fuzzed, including multi-edges, self-loops,
  zero- and one-probability components, disconnected graphs and up to 4 terminals.
- Ping-pong layer maps, the thread-local scratch buffers and the shared `boundary_computer`
  between LBL and BDD are sensible ideas, even where the surrounding costs drown out their benefit.
- The parser gives line-numbered errors for structural problems.

---

## 4. Tests and CI

- **No brute-force oracle.** Most tests compare LBL with BDD, or with constants. Every BDD bug in
  §1 slipped through because the two paths are never compared on graphs that combine perfect edges
  and imperfect vertices, or on a single unreliable terminal. Add `fuzz.cpp` (or a Boost version of
  it) as a test: 10,000 cases run in about 7 seconds.
- **CI never builds or runs the tests.** `ci.yml` only configures and builds.
  `LIBTEDDY_BUILD_TESTS` defaults to `OFF` in every preset, there's no `ctest` step, and Boost isn't
  installed. Add a test preset and a `ctest --preset …` step, and consider an
  ASan/UBSan job (`LIBTEDDY_USE_SANITIZERS` already exists).

---

## Suggested order of work

1. **Correctness, small diffs:** 1.1 and 1.2 (swap the checks in `diagram.cpp`, terminal-vertex
   handling), 1.3 (validate or normalise terminals), 1.4 (validate orderings), 1.5 (drop the
   epsilon), 1.7 (parser). Add the fuzz test and turn tests on in CI.
2. **Cheap performance wins:** size `bell_cache` by F (2.1), make `precompute` incremental (2.2),
   remove the redundant failure checks (2.3), fix the ordering helpers (2.4). Together these turn
   the ladder from quadratic back to roughly linear.
3. **Cleanup:** delete dead code and the duplicate header, merge the `graph_operations` variants,
   fix the docs.
4. **Redesign (bigger):** frontier-local states with exact keys (2.3 / 1.6) and an encapsulated
   `graph` class (3.1).
