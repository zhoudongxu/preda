# R-PREDA Native Workload A/B Harness

This directory contains the reproducible benchmark harness for the first
protocol-guided runtime optimization phase. It compares all four runtime
ablations with **the same optimization-enabled, trace-disabled Native Engine
binary**:

- `baseline`
- `generic_batch_only`
- `verified_reserve_only`
- `verified_reserve_plus_batch`

The harness does not invoke Z3 and does not change contracts or runtime
semantics. Every process receives a fresh `HOME`, while the seed, shard order,
fixture, and workload parameters remain identical across variants.

## Prerequisites

Build the runtime-optimization configuration and install/copy its artifacts to
`bin/bin_release` in the normal PREDA build flow:

```bash
cmake \
  -S . -B build-gcc12 -G Ninja \
  -DDOWNLOAD_3RDPARTY=OFF \
  -DDOWNLOAD_IPP=OFF \
  -DRPREDA_ENABLE_Z3=OFF \
  -DRPREDA_ENABLE_RUNTIME_TRACE=OFF \
  -DRPREDA_ENABLE_RUNTIME_OPTIMIZATION=ON \
  -DBUILD_TESTING=ON

cmake \
  --build build-gcc12 --target chain_simulator preda_engine transpiler
```

The default binary is `bin/bin_release/chsimu`. Use `--binary` if artifacts are
installed elsewhere. Peak RSS is sampled from `/proc` for the simulator process
tree. If GNU time is installed, `--time-binary /path/to/time` can instead use
its maximum-RSS measurement. Runtime contract compilation uses the toolchain
recorded in `build-gcc12/CMakeCache.txt`; override it with `--toolchain-bin` or
`RPREDA_TOOLCHAIN_BIN` when necessary. Before executing a run, the harness also
rejects binaries that do not contain the optimization path or that contain the
full runtime-trace path; `--dry-run` remains available while another build
configuration temporarily occupies `bin/bin_release`.

## Inspect the plan without running it

```bash
python3 oxd_preda/simulator/relay_optimization/benchmarks/run_native_ab.py \
  --dry-run
```

## Full correctness and performance run

```bash
python3 oxd_preda/simulator/relay_optimization/benchmarks/run_native_ab.py \
  --phase all \
  --warmups 1 \
  --repetitions 5 \
  --seed 88 \
  --order 2 \
  --output /tmp/rpreda-native-ab
```

By default this runs Token, Ballot, MillionPixel, Kitty, and AirDrop. Measured
variant order is deterministically shuffled to reduce fixed-order bias. The
harness requires at least two measured repetitions; the default remains five.
Kitty uses `count=addresses=500`: a diagnostic stock-baseline run at 1,000
reaches contract `GasUsedUp` during newborn registration, whereas two clean
500-count probes each execute 1,500 logical relays without an invocation
error. This keeps the benchmark inside the contract's valid operating range
instead of treating expected gas exhaustion as optimization performance.

Select workloads or override parameters without editing fixtures:

```bash
python3 oxd_preda/simulator/relay_optimization/benchmarks/run_native_ab.py \
  --phase performance \
  --workload MillionPixel \
  --workload AirDrop \
  --performance-parameter MillionPixel:count=20000 \
  --performance-parameter MillionPixel:addresses=20000 \
  --performance-parameter AirDrop:count=2000 \
  --warmups 1 --repetitions 7 \
  --output /tmp/rpreda-native-ab-large
```

For Kitty, `addresses` must equal `count`; overriding `count` automatically
updates `addresses` unless `addresses` is also explicitly supplied.

The harness-only normalization and measurement gates can be checked without
running the simulator:

```bash
python3 -m unittest -v \
  oxd_preda/simulator/relay_optimization/benchmarks/test_run_native_ab.py
```

## Correctness gate

`--phase all` runs correctness before any performance sample. A mismatch stops
the run unless `--allow-correctness-mismatch` is explicitly supplied. Only the
correctness fixtures issue state/block visualization queries. Timing fixtures
contain no workload visualization queries, and performance processes do not
receive `-viz`/`-viz_templ`.
The normalized gate compares baseline against every optimization variant using:

