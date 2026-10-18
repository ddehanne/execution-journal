#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <variant>

#include "journal/journal.hpp"
#include "risk/risk_gate.hpp"

namespace ej::execution {

enum class DecisionError {
  InvalidOrderId,
  DuplicateOrderId,
  JournalFailure,
};

struct Approved {
  std::string order_id;
  ej::risk::Approval risk_approval;
  std::uint64_t journal_sequence;
};

struct Refused {
  std::string order_id;
  ej::risk::RefusalReason reason;
  std::uint64_t journal_sequence;
};

using Decision = std::variant<Approved, Refused, DecisionError>;

class Coordinator {
 public:
  static std::variant<Coordinator, ej::journal::JournalError> open(
      const std::string& journal_path,
      const std::string& kill_switch_path,
      ej::risk::RiskConfig config);

  Coordinator(Coordinator&&) noexcept = default;
  Coordinator& operator=(Coordinator&&) noexcept = default;

  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;

  [[nodiscard]] Decision evaluate(
      const ej::risk::OrderIntent& intent,
      std::int64_t current_net_position_units);

 private:
  Coordinator(ej::journal::JournalWriter writer,
              std::string kill_switch_path,
              ej::risk::RiskConfig config,
              std::set<std::string> known_ids);

  ej::journal::JournalWriter writer_;
  std::string kill_switch_path_;
  ej::risk::RiskConfig config_;
  std::set<std::string> known_ids_;
  bool halted_ = false;
};

}  // namespace ej::execution
