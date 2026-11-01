# Auditing and Re-engineering libgraphrel: Exact K-Terminal Network Reliability with Frontier-Local Layer-by-Layer Decomposition

**Aral** — technical report and handoff, November 2026

Branch `feat/libgraphrel-optimisations` of the DecisionDiagrams (TeDDy) fork. Audit baseline: commit `39fe87d`.

---

## Abstract

libgraphrel computes the K-terminal reliability of networks with unreliable edges *and*
vertices. It offers two methods: the layer-by-layer decomposition (LBL) of Wu & Sun (2024), and
a boundary-set binary decision diagram (BS-BDD) whose structure-function view enables TeDDy's
importance analysis.

We audited the library against its four governing references, using a brute-force oracle,
differential fuzzing restricted to each method's stated assumptions, and reproduction of the
published benchmarks. The audit found:
- a semantic error in the BDD construction (edges incident to failed vertices contracted);
- unenforced preconditions that turned invalid inputs into silently wrong answers;
- a hidden probability cutoff that made results inexact and distorted every performance
  measurement taken on top of it;
- costs quadratic in graph size;
- a test suite that no longer compiled.

We then re-engineered the library in verified steps.
1. The preconditions are now enforced and the approximation is opt-in with a rigorous error bound.
2. A cut-width variable ordering replaces breadth-first search. On the paper's 10×10 mesh it cuts
   the peak number of states per layer from 387,481 to 65,858.
3. A **frontier-local state representation** replaces whole-graph states. It stores, per state,
   only the partition of the processed/unprocessed boundary, the terminal marks and the vertex
   failures: a few bytes instead of O(|V|+|E|).

Every result is unchanged: 0 mismatches against exhaustive enumeration on 40,000 random graphs,
and identical canonical BDD node counts. The 10×10 mesh now takes 4.8 s and 11 MB, against 145 s
and 1 GB before the redesign and 42.9 s in the paper. Exact K₁₂ takes 8.7 s and 164 MB, against
75 s and 1.2 GB. The 12×12 mesh takes 131 s, against 1,003 s in the paper. The paper's largest
mesh, 1000×10 (10,000 vertices), was previously out of reach. It now reproduces the published
reliability exactly in 664 s and 20 MB, against 8,066 s and 540 MB in the paper.

---

## 1. Introduction

### 1.1 Problem

Given an undirected graph G = (V, E), independent working probabilities p(e) and p(v), and a
terminal set K ⊆ V, the **K-terminal reliability with imperfect vertices** (k-NR-IV) is the
probability that every terminal works and all terminals are connected by working edges through
working vertices. The problem is #P-complete, so exact methods trade exponential cost in a
structural width for polynomial cost in graph size.

### 1.2 The two methods in libgraphrel

| Entry point | Method | Governing reference |
|---|---|---|
| `calculate_reliability` | Layer-by-layer recursive decomposition (LBL) | Wu & Sun (2024) |
| `build_diagram` | Boundary-set BDD (a TeDDy `bss_manager` diagram) | Hardy, Lucet & Limnios (2007); used as baseline in Wu & Sun §2.3 |
| Shared machinery | Boundary sets, partitions, loop/coloop rules | Carlier & Lucet (1996); Imai, Sekine & Imai (1999) |
| Vertex-failure semantics, variable order | k-NR-IV, R_EV | Kawahara, Sonoda, Inoue & Kasahara (2019) |

### 1.3 Contributions

1. An audit methodology for exact reliability code. The oracle is brute force on small graphs,
   with fuzzing modes that mirror each method's assumptions. This separates genuine defects from
   inputs outside a method's scope.
2. A catalogue of defects with root causes, plus the corresponding fixes and regression tests.
3. Proof by measurement that the performance changes preserve behaviour: identical orderings,
   layer sizes and canonical BDD node counts.
4. A cut-width ordering heuristic, and the empirical finding that the **edge cut**, not the
   boundary size, predicts the state count.
5. A frontier-local engine shared by LBL and BDD. It has exact state keys, O(F) work per state,
   and no Bell-number ceiling.
6. A traceable record of every decision, including the mistakes and how they were corrected.

---

## 2. Background

### 2.1 Decomposition and isomorphic subproblems

Processing the components in a fixed order and branching on each one (Shannon expansion,
R(G) = p·R(G | x works) + (1−p)·R(G | x fails)) yields a binary tree with exponentially many
leaves. Exact methods become practical by **merging** subproblems that are guaranteed to have the
same future:
- BDDs share isomorphic nodes;
- LBL merges isomorphic subgraphs within a layer and adds their probabilities (Wu & Sun 2024, Eq. 4).

