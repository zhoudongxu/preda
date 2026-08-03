# PREDA Native A/B Evidence

This directory contains the accepted correctness and performance evidence for
the protocol-guided Native Engine runtime optimizations. It intentionally
contains no host name, user identity, absolute home path, raw process log, or
per-run directory.

## Validation status

- Correctness: **PASS** for all five workloads and all four runtime variants.
- Performance acceptance: **100/100** measured samples succeeded.
- Build mode: runtime trace disabled; runtime optimization enabled; Z3 disabled.
- Binary SHA256:
  `f9d670886135682c3a53ab78fe1b28fc55025d3f437b9ef2295f96f565d437c1`
- Workload configuration SHA256:
  `190bc4113e29f5d5441f4682dab528b3370e94e56d26af4f1b800fcb0f3fe224`

The performance matrix is:

```text
5 workloads x 4 variants x 5 measured repetitions = 100 samples
```

Each workload/variant pair had one warmup run, excluded from the statistics.
The simulator seed was `88`, shard order was `2`, variant scheduling used seed
`20260730`, and performance processes were pinned to CPUs `0-7`.

The four variants are:

- `baseline`
- `generic_batch_only`
- `verified_reserve_only`
- `verified_reserve_plus_batch`

## Files

- `performance_summary.json`: complete aggregate statistics and raw metric
  vectors grouped by workload and variant.
- `performance_summary.csv`: long-form aggregate statistics.
- `performance_samples.csv`: one row per accepted measured sample. The original
  `run_directory` field was removed because it contained machine-local paths.
- `correctness.json`: normalized semantic invariants and baseline comparisons
  for all variants.
- `metadata.json`: hashes, build options, schedule, parameters, fixture hashes,
  and acceptance counts.

Raw logs and warmup directories are not part of this evidence bundle.

## Workload parameters

| Workload | Performance `count` | Performance `addresses` | Correctness `count` | Correctness `addresses` |
| --- | ---: | ---: | ---: | ---: |
| Token | 10000 | 1024 | 128 | 128 |
| Ballot | 10000 | 10000 | 64 | 64 |
| MillionPixel | 10000 | 10000 | 128 | 128 |
| Kitty | 500 | 500 | 32 | 32 |
| AirDrop | 1000 | 1024 | 16 | 128 |

Ballot uses a measured-source-transaction offset of one; all other workloads
use zero. Exact fixture paths and hashes are recorded in `metadata.json`.

## Measurement scope

Timing, source TPS, uTPS, relay-path counters, and optimization-path counters
come from each fixture's completed `measurement_window`. Peak RSS covers the
complete simulator process. Every accepted optimization sample was required to
exercise its requested runtime paths; warmups do not contribute to aggregate
statistics.

Correctness compares the baseline with every optimization variant using final
contract state, semantic transaction and dependency projections, relay and
route counts, destination-shard multisets, broadcast clone counts, invocation
results, and diagnostics. Full invariant values and hashes are retained in
`correctness.json`.

## Performance conclusion

The campaign proves semantic invariance and runtime-path activation. It does
not prove a workload-independent end-to-end speedup. Source TPS and uTPS are
derived from the same stopwatch elapsed value and are not independent evidence.
The table below therefore reports median stopwatch change relative to baseline;
negative elapsed is faster.

| Workload | Generic batch | Verified reserve | Reserve + batch | Reviewer-safe conclusion |
| --- | ---: | ---: | ---: | --- |
| Token | -33.8% | -30.5% | -13.0% | Large but non-causal observation: batch size is 1, locks do not fall, queue time rises, and reserve never runs |
| Ballot | +0.5% | +1.1% | +12.4% | Relay traffic is too sparse to amortize per-invocation plan lookup |
| MillionPixel | -8.3% | +8.3% | +8.3% | Generic is a positive stopwatch candidate, but queue counters do not attribute the change to batching |
| Kitty | +2.1% | +0.6% | -0.7% | Contract arithmetic and nested state execution dominate; differences are neutral within observed variation |
| AirDrop | +0.8% | +8.3% | +12.6% | Real multi-element batches do not reduce locks; dynamic-count reserve never runs |

### Per-workload bottleneck evidence

| Workload | Timed relay shape | Batch coverage | Locks, baseline -> batch | Reserve calls; growth, baseline -> reserve | Main blocker |
| --- | --- | --- | ---: | --- | --- |
| Token | 10,000 source / 10,000 one-hop relays | 7,442 calls / 7,442 elements; average/max 1/1 | 10,000 -> 10,000 | 0; 4 -> 4 | State-dependent count falls back, but 20,000 plan decisions cost 127-161 ms |
| Ballot | about 10,000 source / 6 logical / 9 physical relays | 9 / 9; average/max 1/1 | 9 -> 9 | 6; 4 -> 0 | Only six useful reserves after about 10,010 lookups |
| MillionPixel | 10,000 source / 10,000 one-hop relays | 7,475 / 7,475; average/max 1/1 | 10,000 -> 10,000 | 10,000; 4 -> 0 | Retained buffers grow only four times, but lookup/reserve runs per invocation |
| Kitty | 500 source / 1,500 nested relays | about 1,133 / 1,133; average/max 1/1 | 1,500 -> 1,500 | 1,500; 4 -> 0 | Big-integer and nested handler work dominates a sub-millisecond queue component |
| AirDrop | 1,000 source / 100,000 relays | 3,000 / 74,777; average 24.93, max 41 | 28,223 -> 28,223 | 0; 24 -> 24 | Legacy bulk already uses one lock; the input-length loop has no usable constant reserve count |

The strongest mechanism-local result is allocation behavior: verified reserve
reduces capacity-growth events from four to zero in Ballot, MillionPixel, and
Kitty. It does not translate into throughput because the planner performs
lookup/decision work for every relevant microtransaction while stock
`BufferEx` retains capacity after its first growth. Token and AirDrop perform
no direct reserve at all, so their reserve-variant elapsed values cannot be
attributed to preallocation.

Generic batching does not reduce queue-lock acquisitions in any workload.
Stock PREDA already takes one mutex for each destination bulk vector. Four
workloads produce only size-one optimized batches; AirDrop produces larger
batches, but the lock count is still identical and its median queue-push time
is slightly higher. Token's and MillionPixel's lower generic stopwatch medians
occur with lower generation/routing counters rather than lower queue cost, so
they remain exploratory observations rather than batching-caused results.

Component timers are accumulated work across workers and are not exclusive
critical-path intervals. The accepted matrix has only five measured samples
per group; Token is highly dispersed and AirDrop has one late optimized
outlier in each series. The performance contracts also retain diagnostic
`__debug.print` output, which was captured but not separately instrumented.

The run uses shard order 2: four normal shard workers plus a global worker in
synchronous sharding mode. CPU affinity `0-7` does not make this an eight-shard
scaling result. Core-count scaling requires a separate experiment that changes
shard order and physical-core allocation together.

The complete implementation, workload-by-workload counter analysis, blockers,
and follow-up design are documented in
[`R_PREDA_PROTOCOL_GUIDED_RUNTIME_OPTIMIZATION_IMPLEMENTATION.md`](../../../../../../R_PREDA_PROTOCOL_GUIDED_RUNTIME_OPTIMIZATION_IMPLEMENTATION.md).
