// Contract parity tests with verified-ledger S1 (Rust): overflow is a
// value (nullopt), never UB; validation at construction; overfill
// representable and named; sequence successor law.
#include <gtest/gtest.h>

#include "core/types.hpp"

using namespace ej;

TEST(UsdCents, AddIsExact) {
  const auto r = UsdCents{10'050}.checked_add(UsdCents{25});
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->minor_units(), 10'075);
}

TEST(UsdCents, OverflowIsNulloptNotUB) {
  EXPECT_FALSE(UsdCents{INT64_MAX}.checked_add(UsdCents{1}).has_value());
  EXPECT_FALSE(UsdCents{INT64_MIN}.checked_sub(UsdCents{1}).has_value());
  EXPECT_FALSE(UsdCents{INT64_MAX}.checked_mul(2).has_value());
}

TEST(Quantities, ValidationAtConstruction) {
  EXPECT_FALSE(OrderQuantity::make(0).has_value());
  EXPECT_FALSE(OrderQuantity::make(-5).has_value());
  ASSERT_TRUE(OrderQuantity::make(1).has_value());
  EXPECT_FALSE(FilledQuantity::make(-1).has_value());
  EXPECT_TRUE(FilledQuantity::make(0).has_value());
}

TEST(Quantities, OverfillRepresentableAndNamed) {
  const auto ordered = OrderQuantity::make(100);
  const auto filled = FilledQuantity::make(150);
  ASSERT_TRUE(ordered && filled);
  EXPECT_EQ(fill_status(*ordered, *filled), FillStatus::Overfill);
}

TEST(Quantities, FillAccumulationChecked) {
  const auto a = FilledQuantity::make(INT64_MAX);
  const auto b = FilledQuantity::make(1);
  ASSERT_TRUE(a && b);
  EXPECT_FALSE(a->checked_add(*b).has_value());
}

TEST(Sequence, SuccessorLaw) {
  const auto s = SequenceNumber{41};
  const auto n = s.next();
  ASSERT_TRUE(n.has_value());
  EXPECT_TRUE(n->is_successor_of(s));
  EXPECT_FALSE(SequenceNumber{43}.is_successor_of(s));  // gap
  EXPECT_FALSE(SequenceNumber{41}.is_successor_of(s));  // duplicate
  EXPECT_FALSE(SequenceNumber{UINT64_MAX}.next().has_value());
}