### 2.2 Why the boundary is enough

At any point in the decomposition the graph splits into the processed part (variables already
decided) and the unprocessed part. Two kinds of information are involved:
- **Static information**, stored once and shared by all states: the graph, the probabilities,
  the ordering, and facts derived from them, such as the level at which each vertex sees its last
  edge. All states at the same layer have the same unprocessed part.
- **Dynamic information**, per state: the outcomes of the processed variables. Most of it can be
  forgotten.

The **front** (boundary set) at a level is the set of processed vertices that still have
unprocessed edges (Carlier & Lucet 1996 §4.1; Imai et al. 1999's elimination front).

**Proposition 1 (sufficiency of the front state).** Let x and y be two outcomes of the processed
variables that agree on:
- (a) which front vertices are up;
- (b) the partition of the up front vertices into blocks connected through the processed part;
- (c) which blocks contain a terminal.

Suppose also that neither x nor y has already failed or succeeded. Then for every outcome z of
the unprocessed variables, the system works under (x, z) if and only if it works under (y, z).

*Proof sketch.* Take any path between two terminals in the working subgraph under (x, z). Split
it into maximal segments that use only processed edges and segments that use only unprocessed
edges. A processed segment can leave the processed region only at a front vertex, because
interior processed vertices have no unprocessed edges. So each processed segment either connects
two front vertices of the same block, or connects a terminal to a front vertex of a terminal
block. Unprocessed segments depend only on z and on which front vertices are up. Hence (a)–(c)
determine every possible connection, and the same path structure exists under (y, z). Terminals
inside the processed region are represented by the marks (c). A terminal block that lost its
last front vertex was already resolved as success or failure, which the hypothesis excludes. ∎

So two states with equal (a)–(c) are interchangeable, and merging them with summed probabilities
is exact. This is the isomorphism rule in Carlier & Lucet (§4.4, §5.2), Imai et al. (Theorem 1)
and Wu & Sun (§2.3).

**Lemma 2 (dead ends).** A block with no terminal and at most one usable unprocessed edge cannot
lie on a path between terminals: such a path would have to enter and leave the block through two
different edges. Marking its vertices absent changes no future outcome. ∎

**Example.** The bridge of Wu & Sun Fig. 1: K = {v1, v4}, ordering v1, v2, e1, v3, e2, e3, v4,
e4, e5, all probabilities 0.9. After e3, v1 has no unprocessed edges, so the front is {v2, v3}.
The live states are:
- `[v2 v3]*` — both connected to v1;
- `[v2]* [v3]` — only v2 reaches v1;
- `[v2] [v3]*` — only v3 reaches v1;
- `[v2]*` with v3 down;
- `[v3]*` with v2 down.

By Lemma 2, `[v3]` in the second state has no terminal and a single remaining edge (e5), so it is
equivalent to v3 being down. The same holds for the third state. That leaves three states, which
is what the implementation produces at that layer.

### 2.3 Assumptions of the references

| Assumption | Wu & Sun | Carlier & Lucet | Imai et al. | Kawahara et al. |
|---|---|---|---|---|
| G connected | §2.1 (1) | §2 | §2.1 | §2.1 |
| Independent two-state components | §2.1 (2–3) | §4.3 | §2.1 | §2.1 |
| 2 ≤ \|K\| ≤ \|V\| | §2.1 | §2 | all-terminal | k = \|T\| |
| Vertex before its incident edges ("vertex priority") | §2.3, Fig. 4 | vertex numbering | edges only | §3.1 |
| Failed vertex ⇒ incident edges unusable | §2.3 | §5.2 | — | §3.1 (R_EV) |

---

## 3. Method

### 3.1 Oracle and differential fuzzing

For graphs with V + E ≤ 16, we enumerate all 2^(V+E) component states. On each random graph
(2–7 vertices, up to 9 edges) we compare:
- `calculate_reliability` with the default ordering, BFS, DFS, and random orderings that respect
  vertex priority;
- `calculate_reliability` with fully random orderings, which must be rejected;
- `build_diagram`.

There are four modes:

| Mode | Graphs generated |
|---|---|
| `PAPER` | Exactly the references' assumptions: connected simple graphs, 2 ≤ \|K\|, all p in (0, 1) |
| `PAPER PERFECT` | As above, plus edges with p = 1 |
| `KMIN2` | Anything (disconnected graphs, loops, multi-edges, p ∈ {0, 1}) with \|K\| ≥ 2 |
| default | Anything, including \|K\| = 1 |

The modes separate defects from out-of-scope inputs (§4).

### 3.2 Targeted reproduction and profiling

Each defect has a minimal reproducing graph. Scaling was measured on ladders, meshes, random
geometric graphs and complete graphs, with callgrind profiles on a 7×7 mesh.

### 3.3 Reference reproduction

We reproduced Wu & Sun Table 2: the bridge (Fig. 1–3), N6–N9 (identified as meshes from |V| and
|E|), N20–N26 and N27–N28. All edges and vertices have probability 0.9, and the two terminals are
the most distant pair (opposite corners). Shared code was checked against Carlier & Lucet's
Algorithms 1–2 over all 142,417 partitions with |F| ≤ 10.

### 3.4 Behaviour-preservation protocol

Any change meant to affect only cost had to reproduce these exactly, on fixed benchmark sets:
- reliability values;
- peak layer size G^max and largest front;
- canonical BDD node counts;
- generated orderings, byte for byte (3,036 graphs).

### 3.5 Measurement protocol

- Release builds (GCC 13, `-O2`).
- Timings are best of 5 where differences were small.
- Each long run is a separate process with time and memory limits, so a blow-up is recorded
  rather than crashing the experiment.
- Peak resident memory comes from `getrusage`.
- Sanitizer builds (ASan + UBSan, halt on error) are run for every change.

---

## 4. Audit findings

| # | Finding | Evidence | Root cause | Class |
|---|---|---|---|---|
| A1 | Test suite did not compile | build error | `decomp_state::union_find` renamed to `uf_`; CI never built tests | infrastructure |
| A2 | BDD connected through failed vertices | 0.75 → 1.0 on a 3-vertex graph; 545/10,000 in `PAPER PERFECT` | p = 1 shortcut ran before the "edge removed by vertex failure" check; violates R_EV | **defect** |
| A3 | \|K\| = 1 inconsistent | BDD 1.0 vs exact 0.3 | "connected" assumed for \|K\| < 2 | outside scope; undefined |
| A4 | Unsorted terminals gave wrong answers | 0.855 vs 0.405 | `binary_search` on an unvalidated list | unenforced precondition |
| A5 | Arbitrary custom orderings accepted | 0.81 / 0.0 vs 0.405 | vertex priority not checked (8,068/10,000 wrong) | unenforced precondition |
| A6 | Probability cutoff 1e-15 | 1.2e-6 relative error in 1−R; K₁₂ silently lost ~97 % of its states | hidden approximation | **defect** (inexact) |
| A7 | Parser accepted `O.9` as 0, trailing junk, negative counts; writer lost precision | repros | `operator>>` failure semantics; 15 digits | defect |
| A8 | Validation after use; movable object with dangling reference; pool not exception-safe | review | member order; destroy + placement-new | defect (latent) |
| A9 | Quadratic memory and time | 2×4000 ladder: 70 s, 2.76 GB | Stirling table sized V+E; per-level tables; whole-graph checks per state | performance |
| A10 | Ordering helpers polynomially slow | 40×40: 73 s | full edge scans; O(L²E) estimator | performance |
| A11 | State identity by 64-bit hash; partition numbers overflow beyond \|F\| = 25 | review | hash-only keys; B(26) > 2⁶⁴ | soundness risk |
| A12 | More states per layer than Wu & Sun on meshes | N20: 387,481 vs 53,924 | ordering (§5.4) | performance |
| A13 | Dead code, duplicated operations, wrong attribution, 15 warnings | review | — | quality |

Within the references' assumptions (`PAPER`), both methods were already correct (0/10,000). Every
correctness failure needed either an input outside those assumptions or a p = 1 edge.

---

## 5. Interventions

Each intervention lists the alternatives considered and why the chosen one won. Commits are on
`feat/libgraphrel-optimisations` (Appendix A).

### 5.1 Correctness and preconditions (`a29ca5e`, `1ef297d`)

- **A2.** Check for an edge removed by a vertex failure *before* the p = 1 shortcut. Rejected
  alternatives: dropping the shortcut (it saves BDD variables), and pre-contracting p = 1 edges
  (a larger change).
- **A3.** Define R = p(t) for |K| = 1 on both paths, and reject |K| = 0. The existing test showed
  |K| = 1 was meant to be supported. Rejecting it, as the references do, remains a one-line option.
- **A4.** `validate()` now checks that terminals are sorted, unique and non-empty, rejects NaN,
  and checks edge ids and that adjacency lists are complete. A new `set_terminals()` normalises the
  list. Silently sorting a `const graph&` would have required a copy.
- **A5.** Custom orderings must contain every variable exactly once and respect vertex priority,
  checked in O(V+E). Fuzzing showed this, together with |K| ≥ 2, is the exact precondition:
  BFS, DFS and random vertex-priority orderings all gave 0/10,000 mismatches.
- **A6.** Exact by default. The cutoff becomes `calculate_reliability_bounds(g, threshold)`, which
  adds up the probability it drops and returns an interval [lower, lower + dropped] guaranteed to
  contain R. Rejected: keeping the cutoff (inexact with no bound), and exact-only (dense graphs
  lose a large speed-up for no gain in practice).
- **A7.** Tokenised strict parsing (`from_chars`, classic-locale reals, full token consumption),
  and `max_digits10` output.
- **A8.** Validate in constructor initialiser lists; delete copy and move operations.
- **Tests and CI (`9771441`).** Brute-force fuzz, precondition, parser and paper suites (about
  6 s); Linux GCC and Clang jobs that run them; an ASan/UBSan job with `halt_on_error`. The
  sanitizer job exposed a harness defect: the header-only and shared Boost.Test were mixed, giving
  two framework instances.

### 5.2 Removing quadratic costs without changing results (`0a43644`)

- Size the Stirling table by the largest static front, capped at 25, and throw beyond it rather
  than overflow.
- Replace the per-level boundary tables by two flat arrays.
- Make the ordering helpers O(V+E).
- Remove the whole-graph connectivity checks that are provably redundant: deleting a non-coloop
  edge cannot disconnect the terminals, and contracting an edge never does (Imai et al. Lemma 1).
  Debug builds assert the invariant.

Verification: identical outputs (§3.4). Result: 2×2000 ladder 11.6 s / 693 MB → 2.3 s / 5 MB;
ordering helpers on 100×100: more than 600 s → under 10 ms.

### 5.3 Cleanup (`195280d`, `63bf4fb`)

Dead code, the three copies of each graph operation, attribution (the BDD builder credited to
Hardy et al.), and warnings (15 → 0). The `state_pool` now recycles states by assignment: this is
exception-safe and reuses capacity, 11–20 % faster. Behaviour unchanged (§3.4).

### 5.4 Cut-width ordering (`7a98b40`)

**Observation.** On the 10×10 mesh, BFS and a row-by-row sweep have the same largest front
(F = 11) but 387,481 vs 65,858 states.

**Hypothesis.** With a row sweep, each front vertex keeps one unprocessed edge, so blocks touching
the front once are dead ends (Lemma 2) and merge. With BFS's diagonal sweep each front vertex keeps
two. The governing quantity should be the **edge cut**, the number of unprocessed edges leaving the
processed part: 11 vs 18.

**Heuristic.**
- Greedily take the reached vertex that grows the cut least (deg − 2·processed neighbours),
  preferring to continue the current line.
- Run it from the seed terminal, freely and forced through each neighbour (up to 16). The forced
  runs decide the sweep direction: the long side of a rectangle is catastrophic (4×12: 705,376
  states).
- Include BFS as a candidate.
- Choose the smallest (largest cut, largest front, total cut).

**Evaluation** (`ordering_lab.cpp`). 12 meshes and 12 random geometric graphs, with perfect and
imperfect vertices. Measured on the previous engine:

| Graph (vertices 0.9) | BFS states | Cut-width states |
|---|---|---|
| 10×10 mesh (N20) | 387,481 | 65,858 |
| 6×10 mesh | 4,582 | 891 |
| geo40_1 | 162,075 | 1,683 |
| geo40_2 | out of memory (> 6 GB) | 8,758 |
| geo60_1 | 1,329,800 | 54,528 |
| geo40_4 | 329,038 | 478,760 (**worse**) |

With perfect vertices, square meshes do up to 2.2× more total work, because the peak is the same
but lasts longer. The edge cut does not capture how long a layer stays wide.

**Decision.** Make cut-width the default. BFS's failures reach 24–96× or run out of memory, while
cut-width's worst case observed is 2.2×. BFS stays available to reproduce the paper's setup exactly.

### 5.5 Frontier-local engine (`6c37164`, `7916203`, `b389dcb`)

**Motivation.** The key already used only front information (Proposition 1), but each state
*stored* a union-find over all V vertices and a list of all remaining edges. Every copy and check
cost Θ(V+E), and the key needed a hash of the boundary vertex set (A11).

**State.** Per front position, one byte: 0 = absent (failed or pruned), otherwise a block label,
canonical by first appearance. Plus a 64-bit mask of terminal blocks. Size: 8 + F bytes. Keys are
compared byte for byte, so no hash collision can merge two states, and there is no Bell-number
ceiling. The front limit is 64, compared with 25 before.

**Static plan**, computed once:
- the front at each level;
- for each edge, its endpoints' positions and whether each endpoint leaves afterwards;
- remaining-edge counts;
- unprocessed edges between front vertices;
- terminals not yet reached;
- a look-ahead table (below).

**Transition** (Algorithm 1). Applying `ordering[l]` to a state:

```
vertex v:  works  -> v joins the front as a new block (terminal-marked if v ∈ K)
           fails  -> v ∈ K ? FAILURE : v is absent (its edges become unusable)
edge (u,v): if u or v absent, or same block  -> single child, state unchanged
           works  -> merge the blocks of u and v
           fails  -> unchanged
then, for each child:
  1. departures: an endpoint with no edges left leaves; a terminal block losing its last
     front vertex is SUCCESS if it is the only terminal block and no terminal is
     unreached, else FAILURE
  2. usable exits: an edge to an absent front vertex is unusable; an edge inside a block
     is internal. Until stable:
       terminal block with no exits      -> SUCCESS / FAILURE as in 1
       non-terminal block, <= 1 exit     -> absent                      (Lemma 2)
       vertex with no exit               -> absent (its block keeps the others)
  3. canonical relabelling
  4. look-ahead (only on levels where the unprocessed graph is disconnected): FAILURE if
     terminal blocks and unreached terminals cannot be joined through blocks and future
     components
  5. SUCCESS if all terminals are reached and lie in one block
```

**Two refinements, each motivated by measurement.**
- *Usable exits (step 2).* The first version counted exits statically. With vertex failures it
  kept 1.6× more states than the old engine (8×8: 8,866 vs 5,434). The cause: a front vertex
  whose remaining edges all lead to failed vertices was still treated as live. Counting only
  usable exits restored the old counts exactly. On 6×40 it also went *below* them (621 vs 801),
  because internal edges are now recognised.
- *Look-ahead (step 4).* On meshes it removed no states and cost 30 % in time. On geo60_1 it
  removed 24 % at no time cost. It can only remove a state where the unprocessed graph is split
  into several components, so the plan enables it only on those levels: no cost on meshes, and
  the gain is kept.

**BDD.** The same engine drives a two-pass construction. The forward pass records each state's
successors, 16 bytes per state. The backward pass creates the TeDDy nodes from the last layer up.
There is no recursion, so the stack-depth risk on MSVC is gone. Because a reduced, ordered BDD is
canonical, node counts must be identical to the old builder's for the same ordering. They are, on
every case checked (§6.2).

**Removal.** The old engine and its unit tests were deleted (`b389dcb`). Two new tests cover what
only the frontier engine can do: a front of 31 vertices (R = 1 − 0.5³⁰ exactly) and rejection of
fronts above 64.

---

## 6. Results

### 6.1 Correctness

Brute-force fuzzing, 10,000 graphs per mode:

| Mode | LBL, original | BDD, original | LBL, final | BDD, final |
|---|---|---|---|---|
| `PAPER` | 0 | 0 | 0 | 0 |
| `PAPER PERFECT` | 0 | 545 | 0 | 0 |
| `KMIN2` | 0 | 9 | 0 | 0 |
| default (incl. \|K\| = 1) | 0 | 963 | 0 | 0 |

In the final version, BFS, DFS and random vertex-priority orderings all give 0 mismatches, and
every ordering that violates vertex priority is rejected. Sanitizer runs are clean throughout.

### 6.2 Canonical BDD check

| Case | Old builder: nodes, time, memory | Frontier builder: nodes, time, memory |
|---|---|---|
| 2×500 ladder | 3,492, 0.14 s, 49 MB | 3,492, 0.03 s, 40 MB |
| 6×6 mesh, vertices 0.9 | 15,639, 0.05 s | 15,639, 0.01 s |
| 7×7 mesh, vertices 0.9 | 76,522, 0.48 s, 23 MB | 76,522, 0.07 s, 11 MB |
| 8×8 mesh | 180,574, 1.23 s, 57 MB | 180,574, 0.14 s, 15 MB |
| 3×20 mesh, vertices 0.9 | 1,288 | 1,288 |
| 5×12 mesh | 6,358 | 6,358 |

### 6.3 Performance progression (LBL, exact)

| Case | Audit baseline | After §5.2 | After §5.4 | Frontier (§5.5) |
|---|---|---|---|---|
| 2×2000 ladder | 17.9 s, 692 MB | 2.3 s, 5 MB | 1.5 s | < 0.01 s, 4 MB |
| 6×40 mesh | 7.5 s (with cutoff) | 3.0 s | 1.3 s | 0.05 s (621 states) |
| 8×8 mesh, vertices 0.9 | — | 3.7 s (BFS) | 1.66 s | 0.22 s, 4 MB |
| 10×10 mesh (N20) | 200.8 s, 882 MB (BFS, with cutoff) | 145 s, 1.0 GB (BFS) | 63 s | **4.8 s, 11 MB** |
| K₁₂ (N27) | 2.2 s with cutoff (inexact) | 75.5 s, 1.2 GB | 75.5 s | **8.7 s, 164 MB** |
| geo40_4 | — | 32 s (BFS) | 45 s | 3.2 s |

### 6.4 Reproducing Wu & Sun, Table 2

All edges and vertices 0.9, terminals at maximum distance, exact computation. Wu & Sun's times
are from an i5-7300 laptop; ours from one core of a cloud VM. "BFS" uses the paper's ordering;
"cut-width" is the library default.

| Net | Graph | Paper R | Our R | Paper B¹ | G^max, BFS | G^max, cut-width | Paper time | Our time (cut-width) | Our peak memory |
|---|---|---|---|---|---|---|---|---|---|
| — | bridge, Fig. 1 | 0.760078728 | 0.760078728 | 5 (Fig. 3) | 5 | — | — | < 0.01 s | 3 MB |
| N9 | 2×20 mesh | 0.230160 | 0.230160 | 4 | 5 | 5 | 0.0 s | < 0.01 s | 3 MB |
| N6 | 3×10 mesh | 0.609632 | 0.609632 | 12 | 19 | 15 | 0.0 s | < 0.01 s | 3 MB |
| N7 | 3×12 mesh | 0.579399 | 0.579399 | 12 | 19 | 15 | 0.0 s | < 0.01 s | 3 MB |
| N8 | 5×5 mesh | 0.711988 | 0.711988 | 118 | 253 | 146 | 0.0 s | < 0.01 s | 3 MB |
| N20 | 10×10 mesh | 0.714299 | 0.714299 | 53,924 | 387,481 (10.4 s) | 65,858 | 42.9 s | 4.8 s | 11 MB |
| N21 | 11×11 mesh | 0.714418 | 0.714418 | 191,216 | — | 233,206 | 215.1 s | 22.7 s | 26 MB |
| N22 | 12×12 mesh | 0.714482 | 0.714482 | 684,114 | — | 833,340 | 1,003.4 s | 131.3 s | 97 MB |
| N23 | 13×13 mesh | 0.714515 | 0.714515 | 2,466,428 | — | 3,001,316 | 4,722.2 s | 604.4 s | 362 MB |
| N25 | 1000×10 mesh | 0.673028 | 0.673028 | 53,924 | — | 65,858 | 8,066.5 s (540 MB) | 663.5 s | **20 MB** |
| N26 | 1200×10 mesh | 0.664961 | 0.664961 | 53,924 | — | 65,858 | 9,082.6 s (540 MB) | 787.4 s | **23 MB** |
| N27 | K₁₂ | 0.810000 | 0.810000 | 816,465 | — | 1,459,298 | 72.9 s | 8.7 s | 164 MB |
| N28 | K₁₃ | 0.810000 | 0.810000 | 5,460,095 | — | 9,388,752 | 555.8 s | 72.7 s | 1.0 GB |

- Every R reproduced matches the paper to all published digits.
- On meshes, cut-width stays within 1.2–1.25× of the paper's B¹ (N20–N23: 1.22×) while BFS is up
  to 7.2× above it.
- Run times are 7–12× below the paper's on every mesh, and memory 27× below on N25/N26.
- Memory is bounded by the widest layer only.

---

## 7. Discussion

### 7.1 What explained the gaps to the paper

The gaps were not in the state definition. They came from:
1. **ordering** (§5.4);
2. **the cutoff**, which made earlier counts look *better* than the paper's (K₁₂ appeared 19×
   below it; exact, it is 1.8× above);
