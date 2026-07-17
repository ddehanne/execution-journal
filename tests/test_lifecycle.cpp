// Contract parity with verified-ledger S3 tests.
#include <gtest/gtest.h>

#include "lifecycle/order_state.hpp"

using namespace ej;

namespace {
Order make_order(std::int64_t qty) {
  return Order::create("ord-1", *OrderQuantity::make(qty));
}
FilledQuantity fq(std::int64_t n) { return *FilledQuantity::make(n); }
const Applied& ok(const TransitionResult& r) {
  return std::get<Applied>(r);
}
}  // namespace

TEST(Lifecycle, OrdinaryPathToFilled) {
  auto o = make_order(10);
  o = ok(transition(o, EvSubmitted{})).order;
  o = ok(transition(o, EvAcknowledged{})).order;
  auto r = ok(transition(o, EvExecution{fq(4)}));
  EXPECT_EQ(r.order.state, OrderState::PartiallyFilled);
  EXPECT_TRUE(std::holds_alternative<Ordinary>(r.cls));
  o = ok(transition(r.order, EvExecution{fq(6)})).order;
  EXPECT_EQ(o.state, OrderState::Filled);
}

TEST(Lifecycle, FillBeforeAckAnomalousButApplied) {
  auto o = make_order(5);
  o = ok(transition(o, EvSubmitted{})).order;
  auto r = ok(transition(o, EvExecution{fq(5)}));
  EXPECT_EQ(r.order.state, OrderState::Filled);
  const auto* a = std::get_if<Anomalous>(&r.cls);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->kind, AnomalyKind::FillBeforeAck);
}

TEST(Lifecycle, LateFillAfterCancelQuarantinedEvidencePreserved) {
  auto o = make_order(10);
  o = ok(transition(o, EvSubmitted{})).order;
  o = ok(transition(o, EvCancelRequested{})).order;
  o = ok(transition(o, EvCancelConfirmed{})).order;
  ASSERT_EQ(o.state, OrderState::Cancelled);
  auto r = ok(transition(o, EvExecution{fq(3)}));
  EXPECT_EQ(r.order.state, OrderState::IntegrityConflict);
  EXPECT_EQ(r.order.filled.units(), 3);  // evidence preserved
  const auto* a = std::get_if<Anomalous>(&r.cls);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->kind, AnomalyKind::LateFillAfterTerminal);
}

TEST(Lifecycle, OverfillQuarantinesWithEvidence) {
  auto o = make_order(10);
  o = ok(transition(o, EvSubmitted{})).order;
  o = ok(transition(o, EvAcknowledged{})).order;
  auto r = ok(transition(o, EvExecution{fq(15)}));
  EXPECT_EQ(r.order.state, OrderState::IntegrityConflict);
  EXPECT_EQ(r.order.filled.units(), 15);
  const auto* a = std::get_if<Anomalous>(&r.cls);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->kind, AnomalyKind::Overfill);
}

TEST(Lifecycle, UndeclaredTransitionTypedErrorInputUntouched) {
  const auto o = make_order(10);  // IntentCreated
  const auto before = o;
  const auto r = transition(o, EvCancelConfirmed{});
  const auto* err = std::get_if<TransitionError>(&r);
  ASSERT_NE(err, nullptr);
  EXPECT_EQ(*err, TransitionError::UndeclaredTransition);
  EXPECT_EQ(o.state, before.state);  // pure by construction
  EXPECT_EQ(o.filled.units(), before.filled.units());
}
