#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>

#include "risk/risk_gate.hpp"

using namespace ej;
using namespace ej::risk;

namespace {
RiskConfig cfg() {
  return RiskConfig{
      1,
      UsdCents{100'000 * 100},  // $100k notional cap
      1'000,                    // 1000 units position cap
      UsdCents{100},            // $1.00 min
      UsdCents{1'000'000},      // $10,000 max
  };
}
OrderIntent intent(Side s, std::int64_t qty, std::int64_t price_cents) {
  return OrderIntent{"ord-1", s, *OrderQuantity::make(qty),
                     UsdCents{price_cents}};
}
}  // namespace

TEST(Risk, KillSwitchDominatesEverything) {
  // Even a perfectly valid order is refused when the latch is on.
  const auto r = check_intent(cfg(), intent(Side::Buy, 1, 10'000), 0, true);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, RefusalReason::KillSwitchActive);
}

TEST(Risk, KillSwitchIsFileLatch) {
  const std::string latch = "/tmp/ej_kill_latch_test";
  std::remove(latch.c_str());
  EXPECT_FALSE(kill_switch_active(latch));
  std::ofstream(latch) << "halt";
  EXPECT_TRUE(kill_switch_active(latch));
  std::remove(latch.c_str());
  EXPECT_FALSE(kill_switch_active(latch));
}

TEST(Risk, NotionalCap) {
  // 200 units × $600 = $120k > $100k cap
  const auto r = check_intent(cfg(), intent(Side::Buy, 200, 60'000), 0, false);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, RefusalReason::NotionalCapExceeded);
  // 100 × $600 = $60k: fine
  EXPECT_FALSE(check_intent(cfg(), intent(Side::Buy, 100, 60'000), 0, false)
                   .has_value());
}

TEST(Risk, PositionCapOnProjectedNet) {
  // Net 950 + Buy 100 => 1050 > 1000
  auto r = check_intent(cfg(), intent(Side::Buy, 100, 1'000), 950, false);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, RefusalReason::PositionCapExceeded);
  // Short side symmetric: net -950, Sell 100 => |-1050| > 1000
  r = check_intent(cfg(), intent(Side::Sell, 100, 1'000), -950, false);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, RefusalReason::PositionCapExceeded);
  // Reducing exposure is fine: net 950, Sell 100 => 850
  EXPECT_FALSE(check_intent(cfg(), intent(Side::Sell, 100, 1'000), 950, false)
                   .has_value());
}

TEST(Risk, PriceBandAndOverflow) {
  auto r = check_intent(cfg(), intent(Side::Buy, 1, 50), 0, false);  // $0.50
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, RefusalReason::PriceOutOfBand);
  r = check_intent(cfg(), intent(Side::Buy, 1, 2'000'000), 0, false);  // $20k
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, RefusalReason::PriceOutOfBand);
}

TEST(Risk, ConfigHashDeterministicAndVersionSensitive) {
  const auto a = cfg().content_hash();
  EXPECT_EQ(a, cfg().content_hash());
  auto c2 = cfg();
  c2.version = 2;
  EXPECT_NE(a, c2.content_hash());
}