3. **per-state overhead**, which the frontier engine removes.

The remaining gap to the paper's B¹ is about 1.2× on meshes. Most likely it comes from tie-breaking
in the authors' ordering, which the paper does not specify. On the complete graphs K₁₂ and K₁₃, where
the ordering is irrelevant by symmetry, our layers are 1.7–1.8× wider, yet the runs are 8× faster. The
paper's state definition may merge more aggressively on dense graphs; we did not investigate this
further.

### 7.2 Threats to validity

- The brute-force oracle covers graphs with V + E ≤ 16. Larger graphs are covered only indirectly:
  by agreement between independent methods and orderings, and by reproducing published values.
- The look-ahead and usable-exit rules are proved informally (Proposition 1, Lemma 2). Their
  soundness rests on those arguments and on the fuzzing.
- The ordering heuristic was evaluated on meshes and random geometric graphs; other topologies may
  behave differently. Cut-width loses on square meshes with perfect vertices.
- Timing comparisons with the paper cross hardware generations.
- The CI workflow has been dry-run locally but not yet executed on GitHub.
- Hardy, Lucet & Limnios (2007) was not available. The BDD builder was checked against Carlier &
  Lucet, Imai et al. and Kawahara et al., and against itself through canonical node counts.

### 7.3 Lessons

