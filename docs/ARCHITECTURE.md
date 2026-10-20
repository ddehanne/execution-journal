# Architecture and Engineering Decisions

## 1. Purpose and Scope

Execution Journal (QS1) is a C++20 engineering project exploring
durable execution-intent recording, integrity verification, recovery,
and controlled execution eligibility.

It is not a production trading system, an HFT engine, or a claim
of validated broker execution.

The core design principle is:

    Record intent
        |
        v
    Append to journal
        |
        v
    fsync succeeds
        |
        v
    Intent becomes eligible for dispatch

Local durability confirmation is distinct from broker acknowledgement,
order acceptance, and execution.

## 2. Component Organization

The source tree separates several responsibilities:

| Component | Location | Responsibility |
| --- | --- | --- |
| Journal | `src/journal/` | Persistent records and integrity |
| Core types | `src/core/` | Shared domain representations |
| Risk gate | `src/risk/` | Risk-related decision logic |
| Lifecycle | `src/lifecycle/` | Order-state handling |
| Recovery | `src/recovery/` | Reconciliation and recovery gating |
| Coordinator | `src/execution/` | Execution workflow coordination |
| Broker port | `src/broker/` | Broker-facing abstraction |
| IBKR adapter | `src/ibkr/` | Optional broker integration |
| Audit CLI | `src/cli/` | Command-line entry point |

These directories identify responsibility boundaries. Detailed
behavioral guarantees must be established from each component's
implementation and tests.

## 3. Journal Record Format

Each record is represented as one newline-terminated line:

    <sequence>|<previous_hash>|<record_hash>|<payload>\n

The record hash is calculated as:

    SHA256(sequence + "|" + previous_hash + "|" + payload)

The initial previous hash is 64 hexadecimal zero characters.

The journal enforces contiguous sequence numbers and rejects
payloads containing newline characters.

The payload is opaque to the journal layer.

### Integrity properties

A valid journal must satisfy:

1. Parseable record structure.
2. Valid record hashes.
3. Consistent previous-hash references.
4. Contiguous sequence numbers.

The hash chain detects accidental modifications and inconsistencies
under the assumptions of the implemented verification algorithm.

It does not provide cryptographic authentication against an attacker
who can rewrite the journal and recompute its hashes.

## 4. Writer Lifecycle

`JournalWriter::open()`:

1. Opens the companion `.lock` file.
2. Acquires a nonblocking exclusive `flock`.
3. Opens or creates the journal.
4. Checks file identity.
5. Reads and validates existing records.
6. Performs controlled recovery when applicable.
7. Determines the next sequence and last hash.
8. Returns an active writer only after validation.

A writer is moveable but not copyable.

The exclusive lock is associated with the writer's lifetime.

The implementation also checks the journal path against the open
file descriptor to detect certain replacement or inode changes.

## 5. Append and Durability Semantics

`JournalWriter::append()`:

1. Refuses operations when the writer is poisoned.
2. Verifies journal file identity.
3. Validates the payload and expected sequence.
4. Calculates the chained SHA-256 record hash.
5. Writes the serialized record.
6. Calls `fsync()` on the journal descriptor.
7. Advances in-memory sequence and hash state only after success.

If writing or synchronization fails, the writer becomes poisoned
and refuses subsequent appends.

An append success is a local durability acknowledgement at the
operating-system interface.

It is not proof of physical persistence across every possible
power-loss or storage-controller failure.

## 6. Recovery and Quarantine

The journal distinguishes recoverable trailing damage from
integrity failures that must stop normal processing.

A torn or unparseable final record can be treated as a crash
artifact, preserving the longest verified prefix.

The open path persists a recovery-quarantine marker before
repairing a truncated tail.

The marker is not automatically removed.

The implementation also handles a valid final record missing
its trailing newline.

After a truncated-tail repair, the writer reopens and verifies
the resulting journal before continuing.

Interior corruption, invalid complete records, and chain breaks
are not silently accepted as normal recovery.

A repaired journal is not automatically evidence that the
surrounding trading workflow is safe to resume.

## 7. Concurrency Model

The journal uses a single-writer model.

A companion lock file and nonblocking exclusive `flock` prevent
cooperating processes from simultaneously opening active writers.

This is a process-coordination mechanism, not a distributed
consensus protocol.

The system does not claim multi-host linearizability or
distributed transaction guarantees.

## 8. Failure Model

Relevant failure scenarios include:

- Process termination during append.
- Partial trailing records.
- Missing trailing newline.
- Corrupted records or broken hash chains.
- Write and `fsync` failures.
- Journal path replacement.
- Competing writer processes.

The test suite includes real process termination with `SIGKILL`
and fault-injection builds.

A dedicated test uses an interprocess pipe to acknowledge
successful appends before terminating the writer process.

