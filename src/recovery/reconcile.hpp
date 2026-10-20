// Boot reconciliation — native INFRA-003.
//
// Local truth = replayed journal state. Broker truth = fresh snapshot.
// Every divergence is CLASSIFIED, never just flagged; any unresolved
// divergence forbids trading. The engine never guesses which side is
// right — it refuses and reports.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "broker/broker_port.hpp"

namespace ej::recovery {

struct LocalState {
  std::map<std::string, std::int64_t> positions;   // from journal replay
  std::map<std::string, std::int64_t> open_orders; // intents dispatched,
                                                   // not terminal
};

enum class MismatchKind : std::uint8_t {
  PhantomBrokerPosition,   // broker has it, journal doesn't
  MissingBrokerPosition,   // journal has it, broker doesn't
  QuantityDivergence,      // both have it, amounts differ
  PhantomBrokerOrder,      // broker shows an order we never journaled
  MissingBrokerOrder,      // we journaled a dispatch, broker shows nothing
};

struct Mismatch {
  MismatchKind kind;
  std::string key;          // symbol or order_id
  std::int64_t local_value;
  std::int64_t broker_value;
};

struct ReconcileReport {
  bool broker_snapshot_complete = false;
  std::vector<Mismatch> mismatches;
  [[nodiscard]] bool trading_allowed() const noexcept {
    return broker_snapshot_complete && mismatches.empty();
  }
};

[[nodiscard]] ReconcileReport reconcile(const LocalState& local,
                                        const ej::broker::BrokerSnapshot& broker);

}  // namespace ej::recovery
