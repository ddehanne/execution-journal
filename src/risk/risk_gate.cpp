#include "risk/risk_gate.hpp"

#include <sys/stat.h>

#include <sstream>

#include "journal/sha256.hpp"

namespace ej::risk {

std::string RiskConfig::content_hash() const {
  std::ostringstream os;
  os << version << '|' << max_order_notional.minor_units() << '|'
     << max_position_units << '|' << min_price.minor_units() << '|'
     << max_price.minor_units();
  return ej::sha256::hex_digest(os.str());
}

bool kill_switch_active(const std::string& latch_path) {
  struct stat st{};
  return ::stat(latch_path.c_str(), &st) == 0;
}

std::optional<RefusalReason> check_intent(
    const RiskConfig& config, const OrderIntent& intent,
    std::int64_t current_net_position_units, bool kill_switch) {
  // Order of checks is part of the contract: the kill switch dominates.
  if (kill_switch) {
    return RefusalReason::KillSwitchActive;
  }

  // Price sanity band.
  if (intent.limit_price < config.min_price ||
      intent.limit_price > config.max_price) {
    return RefusalReason::PriceOutOfBand;
  }

  // Notional cap (checked arithmetic — overflow is its own refusal).
  const auto notional = intent.limit_price.checked_mul(intent.quantity.units());
  if (!notional.has_value()) {
    return RefusalReason::NotionalOverflow;
  }
  if (*notional > config.max_order_notional) {
    return RefusalReason::NotionalCapExceeded;
  }

  // Position cap: worst-case resulting |net| if fully filled.
  const std::int64_t signed_units = intent.side == Side::Buy
                                        ? intent.quantity.units()
                                        : -intent.quantity.units();
  std::int64_t projected{};
  if (__builtin_add_overflow(current_net_position_units, signed_units,
                             &projected)) {
    return RefusalReason::PositionCapExceeded;  // overflow => certainly over
  }
  const std::int64_t abs_projected = projected < 0 ? -projected : projected;
  if (abs_projected > config.max_position_units) {
    return RefusalReason::PositionCapExceeded;
  }

  return std::nullopt;  // approved under this config
}

}  // namespace ej::risk
