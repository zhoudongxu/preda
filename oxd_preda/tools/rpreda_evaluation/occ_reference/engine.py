"""Deterministic Block-STM-style OCC replay for normalized relay records.

This module models scheduling and version validation only.  It does not execute
PREDA bytecode and must not be used as a Native Engine correctness result.
"""

from __future__ import annotations

import heapq
from dataclasses import dataclass, field
from typing import Any, Dict, Iterable, List, Mapping, MutableMapping, Optional, Set, Tuple


@dataclass(frozen=True)
class Record:
    identifier: str
    source_order: int
    scope: str
    reads: Tuple[str, ...]
    writes: Tuple[str, ...]
    work: int
    depth: int
    predecessors: Tuple[str, ...] = ()
    opaque: bool = False


@dataclass
class Event:
    finish: int
    worker: int
    record: Record
    snapshot: Dict[str, int]
    start: int
    attempt: int

    def __lt__(self, other: "Event") -> bool:
        return (self.finish, self.record.source_order, self.worker) < (
            other.finish,
            other.record.source_order,
            other.worker,
        )


def _record(value: Mapping[str, Any], fallback_order: int) -> Record:
    identifier = str(value.get("id", value.get("transaction_id", fallback_order)))
    reads = tuple(str(x) for x in value.get("reads", []))
    writes = tuple(str(x) for x in value.get("writes", []))
    scope = str(value.get("scope", ""))
    if not reads and not writes and scope:
        reads = (scope,)
        writes = (scope,)
    work = max(1, int(value.get("work", 1)))
    depth = max(0, int(value.get("depth", 0)))
    return Record(
        identifier=identifier,
        source_order=int(value.get("source_order", fallback_order)),
        scope=scope,
        reads=reads,
        writes=writes,
        work=work,
        depth=depth,
        predecessors=tuple(str(x) for x in value.get("predecessors", [])),
        opaque=bool(value.get("opaque", False)),
    )


def _load_records(payload: Mapping[str, Any]) -> List[Record]:
    raw = payload.get("transactions", payload.get("records", []))
    if not isinstance(raw, list):
        raise ValueError("transactions/records must be an array")
    records = [_record(value, index) for index, value in enumerate(raw)]
    if len({record.identifier for record in records}) != len(records):
        raise ValueError("transaction identifiers must be unique")
    records.sort(key=lambda record: (record.source_order, record.identifier))
    return records


def run_reference(payload: Mapping[str, Any], workers: int = 1, policy: str = "priority") -> Dict[str, Any]:
    """Run a deterministic OCC replay and return publication-ready metrics.

    ``policy`` is ``fifo`` or ``priority``.  A transaction commits in source
    order.  Version conflicts abort the attempt and cause a deterministic retry.
    """
    if workers < 1:
        raise ValueError("workers must be positive")
    if policy not in ("fifo", "priority"):
        raise ValueError("policy must be fifo or priority")
    records = _load_records(payload)
    by_id = {record.identifier: record for record in records}
    committed: Set[str] = set()
    versions: MutableMapping[str, int] = {}
    next_commit = 0
    attempts: MutableMapping[str, int] = {record.identifier: 0 for record in records}
    active: List[Event] = []
    waiting: Dict[int, Event] = {}
    ready_time: Dict[str, int] = {}
    worker_free = [0] * workers
    available: Set[str] = set()
    inflight: Set[str] = set()
    committed_ids: Set[str] = set()
    time = 0
    speculative = validations = aborts = retries = conflicts = 0
    busy_time = 0
    queue_wait: List[int] = []
    commit_times: Dict[str, int] = {}

    def refresh_available() -> None:
        for record in records:
            if (
                record.identifier in committed_ids
                or record.identifier in available
                or record.identifier in inflight
            ):
                continue
            if all(predecessor in committed_ids for predecessor in record.predecessors):
                available.add(record.identifier)

    def start_one(worker: int, now: int) -> bool:
        nonlocal busy_time
        refresh_available()
        candidates = [by_id[identifier] for identifier in available]
        if not candidates:
            return False
        opaque_active = any(event.record.opaque for event in active)
        if opaque_active:
            return False
        if any(record.opaque for record in candidates) and active:
            return False
        if policy == "fifo":
            chosen = min(candidates, key=lambda record: (record.source_order, record.identifier))
        else:
            chosen = min(
                candidates,
                key=lambda record: (-record.depth, -record.work, record.source_order, record.identifier),
            )
        available.remove(chosen.identifier)
        inflight.add(chosen.identifier)
        snapshot = {key: versions.get(key, 0) for key in set(chosen.reads) | set(chosen.writes)}
        attempt = attempts[chosen.identifier]
        attempts[chosen.identifier] += 1
        start = max(now, worker_free[worker])
        ready_time.setdefault(chosen.identifier, start)
        queue_wait.append(start - ready_time[chosen.identifier])
        event = Event(start + chosen.work, worker, chosen, snapshot, start, attempt)
        heapq.heappush(active, event)
        worker_free[worker] = event.finish
        busy_time += chosen.work
        return True

    def drain_commits(now: int) -> None:
        nonlocal next_commit, aborts, retries, conflicts, validations, time
        while next_commit < len(records):
            record = records[next_commit]
            event = waiting.get(record.source_order)
            if event is None:
                break
            waiting.pop(record.source_order)
            validations += 1
            if any(versions.get(key, 0) != version for key, version in event.snapshot.items()):
                aborts += 1
                retries += 1
                conflicts += 1
                available.add(record.identifier)
                time = max(time, now)
                continue
            for key in record.writes:
                versions[key] = versions.get(key, 0) + 1
            committed_ids.add(record.identifier)
            committed.add(record.identifier)
            commit_times[record.identifier] = now
            next_commit += 1

    while next_commit < len(records):
        for worker in range(workers):
            if not any(event.worker == worker for event in active):
                start_one(worker, time)
        if not active:
            refresh_available()
            if not available:
                raise ValueError("dependency cycle or unsatisfied predecessor")
            continue
        event = heapq.heappop(active)
        inflight.discard(event.record.identifier)
        time = max(time, event.finish)
        waiting[event.record.source_order] = event
        drain_commits(time)

    makespan = time
    total_work = sum(record.work for record in records)
    return {
        "schema_version": 1,
        "engine": "rpreda-block-stm-style-occ-reference",
        "policy": policy,
        "workers": workers,
        "transaction_count": len(records),
        "makespan": makespan,
        "total_work": total_work,
        "effective_parallelism": (float(total_work) / makespan) if makespan else 0.0,
        "speculative_executions": sum(attempts.values()),
        "validation_count": validations,
        "abort_count": aborts,
        "retry_count": retries,
        "retry_rate": (float(retries) / len(records)) if records else 0.0,
        "version_conflicts": conflicts,
        "queue_wait_median": sorted(queue_wait)[len(queue_wait) // 2] if queue_wait else 0,
        "serial_equivalent": len(committed) == len(records) and next_commit == len(records),
        "commit_times": commit_times,
    }
