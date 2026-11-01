# libgraphrel audit

Scope: everything under `libgraphrel/` (about 4,800 lines), plus how `tests/graphrel.test.cpp`
and CI use it. Audited at commit `39fe87d`.

## Status on branch `feat/libgraphrel-optimisations`

This file is the original audit as found at `39fe87d`, plus the status table below. The full
technical report on the work (method, decisions, results, lessons, commit map) is
[`HANDOFF.md`](HANDOFF.md).

| Finding | Status |
|---|---|
| Existing graphrel tests did not compile (`decomp_state::union_find` → `uf_`) | Fixed (`1ef297d`) |
| 1.1 BDD perfect edges through a failed vertex | Fixed (`a29ca5e`) |
| 1.2 \|K\| = 1 | Defined as R = p(terminal) on both paths; \|K\| = 0 rejected (`a29ca5e`) |
| 1.3 Unsorted terminals | Rejected by `validate()`; new `graph::set_terminals()` normalises (`a29ca5e`) |
| 1.4 Custom orderings | Must be a permutation that respects vertex priority (`a29ca5e`) |
| 1.5 1e-15 cutoff | LBL exact again; opt-in `calculate_reliability_bounds()` keeps the speed with a rigorous interval (`a29ca5e`) |
| 1.6 Overflow beyond F = 25, hash-only identity | Fixed by the frontier engine: exact byte-compared keys, no Bell-number limit, fronts up to 64 (`6c37164`) |
| 1.7 Parser / writer | Strict token parsing; exact round-trip (`a29ca5e`) |
| 1.8 Validation order, `lbl_calculator` lifetime, `state_pool` exception safety | Fixed (`a29ca5e`, `63bf4fb`). Note: `lbl_calculator` was movable, not copyable; see HANDOFF §3.17 |
| 1.9 Docs / attribution | Fixed (`195280d`) |
| 2.1 `bell_cache` O((V+E)²) | Fixed (`0a43644`) |
| 2.2 Boundary precompute O(L·V) | Fixed (`0a43644`) |
| 2.3 Per-state Θ(V+E) work | Redundant rebuilds removed (`0a43644`); frontier-local states, O(F) per state (`6c37164`) |
| 2.4 Ordering helpers | O(V+E), identical output (`0a43644`) |
| Ordering quality (gap to Wu & Sun Table 2) | New `ordering_strategy::cutwidth`, default for LBL and BDD: N20 387,481 → 65,858 states (`7a98b40`); see HANDOFF §3.16 |
| 2.5 `state_pool` | Fixed: recycled states reuse their allocations (`63bf4fb`) |
| 2.6 Allocation churn, 2.7 BDD recursion depth | Fixed: frontier engine with flat state tables; BDD built layer by layer without recursion (`6c37164`, `7916203`) |
| §3 Dead code, duplication, docs, warnings | Fixed (`195280d`); `graph` encapsulation (3.1): open |
| §4 Tests and CI | Brute-force fuzz, precondition, I/O and paper tests; CI runs them on GCC and Clang, plus an ASan/UBSan job (`a29ca5e`, `9771441`) |

## Which reference applies to which code

libgraphrel contains two different algorithms, and they must be judged against different sources:

| Entry point | Algorithm | Reference |
|---|---|---|
| `calculate_reliability` (`src/lbl/`) | Layer-by-layer recursive decomposition (LBL) | **Wu & Sun (2024)**, RESS 244:109968 — the paper's contribution |
| `build_diagram` (`src/diagram/`) | Classic boundary-set (BS) based BDD, built top-down | Hardy, Lucet & Limnios (2007) [WS 20]. Wu & Sun only summarise it in §2.3 as their **baseline**. |
| Shared: `partition.cpp`, `bell_cache.cpp`, `boundary.cpp`, `graph_operations.cpp` | BS partitions, class numbering, loop/coloop rules | **Carlier & Lucet (1996)** [WS 32]; **Imai, Sekine & Imai (1999)** [WS 33] |
| Vertex-failure semantics and variable order (both paths) | k-NR-IV, R_EV | **Kawahara, Sonoda, Inoue & Kasahara (2019)** [WS 24] |

"[WS n]" means reference n in Wu & Sun. The Wu & Sun preconditions are checked against **LBL
only**.

`diagram.hpp` and `diagram.cpp` describe the BDD builder as "Wu & Sun 2024, BS-BDD"; that should
cite Hardy et al. I haven't seen Hardy et al. itself. The BDD builder is checked below against the
semantics and BS definitions in Carlier & Lucet, Imai et al. and Kawahara et al. instead.

