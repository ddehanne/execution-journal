#include "lifecycle/order_state.hpp"

namespace ej {
namespace {

// Shared fill application: accumulate, classify by fill_status, quarantine
// overfill (dominates any other anomaly).
TransitionResult apply_fill(const Order& order, FilledQuantity qty,
                            std::optional<AnomalyKind> anomaly) noexcept {
  const auto accumulated = order.filled.checked_add(qty);
  if (!accumulated.has_value()) {
    return TransitionError::ArithmeticOverflow;
  }
  Order next = order;
  next.filled = *accumulated;

  switch (fill_status(order.ordered, *accumulated)) {
    case FillStatus::Partial:
      next.state = OrderState::PartiallyFilled;
      break;
    case FillStatus::Exact:
      next.state = OrderState::Filled;
      break;
    case FillStatus::Overfill:
      next.state = OrderState::IntegrityConflict;
      return Applied{next, Anomalous{AnomalyKind::Overfill}};
  }
  if (anomaly.has_value()) {
    return Applied{next, Anomalous{*anomaly}};
  }
  return Applied{next, Ordinary{}};
}

Order with_state(const Order& order, OrderState s) noexcept {
  Order next = order;
  next.state = s;
  return next;
}

}  // namespace

TransitionResult transition(const Order& order,
                            const OrderEvent& event) noexcept {
  using S = OrderState;
  const S s = order.state;

  // ── Ordinary lifecycle ─────────────────────────────────────────────
  if (std::holds_alternative<EvSubmitted>(event) && s == S::IntentCreated) {
    return Applied{with_state(order, S::Submitted), Ordinary{}};
  }
  if (std::holds_alternative<EvAcknowledged>(event) && s == S::Submitted) {
    return Applied{with_state(order, S::Acknowledged), Ordinary{}};
  }
  if (std::holds_alternative<EvRejected>(event) &&
      (s == S::IntentCreated || s == S::Submitted || s == S::Acknowledged)) {
    return Applied{with_state(order, S::Rejected), Ordinary{}};
  }
  if (std::holds_alternative<EvCancelRequested>(event) &&
      (s == S::Submitted || s == S::Acknowledged || s == S::PartiallyFilled)) {
    return Applied{with_state(order, S::CancelPending), Ordinary{}};
  }
  if (std::holds_alternative<EvCancelConfirmed>(event) && s == S::CancelPending) {
    return Applied{with_state(order, S::Cancelled), Ordinary{}};
  }

  // ── Executions ─────────────────────────────────────────────────────
  if (const auto* ex = std::get_if<EvExecution>(&event)) {
    if (s == S::Acknowledged || s == S::PartiallyFilled) {
      return apply_fill(order, ex->quantity, std::nullopt);
    }
    if (s == S::Submitted) {
      return apply_fill(order, ex->quantity, AnomalyKind::FillBeforeAck);
    }
    if (s == S::CancelPending) {
      return apply_fill(order, ex->quantity,
                        AnomalyKind::FillDuringCancelPending);
    }
    if (s == S::Filled || s == S::Cancelled || s == S::Rejected) {
      // Late fill after terminal: evidence PRESERVED, order quarantined.
      const auto accumulated = order.filled.checked_add(ex->quantity);
      if (!accumulated.has_value()) {
        return TransitionError::ArithmeticOverflow;
      }
      Order next = order;
      next.filled = *accumulated;
      next.state = S::IntegrityConflict;
      return Applied{next, Anomalous{AnomalyKind::LateFillAfterTerminal}};
    }
  }

  // ── Everything else: undeclared (INV-17) ──────────────────────────
  return TransitionError::UndeclaredTransition;
}

}  // namespace ej
