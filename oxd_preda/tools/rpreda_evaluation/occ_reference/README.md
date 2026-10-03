# Block-STM-style OCC reference

This tool replays normalized relay records. It models fixed source order,
speculative workers, per-scope versions, validation, abort, and deterministic
retry. It does not execute PREDA code and its output is an abstract scheduling
result, not Native Engine correctness evidence.

Input example:

```json
{
  "transactions": [
    {"id": "r0", "source_order": 0, "scope": "alice", "reads": ["alice"], "writes": ["alice"], "work": 3, "depth": 1},
    {"id": "r1", "source_order": 1, "scope": "bob", "reads": ["bob"], "writes": ["bob"], "work": 3, "depth": 1}
  ]
}
```

Run it with:

```bash
PYTHONPATH=oxd_preda/tools/rpreda_evaluation python3 -m occ_reference \
  --input records.json --output occ.json --workers 4 --policy priority
```

Use `fifo` and `priority` with the same records. Report makespan,
effective parallelism, validation count, abort/retry count, version conflicts,
and serial-equivalence separately from Native Engine results.

## Replaying Native correctness output

The Native benchmark writes a normalized semantic record file at
`correctness.normalized.json`.  The adapter can consume that file directly:

```bash
PYTHONPATH=oxd_preda/tools/rpreda_evaluation python3 -m occ_reference \
  --native-normalized /path/to/correctness.normalized.json \
  --output occ-token-fifo.json --workers 4 --policy fifo
```

Run the same file with `--policy priority` to obtain the paired abstract
comparison.  The adapter preserves the observed target as a scope key and
the Native record order as `source_order`.  Native normalized output does not
contain read/write sets, relay-site certificates, or complete predecessor
edges.  Therefore every adapted record is `opaque=true` by default.  The
reference then serializes unknown effects and reports a conservative lower
bound on available parallelism.  This output is an abstract OCC replay and
does not establish Native Engine speedup.

For debugging only, `--allow-target-key-effects` clears the opaque marker and
allows different observed target keys to run concurrently.  This is a
sensitivity result based on target-key equality, not a recovered PREDA read /
write analysis, and must not be used as a correctness or Block-STM claim.

The adapter stores the source workload and variant in the input metadata and
adds `input.conservative_opaque` to the replay result.  Missing targets use a
single `<unknown-target>` scope so they cannot be treated as independent.
