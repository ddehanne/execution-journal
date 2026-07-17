// Reconciliation tests — every lie the simulator can tell, incl. replays
// of the two real incidents (phantom position; PM-018 stale inversion).
#include <gtest/gtest.h>

#include "broker/broker_port.hpp"
#include "recovery/reconcile.hpp"

using namespace ej::broker;
using namespace ej::recovery;

TEST(Reconcile, CleanStateAllowsTrading) {
  LocalState local;
  local.positions["AAPL"] = 10;
  SimulatedBroker b;
  b.set_position("AAPL", 10);
  const auto r = reconcile(local, b.snapshot());
  EXPECT_TRUE(r.trading_allowed());
}

TEST(Reconcile, PhantomBrokerPosition_RGTIClass) {
  // Broker reports a position the journal has never seen.
  LocalState local;
  SimulatedBroker b;
  b.set_position("RGTI", 1000);
  const auto r = reconcile(local, b.snapshot());
  ASSERT_EQ(r.mismatches.size(), 1u);
  EXPECT_EQ(r.mismatches[0].kind, MismatchKind::PhantomBrokerPosition);
  EXPECT_EQ(r.mismatches[0].broker_value, 1000);
  EXPECT_FALSE(r.trading_allowed());
}

TEST(Reconcile, QuantityDivergence_PM018InversionClass) {
  // Journal believes long 32; broker is short -32 (the stale-state
  // double-liquidation inversion). Must be a divergence, not silence.
  LocalState local;
  local.positions["WM"] = 32;
  SimulatedBroker b;
  b.set_position("WM", -32);
  const auto r = reconcile(local, b.snapshot());
  ASSERT_EQ(r.mismatches.size(), 1u);
  EXPECT_EQ(r.mismatches[0].kind, MismatchKind::QuantityDivergence);
  EXPECT_EQ(r.mismatches[0].local_value, 32);
  EXPECT_EQ(r.mismatches[0].broker_value, -32);
  EXPECT_FALSE(r.trading_allowed());
}

TEST(Reconcile, MissingBrokerPositionAndZeroIsNotAPosition) {
  LocalState local;
  local.positions["NET"] = 17;
  local.positions["FLAT"] = 0;  // zero local must NOT demand broker presence
  SimulatedBroker b;
  b.set_position("FLAT", 0);    // zero broker must NOT read as phantom
  const auto r = reconcile(local, b.snapshot());
  ASSERT_EQ(r.mismatches.size(), 1u);
  EXPECT_EQ(r.mismatches[0].kind, MismatchKind::MissingBrokerPosition);
  EXPECT_EQ(r.mismatches[0].key, "NET");
}

TEST(Reconcile, OrderBookLies) {
  LocalState local;
  local.open_orders["ord-A"] = 5;  // dispatched per journal
  SimulatedBroker b;
  b.set_open_order("ord-B", 3);    // broker shows an unknown order
  const auto r = reconcile(local, b.snapshot());
  ASSERT_EQ(r.mismatches.size(), 2u);
  bool phantom = false, missing = false;
  for (const auto& m : r.mismatches) {
    phantom |= (m.kind == MismatchKind::PhantomBrokerOrder && m.key == "ord-B");
    missing |= (m.kind == MismatchKind::MissingBrokerOrder && m.key == "ord-A");
  }
  EXPECT_TRUE(phantom);
  EXPECT_TRUE(missing);
  EXPECT_FALSE(r.trading_allowed());
}