### Checks against Carlier & Lucet, Imai et al. and Kawahara et al.

| What | Reference | Status |
|---|---|---|
| Class number from partition (`partition_to_number`) and back (`number_to_partition`) | C&L §4.7, Algorithms 2 and 1 | **Faithful.** I checked it against a literal transcription of Algorithm 2 over all 142,417 partitions with \|F\| ≤ 10. It is bijective onto [1, B(F)], and Algorithm 1 inverts it (`sweep.cpp numbering`). |
| Stirling recursion \|A(i,j)\| = j·\|A(i−1,j)\| + \|A(i−1,j−1)\| | C&L §4.8 | Faithful. C&L only treat F_max ≤ 12; the library never checks the uint64 overflow at F ≥ 26 (1.6). |
| Loop → R(G\e); coloop → p·R(G/e); otherwise Shannon | Imai Lemma 1 | Faithful. The library applies the K-terminal generalisation: a coloop is an edge whose deletion disconnects the terminals. |
| State = boundary set F, up/down of each boundary vertex, partition of the up vertices, K-mark per block | C&L §5.2 (K-terminal with failed vertices) | Same information, encoded differently. C&L's F_k is **static**, fixed by the vertex numbering, with explicit up/down flags. The library computes the boundary **per state**: "down" shows up as absence, and so does an up vertex whose remaining edges were pruned. Merging those two cases is correct, because neither can carry connectivity any more. |
| Two minors are isomorphic iff their elimination partitions are equal, **given the same remaining edge set Ē_i** | Imai Theorem 1 (front V_i defined per level) | **Not checked by the code.** Pruning and vertex deletion mean states at one level can have different remaining edge sets, so Theorem 1 doesn't cover the library's key. Fuzzing found no counterexample (0 / 30,000), but none of these references proves it. Make it a documented invariant, and add a debug assertion that compares the remaining unprocessed edge sets on every key hit. |
| Failed vertex ⇒ every incident edge is 0: R_EV = R_E \|_{e←0 ∀ e∈E_v, v down} | Kawahara §3.1 | LBL: correct. **BDD: violated for p(e) = 1** (1.1). The perfect-edge shortcut sets e to 1 even when an endpoint is down. |
| Vertex variable placed just before the first edge in E_v | Kawahara §3.1; Wu & Sun §2.3 | The library's BFS does exactly this, e.g. v1 < v2 < e1 < v3 < e2 < e3 on the Fig. 1 bridge. |
| Ordering quality: F_max should be as small as possible; BFS with random swaps accepted when ΔF_max < 0 | C&L §4.9; Imai §2.2 (Theorem 2: width ≤ B_l); Kawahara §4.4 | **Only plain BFS is provided, with no improvement step.** On meshes this is the cause of the gap to Wu & Sun's Table 2 (see the reproduction below). F_max alone isn't a good enough target: on 10×10, BFS and the row sweep both have F_max = 11, but G^max is 387,481 vs 65,858 (exact). |
| Graph connected; 2 ≤ \|K\| | C&L §2; Imai §2.1; Kawahara §2.1; Wu & Sun §2.1 | Not enforced (1.2). |

### Wu & Sun (2024) preconditions and whether the code enforces them

| Paper precondition | Where in paper | Enforced? | What happens if violated (measured) |
|---|---|---|---|
| All vertices of V connected | §2.1 Assumption 1 | No | Harmless: LBL is still exact on disconnected graphs (0 / 10,000 fuzz mismatches) |
| Two-state, independent variables | §2.1 Assumptions 2–3 | By construction | — |
| 2 ≤ \|K\| ≤ \|V\| | §2.1 Notations | **No** (parser and `graph` accept \|K\| = 1) | With \|K\| = 1 and an ordering that doesn't start at the terminal, LBL is wrong (666 / 10,000), because early success fires before the terminal's vertex variable is processed. The BDD is wrong for \|K\| = 1 with any ordering (1.2). The library's own test `single_terminal_trivial` relies on \|K\| = 1 being accepted. |
| Ordering by BFS from a seed k ∈ K, with **vertex priority** (each vertex before its incident edges) | §2.3; Fig. 4 Step 1 | Built-in generators: **yes**. `calculate_reliability(g, ordering)`: **no** | See the ordering results below and 1.4 |
| Isomorphism = identical boundary-set partition with K-marks | §2.3, §3.2 | Only approximately: identity goes through a 64-bit hash (1.6) | — |
| Exact result | Abstract, §5 | **No**: branches with p ≤ 1e-15 are dropped (1.5) | 1.2e-6 relative error in 1-R |

