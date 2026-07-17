// Strong types — mirror of the verified-ledger (Rust) core contract.
// Same laws: exact integer money, validated quantities, sequence as
// ordering authority, checked arithmetic everywhere. No exceptions for
// control flow: fallible operations return std::optional / expected-like
// results; invalid input is a value, never UB, never a throw.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace ej {

// ── Money ────────────────────────────────────────────────────────────
// USD minor units (cents), exact. Overflow => std::nullopt (checked via
// __builtin_*_overflow: defined behavior on all inputs).
class UsdCents {
 public:
  constexpr explicit UsdCents(std::int64_t minor_units) noexcept
      : minor_units_(minor_units) {}
  static constexpr UsdCents zero() noexcept { return UsdCents{0}; }

  [[nodiscard]] constexpr std::int64_t minor_units() const noexcept {
    return minor_units_;
  }

  [[nodiscard]] std::optional<UsdCents> checked_add(UsdCents rhs) const noexcept {
    std::int64_t out{};
    if (__builtin_add_overflow(minor_units_, rhs.minor_units_, &out)) {
      return std::nullopt;
    }
    return UsdCents{out};
  }

  [[nodiscard]] std::optional<UsdCents> checked_sub(UsdCents rhs) const noexcept {
    std::int64_t out{};
    if (__builtin_sub_overflow(minor_units_, rhs.minor_units_, &out)) {
      return std::nullopt;
    }
    return UsdCents{out};
  }

  [[nodiscard]] std::optional<UsdCents> checked_mul(std::int64_t factor) const noexcept {
    std::int64_t out{};
    if (__builtin_mul_overflow(minor_units_, factor, &out)) {
      return std::nullopt;
    }
    return UsdCents{out};
  }

  constexpr bool operator==(const UsdCents&) const noexcept = default;
  constexpr auto operator<=>(const UsdCents&) const noexcept = default;

 private:
  std::int64_t minor_units_;
};

// ── Quantities ───────────────────────────────────────────────────────
// Ordered vs filled are DISTINCT types (as in Rust): comparing them goes
// through one named function. Overfill is representable, classified.
class OrderQuantity {
 public:
  // Validated factory: strictly positive or nullopt.
  static std::optional<OrderQuantity> make(std::int64_t units) noexcept {
    if (units <= 0) return std::nullopt;
    return OrderQuantity{units};
  }
  [[nodiscard]] constexpr std::int64_t units() const noexcept { return units_; }
  constexpr bool operator==(const OrderQuantity&) const noexcept = default;

 private:
  constexpr explicit OrderQuantity(std::int64_t u) noexcept : units_(u) {}
  std::int64_t units_;
};

class FilledQuantity {
 public:
  static constexpr FilledQuantity zero() noexcept { return FilledQuantity{0}; }
  // Validated factory: non-negative or nullopt.
  static std::optional<FilledQuantity> make(std::int64_t units) noexcept {
    if (units < 0) return std::nullopt;
    return FilledQuantity{units};
  }
  [[nodiscard]] constexpr std::int64_t units() const noexcept { return units_; }

  [[nodiscard]] std::optional<FilledQuantity> checked_add(FilledQuantity more) const noexcept {
    std::int64_t out{};
    if (__builtin_add_overflow(units_, more.units_, &out)) return std::nullopt;
    return FilledQuantity{out};
  }
  constexpr bool operator==(const FilledQuantity&) const noexcept = default;

 private:
  constexpr explicit FilledQuantity(std::int64_t u) noexcept : units_(u) {}
  std::int64_t units_;
};

enum class FillStatus { Partial, Exact, Overfill };

// The ONLY sanctioned ordered-vs-filled comparison.
[[nodiscard]] constexpr FillStatus fill_status(OrderQuantity ordered,
                                               FilledQuantity filled) noexcept {
  const auto o = ordered.units();
  const auto f = filled.units();
  if (f < o) return FillStatus::Partial;
  if (f == o) return FillStatus::Exact;
  return FillStatus::Overfill;
}

// ── Sequence ─────────────────────────────────────────────────────────
// Journal ordering authority: contiguous, checked exhaustion.
class SequenceNumber {
 public:
  static constexpr SequenceNumber first() noexcept { return SequenceNumber{1}; }
  constexpr explicit SequenceNumber(std::uint64_t v) noexcept : value_(v) {}
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  [[nodiscard]] std::optional<SequenceNumber> next() const noexcept {
    if (value_ == UINT64_MAX) return std::nullopt;
    return SequenceNumber{value_ + 1};
  }
  [[nodiscard]] constexpr bool is_successor_of(SequenceNumber prev) const noexcept {
    return value_ > prev.value_ && value_ - prev.value_ == 1;
  }
  constexpr bool operator==(const SequenceNumber&) const noexcept = default;

 private:
  std::uint64_t value_;
};

}  // namespace ej