The recovery test then checks that acknowledged records remain
present with the expected sequences and payloads.

These tests establish behavior under the tested process-failure
conditions, not physical power-loss guarantees.

## 9. Build and Verification

The project uses CMake and C++20.

The core library is built without the optional IBKR adapter
unless `EJ_WITH_IBKR` is explicitly enabled.

Tests use GoogleTest.

A separate test-core build enables journal fault injection
without changing the production core configuration.

The benchmark can be enabled with:

    -DEJ_BUILD_BENCHMARKS=ON

Its methodology and limitations are documented in
`benchmarks/README.md`.

The benchmark measures journal operations with synthetic payloads;
it does not measure end-to-end order execution.

## 10. Engineering Trade-offs

### Per-record fsync

Advantage: an explicit synchronization barrier before append success.

Cost: synchronization overhead on the critical write path.

The implementation prioritizes clear acknowledgement semantics
over maximizing append throughput.

### Single writer

Advantage: simpler ordering, sequence ownership, and recovery.

Cost: write serialization and limited concurrency.

### Hash-chained text records

Advantage: inspectable serialization and deterministic verification.

Cost: serialization overhead and no authenticated tamper resistance.

### Conservative recovery

Advantage: corruption is not silently reinterpreted as success.

Cost: some failures require operator investigation and prevent
automatic continuation.

## 11. Known Limitations

- No claim of production readiness.
- No claim of HFT suitability.
- No demonstrated physical power-loss durability.
- No demonstrated distributed-writer coordination.
- No cryptographic authentication of journal contents.
- No claim of validated live broker execution.
- No guarantee that journal integrity alone authorizes trading.
- Performance results depend on the test environment.

## 12. Design Principle

A journal can be internally consistent while the external broker
state remains uncertain.

Therefore:

    Journal integrity
        !=
    Broker reconciliation
        !=
    Authorization to resume trading

The system treats these as separate engineering concerns.

## 13. Decision Coordination and Risk Controls

### Durable decision recording

The coordinator evaluates an order intent against a versioned
risk configuration and the current net position supplied by its caller.

It serializes the resulting decision into a dedicated decision journal.

Both approvals and refusals are recorded.

A successful journal append is required before the coordinator
returns an `Approved` or `Refused` result.

A journal failure halts further evaluations through that
coordinator instance.

### Order identity

Order identifiers must contain 1–128 ASCII characters drawn from
letters, digits, hyphens, and underscores.

The coordinator rejects duplicate identifiers.

On startup, it reconstructs the set of known identifiers from
validated decision records.

Unknown, malformed, or duplicate decision records prevent
successful coordinator initialization.

This is local duplicate-decision prevention, not proof of
end-to-end broker idempotency.

### Risk controls

The risk gate checks:

1. Persistent kill-switch state.
2. Price bounds.
3. Per-order notional limits.
4. Projected absolute net-position limits.

Arithmetic overflow is handled conservatively.

The kill switch is checked at every evaluation rather than
cached. Filesystem uncertainty is treated as an active switch.

Each recorded decision includes the risk configuration version
and its content hash.

The current net position is an input to the evaluation.
This component does not independently verify that the supplied
position reflects the broker's current state.

## 14. Reconciliation and Recovery Authorization

### Reconciliation

The reconciliation component compares caller-supplied local
positions and open orders with a broker snapshot.

It classifies differences, including:

- Positions visible only at the broker.
- Positions visible only in local state.
- Quantity divergences.
- Orders visible only at the broker.
- Orders visible only in local state.

The reconciliation report allows trading only when the broker
snapshot is marked complete and no mismatches are detected.

This is a comparison mechanism, not evidence that live broker
queries or complete journal replay are implemented.

### Recovery gate

The recovery gate evaluates three conditions:

1. Journal integrity must have been verified.
2. Reconciliation must report no blocking divergence.
3. No unresolved approvals may remain.

Failure of any condition prevents a `Verified` recovery result.

Importantly, the recovery gate itself does not submit orders
or authorize live broker dispatch.

### Unresolved submission uncertainty

A durable approval is not evidence of broker submission.

If a process stops after recording an approval, the broker
may or may not have received a subsequent order.

Until that uncertainty can be resolved through a complete
submission and reconciliation protocol, the recovery gate
conservatively blocks recovery when unresolved approvals exist.

## 15. Current Integration Boundary

The reviewed code establishes durable decision recording,
risk evaluation, journal verification, reconciliation logic,
and conservative recovery gating.

It does not, by itself, establish:

- End-to-end broker submission.
- Exactly-once broker execution.
- Complete replay of positions from decision records.
- Automatic resolution of uncertain broker submissions.
- Production-safe restart authorization.

These are separate integration and verification requirements.
