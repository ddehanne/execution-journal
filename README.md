# execution-journal

C++20 execution integrity engine — order lifecycle, durable journaling,
crash recovery, and broker reconciliation for systematic trading systems.

**Status: v0.1.** All 7 modules complete. The IBKR adapter has been
exercised against a live paper gateway (read-only snapshot): session
establishment, position/order retrieval under ASan+UBSan, and clean
teardown — with positions cross-validated symbol-by-symbol against an
independent ledger implementation.

Companion to [verified-ledger](https://github.com/ddehanne/verified-ledger)
(Rust): the same correctness contract, carried from formal verification
into live-execution engineering. Where the Rust project proves the model
(Kani, Miri, fuzzing), this project confronts it with sockets, fsync,
threads, and a real broker API.

## Design laws (inherited from the Rust contract, adapted for execution)

- **The journal is the authoritative durable state** — not a write-ahead
  log for something else. State is reconstructed by replay.
- **Intent vs eligibility** — an order intent EXISTS from creation; the
  fsync durability barrier gates its ELIGIBILITY for broker dispatch. An
  intent is never submitted before its durable local proof exists. Local
  durability confirmation is distinct from broker acknowledgement.
- **Evidence over intention** — anomalous broker reports (fill before
  ack, fill during cancel, late fill after terminal, overfill) are
  representable, classified, and quarantined with evidence preserved.
- **Reconciliation before trading** — at boot, replayed local state is
  compared against a fresh broker snapshot; every divergence is
  classified; any unresolved mismatch forbids trading. The engine never
  guesses which side is right.
- **The kill switch is a persistent file latch** — survives restarts,
  checked fresh at every decision, cleared only by deliberate external
  act. Alerting components may alert; they never start or restart
  anything (a lesson paid for: see companion post-mortems PM-018/PM-019).
- **No exceptions for control flow** — fallible operations return typed
  values (`std::optional` / `std::variant`); invalid input is a value.

## Architecture
src/core/       strong types: UsdCents, Order/FilledQuantity, SequenceNumber
(checked arithmetic via _builtin*_overflow — overflow is
a value, never UB)
src/lifecycle/  deterministic order state machine, classified anomalies
src/journal/    durable append-only journal: seq|prev_hash|record_hash|payload,
local SHA-256 (FIPS 180-4 validated), fsync barrier,
torn-tail heal (atomic tmp+rename), interior corruption fatal
src/risk/       versioned RiskConfig (content hash), file-latch kill switch,
notional/position caps on projected |net|
src/broker/     BrokerPort interface + scriptable lying SimulatedBroker
src/recovery/   reconcile(): five mismatch classes; real incident classes
replayed as named tests (PhantomBrokerPosition_RGTIClass,
QuantityDivergence_PM018InversionClass)
src/ibkr/       IBKR TWS API adapter behind BrokerPort (pimpl; EReader
thread; bounded-blocking snapshot; fail-closed on timeout
by composition — partial snapshot ⇒ mismatches ⇒ refusal)
src/cli/        ej-audit verify|status|replay (exit 0/2/1)

## Verification

- Full test suite under **ASan+UBSan** (default build flags); strict
  warnings (`-Wall -Wextra -Wpedantic -Wconversion -Wshadow`) on our
  code, third-party isolated
- SHA-256 validated against FIPS 180-4 test vectors
- Journal corruption scenarios tested: torn final line (recovered +
  healed), missing trailing newline, interior corruption (fatal at exact
  line), removed interior line (chain break), crash → reopen → heal →
  append
- Fault injection with REAL process kills: SIGKILL mid-append storm
  (reopen heals), crash between durability barrier and dispatch (intent
  recovered as eligible-never-dispatched — no blind-resubmission path),
  mute-broker partial snapshot (trading refused by composition)
- TSan build: full suite, zero data races
- One-command health: `./check.sh` (build + ctest + CLI smoke)

## Build

Requires the IBKR TWS API (not vendored — see `third_party/README.md`).
mkdir -p build && cd build && cmake .. && make -j2 && ctest
## Roadmap beyond v0.1

- Broker-disconnect-mid-session fault scenario
- Micro-live: small real orders, measured slippage, duplicate-submission
  accounting across restarts
- TSan run against a live gateway session