1. Establish which reference governs which code before judging it.
2. Agreement between two implementations is not an oracle. The original tests compared LBL with
   BDD and passed while the BDD was wrong.
3. Separate defects from unenforced preconditions. Fuzzing modes that mirror the assumptions make
   this measurable.
4. Hidden approximations corrupt every measurement built on them, including the auditor's.
5. Prove that refactors preserve outputs: identical orderings, layer sizes and canonical node
   counts.
6. Measure before explaining. The state-count gap was first attributed to the state definition;
   it was the ordering.
7. Validate proxy metrics across regimes. The edge cut predicts states well with imperfect
   vertices, less well with perfect ones.
8. Unrun tests stop compiling, and sanitizers check the harness as well as the code.
9. Keep corrections in the record (Appendix D).

---

## 8. Conclusion and future work

libgraphrel is now exact by default, enforces its preconditions, and runs both methods on a
frontier-local engine. That engine has exact keys and O(F) work per state, and its memory is
bounded by the widest layer (LBL) or by the diagram itself (BDD).

Future work:
- an ordering score that accounts for how long a layer stays wide (perfect-vertex meshes);
- ordering the edges emitted with each vertex (5×5: 146 vs 208 states depending on edge ids);
- why K₁₂'s layers are wider than the paper's;
- turning `graph` into a class with private invariants (an API change);
- reviewing the BDD builder against Hardy et al. (2007);
- running the CI workflow on GitHub.

