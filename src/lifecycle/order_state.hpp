// Deterministic order state machine — mirror of verified-ledger order.rs.
// The ledger records EXTERNAL EVIDENCE: late/contradictory broker reports
// are representable and classified, never dropped, never UB, never thrown.
// Undeclared transitions: typed error, provably untouched input (INV-17 —
// enforced here by value semantics: transition() is a pure function).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "core/types.hpp"

namespace ej {

enum class OrderState : std::uint8_t {
  IntentCreated,
  Submitted,
  Acknowledged,
  PartiallyFilled,
  CancelPending,
  Filled,
  Cancelled,
  Rejected,
  IntegrityConflict,  // evidence contradicts local knowledge — quarantined
};

[[nodiscard]] constexpr bool is_terminal(OrderState s) noexcept {
  return s == OrderState::Filled || s == OrderState::Cancelled ||
         s == OrderState::Rejected || s == OrderState::IntegrityConflict;
}

enum class AnomalyKind : std::uint8_t {
  FillBeforeAck,
  FillDuringCancelPending,
  LateFillAfterTerminal,
  Overfill,
};

// Classification of an APPLIED transition.
struct Ordinary {};
struct Anomalous {
  AnomalyKind kind;
};
using TransitionClass = std::variant<Ordinary, Anomalous>;

// Order aggregate (value type — transition is pure).
struct Order {
  std::string id;
  OrderQuantity ordered;
  FilledQuantity filled;
  OrderState state;

  static Order create(std::string id, OrderQuantity ordered) noexcept {
    return Order{std::move(id), ordered, FilledQuantity::zero(),
                 OrderState::IntentCreated};
  }
};

// Order-scoped events (mirror of Rust OrderEvent).
struct EvSubmitted {};
struct EvAcknowledged {};
struct EvRejected {};
struct EvExecution {
  FilledQuantity quantity;
};
struct EvCancelRequested {};
struct EvCancelConfirmed {};
using OrderEvent = std::variant<EvSubmitted, EvAcknowledged, EvRejected,
                                EvExecution, EvCancelRequested,
                                EvCancelConfirmed>;

// Typed transition errors — never thrown, returned as values.
enum class TransitionError : std::uint8_t {
  UndeclaredTransition,  // INV-17: not in the table
  ArithmeticOverflow,    // fill accumulation overflow
};

struct Applied {
  Order order;  // the NEW order value (input is untouched by construction)
  TransitionClass cls;
};
using TransitionResult = std::variant<Applied, TransitionError>;

// Pure exhaustive transition function.
[[nodiscard]] TransitionResult transition(const Order& order,
                                          const OrderEvent& event) noexcept;

}  // namespace ej
