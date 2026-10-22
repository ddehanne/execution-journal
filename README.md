# execution-journal

**C++20 · Execution Integrity · Fault Tolerance · Systems Engineering**

An independent engineering experiment investigating execution integrity in systematic trading infrastructure.

The project examines how an execution system can preserve durable order intent, reconstruct state after interruptions, classify contradictory broker observations, and refuse unsafe progression when reconciliation remains unresolved.

**Status:** v0.1 — offline core reproducible.

## Research Question

Can an execution system maintain a coherent, recoverable account of order intent and external observations across crashes, interrupted writes, and inconsistent broker reports?

The central engineering problem is not merely recording events. It is determining what the system can safely conclude from durable local evidence and potentially incomplete external information.

## Engineering Principles

### 1. Durable state

The journal is the authoritative local durable record.

Application state is reconstructed through deterministic replay rather than treated as independently authoritative.

### 2. Intent before execution

An order intent exists before it becomes eligible for dispatch.

The durability barrier separates recorded intent from dispatch eligibility.

Local durability confirmation and broker acknowledgement remain distinct.

### 3. Evidence over assumptions

Contradictory broker observations must be representable and classified.

Examples include:

- Fill before acknowledgement.
- Fill during cancellation.
- Late fill after a terminal state.
- Overfill.

Anomalous observations are preserved rather than silently discarded.

### 4. Reconciliation before progression

Recovered local state is compared with an external broker snapshot.

Unresolved discrepancies prevent trading progression.

The system does not assume that either source is automatically correct.

### 5. Conservative failure behavior

The implementation includes a persistent kill-switch mechanism and explicit handling of selected failure conditions.

Failure handling is designed to avoid silently converting uncertainty into permission to proceed.

## Architecture

| Component | Responsibility |
|---|---|
| `core` | Strong types, checked arithmetic, sequence identifiers |
| `lifecycle` | Deterministic order-state transitions and anomaly classification |
| `journal` | Durable append-only records, integrity checks, replay |
| `risk` | Versioned risk configuration, position limits, kill switch |
| `broker` | Broker abstraction and simulated broker |
| `execution` | Execution coordination |
| `recovery` | Reconciliation and recovery-related controls |
| `ibkr` | Optional Interactive Brokers adapter |
| `cli` | Audit, status, and replay commands |

The journal uses chained record hashes and SHA-256 integrity checks.

The implementation distinguishes recoverable torn-tail conditions from interior corruption, which is treated as fatal.

## Verification

### Reproduced baseline — October 18, 2026

The previously documented offline verification reported:

| Property | Result |
|---|---|
| Platform | Apple Silicon |
| Compiler | Apple Clang 21 |
| Build system | CMake 4.4.4 |
| IBKR SDK | Not required |
| Tests | 32/32 passed |

### Local development verification — October 20, 2026

A subsequent local test execution reported:

**62/62 tests passed — 0.47 seconds.**

This result relates to a development working tree containing modifications not yet incorporated into the documented repository baseline.

It should not be interpreted as independent verification of a published revision.

### Verification scope

The documented test coverage includes scenarios involving:

- Order-state anomalies.
- Journal corruption.
- Interrupted writes and recovery.
- Crash-related fault injection.
- Risk controls.
- Broker reconciliation.
- Incomplete broker observations.

Selected fault-injection tests use real process termination.

The offline test suite does not establish production safety, exhaustive fault coverage, or reliability against a live broker.

### Historical verification

Earlier development reportedly included:

- AddressSanitizer and UndefinedBehaviorSanitizer runs.
- ThreadSanitizer testing.
- An Interactive Brokers paper-gateway snapshot exercise.

These results were not reproduced during the October 18 verification session and should not be treated as current integration validation.

## Build and Reproduce

Requirements:

- C++20-compatible compiler.
- CMake 3.22 or later.
- Network access to retrieve GoogleTest.

The offline core, CLI, and tests build without the proprietary IBKR SDK.

```bash
cmake -S . -B build-public -DEJ_WITH_IBKR=OFF -DSANITIZE=OFF
cmake --build build-public --parallel 4
ctest --test-dir build-public --output-on-failure
```

Alternatively:

```bash
./check.sh
```

The verification script configures, builds, and executes the test suite. It does not include a separate CLI smoke test.

### Optional IBKR Adapter

The optional Interactive Brokers adapter requires separately obtained licensed dependencies.

See `third_party/README.md` for dependency information.

The real-broker integration is not validated by the offline test results.

## Experimental Findings — October 22, 2026

### M8-01 — Persistent recovery quarantine

**Research question:** Does recovery quarantine remain effective after repeated journal reopenings, preventing an execution coordinator from silently resuming after a repaired journal?

**Method:**

- Construct a journal containing valid records.
- Append an incomplete final record to simulate an interrupted write.
- Trigger journal recovery.
- Reopen the journal repeatedly.
- Attempt to reopen the execution coordinator after recovery.

**Observed results:**

- The journal reported its repaired state across repeated reopenings.
- Previously valid records remained readable.
- The coordinator rejected repeated attempts to resume from the repaired journal.
- The development working tree passed **72/72 tests**; this result includes changes outside the isolated public revision.
- The isolated public revision passed **65/65 tests** on a separate clean worktree.

**Regression tests:**

- `Journal.RecoveryQuarantineSurvivesTwoRestarts`
- `Coordinator.RejectsRepairedJournalAfterRepeatedReopens`

**Local commits:**

- `9044a41` — Journal quarantine regression.
- `5add630` — Coordinator recovery refusal regression.

**Limitations:**

The new regression tests exercise repeated reopenings within a single process. They do not independently establish persistence across separate process lifetimes or durability after abrupt machine failure.

The results do not establish production readiness, exhaustive crash safety, or real-broker integration correctness.

**Conclusion:**

Within the tested scenarios, recovery quarantine remains effective after repeated journal reopenings, and the coordinator refuses unsafe resumption.

The M8-01 investigation was closed with the documented limitations accepted.

---

## Research Boundaries

This repository investigates selected engineering properties of execution systems.

It does not demonstrate:

- Production readiness.
- Safety under every possible failure.
- Correctness of all real-broker interactions.
- Real-money execution reliability.
- Trading profitability or economic advantage.

Passing tests establishes only the behavior exercised under their specified conditions.

## Development Status

**v0.1 — Offline engineering prototype**

The current research concerns execution integrity, durable state, and recovery under specified failure scenarios.

Further engineering work and independent verification would be required before any production deployment.