---

## References

1. B. Wu, L. Sun. A novel layer-by-layer recursive decomposition algorithm for calculation of
   network reliability. *Reliability Engineering & System Safety* 244 (2024) 109968.
2. J. Carlier, C. Lucet. A decomposition algorithm for network reliability evaluation. *Discrete
   Applied Mathematics* 65 (1996) 141–156.
3. H. Imai, K. Sekine, K. Imai. Computational investigations of all-terminal network reliability
   via BDDs. *IEICE Trans. Fundamentals* E82-A(5) (1999) 714–721.
4. J. Kawahara, K. Sonoda, T. Inoue, S. Kasahara. Efficient construction of binary decision
   diagrams for network reliability with imperfect vertices. *Reliability Engineering & System
   Safety* 188 (2019) 142–154.
5. G. Hardy, C. Lucet, N. Limnios. K-terminal network reliability measures with binary decision
   diagrams. *IEEE Trans. Reliability* 56(3) (2007) 506–515.

---

## Appendix A. Commit map

| Commit | Content |
|---|---|
| `bca017f` | Audit report, fuzz and probe harnesses |
| `0f4e3be` | Audit: Wu & Sun preconditions, ordering experiments, Table 2 reproduction |
| `2eb0e4e` | Audit: checks against Carlier & Lucet, Imai et al., Kawahara et al. |
| `1ef297d` | Fix test suite compilation (A1) |
| `a29ca5e` | Correctness and preconditions (A2–A8), exact LBL, bounds API, tests |
| `9771441` | CI: graphrel tests on GCC and Clang, ASan/UBSan job |
| `0a43644` | Remove quadratic costs, outputs identical (A9, A10) |
| `e00cead` | Audit report status table |
| `3940bcd` | First version of this log |
| `7a98b40` | Cut-width ordering as default (A12) |
| `24138b1` | Log: ordering work |
| `195280d` | Cleanup (A13) |
| `63bf4fb` | `state_pool` exception safety and reuse (A8) |
| `dc32779` | Log: cleanup |
| `6c37164` | Frontier engine; LBL on frontier states (A11) |
| `7916203` | BDD built layer by layer on frontier states |
| `b389dcb` | Remove the whole-graph engine |

