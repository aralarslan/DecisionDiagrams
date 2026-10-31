# libgraphrel: audit and optimisation — handoff log

This is the traceability record for the libgraphrel audit and the follow-up work. For each
finding it records what was seen, how it was found, why a given fix was chosen over the
alternatives, what the measured result was, and what the process taught us. It is meant to be
read cold by someone picking the work up.

| | |
|---|---|
| Library | `libgraphrel/` in the DecisionDiagrams (TeDDy) fork |
| Audit baseline | commit `39fe87d` ("More fixes") |
| Audit report | [`README.md`](README.md) in this folder (findings 1.1–4, reference checks, status table) |
| Audit commits | the first three commits of the work branch — report and harnesses only, no library changes |
| Work branch | `feat/libgraphrel-optimisations` — fixes, tests, CI, performance |
| References used | Wu & Sun 2024 (LBL); Carlier & Lucet 1996; Imai, Sekine & Imai 1999; Kawahara et al. 2019. Hardy, Lucet & Limnios 2007 was **not** available |

---

## 1. What libgraphrel computes, and which reference governs what

libgraphrel computes **K-terminal network reliability with imperfect vertices**: the probability
that every terminal in K is up and all terminals are connected by working edges through working
vertices. Edges and vertices fail independently.

It has two algorithms. They follow different references, and that matters when judging them:

| Entry point | Algorithm | Governing reference |
|---|---|---|
| `calculate_reliability` (`src/lbl/`) | Layer-by-layer decomposition (LBL) | Wu & Sun (2024), the paper's own contribution |
| `build_diagram` (`src/diagram/`) | Boundary-set BDD, built top-down with memoisation | Hardy, Lucet & Limnios (2007); Wu & Sun only describe it as their §2.3 baseline |
| Shared (`partition`, `bell_cache`, `boundary`, `graph_operations`) | Boundary-set partitions, class numbering, loop/coloop rules | Carlier & Lucet (1996); Imai et al. (1999) |
| Vertex-failure semantics, variable order | k-NR-IV, R_EV | Kawahara et al. (2019) |

**Lesson 1.** The first version of the audit judged both algorithms against Wu & Sun. You pointed
out that the paper covers LBL only, and that reframed several findings: what had looked like
"bugs" were actually unenforced preconditions of the method. Start an audit of
algorithmic code by establishing which source defines which piece of code.

---

## 2. How the work was done

The process, in order. Each step produced evidence that the next one used.

