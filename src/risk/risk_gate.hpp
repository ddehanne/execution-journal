// Pre-trade risk gate — versioned config, persistent kill switch.
//
// Laws:
// - every intent passes ALL checks or is refused with a typed reason
// - the kill switch is a persistent file latch: it survives restarts and
//   is cleared only by a deliberate external act (file removal), never
//   by this process
// - the config carries a version + content hash; approvals are meaningful
//   only relative to the config that granted them
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/types.hpp"

namespace ej::risk {

struct RiskConfig {
  std::uint32_t version;
  ej::UsdCents max_order_notional;   // per-order cap
  std::int64_t max_position_units;   // absolute net position cap
  ej::UsdCents min_price;            // sanity band: refuse absurd prices
  ej::UsdCents max_price;
  [[nodiscard]] std::string content_hash() const;  // SHA-256 of fields
};

enum class Side : std::uint8_t { Buy, Sell };

struct OrderIntent {
  std::string order_id;
  Side side;
  ej::OrderQuantity quantity;
  ej::UsdCents limit_price;
};

enum class RefusalReason : std::uint8_t {
  KillSwitchActive,
  NotionalCapExceeded,
  PositionCapExceeded,
  PriceOutOfBand,
  NotionalOverflow,
};

struct Approval {
  std::uint32_t config_version;
  std::string config_hash;
};
struct Refusal {
  RefusalReason reason;
};

// Kill switch = file existence. Checked FRESH at every decision —
// no caching (a stale cached "off" is how kill switches fail).
[[nodiscard]] bool kill_switch_active(const std::string& latch_path);

[[nodiscard]] std::optional<RefusalReason> check_intent(
    const RiskConfig& config, const OrderIntent& intent,
    std::int64_t current_net_position_units, bool kill_switch);

}  // namespace ej::risk