## Appendix B. Reproducing the evidence

```bash
# Build and run all graphrel suites (needs libboost-test-dev, libfmt-dev)
cmake --preset linux-release -DLIBTEDDY_BUILD_TESTS=ON
cmake --build --preset linux-release
ctest --test-dir build -R graphrel --output-on-failure
```

```bash
# Sanitizers
cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLIBTEDDY_BUILD_TESTS=ON -DLIBTEDDY_BUILD_GRAPHREL=ON -DLIBTEDDY_USE_SANITIZERS=ON
cmake --build build/asan --target graphrel-test
UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/asan -R graphrel --output-on-failure
```

```bash
# Large brute-force fuzz (this folder; link against build/libgraphrel/libgraphrel.a)
g++ -std=c++20 -O2 -I. -Ilibgraphrel/include -Ilibgraphrel/src libgraphrel/audit/fuzz.cpp build/libgraphrel/libgraphrel.a -o fuzz
PAPER=1 ./fuzz 10000
```

```bash
# Wu & Sun Table 2 (this folder)
g++ -std=c++20 -O2 -I. -Ilibgraphrel/include -Ilibgraphrel/src libgraphrel/audit/table2.cpp build/libgraphrel/libgraphrel.a -o table2
./table2 mesh 10 10 cutwidth
```

