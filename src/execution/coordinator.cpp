#include "execution/coordinator.hpp"

#include <charconv>
#include <limits>
#include <string_view>
#include <utility>

namespace ej::execution {
namespace {

bool valid_id(const std::string& id) {
  if (id.empty() || id.size() > 128) return false;

  for (char raw : id) {
    const auto c = static_cast<unsigned char>(raw);
    if (!((c >= 'a' && c <= 'z') ||
          (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') ||
          c == '-' || c == '_')) {
      return false;
    }
  }
  return true;
}

template <typename T>
bool parse_number(std::string_view input, T& value) {
  if (input.empty()) return false;
  const auto* begin = input.data();
  const auto* end = begin + input.size();
  const auto result = std::from_chars(begin, end, value);
  return result.ec == std::errc{} && result.ptr == end;
}

std::optional<std::string> parse_id(const std::string& payload) {
  constexpr std::string_view prefix = "DECISION|";
  if (!payload.starts_with(prefix)) return std::nullopt;

  std::string_view fields[9];
  std::string_view remaining(payload);

  for (auto& field : fields) {
    const auto separator = remaining.find('|');
    if (separator == std::string_view::npos) {
      field = remaining;
      remaining = {};
    } else {
      field = remaining.substr(0, separator);
      remaining.remove_prefix(separator + 1);
    }
  }

  if (!remaining.empty() ||
      fields[0] != "DECISION" ||
      !valid_id(std::string(fields[1]))) {
    return std::nullopt;
  }

  const bool approved = fields[2] == "APPROVED";
  const bool refused = fields[2] == "REFUSED";
  if (!approved && !refused) return std::nullopt;

  auto strip = [](std::string_view value,
                  std::string_view key) -> std::string_view {
    if (!value.starts_with(key)) return {};
    return value.substr(key.size());
  };

  std::uint32_t version{};
  std::int64_t quantity{};
  std::int64_t price{};
  int reason{};

  if (!parse_number(strip(fields[3], "config_version="), version) ||
      !parse_number(strip(fields[6], "quantity="), quantity) ||
      !parse_number(strip(fields[7], "price_cents="), price) ||
      quantity <= 0 || price < 0) {
    return std::nullopt;
  }

  const auto hash = strip(fields[4], "config_hash=");
  if (hash.size() != 64) return std::nullopt;

  for (const char c : hash) {
    if (!((c >= '0' && c <= '9') ||
          (c >= 'a' && c <= 'f'))) {
      return std::nullopt;
    }
  }

  const auto side = strip(fields[5], "side=");
  if (side != "BUY" && side != "SELL") {
    return std::nullopt;
  }

  const auto reason_text = strip(fields[8], "reason=");
  if (approved) {
    if (reason_text != "NONE") return std::nullopt;
  } else {
    if (!parse_number(reason_text, reason) ||
        reason < 0 ||
        reason > static_cast<int>(
            ej::risk::RefusalReason::NotionalOverflow)) {
      return std::nullopt;
    }
  }

  return std::string(fields[1]);
}

std::string payload_for(const ej::risk::OrderIntent& intent,
                        const ej::risk::RiskConfig& config,
                        const std::optional<ej::risk::RefusalReason>& refusal) {
  const auto status = refusal ? "REFUSED" : "APPROVED";

  return "DECISION|" + intent.order_id + "|" + status +
         "|config_version=" + std::to_string(config.version) +
         "|config_hash=" + config.content_hash() +
         "|side=" + (intent.side == ej::risk::Side::Buy ? "BUY" : "SELL") +
         "|quantity=" + std::to_string(intent.quantity.units()) +
         "|price_cents=" + std::to_string(intent.limit_price.minor_units()) +
         "|reason=" + (refusal ? std::to_string(static_cast<int>(*refusal))
                              : "NONE");
}

}  // namespace

Coordinator::Coordinator(ej::journal::JournalWriter writer,
                         std::string kill_switch_path,
                         ej::risk::RiskConfig config,
                         std::set<std::string> known_ids)
    : writer_(std::move(writer)),
      kill_switch_path_(std::move(kill_switch_path)),
      config_(config),
      known_ids_(std::move(known_ids)) {}

std::variant<Coordinator, ej::journal::JournalError> Coordinator::open(
    const std::string& journal_path,
    const std::string& kill_switch_path,
    ej::risk::RiskConfig config) {
  // Acquire the exclusive writer lock before reconstructing state.
  auto opened = ej::journal::JournalWriter::open(journal_path);
  if (auto* error = std::get_if<ej::journal::JournalError>(&opened)) {
    return *error;
  }

  auto writer =
      std::move(std::get<ej::journal::JournalWriter>(opened));

  // A repaired journal may have lost an incomplete final decision.
  // Do not silently resume coordination after recovery.
  if (writer.recovery_repaired()) {
    return ej::journal::JournalError{
        ej::journal::JournalErrorKind::InteriorCorruption,
        0,
        "Journal recovery quarantine active: reconciliation required"};
  }

  auto recovered = writer.snapshot();
  if (auto* error = std::get_if<ej::journal::JournalError>(&recovered)) {
    return *error;
  }

  std::set<std::string> known_ids;
  for (const auto& record :
       std::get<ej::journal::Recovered>(recovered).records) {
    // This Coordinator owns a dedicated decision journal.
    // Unknown record types must not be silently ignored.
    const auto id = parse_id(record.payload);
    if (!id || !known_ids.insert(*id).second) {
      return ej::journal::JournalError{
          ej::journal::JournalErrorKind::InteriorCorruption,
          0,
          "Unknown, invalid or duplicate decision record"};
    }
  }

  return Coordinator{
      std::move(writer),
      kill_switch_path,
      config,
      std::move(known_ids)};
}

Decision Coordinator::evaluate(
    const ej::risk::OrderIntent& intent,
    std::int64_t current_net_position_units) {
  if (halted_) return DecisionError::JournalFailure;
  if (!valid_id(intent.order_id)) return DecisionError::InvalidOrderId;
  if (known_ids_.contains(intent.order_id)) {
    return DecisionError::DuplicateOrderId;
  }

  const bool killed = ej::risk::kill_switch_active(kill_switch_path_);
  const auto refusal = ej::risk::check_intent(
      config_, intent, current_net_position_units, killed);

  const auto sequence = writer_.next_sequence();
  if (sequence == std::numeric_limits<std::uint64_t>::max()) {
    halted_ = true;
    return DecisionError::JournalFailure;
  }

  const ej::journal::Record record{
      sequence,
      payload_for(intent, config_, refusal)};

  if (writer_.append(record).has_value()) {
    halted_ = true;
    return DecisionError::JournalFailure;
  }

  known_ids_.insert(intent.order_id);

  if (refusal) {
    return Refused{intent.order_id, *refusal, sequence};
  }

  return Approved{
      intent.order_id,
      ej::risk::Approval{config_.version, config_.content_hash()},
      sequence};
}

}  // namespace ej::execution