**Ordering: BFS vs DFS.** I fuzzed 10,000 graphs inside the paper's assumptions (connected simple
graphs, 2 ≤ |K| ≤ 4, all p in (0,1)) and compared LBL against brute force with each ordering:

| Ordering passed to LBL | Mismatches |
|---|---|
| default BFS (paper, Fig. 4) | 0 |
| library DFS | 0 |
| random ordering that respects vertex priority (not BFS/DFS shaped) | 0 |
| random ordering that ignores vertex priority | **8,068** |

So for **correctness** the real precondition is vertex priority plus |K| ≥ 2. BFS vs DFS doesn't
matter for the answer. For **efficiency** it matters enormously. The paper prefers BFS, and DFS can
be orders of magnitude worse. Here is G^max (the peak layer size) for the same mesh under different
edge insertion orders (`order_sens.cpp`):

| Mesh | BFS G^max (40 edge orders) | DFS G^max (first 3 orders) |
|---|---|---|
| 5×5 (= N8) | 253 – 782 | 146, 28,569, 10,943 |
| 3×10 (= N6) | 24 – 66 | 26,245, 7,008, 7,872 |
| 20×2 (= N9) | 5 – 11 | 5, 23,532, 26,019 |

`ordering_strategy::dfs` is exposed as an equal alternative. It should be documented as
correct-but-risky, or removed.

### Reproducing the paper's numbers with LBL

Every reliability value I could reproduce matches Wu & Sun **exactly**. The peak layer sizes
(G^max, Table 2 column B¹) with the library's default BFS ordering do **not**. With a row-by-row
sweep ordering they come within 1.22–1.25× of the paper. All counts below are exact (no probability
cutoff); the first version of this table had N20 at 340,599 / 58,039 because the old 1e-15 cutoff
was silently dropping states.

| Network | Paper R | libgraphrel R | Paper B¹ (G^max) | G^max, library BFS | G^max, row sweep | Paper time | libgraphrel time (BFS / sweep) |
|---|---|---|---|---|---|---|---|
| Bridge, Fig. 1–3 | 0.760078728 | 0.760078728 | layers 1,1,2,3,4,5,4,3,1 | layers 1,1,2,3,4,5,3,3,1 | — | — | 0 s |
| N9 (2×20 mesh) | 0.230160 | 0.230160 | 4 | 5 | 5 | 0.0 s | 0.0 s |
| N6 (3×10 mesh) | 0.609632 | 0.609632 | 12 | 19 | 15 | 0.0 s | 0.0 s |
| N7 (3×12 mesh) | 0.579399 | 0.579399 | 12 | 19 | 15 | 0.0 s | 0.0 s |
| N8 (5×5 mesh) | 0.711988 | 0.711988 | 118 | 253 | 146 | 0.0 s | 0.0 s |
| N20 (10×10 mesh) | 0.714299 | 0.714299 | 53,924 | **387,481** | **65,858** | 42.9 s | 140.2 s / 65.9 s (after `0a43644`) |
| N27 (complete K₁₂) | 0.810000 | 0.810000 | 816,465 | 1,459,298 exact (42,665 with the old 1e-15 cutoff) | — | 72.9 s | 104 s exact (2.2 s with cutoff) |

All edges and vertices are 0.9, with the two terminals at the largest distance apart (opposite mesh
corners). I identified N6–N9 from |V| and |E| in Table 1; the exact match of R confirms the
identification. "Row sweep" means: vertices row by row along the short side, each followed by its
edges to earlier vertices. That respects vertex priority and is the street-network numbering of
C&L §4.9 / Fig. 3. Script: `sweep.cpp`.

- On the bridge, the library is at or below the paper layer by layer. Coloop detection and
  dead-component pruning (Imai Lemma 1; not in Wu & Sun) remove one state at layer 6.
- On K₁₂ it *appears* 19× below the paper, but **this was wrong in the first version of this
  report.** The low count came from the 1e-15 probability cutoff (1.5), not from coloop
  detection and pruning. Computed exactly, K₁₂ peaks at 1,459,298 states, 1.8× the paper's
  816,465, and takes 104 s / 1.2 GB. The cutoff silently discarded about 97 % of the states;
  the answer still matched to 6 digits.
