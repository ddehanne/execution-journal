# Verification and Experimental Evidence

## Purpose

This document maps selected engineering properties of Execution Journal
to their corresponding tests, observed behavior, and verification limits.

It describes evidence from the offline C++20 implementation. It does
not establish production readiness, live-broker correctness, or
exhaustive fault tolerance.

## Verification Baseline

| Field | Value |
|---|---|
| Public revision | `01b4cff` |
| Verification date | 2026-10-22 |
| Configuration | Offline core; IBKR adapter disabled |
| Reported test result | 65/65 passed |
| Scope | Local verification of the isolated public revision |

The test count describes the result of the executed test suite.
It does not imply exhaustive coverage.

## Evidence Matrix

| Property | Evidence | Observed behavior | Boundary |
|---|---|---|---|
| Journal integrity | `Sha256.FipsVectors`, journal corruption tests | Hashing and selected integrity failures are exercised | No authenticated tamper resistance |
| Sequence continuity | `Journal.SequenceOverflowIsRejected`, `Journal.RemovedInteriorLineBreaksChain` | Selected invalid sequence and chain conditions are rejected | Not exhaustive input verification |
| Torn-tail recovery | `Journal.TornFinalLineRecovered` | Selected incomplete trailing records can be repaired | Does not establish recovery from arbitrary corruption |
| Interior corruption refusal | `Journal.InteriorCorruptionFatal` | Selected interior corruption prevents normal recovery | Limited to tested corruption patterns |
| Recovery quarantine | `Journal.RecoveryQuarantineSurvivesTwoRestarts` | Quarantine survives repeated reopenings | Regression exercises reopenings within one process |
| Coordinator recovery refusal | `Coordinator.RejectsRepairedJournalAfterRepeatedReopens` | Coordinator refuses the tested repaired-journal recovery path | Not a complete production restart protocol |
| Process termination recovery | `FaultInjection.F1_SigkillDuringAppendStorm_ReopenHeals` | Recovered records form a consistent prefix and subsequent append succeeds | Kill timing does not guarantee interruption inside `write()` or actual repair |
| Acknowledged append recovery | `FaultInjection.ConfirmedAppendsSurviveSigkill` | At least 100 acknowledged records are recovered after `SIGKILL` | Process termination is not physical power loss |
| Intent without dispatch | `FaultInjection.F2_CrashBetweenDurabilityAndDispatch_NoDuplicatePath` | An intent remains recorded without a dispatch record after controlled process exit | Does not establish broker idempotency or exactly-once execution |
| Incomplete broker snapshot | `FaultInjection.F3_MuteBrokerPartialSnapshot_TradingRefused` | Selected local/broker divergence prevents trading progression | Does not validate live broker connectivity |
| Single-writer exclusion | `FaultInjection.SingleWriterExclusiveAcrossProcesses` | A cooperating second writer is refused while the first holds the lock | Advisory locking; no distributed coordination |
| Write failure handling | `FaultInjection.WriterPoisonedAfterWriteFailure` | Writer rejects further appends after an injected write failure | Selected failure mechanism |
| Synchronization failure handling | `FaultInjection.WriterPoisonedAfterFsyncFailure` | Writer rejects further appends after an injected `fsync` failure | Injected failure, not a storage power-loss test |
| Writer move semantics | `FaultInjection.PoisonedStateSurvivesMoveOperations` | Poisoned state persists across tested move operations | Limited to tested move paths |
| Journal path identity | `FaultInjection.WriterRejectsReplacedJournalPath` | Append is refused after tested path replacement | Does not establish protection against every filesystem race |
| Durable coordinator decisions | `Coordinator.ApprovedDecisionIsDurable`, `Coordinator.RefusalIsDurableAndNotApproved` | Selected approval and refusal records are persisted | No proof of end-to-end broker submission |
| Duplicate decision prevention | `Coordinator.DuplicateBlockedAfterRestart` | Previously recorded identifiers block selected duplicate decisions | Local decision identity is not broker execution idempotency |

Test names identify the relevant source-level evidence. The observations
above are limited to the inspected test scenarios and reported results.

## Recovery Authority Boundary

Three distinct questions must not be conflated:

1. Is the journal structurally valid and readable?
2. Has the external broker state been reconciled?
3. Is progression through the execution workflow authorized?

A journal writer's ability to append after recovery does not itself
authorize an execution coordinator to resume trading.

A repaired journal may remain subject to persistent recovery quarantine,
and coordinator-level recovery refusal is a separate safety control.

## Process Failure Versus Power Failure

The fault-injection suite includes actual `SIGKILL` termination and
controlled process exit.

A successful `fsync()` is treated as an operating-system-level
synchronization acknowledgement. The experiments do not establish
physical persistence across arbitrary power loss, controller failure,
or all filesystem and hardware configurations.

## Performance Evidence

The optional journal benchmark measures append latency, append
throughput, read/verification time, and reopening time.

Its methodology and environmental limitations are documented in
[`benchmarks/README.md`](../benchmarks/README.md).

Initial single-run measurements are preliminary observations rather
than a statistically characterized performance baseline.

## Explicitly Unverified

This repository does not establish:

- Production readiness or HFT suitability.
- Exactly-once execution at a broker.
- Complete recovery from arbitrary storage corruption.
- Durability across every physical power-loss scenario.
- Authenticated protection against deliberate journal rewriting.
- Distributed multi-writer correctness.
- Production-safe restart authorization.
- Live-broker integration reliability or trading profitability.

## Reproduction

From the repository root:

```sh
cmake -S . -B build-public -DEJ_WITH_IBKR=OFF -DSANITIZE=OFF
cmake --build build-public --parallel 4
ctest --test-dir build-public --output-on-failure
```

The documented test results apply to their specified revisions,
configurations, and execution environments.
