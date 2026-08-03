# R-PREDA Semantic Mutation Benchmarks

This directory contains small deterministic Native Engine fixtures for the
semantic mutation study. Every template deploys `{{SOURCE}}`; the runner must
replace that token with the baseline or mutant `.prd` path before execution.

## Workload mapping

| Workload | Contract source | Covered relay-emitting functions | Suitable semantic categories |
|---|---|---|---|
| Token | `oxd_preda/simulator/contracts/Token.prd` | `transfer` | Argument, Guard, Work, Runtime |
| Ballot | `oxd_preda/simulator/contracts/Ballot.prd` | `init`, `finalize`, and the two reachable relay lambdas | Work, Depth, Broadcast, Runtime |
| MillionPixel | `oxd_preda/simulator/contracts/MillionPixel.prd` | `occupy` | Target, Work, Runtime |
| Kitty | `oxd_preda/simulator/contracts/Kitty.prd` | `mint`, `breed`, `registerNewBorns`, and their relay lambdas | Target, Argument, Guard, Work, Depth, Runtime |
| AirDrop | `oxd_preda/simulator/contracts/Token.prd` | `transfer_n` | Argument, Guard, Runtime |
| Synthetic | `oxd_preda/tools/rpreda_mutation/fixtures/SemanticMutationStudy.prd` | target arithmetic, guarded argument, bounded loop, ordered/independent and nested relay functions | Target, Argument, Guard, Order, Alias, Work, Depth, Runtime |

AirDrop is a one-to-many workload, not a separate PREDA contract. It reuses
`Token.transfer_n`. Token and AirDrop therefore compile the same source but use
disjoint coverage allowlists. A runner must not require `transfer_n` coverage
from the Token fixture or `transfer` coverage from the AirDrop fixture.

The exact baseline function IDs and portable source signatures are recorded in
`benchmarks.json`. Generated relay-lambda IDs are baseline identities; mutant
coverage should be transferred through relay-site edit anchors or semantic
fingerprints rather than listener ordinal alone.

## Boundary inputs

- Token contains both an exact-balance transfer distinguishing `>=` from `>`
  and a strictly-valid transfer that keeps the mutated relay site covered.
- AirDrop has a total exactly equal to the available balance and includes one
  zero-valued recipient, distinguishing `<=` from `<` and `> 0` from `>= 0`.
- MillionPixel includes `(1,2)`, `(1,3)`, and `(2,1)`, exposing arithmetic
  offset and `x`/`y` swap mutations.
- Ballot uses one address per shard and executes the complete
  address-to-global-to-all-shards-to-global chain.
- Kitty initializes both genders, executes breeding, and registers newborns,
  covering both nested relay chains.
- Synthetic includes distinct coordinates, equality and strictly-valid guard
  inputs, a three-iteration bounded loop, ordered/independent sibling relays,
  a nested relay chain, and a shards broadcast.

## Rendering and running a template

For a smoke run, copy a template to a temporary `.prdts`, replace `{{SOURCE}}`
with an absolute or script-relative contract path, and invoke:

```bash
HOME="${RPREDA_RUN_HOME}" \
LD_LIBRARY_PATH="bin/bin_release:${Z3_LIBRARY_DIR}" \
bin/bin_release/chsimu "${RENDERED_SCRIPT}" \
  -seed:88 -order:2 -addresses:8 -count:1 -stdout
```

Use a fresh `HOME` for every baseline and mutant process. For strict validation,
also pass `-rpreda_trace:strict` and a unique `-rpreda_trace_report:<path>`.

## Static-analysis limits relevant to the matrix

- `Token.transfer_n` uses an array-length loop, so direct relay count and its
  finite upper bound are currently `Unknown`; loop-occurrence target, argument,
  and guard relations are `Unsupported`.
- Ballot's all-shards subtree currently has an unknown transitive logical-work
  bound, although the relay-tree depth upper bound is 3 and the physical-route
  bound is parameterized by active shard count.
- Kitty `breed` has a finite transitive work bound of 3 and depth bound of 3.
  The newborn-registration array-length loop still has unknown work.
- These real contracts do not contain two sibling relay sites in one source
  function. MustPrecede and CoEmission/alias mutations require the separate
  synthetic ordered and independent-relay fixtures.

## Smoke evidence

All six rendered baseline templates were compiled, linked, and executed with
the current Native Engine in strict trace mode. The coverage allowlists were
checked against the observed function and relay-site IDs.

| Workload | Status | Total transactions | Observed relay sites | Strict passed | Skipped unsupported |
|---|---:|---:|---:|---:|---:|
| Token | Passed | 6 | 1/1 | 45 | 9 |
| Ballot | Passed | 17 | 4/4 | 153 | 23 |
| MillionPixel | Passed | 7 | 1/1 | 61 | 9 |
| Kitty | Passed | 44 | 6/6 | 437 | 108 |
| AirDrop | Passed | 10 | 1/1 | 107 | 29 |
| Synthetic | Passed | 25 | 11/11 | 261 | 48 |

No baseline produced a certificate mismatch, runtime mismatch, timeout, or
infrastructure failure. `Skipped unsupported` is reported separately and is
not counted as a successful validation check. Machine-readable evidence and
artifact hashes are in `smoke_results.json`; raw local traces are intentionally
not part of the publishable fixture bundle.