- On meshes the library's corner-seeded BFS is 1.25–7.2× *above* the paper. The cause is the
  **ordering**, not the state definition:
  - BFS from a corner sweeps diagonally, and those fronts allow many more partitions than a row
    front of the same size.
  - With a row sweep the gap shrinks to 1.22× on N20 and 1.25× on the small meshes. N20's run
    time halves at the same time.
  - The small residual is plausibly BFS tie-breaking in the authors' code, which the paper doesn't
    specify. Every state I inspected is genuinely distinct.
  - Practical consequence: the library's ordering heuristic is the main thing standing between it
    and the paper's state counts. C&L §4.9 and Kawahara §4.4 both say the same about ordering.
    Sweeping along the *long* side is catastrophic (2×20: 1,048,576 states, F_max = 21), so any
    sweep must pick its direction by F_max.
- The paper's large meshes N25/N26 (1000×10 and 1200×10, 28,990 and 34,790 layers) need 540 MB
  in the paper. In libgraphrel, the `bell_cache` alone needs (L+1)(L+2)/2 × 8 bytes, which is
  **3.4 GB for N25 and 4.8 GB for N26**. The per-level boundary tables add roughly another 1 GB.
  That's before a single state is created (see 2.1–2.2). Both figures are computed from the code,
  not measured.

## How this was checked

- I read every source and header, the Wu & Sun paper, and the three supporting papers: Carlier &
  Lucet 1996, Imai et al. 1999 and Kawahara et al. 2019.
- I compiled with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wmissing-declarations`.
  There were only 15 warnings, mostly `int`/`size_t` mixing in `partition.cpp`.
- **Differential fuzzing** (`fuzz.cpp` in this folder). I generated random small graphs (3–7
  vertices, up to 9 edges) and compared `calculate_reliability` (LBL) and `build_diagram` (BDD)
  against brute-force enumeration of all states. The fuzzer has these modes:
  - `PAPER=1`: Wu & Sun's assumptions exactly.
  - `PAPER=1 PERFECT=1`: the same, but some edges have p = 1.
  - `KMIN2=1`: anything goes, with |K| ≥ 2.
  - default: anything goes, including disconnected graphs, loops, multi-edges, p ∈ {0, 1} and |K| = 1.

  It also runs LBL with DFS, random vertex-priority orderings and unconstrained random orderings.
- I ran the same fuzz under ASan and UBSan. It found **no memory errors and no undefined behaviour**.
- **Targeted repros** (`probe.cpp`) for each finding below, plus timing and peak-memory scaling runs.
- Paper reproduction and reference checks (`paper.cpp`, `order_sens.cpp`, `sweep.cpp`), and a
  callgrind profile on a 7×7 grid.

**Headline.** 10,000 graphs per mode:

| Mode | LBL mismatches | BDD mismatches |
|---|---|---|
| `PAPER=1` (Wu & Sun assumptions) | 0 | 0 |
| `PAPER=1 PERFECT=1` (+ some p = 1 edges) | 0 | **545** (bug 1.1) |
| `KMIN2=1` (anything, \|K\| ≥ 2) | 0 | 9 (bug 1.1) |
| default (anything, incl. \|K\| = 1) | 0 | 963 (mostly 1.2, which is outside both methods' scope) |

- **LBL is exact within the paper's assumptions** with every ordering that respects vertex
  priority, and it reproduces the paper's R values exactly. Its problems are unenforced
  preconditions (|K| ≥ 2, vertex-priority orderings, sorted terminals), the 1e-15 cutoff, and
  efficiency: more states per layer than the paper, and memory that grows quadratically with graph size.
- The BDD builder (not Wu & Sun's method) has one real bug, triggered by edges with p = 1
  together with failed vertices (1.1). With 1.1 and 1.2 fixed locally, it had 0 mismatches in
  every mode.

```
# compile all libgraphrel/src/*/*.cpp into libgraphrel.a first, then:
g++ -std=c++20 -O2 -I<repo> -I<repo>/libgraphrel/include -I<repo>/libgraphrel/src \
    libgraphrel/audit/fuzz.cpp libgraphrel.a -o fuzz