1. **Read the whole library**, about 4,800 lines.
2. **Strict compile** with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wmissing-declarations`. There were only 15 warnings, so the compiler wasn't where the problems were.
3. **Differential fuzzing against a brute-force oracle** (`fuzz.cpp`). Random graphs with 2–7
   vertices and up to 9 edges are small enough to enumerate all 2^(V+E) states exactly. Each one
   was checked against `calculate_reliability` and `build_diagram`.
4. **Targeted repros** (`probe.cpp`): the smallest graph that shows each finding.
5. **Scaling and profiling**: ladders and grids for time and peak memory; callgrind on a 7×7 grid.
6. **Read Wu & Sun** and restrict fuzzing to its assumptions (`PAPER=1`): connected simple graph,
   2 ≤ |K|, all probabilities strictly inside (0, 1).
7. **Reproduce the paper's Table 2** (`paper.cpp`): the bridge example, N6–N9, N20 and N27.
8. **Read the supporting references** and check the shared code against them, e.g. class
   numbering against Carlier & Lucet's Algorithm 2 over every partition (`sweep.cpp numbering`).
9. **Ordering experiments** (`order_sens.cpp`, `sweep.cpp`) to explain why our peak layer sizes
   differ from the paper's.
10. **Work branch**: fixes in small, separately reviewable commits. Each commit is verified with
    the brute-force tests, the existing tests, a sanitizer build, and before/after benchmarks
    whose results had to be **identical** whenever the change was meant to be performance-only.

Everything is reproducible from the files in this folder (see §6).

---

## 3. Findings, decisions and results

Each entry has the same shape: **Seen**, **How found**, **Root cause**, **Options**,
**Decision**, **Result**, **Commit**.

### 3.1 The existing graphrel test suite did not compile

- **Seen.** `graphrel-test` failed to build: `decomp_state` has no member `union_find`.
- **How found.** The first time the real test suite was built on the work branch (Boost.Test
  had to be installed first).
- **Root cause.** The member was renamed to `uf_`; one test still used the old name. CI never built tests, so nobody noticed.
- **Decision.** One-line fix, committed on its own so it doesn't get mixed up with behaviour changes.
- **Result.** All 4 existing suites pass.
- **Commit.** `b6551c7`.
- **Lesson 2.** Tests that are never run stop compiling. Getting CI to run the tests was
  therefore part of the fix, not an extra (see 3.13).

### 3.2 BDD routes connectivity through a failed vertex via p = 1 edges

- **Seen.** With s=0 and t=2, edges 0–1 and 1–2 at p = 1 and 0–2 at p = 0.5, and vertex 1 at
  p = 0.5: brute force and LBL give 0.75, the BDD gives 1.0.
- **How found.** Brute-force fuzzing. Within Wu & Sun's assumptions plus some p = 1 edges, the BDD
  was wrong on 545 of 10,000 graphs; with all p strictly inside (0, 1) it was wrong on 0.
- **Root cause.** `diagram.cpp` contracted any edge with p ≥ 1 *before* checking whether the
  edge had already been removed by a vertex failure (a "ghost" edge). Kawahara et al. §3.1 define
  R_EV so that every edge incident to an unavailable vertex is 0. The shortcut set it to 1.
- **Options.**
  1. Drop the perfect-edge shortcut.
  2. Reorder the two checks.
  3. Turn p = 1 edges into structure up front, by contracting them in the input graph.
- **Decision.** Reorder (2). It's the smallest diff, it keeps the shortcut (which saves BDD
  variables), and it follows the reference's semantics exactly.
- **Result.** 0 mismatches in every fuzz mode, plus a regression test.
- **Commit.** `5df2a69`.

### 3.3 |K| = 1

- **Seen.** One terminal with p = 0.3: LBL gives 0.3, BDD gives 1.0, and LBL gives the wrong
  answer too when the ordering doesn't start at the terminal (666 of 10,000).
- **Root cause.** Both algorithms treat |K| < 2 as "already connected" and stop before the
  terminal's own vertex variable is processed.
- **Options.**
  1. Reject |K| < 2, as all four references assume 2 ≤ |K|.
  2. Define R = p(terminal).
- **Decision.** Option 2, and reject |K| = 0. The existing test `single_terminal_trivial` shows
  |K| = 1 was meant to be supported, and R = p(t) is the natural meaning. It's handled up front
  in both `calculate()` and `build()`, so it doesn't depend on the ordering. Easy to flip to option 1.
- **Result.** Both paths agree with brute force in the `any` fuzz mode.
- **Commit.** `5df2a69`.

### 3.4 Unsorted terminals silently gave wrong answers

- **Seen.** Path 0–1–2 with terminals `{2, 0}` and p(v0) = 0.5: exact 0.405, both algorithms 0.855.
- **Root cause.** The code calls `binary_search` on `graph::terminals`. Only `read_graph` sorted
  that list, and `validate()` never checked it. An unrecognised terminal's failure was treated as
  an ordinary vertex deletion, so a *failure* was counted as *success*.
- **Options.**
  1. Sort silently inside the algorithms.
  2. Reject unsorted input.
  3. Make `graph` a class with private terminals.
- **Decision.** Option 2, plus a new `graph::set_terminals()` that sorts and deduplicates. Sorting
  a `const graph&` silently would mean copying the graph; option 3 is an API break, deferred to the
  redesign.
- **Result.** `validate()` now also checks for an empty terminal set, NaN probabilities, edge ids
  and that the adjacency lists are complete. Error messages name the specific problem.
- **Commit.** `5df2a69`.

### 3.5 Custom orderings were not checked

- **Seen.** Path 0–1–2 with p(v1) = 0.5: an edges-first ordering returns 0.81 and an ordering
  with missing variables returns 0.0; the exact value is 0.405.
- **How we established the real precondition.** We fuzzed LBL inside the paper's assumptions with
  four kinds of ordering:

  | Ordering | Mismatches out of 10,000 |
  |---|---|
  | BFS | 0 |
  | DFS | 0 |
  | random, respecting vertex priority | 0 |
  | random, ignoring vertex priority | 8,068 |

  So the precondition is "every variable exactly once, and each vertex before its incident edges"
  (vertex priority), together with |K| ≥ 2. The paper's BFS is one way to satisfy it, not a
  requirement in itself.
- **Decision.** `calculate_reliability(g, ordering)` checks exactly that precondition in O(V+E)
  and throws `std::invalid_argument` otherwise.
- **Commit.** `5df2a69`.

### 3.6 The 1e-15 probability cutoff (the biggest surprise)

- **Seen.** LBL silently dropped every branch with probability ≤ 1e-15. On a 5×5 grid with
  p = 0.9999 the unreliability 1−R was off by 1.2e-6 relative (5.5e-9 without the cutoff).
- **Decision, first version.** Remove it: the paper's algorithm is exact.
- **What happened next.** The K₁₂ paper test went from 2 s to 107 s. Measured exactly, K₁₂ peaks
  at **1,459,298** states, against 42,665 with the cutoff: the cutoff had been discarding about 97 %
  of the states. It had also distorted the audit's own figures:
  - K₁₂ was reported "19× below the paper, thanks to coloop detection"; it is really 1.8× above.
  - N20 was reported at 340,599 states; exactly it is 387,481.
- **Options.**
  1. Keep the cutoff (fast but inexact, with no error bound).
  2. Exact only (correct, slow on dense graphs).
  3. Exact by default, plus an opt-in pruned mode that tracks how much probability it drops.
- **Decision.** Option 3. `calculate_reliability()` is exact.
  `calculate_reliability_bounds(g, threshold)` drops branches at or below the threshold, adds up
  their probability, and returns `[lower, upper]` with `upper = lower + dropped`. That interval is
  guaranteed to contain R. Callers get the speed when they ask for it, and they know the error.
- **Result.** K₁₂ in 1.9 s with an interval narrower than 1e-9. A test checks that the interval
  contains the brute-force value on 300 random graphs, even with an aggressive threshold of 0.05.
- **Commit.** `5df2a69`. The corrected figures are in the report (`732ea1c`).
- **Lesson 3.** An approximation hidden inside an "optimisation" also corrupts every measurement
  made on top of it, including the auditor's. Measure the exact behaviour first. Make
  approximations opt-in, explicit and bounded.

### 3.7 Parser and writer

- **Seen.**
  - `e 0 1 O.9` (letter O) parsed as probability **0**, because a failed `operator>>` stores 0.
  - Trailing junk was accepted.
  - A negative count wrapped around to a huge `size_t`.
  - `write_graph` used 15 significant digits, so doubles did not round-trip.
- **Decision.** Tokenise each line; parse integers with `std::from_chars` and probabilities with a
  classic-locale stream; require every token to be consumed fully; check token counts per line
  type; cap the vertex count at `graph::max_vertices`; don't trust the declared edge count for a
  large `reserve`. Write probabilities with `max_digits10` and restore the stream's state
  afterwards.
  - `std::from_chars` for `double` was avoided because libc++ only gained it recently.
- **Result.** Ten malformed inputs are rejected with line-numbered `ParseError`s, and the
  round-trip is bit-exact. The example program's output files now show `0.90000000000000002`,
  which is the actual stored value.
- **Commit.** `5df2a69`.

### 3.8 Validation order and object lifetime

- **Seen.**
  - `lbl_calculator` built the ordering and boundary tables *before* calling `validate()`, so an
    invalid graph could index out of bounds before the check could report it.
  - `build_diagram` never validated at all.
  - `lbl_calculator` could be copied, but its `boundary_` member points at `ordering_`, so a copy
    would dangle.
- **Decision.** Validate in the constructor initialiser list
  (`net_(detail::require_valid(g, ...))`), validate in every public entry point, and delete the copy operations.
- **Commit.** `5df2a69`.

### 3.9 LeakSanitizer reported leaks in every suite

- **Seen.** Every graphrel suite failed under ASan, including the trivial unit tests.
- **How found.** The stack traces were all inside the static initialisers of
  `libboost_unit_test_framework.so`.
- **Root cause.** The test file included the header-only `boost/test/included/unit_test.hpp` *and*
  the target linked the shared Boost.Test library, so two framework instances existed. The main
  TeDDy tests already used `BOOST_TEST_DYN_LINK`.
- **Decision.** Do the same: shared library plus `BOOST_TEST_DYN_LINK`, set once in CMake for both
  test translation units.
- **Result.** Zero sanitizer reports.
- **Commit.** `5df2a69`.
- **Lesson 4.** Sanitizers check the test harness as well as the library. Confirm which side a
  report comes from before changing library code.

### 3.10 `bell_cache` sized by V+E, and silent overflow beyond F = 25

- **Seen.**
  - The Stirling-number table was built for n = V+E, i.e. O((V+E)²) memory: 1.6 GB of the
    2×4000 ladder's 2.76 GB. Yet a state's boundary can never exceed the static front.
  - Separately, B(26) > 2⁶⁴, so for boundaries of 26+ vertices the partition numbers wrap around
    and distinct states can collide. `stirling()` also returned 0 beyond the table.
- **Decision.**
  - `boundary_computer` computes the largest static front in O(V+E), and the cache is sized to
    `min(that, 25)`.
  - `bell_cache` refuses sizes above 25.
  - `canonicalize_partition` throws `std::length_error` if a state's boundary exceeds the cache.
  - Throwing is better than a wrong answer; such graphs have astronomically many states anyway.
- **Commit.** `e791a17`.

### 3.11 Boundary precomputation was O(L·V) memory and O(L·(V+E)) time

- **Seen.** Two `std::set`s were rebuilt for every level and a vertex list stored per level; one
  of the two lists was never read. An `unordered_map` keyed by dense edge ids was looked up for
  every remaining edge of every state.
- **Decision.** Two flat arrays: the level of each edge, and the first level at which each vertex
  is reached. A vertex is on a state's boundary at level l if `first_level(v) < l` and it still has
  a remaining edge at level ≥ l. That is the same definition as before (Imai's elimination front,
  restricted to the state's remaining edges), computed in O(remaining edges) per state with
  thread-local scratch buffers and no per-level tables.
- **Commit.** `e791a17`.

### 3.12 Redundant whole-graph connectivity rebuilds

- **Seen.** Every state rebuilt a whole-graph union-find to check "terminals still connectable?",
  and the standard branching did it twice more. The profile showed union-find taking about 35 % of
  the time.
- **Reasoning.**
  - States enter a layer only through operations that cannot disconnect the terminals: contraction
    never does, and deleting a non-coloop edge doesn't by definition (Imai et al., Lemma 1). The
    coloop test runs first.
  - Vertex deletion *can* disconnect, and it is still checked.
  - So one check on the initial state is enough.
- **Decision.** Remove the redundant checks in both LBL and BDD. Debug builds `assert` the
  invariant on every state, so the sanitizer CI job, which builds Debug, verifies it on every fuzz case.
- **Commit.** `e791a17`.

### 3.13 Ordering helpers were polynomially slow

- **Seen.**
  - BFS and DFS scanned all E edges for every vertex.
  - DFS recursed through `std::function`, so it could overflow the stack on long paths.
  - `estimate_max_boundary_size` was O(L²·E): 73 s on a 40×40 grid in the audit, and the 100×100
    benchmark timed out after 600 s.
- **Decision.**
  - When a vertex is emitted, only its own incident edges can become ready, so scan just those
    (adjacency order = edge-id order, which is what the old full scan produced).
  - DFS uses an explicit stack.
  - The estimator is maintained incrementally.
  - The internal helpers are no longer exported.
- **Verification.** The output must be byte-identical: BFS and DFS orderings plus estimates on
  3,036 graphs (3,000 random, including self-loops, multi-edges and disconnected graphs, plus 36
  grids), diffed against the old implementation, give 6,072 identical lines.
- **Commit.** `e791a17`.

### 3.14 CI never ran the tests

- **Seen.** The workflow only configured and built. `LIBTEDDY_BUILD_TESTS` was off in every
  preset, and Boost wasn't installed.
- **Decision.**
  - The Linux GCC and Clang jobs install Boost.Test and fmt, build with tests on, and run the
    graphrel suites (about 6 s).
  - The TeDDy core and reliability suites take about 14 minutes, so they stay out of per-push CI.
  - A new job runs a Debug ASan/UBSan build with `UBSAN_OPTIONS=halt_on_error=1`, because UBSan
    only *prints* errors by default.
  - A manual `workflow_dispatch` trigger was added.
  - Windows still only builds (no Boost on that runner).
- **Verification.** Both Linux jobs were dry-run locally with the exact commands. The workflow has
  **not yet run on GitHub**.
- **Commit.** `542bf11`.

### 3.15 Analysis findings

- **Peak layer size vs Wu & Sun's Table 2.** Every R we could reproduce matches the paper exactly,
  but the library kept more states per layer.
  - We first suspected the state definition. Inspecting states and reading Carlier & Lucet /
    Imai et al. ruled that out.
  - It is the **ordering**. BFS from a corner sweeps diagonally across a mesh; sweeping row by
    row along the short side (Carlier & Lucet's street-network numbering) gives N20 65,858 states
    (1.22× the paper) against 387,481 (7.2×) with BFS.
  - The maximum boundary size can't tell them apart: it is 11 for both.
  - This led to 3.16.

### 3.16 Cut-width ordering, now the default

- **Seen.** Orderings with the same largest boundary size (F_max) gave very different state counts
  (N20: 387,481 with BFS vs 65,858 with a row sweep; both F_max = 11). Exposing a mesh-only "row
  sweep" wouldn't help general graphs.
- **Hypothesis (from inspecting states).**
  - In a row sweep each boundary vertex has **one** unprocessed edge. A non-terminal component
    that touches the boundary once is then a dead end, and the existing pruning removes it, so
    states merge.
  - In BFS's diagonal sweep each boundary vertex has **two** unprocessed edges, so nothing is pruned.
  - So the quantity that matters should be the **edge cut**: the number of unprocessed edges
    leaving the processed part. That's 11 for the row sweep and 18 for BFS on 10×10.
- **How it was tested** (`ordering_lab.cpp`).
  - For BFS, DFS and several greedy variants, compute the cheap metrics (largest boundary,
    largest cut, total cut) and the exact LBL peak layer size and total work.
  - Cases: 12 meshes and 12 random geometric graphs, with perfect vertices and with p = 0.9.
  - Each LBL run is a separate process with time and memory limits, so a blow-up gets recorded
    instead of killing the experiment.
- **Heuristic.**
  - Greedy: repeatedly take the reached vertex that grows the cut least (deg − 2·processed
    neighbours), preferring the most recently touched vertex (continue the current line).
  - The free greedy can sweep along the *long* side of a rectangle, which is catastrophic (4×12:
    705,376 states). So it also runs forced through each neighbour of the seed (up to 16).
  - Plain BFS is always a candidate too.
  - Pick the smallest (largest cut, largest boundary, total cut). The boundary tie-break matters:
    on 8×8 two candidates tie on cut but differ 2× in states.
- **Options for the default.**
  1. Keep BFS.
  2. Make cutwidth the default.
  3. Choose per graph.
  4. Race candidates with real LBL runs.
- **Decision.** Option 2. `calculate_reliability`, `calculate_reliability_bounds` and
  `build_diagram` use `ordering_strategy::cutwidth`. `generate_variable_ordering(g)` stays BFS for
  compatibility, and BFS can be passed explicitly to reproduce Wu & Sun's setup. Why:
  - The risk is lopsided. BFS's losses reach 24–96× or run out of memory; cutwidth's worst
    measured loss is 2.2×.
  - Answers are exact either way.
  - Racing (option 4) would at least double the cost of every call.
- **Result.** Exact LBL, all edges 0.9:

  | Graph | Vertices 0.9: BFS | Vertices 0.9: cutwidth | Perfect vertices: BFS work | Perfect vertices: cutwidth work |
  |---|---|---|---|---|
  | 10×10 mesh (N20) | 387,481 states, 145 s | 65,858, 63 s | | |
  | 8×8 mesh | 19,884, 3.7 s | 5,434, 2.0 s | 184,748 (0.7 s) | 406,609 (1.2 s) — **worse** |
  | 7×7 mesh | 4,578 | 1,595 | 45,775 | 87,618 — **worse** |
  | 6×10 mesh | 4,582 | 891 | 69,453 | 41,158 |
  | geo40_1 | 162,075, 8.5 s | 1,683, 0.11 s | 450,595 | 19,296 |
  | geo40_2 | out of memory (> 6 GB) | 8,758, 0.5 s | timeout | 74,926 |
  | geo40_3 | out of memory | 70,342, 6.4 s | out of memory | 419,662 |
  | geo40_4 | 329,038, 32 s | 478,760, 45 s — **worse** | 1,712,364 | 2,695,487 — **worse** |
  | geo40_6 | 661,287, 36 s | 182,910, 7.4 s | 1,040,216 | 643,195 |
  | geo60_1 | 1,329,800, 125 s | 54,528, 8.4 s | | |
  | geo60_2 | 2,399,606, 294 s | 908,647, 129 s | | |

  On the other cases measured, cutwidth was equal or better.

  - BDD node counts: 7×7 with vertices at 0.9 goes from 108,702 to 76,522; the 2×500 ladder from
    5,486 to 3,492; but 8×8 with perfect vertices goes from 84,885 to 180,574.
  - The guard test checks that cutwidth halves G^max on 7×7 and sweeps a 4×12 mesh along its
    short side. Fuzzing now covers BFS explicitly, since it's no longer the default.
- **Known weak spot.** Square meshes with perfect vertices: the peak is the same, but the row sweep
  holds it for more layers, so total work is up to 2.2× higher. The cut proxy doesn't see layer
  *duration*. See §7.
- **Commit.** `f4c3dc1`.
- **Lesson 10.** A proxy metric must be validated across regimes. The cut proxy is excellent with
  imperfect vertices and weaker with perfect ones. We only found that by measuring total work as
  well as the peak.
- **Imai Theorem 1 gap.** Imai's theorem identifies two minors by their partition *and* the same
  remaining edge set. The library's key leaves out the edge set. Fuzzing found no counterexample,
  but nothing proves it safe. Open.
- **Hash-only state identity.** Two states could in principle collide on the 64-bit boundary hash.
  Open; the frontier-local redesign removes it.

---

## 4. Results

### Correctness: brute-force fuzzing, 10,000 graphs per mode

| Mode | LBL before | BDD before | LBL after | BDD after |
|---|---|---|---|---|
| Paper assumptions | 0 | 0 | 0 | 0 |
| + some p = 1 edges | 0 | 545 | 0 | 0 |
| Anything, \|K\| ≥ 2 | 0 | 9 | 0 | 0 |
| Anything, incl. \|K\| = 1 | 0 | 963 | 0 | 0 |

The CI test runs 1,500 graphs per mode, with DFS and random orderings as well.

### Performance (Release build, every output identical)

| Case | Before | After |
|---|---|---|
| 2×2000 ladder, LBL | 11.6 s, 693 MB | 2.3 s, 5 MB |
| 2×4000 ladder, LBL (audit) | 69.9 s, 2.76 GB | not re-run; memory now flat |
| 6×40 mesh, LBL | 5.0 s | 3.0 s |
| 2×500 ladder, BDD | 0.66 s, 100 MB | 0.21 s, 50 MB |
| Ordering helpers, 30×30 | 12.1 s | < 0.01 s |
| Ordering helpers, 100×100 | > 600 s (timed out) | < 0.01 s |
| N20 (10×10, p = 0.9), exact LBL | — | 140 s, 1.0 GB (paper: 42.9 s) |
| K₁₂, exact LBL | 104 s | 75.5 s, 1.2 GB |
| K₁₂, `calculate_reliability_bounds(1e-15)` | — | 1.9 s, interval width < 1e-9 |
| N20, default ordering (cutwidth, `f4c3dc1`) | 387,481 states, 145 s (BFS) | 65,858 states, 63 s |
| geo40_1 (random geometric, 40 vertices) | 162,075 states, 8.5 s (BFS) | 1,683 states, 0.11 s |

### Reproducing Wu & Sun Table 2

R matches exactly for the bridge, N6, N7, N8, N9, N20 and N27. These are pinned in
`graphrel_paper_test`; N20 is excluded there because it takes minutes.

### Tests

- 8 graphrel suites, about 6 s.
- Clean under GCC, Clang and ASan/UBSan.
- The existing TeDDy suites still pass.

---

## 5. What we learned

1. **Know which reference governs which code before judging it.** See §1.
2. **Two implementations agreeing is not a test oracle.** The old tests mostly checked
   LBL == BDD, which passed while the BDD had real bugs. A brute-force oracle on graphs small
   enough to enumerate found them within seconds. Keep it in CI.
3. **Separate "bug" from "unenforced precondition".** Several failures came from inputs outside
   the method's assumptions (|K| = 1, orderings that break vertex priority, unsorted terminals).
   The fix is to enforce or define the precondition, not to patch the algorithm. Fuzz modes that
   mirror the assumptions (`PAPER`, `KMIN2`, `any`) make the distinction measurable.
4. **Hidden approximations corrupt measurements.** See 3.6. Two of the audit's own figures were wrong because of the cutoff.
5. **Prove that refactors don't change outputs.** Every performance change was checked with
   identical R, peak layer size, boundary size, BDD node counts, and byte-identical orderings.
   That's what makes it safe to speed up research code.
6. **Measure before explaining.** The peak-layer gap was first blamed on the state definition. An
   experiment (ordering sweeps) showed it was the ordering. The maximum boundary size alone
   doesn't predict state counts.
7. **Unrun tests stop compiling.** See 3.1 and 3.14.
8. **Sanitizers check the harness too.** See 3.9.
9. **Keep mistakes in the record.** The first audit headline ("BDD wrong on 10 % of graphs")
   mostly counted inputs outside both methods' scope, and the K₁₂ statement was wrong. Both are
   corrected in the report rather than silently edited.
10. **Validate a proxy metric across regimes.** See 3.16: the edge cut predicts state counts well
    with imperfect vertices and less well with perfect ones. Measuring total work as well as the
    peak exposed this.

---

## 6. Reproducing the evidence

```bash
# Build and run every graphrel suite (needs libboost-test-dev and libfmt-dev)
cmake --preset linux-release -DLIBTEDDY_BUILD_TESTS=ON
cmake --build --preset linux-release
ctest --test-dir build -R graphrel --output-on-failure
```

```bash
# Sanitizer build, as in CI
cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLIBTEDDY_BUILD_TESTS=ON -DLIBTEDDY_BUILD_GRAPHREL=ON -DLIBTEDDY_USE_SANITIZERS=ON
cmake --build build/asan --target graphrel-test
UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/asan -R graphrel --output-on-failure
```

```bash
# Stand-alone harnesses in this folder (link against build/libgraphrel/libgraphrel.a)
g++ -std=c++20 -O2 -I. -Ilibgraphrel/include -Ilibgraphrel/src libgraphrel/audit/fuzz.cpp build/libgraphrel/libgraphrel.a -o fuzz
PAPER=1 ./fuzz 10000
```

| Harness | Purpose |
|---|---|
| `fuzz.cpp` | Brute force vs LBL/BDD and four kinds of ordering; modes `PAPER`, `PAPER PERFECT`, `KMIN2`, default |
| `probe.cpp` | Minimal repros of the original findings, plus scaling runs |
| `paper.cpp` | Wu & Sun Table 2 networks: R, peak layer size, boundary size, time, memory |
| `order_sens.cpp` | How BFS/DFS tie-breaking affects the peak layer size |
| `sweep.cpp` | BFS vs row sweep; `sweep numbering` checks Carlier & Lucet Algorithm 2 exhaustively |
| `ordering_lab.cpp` | Ordering candidates: cheap metrics (cut, boundary) vs exact peak layer size and total work; `LAB_PV` sets vertex probability |

Note: `probe.cpp`, `order_sens.cpp` and `paper.cpp` were written against the original API. Some
of their inputs (unsorted terminals, invalid orderings) are now rejected by design. Since
`f4c3dc1`, `lbl_calculator(g)` uses the cutwidth ordering, so `paper.cpp` reports cutwidth state
counts. The BFS figures in this log came from BFS orderings.

---

## 7. Open items and next steps

In recommended order. The [`README.md`](README.md) status table lists each finding's state.

1. **Ordering heuristic: done** (3.16). Open refinements:
   - capture layer duration, not just peak cut (perfect-vertex square meshes);
   - order the edges within each vertex's emission (it changes G^max: 5×5 gives 146 vs 208
     depending on edge-id order);
   - optionally race the top two candidates with capped LBL runs.
2. **Cleanup** (audit 1.9, 2.5–2.7, §3):
   - remove dead code and the duplicate `src/common/variable_ordering.hpp`;
   - merge the `graph_operations` variants;
   - fix the docs and credit the BDD builder to Hardy et al. rather than Wu & Sun;
   - fix the warnings;
   - fix `state_pool` exception safety.
3. **Frontier-local states (redesign).** Store only the boundary partition, K-marks and up/down
   flags instead of the whole graph. That makes per-state cost O(F) instead of O(V+E), makes keys
   exact (closing the Imai Theorem 1 gap and the hash-identity risk), and is the only route to the
   paper's 1000×10 meshes (N25/N26) within 540 MB.
4. **BDD builder.** Review against Hardy, Lucet & Limnios (2007) once available; build it layer by
   layer to remove the recursion-depth risk on MSVC.
5. **Housekeeping.** Run CI on GitHub once via the manual trigger.

---

## Glossary

| Term | Meaning |
|---|---|
| K, terminals | Vertices that must all be up and connected |
| LBL | Layer-by-layer decomposition (Wu & Sun 2024): process one variable per layer, merging equivalent states |
| BS-BDD | Boundary-set based BDD (Hardy et al. 2007), built top-down with memoisation |
| Boundary set F | Reached vertices that still have unprocessed edges (Imai's "elimination front") |
| G^max | Largest number of states in any layer (Wu & Sun's B¹); drives memory and time |
| Vertex priority | Every vertex appears before its incident edges in the ordering |
| Coloop | Edge whose deletion disconnects the terminals; it must work |
| Ghost edge | Edge already removed because an endpoint failed |
| k-NR-IV, R_EV | k-terminal reliability with imperfect vertices; R_EV forces edges at failed vertices to 0 (Kawahara et al.) |
