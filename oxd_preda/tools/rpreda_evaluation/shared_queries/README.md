# Shared-query effectiveness study

This study complements the four-mode **cost** ablation. It compares semantic
answers on fixed query IDs, not counts of generated certificate objects.

## Frozen question definitions

All questions concern one entry invocation, its finite execution prefixes, and
its logical relay tree, without gas/resource limits or a commit-only trace
filter. A work unit is one logical relay emission, not an executed
instruction, physical broadcast delivery, or scheduler operation.

* `may_alias(A,B)`: some direct trace emits both sites with the same scope/key.
* `mutual_exclusion(A,B)`: no direct trace emits both sites.
* `must_precede(A,B)`: every occurrence of B has an earlier occurrence of A in
  that direct trace. This is a dominance property, not merely source order or
  an order restricted to traces already containing both sites. Vacuous cases
  are retained and identified by the independent oracle.
* `direct_work`, `tree_work`, `depth`: return a uniform finite upper bound over
  all permitted inputs/state, or an independently justified unbounded answer.
  The root invocation is not counted in tree work or depth.

The same queries and denominator apply to every mode. Unknown, unsupported,
timeout and execution error are distinct. Unsupported queries remain in the
main denominator; capability-conditioned coverage is supplementary only.

## Design and independence

The corpus contains separately reported controlled programs, existing contract
entry slices, and source mutations. The existing `*_parallel` entries were
previously added for certificate experiments; they are not independent,
unmodified production applications. Original entries are identified separately.
All unordered pairs of the declared direct sites receive alias/exclusion and
both directional precedence queries. Every entry receives the three resource
questions, including zero-relay and unbounded boundary cases.

`corpus.py` writes source snapshots and source-level reference models before
any mode is run. `oracle.py` exhaustively evaluates finite input quotients and
uses explicit lasso arguments for two unbounded patterns. It never imports a
mode adapter, reads a manifest, or calls Z3. Address/key equality partitions
and Boolean conditions cover the full semantic domains for equality-only
models; a separate 8-bit case enumerates all 256 values. Each real-source
abstraction has a written justification and source anchors. These are
author-constructed reference models, not an independent human audit or Native
execution. Source hashes and models are published for review.

Mode answers are computed without access to oracle answers or reference models.
The scanner uses only local site metadata. CFG uses reachability/dominance and
summary/handler-graph composition. Formula/SMT additionally solves CFG path and
target formulas, with SAT witnesses accepted only for exact supported contexts.
Full first consumes native Boolean certificates, and can fall back to the same
lower rules. For bounds it takes the minimum of the native and lower-layer sound
upper bounds; native-certificate coverage/tightness is reported separately. Thus a missing native
certificate never automatically makes a simpler method fail.

The result must be allowed to show no full-mode gain over Formula/SMT. The
study cannot establish superiority over independent third-party analyzers or
runtime scheduling speedups.

## Run

The existing analysis driver must use profiling and Z3, with runtime tracing and
runtime optimization disabled. Install the Python bindings for the same Z3
version (the archived run uses 4.12.1.0). From the repository root:

```sh
python3 oxd_preda/tools/rpreda_evaluation/shared_queries/run.py prepare --repo . --output results/shared_queries
python3 oxd_preda/tools/rpreda_evaluation/shared_queries/run.py compile --output results/shared_queries --driver bin/bin_release/rpreda_analysis_driver --library-path /path/to/runtime/libs --repetitions 3
python3 oxd_preda/tools/rpreda_evaluation/shared_queries/run.py answer --output results/shared_queries --repetitions 3
python3 oxd_preda/tools/rpreda_evaluation/shared_queries/run.py report --output results/shared_queries
RPREDA_SHARED_QUERY_RESULTS=results/shared_queries python3 -m unittest discover -s oxd_preda/tools/rpreda_evaluation/shared_queries -p test_shared_queries.py -v
```

Compilation uses a fixed round-major shuffle of (program, mode), preserving the
same source and binary for every mode. `--resume` requires matching source and
binary hashes and does not silently rerun failed processes. Query inference is
kept separate so a harness repair does not needlessly rerun the compiler matrix.
The main table always uses repetition zero, and the other repetitions check
stability. The corpus and oracle hashes in `FREEZE.json` must remain unchanged
during inference. Mode adapters receive queries/locators only, without oracle
models; scoring happens after inference.

Original source snapshots normalize line endings to LF. No source file in the
simulator contracts directory is edited. Generated C++ hashes are checked across
all four modes and all three repetitions. Unsupported and unknown answers remain
visible; no program is dropped after seeing its analysis result.