PAPER=1 ./fuzz 10000 && PAPER=1 PERFECT=1 ./fuzz 10000 && KMIN2=1 ./fuzz 10000
```

---

## 1. Correctness

### 1.1 BDD connects through a failed vertex using perfect edges *(BDD builder; medium)*
`src/diagram/diagram.cpp:177-187`. The perfect-edge shortcut (`prob >= 1.0` → contract) is an
optimisation of this implementation; it isn't in any of the references. It breaks the k-NR-IV
semantics of Kawahara et al. §3.1, where every edge incident to an unavailable vertex is forced
to 0. It runs **before** the
ghost-edge check, so an edge already removed by `delete_vertex_in_place` gets contracted anyway,
which joins its endpoints through the failed vertex. This happens within Wu & Sun-style assumptions
(connected graph, |K| = 2) whenever some edge has p = 1 exactly: 545 of 10,000 cases.

```
s=0, v=1 (p_v=0.5), t=2;  edges 0-1 (p=1), 1-2 (p=1), 0-2 (p=0.5)
exact = 0.75    LBL = 0.75    BDD = 1.0
```
**Fix:** swap the two `if` blocks so the ghost check comes first. Verified: 0 fuzz mismatches in
every mode afterwards.

### 1.2 |K| = 1 isn't rejected, and LBL and BDD disagree on it *(precondition; low–medium)*
Wu & Sun require 2 ≤ |K| (§2.1), and K-terminal BDD methods assume the same. libgraphrel accepts
|K| = 1 anyway (`read_graph`, `graph`, and the test `single_terminal_trivial`).
- BDD (`diagram.cpp:143`): `is_success_terminal()` is trivially true, so it returns `1` before the
  terminal's vertex variable is processed.
- LBL is correct only when the ordering starts at the terminal, which BFS and DFS do.
```
2 vertices, terminal {0}, p_v0 = 0.3      exact = 0.3    LBL = 0.3    BDD = 1.0
```
**Fix:** reject |K| < 2 with `std::invalid_argument` in `validate()`, matching the paper. If you
want to keep it supported, define it as `R = p(terminal)`.

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

### 1.4 `calculate_reliability(g, ordering)` doesn't enforce the paper's ordering precondition *(medium, public API)*
`reliability.hpp:32`. Wu & Sun build the ordering by BFS from a seed in K under the vertex
priority principle (§2.3, Fig. 4 Step 1). The built-in BFS and DFS generators satisfy this. The
custom-ordering overload accepts anything. Measured: any vertex-priority ordering is exact when
|K| ≥ 2, and orderings that break vertex priority are wrong 77–81 % of the time (see the
ordering table above).
```
path 0-1-2, p_v1 = 0.5      exact = 0.405
  edges-first ordering      → 0.81   (middle vertex never matters: early success)
  ordering missing vars     → 0.0
```
**Fix:** check in O(V+E) that the ordering is a permutation of all variables and puts each vertex
before its incident edges, and throw `std::invalid_argument` if not. Document that DFS is valid but
can be orders of magnitude slower than BFS.

### 1.5 Probability cutoff `PROB_EPSILON = 1e-15` biases small unreliabilities *(medium)*
`lbl_variable_processing.cpp:19`. Branches with probability ≤ 1e-15 are dropped. On a 5×5 grid
with p = 0.9999 the LBL value of `1-R` is off by **1.2e-6 relative**. With the cutoff set to `0` the
gap to BDD falls to 5.5e-9, which is floating-point noise. This matters to anyone computing
unreliability of highly reliable networks. It is also a departure from Wu & Sun, whose LBL is
exact: Fig. 4 drops only invalid subgraphs, never small-probability ones. **Fix:** skip only exact
zeros, or make the cutoff an opt-in parameter.

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
`diagram.hpp:13` and the `diagram.cpp` file header attribute the BDD builder to "Wu & Sun 2024,
BS-BDD". Wu & Sun's method is LBL; the BS-BDD is their §2.3 baseline from Hardy, Lucet & Limnios
(2007). Cite that instead.

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

1. **Correctness, small diffs.**
   - Enforce Wu & Sun's preconditions for LBL: |K| ≥ 2 (1.2) and vertex-priority orderings (1.4).
   - Validate or normalise the terminal list (1.3).
   - Drop the epsilon (1.5) and fix the parser (1.7).
   - In the BDD builder, swap the ghost and perfect-edge checks (1.1).
   - Add the fuzz test in `PAPER=1` and `KMIN2=1` modes, and turn tests on in CI.
2. **Better ordering.** Offer a sweep/front-minimising ordering next to BFS, in the spirit of the
   C&L §4.9 improvement loop, but scoring candidates on more than F_max, e.g. a short simulated
   layer count. This closes most of the G^max gap to Wu & Sun's Table 2 (N20: 387,481 → 65,858
   states). Fix `estimate_max_boundary_size` (2.4) first so the scoring is cheap.
3. **Cheap performance wins:** size `bell_cache` by F (2.1), make `precompute` incremental (2.2),
   remove the redundant failure checks (2.3), fix the ordering helpers (2.4). Together these turn
   the ladder from quadratic back to roughly linear.
4. **Cleanup:** delete dead code and the duplicate header, merge the `graph_operations` variants,
   fix the docs.
5. **Redesign (bigger):** frontier-local states with exact keys (2.3 / 1.6) and an encapsulated
   `graph` class (3.1).
