// Broker port — the ONLY surface the engine sees. The real IBKR adapter
// (M6) and the lying test simulator implement the same interface.
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace ej::broker {

struct BrokerSnapshot {
  // symbol -> signed net units as REPORTED by the broker
  std::map<std::string, std::int64_t> positions;
  // order_id -> broker-visible open order (units remaining)
  std::map<std::string, std::int64_t> open_orders;
};

class BrokerPort {
 public:
  virtual ~BrokerPort() = default;
  [[nodiscard]] virtual BrokerSnapshot snapshot() = 0;
};

// Test double: a broker whose reported truth is fully scriptable —
// including lies (phantom positions, missing orders, wrong quantities).
class SimulatedBroker final : public BrokerPort {
 public:
  [[nodiscard]] BrokerSnapshot snapshot() override { return state_; }
  void set_position(const std::string& symbol, std::int64_t units) {
    state_.positions[symbol] = units;
  }
  void set_open_order(const std::string& order_id, std::int64_t remaining) {
    state_.open_orders[order_id] = remaining;
  }

 private:
  BrokerSnapshot state_;
};

}  // namespace ej::broker