- final contract state;
- observed non-system source transaction count;
- logical relay, physical route, and relay execution counts;
- destination-shard multiset;
- confirmed semantic transaction multiset;
- per-block dependency shape;
- broadcast clone count;
- invocation-result and diagnostic multisets.

The semantic transaction projection removes runtime placement/accounting fields
`Timestamp`, `PrevBlock`, `Height`, `OriginateHeight`, and `GasBurnt`. The
remaining confirmed transactions are compared as a multiset, so the gate does
not invent a stock-PREDA cross-process global ordering guarantee.

The block/dependency projection is causal rather than height-based. Each
transaction records its invocation type, contract, function, origin shard,
destination shard, result, and delivery class (`source`, same-shard/same-block,
same-shard/later-block, or cross-shard/scope). For relays, the raw
`(OriginateShardIndex, OriginateHeight, OriginateShardOrder, ShardIndex)` tuple
recovers the real producer-to-destination dispatch batch. The absolute height is
then omitted, independent batches are compared as a multiset, and the complete
semantic transaction order inside each batch is retained. Thus the gate allows
stock PREDA's independent producer/block interleaving without reducing all
dependency information to one global multiset or hiding a reorder inside one
destination batch.

Kitty applies one additional workload-specific semantic normalization.
Scheduling-derived `birth_time`/`birthTime`, `lastBreed`, and `newBornIndex`
fields are ignored; `myKitties` map values are compared as a multiset
independent of assigned map keys; and `new_borns`, `newBorns`, and `allKitties`
are sorted as unordered collections. A high-bit (`id >= 2^31`) newborn ID inside
a `myKitties` value is also treated as scheduling-assigned, as is the `id`
argument of the newborn-registration relay that pairs unordered newborns with
owners. Stable initial Kitty IDs below the high-bit boundary and mint relay IDs
remain strict. Semantic fields such as genes, owner, gender, `matronId`, and
`sireId` remain strict. Other nested contract arrays remain ordered.
`registerNewBorns` producer batches are compared as a multiset because their
input is that same stock unordered aggregation; all other producer batches keep
strict order. The complete machine-readable policy is also recorded in
`session.json`.

## Output layout

```text
OUTPUT/
  session.json          binary/config hashes, git state, host, run schedule
  correctness.json      invariant comparison and mismatch details
  samples.csv           one row per measured sample
  summary.json          raw samples plus median/mean/range/stddev
  summary.csv           long-form aggregate table
  raw/<workload>/<variant>/
    correctness/
      correctness.html
      correctness.normalized.json
      optimization_metrics.json
      stdout.log / stderr.log / resource.txt / run.json
    warmup-NNN/          retained, excluded from statistics
    sample-NNN/          full raw evidence for each measured run
```

The summary includes stopwatch elapsed time, source TPS, uTPS, relay generation,
routing, dispatch, and queue insertion timings, lock and batch counters,
plan load/trust/fallback counters, reserve counters, capacity growth/miss
counters, relay counts, and peak RSS. A sample is accepted only if its requested
optimization paths were actually exercised; missing/untrusted plans and dormant
batch/reserve paths cannot silently become nominal ablation samples. Stopwatch values
cover the fixture's marked transaction window. With report schema v2,
optimization counters/timings are read from the same `measurement_window`;
every warmup and measured sample requires that window to be completed.
Schema-v1 lifetime reports are rejected for performance acceptance. Plan-load
and failure checks still inspect lifetime counters, while relay/reserve/batch
activation must occur inside the stopwatch window. Peak RSS covers the complete
simulator process (including setup for Ballot and Kitty). The selected scope is
recorded per sample and must be retained when reporting results.

Do not use `optimize_audit` for peak performance numbers. Audit belongs to the
separate validation suite, while this harness intentionally measures the four
required non-audit ablations.
