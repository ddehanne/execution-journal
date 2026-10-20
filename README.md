# execution-journal

C++20 execution integrity engine — order lifecycle, durable journaling,
crash recovery, and broker reconciliation for systematic trading systems.

**Status: v0.1 — offline core reproducible.**

**Verified locally (2026-10-18): 32/32 tests passed** on Apple Silicon
with Apple Clang 21 and CMake 4.4.4, without the proprietary IBKR SDK.

The test suite exercises order-state anomalies, journal corruption,
risk controls, broker reconciliation, and fault injection, including
a real SIGKILL during journal writes.

The optional IBKR adapter was reportedly exercised during earlier
development against an IBKR paper gateway (read-only snapshot).
That integration, and the historical sanitizer results, were not
revalidated in the October 18 verification session.

Companion to [verified-ledger](https://github.com/ddehanne/verified-ledger)
(Rust): a related correctness-oriented design, adapted for C++ execution
engineering. This repository addresses order state, durability,
reconciliation, and broker integration; verification evidence for
each repository must be assessed independently.

**Latest local development run (2026-10-20): 62/62 tests passed
in 0.47 seconds.** This result concerns a working tree containing
uncommitted changes and does not constitute verification of the
published repository revision.

## Design Laws

### 1. Durable state

The journal is the authoritative durable state — not a write-ahead
log for another database.

Application state is reconstructed through deterministic replay.

### 2. Intent vs. eligibility

An order intent exists from creation.

The fsync durability barrier gates its eligibility for broker dispatch.

An intent must not be submitted before its durable local proof exists.

Local durability confirmation is distinct from broker acknowledgement.

### 3. Evidence over intention

Anomalous broker reports are representable, classified, and
quarantined with evidence preserved.

Examples include:

- Fill before acknowledgement.
- Fill during cancellation.
- Late fill after a terminal state.
- Overfill.

Contradictory observations are not silently discarded.

### 4. Reconciliation before trading

At startup, replayed local state is compared against a fresh
broker snapshot.

Divergences are classified, and unresolved mismatches prevent
trading progression.

The engine does not assume either side is automatically correct.

### 5. Persistent kill switch

The kill switch is implemented as a persistent file latch.

It survives restarts, is checked at each decision, and is cleared
only by deliberate external action.

### 6. Explicit failure handling

Fallible operations return typed values using mechanisms such as
`std::optional` and `std::variant`.

Invalid input is represented as a value rather than relying on
exceptions for control flow.

## Architecture

| Component | Responsibility |
|---|---|
| `src/core/` | Strong types, sequence identifiers, and checked arithmetic |
| `src/lifecycle/` | Deterministic order-state machine and anomaly classification |
| `src/journal/` | Durable append-only journal, integrity checks, and replay |
| `src/risk/` | Versioned risk configuration, position limits, and kill switch |
| `src/broker/` | Broker abstraction and simulated broker |
| `src/execution/` | Execution coordination |
| `src/recovery/` | Broker reconciliation and recovery controls |
| `src/ibkr/` | Optional Interactive Brokers TWS API adapter |
| `src/cli/` | Audit, status, and replay commands |

### Journal integrity

The journal uses an append-only record structure:

```text
sequence | previous_hash | record_hash | payload
```

Integrity checks use a local SHA-256 implementation.

The implementation includes:

- Checked sequence progression.
- Chained record hashes.
- Durability barriers using `fsync`.
- Recovery from selected torn-tail conditions.
- Fatal handling of interior corruption.
- Deterministic replay.

A torn final record can be recovered and healed through the
documented recovery mechanism.

Interior corruption is treated as fatal rather than silently repaired.

### Risk controls

The risk layer includes:

- Versioned `RiskConfig`.
- Configuration content hashing.
- Notional limits.
- Position limits based on projected absolute net exposure.
- Persistent kill-switch enforcement.

### Broker interface

The broker abstraction separates execution coordination from
broker-specific implementations.

A scriptable simulated broker supports testing of incomplete,
contradictory, and anomalous broker observations.

### Recovery and reconciliation

Recovery compares reconstructed local state against external
broker observations.

The reconciliation mechanism classifies discrepancies and
prevents progression when unresolved mismatches remain.

### Optional IBKR integration

The optional adapter uses the Interactive Brokers TWS API
behind the broker abstraction.

Its implementation includes an `EReader` thread and
bounded-blocking snapshot handling.

Incomplete snapshots are intended to produce conservative
failure behavior rather than authorization to proceed.

The offline verification results do not establish correctness
of the real-broker integration.

## Verification

### Reproduced offline baseline — October 18, 2026

| Property | Result |
|---|---|
| Platform | Apple Silicon |
| Compiler | Apple Clang 21 |
| Build system | CMake 4.4.4 |
| IBKR SDK | Not required |
| Sanitizers | Disabled |
| Tests | 32/32 passed |

This verification exercised the offline C++20 core without
the proprietary IBKR SDK.

### Local development run — October 20, 2026

**Result: 62/62 tests passed.**

**Reported execution time: 0.47 seconds.**

This result was obtained on a local development working tree
containing modifications not yet committed to the repository.

The result has not been independently reproduced against a
clean checkout of the corresponding revision.

### Test coverage

The documented verification scenarios include:

**Order lifecycle**

- Fill before acknowledgement.
- Fill during cancellation.
- Late fill after a terminal state.
- Overfill.
- Classification of anomalous transitions.

**Journal integrity**

- Torn final record.
- Missing trailing newline.
- Interior corruption.
- Removed interior record.
- Hash-chain discontinuity.
- Crash, reopen, heal, and append.

**Fault injection**

- Real `SIGKILL` during journal writes.
- Crash between durability confirmation and dispatch.
- Recovery of durable but undispatched intent.
- Incomplete broker snapshot.
- Refusal to proceed when reconciliation remains unresolved.

**Risk and reconciliation**

- Risk configuration enforcement.
- Position and notional controls.
- Persistent kill-switch behavior.
- Classification of local/broker discrepancies.
- Conservative handling of unresolved mismatches.

These scenarios describe the documented test scope.
They do not establish exhaustive failure coverage.

### Historical verification

Earlier development reportedly included:

- AddressSanitizer testing.
- UndefinedBehaviorSanitizer testing.
- ThreadSanitizer testing.
- Interactive Brokers paper-gateway snapshot testing.

Historical ThreadSanitizer results reportedly identified
no data races in the tested configuration.

These results were not independently reproduced during
the October 18 verification session.

The historical results must not be interpreted as current
validation of the latest development changes.

### Compiler diagnostics

The project documents strict compiler warnings for its
own code:

```text
-Wall
-Wextra
-Wpedantic
-Wconversion
-Wshadow
```

Third-party dependencies are isolated from the project's
warning policy.

### Verification boundaries

Passing the offline test suite does not establish:

- Production readiness.
- Correctness under every possible failure.
- Real-broker execution reliability.
- Safety of live financial orders.
- Absence of all concurrency defects.
- Trading profitability or economic advantage.

Verification evidence applies only to the configurations,
revisions, and scenarios actually tested.

## Build and Reproduce

### Requirements

- C++20-compatible compiler.
- CMake 3.22 or later.
- Network access to retrieve GoogleTest.

The offline core, audit CLI, and tests build without
the proprietary IBKR SDK.

### Offline build

```bash
cmake -S . -B build-public -DEJ_WITH_IBKR=OFF -DSANITIZE=OFF
cmake --build build-public --parallel 4
ctest --test-dir build-public --output-on-failure
```

Alternatively:

```bash
./check.sh
```

The verification script configures, builds, and executes
the test suite.

It does not include a separate CLI smoke test.

### Optional IBKR Adapter

The optional Interactive Brokers adapter requires separately
obtained licensed dependencies.

See `third_party/README.md` for dependency information.

Configure with:

```bash
cmake -S . -B build-ibkr -DEJ_WITH_IBKR=ON
```

The optional IBKR build and real-broker integration were
not revalidated during the documented offline verification.

## Scope and Limitations

This repository investigates selected engineering properties
of execution systems:

- Durable state representation.
- Deterministic recovery.
- Order lifecycle integrity.
- Classification of contradictory observations.
- Broker reconciliation.
- Conservative behavior under uncertainty.

It is an engineering prototype, not a production trading system.

The implementation and its tests provide evidence for
specified behaviors under specified conditions.

They do not constitute a guarantee of operational safety,
complete correctness, or economic performance.