| Harness | Purpose |
|---|---|
| `fuzz.cpp` | Brute force vs LBL and BDD; orderings; modes `PAPER`, `PAPER PERFECT`, `KMIN2`, default |
| `table2.cpp` | Wu & Sun Table 2 networks: R, G^max, front size, total work, time, memory |
| `ordering_lab.cpp` | Ordering candidates: cheap metrics vs exact G^max and total work (`LAB_PV` sets vertex p) |
| `sweep.cpp`, `order_sens.cpp`, `paper.cpp`, `probe.cpp` | Audit-time experiments (written against the original API) |

The audit report ([`README.md`](README.md)) documents the findings as found at `39fe87d`.

## Appendix C. Glossary

| Term | Meaning |
|---|---|
| K | Terminal set; all terminals must work and be connected |
| LBL | Layer-by-layer decomposition (Wu & Sun 2024) |
| BS-BDD | Boundary-set BDD (Hardy et al. 2007) |
| Front F | Processed vertices with unprocessed edges (boundary set, elimination front) |
| G^max | Most states in one layer (Wu & Sun's B¹) |
| Vertex priority | Each vertex precedes its incident edges in the ordering |
| Edge cut | Unprocessed edges with exactly one processed endpoint |
| Dead end | Non-terminal block with at most one usable exit (Lemma 2) |
| k-NR-IV, R_EV | K-terminal reliability with imperfect vertices; failed vertices disable their edges (Kawahara et al.) |

## Appendix D. Errata to earlier versions of the audit

| Earlier statement | Correction |
|---|---|
| "The BDD is wrong on about 10 % of graphs" | Mostly inputs outside both methods' scope (\|K\| = 1). Within the assumptions plus p = 1 edges: 545/10,000. |
| "K₁₂ is 19× below the paper thanks to coloop detection" | An artefact of the 1e-15 cutoff. Exactly, K₁₂ has 1,459,298 states, 1.8× the paper. |
| "N20 has 340,599 states" | Measured with the cutoff. Exactly: 387,481 (BFS). |
| "Row sweep brings N20 to 1.08× the paper" | Measured with the cutoff. Exactly: 65,858, 1.22×. |
| "`lbl_calculator` can be copied" | It could be moved, not copied; the move had the dangling-reference problem. |
| "The G^max gap needs the boundary definition from [20]/[33]" | Resolved: it was the ordering (§5.4). |
