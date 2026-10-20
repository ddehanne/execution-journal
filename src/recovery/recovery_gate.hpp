#pragma once

#include <cstddef>
#include <cstdint>

#include "recovery/reconcile.hpp"

namespace ej::recovery {

// M8-01: conservative recovery gate.
//
// Preconditions:
// - journal_integrity_verified is established by validated journal
//   recovery, not inferred from a broker snapshot.
// - unresolved_approvals is derived from durable decision evidence.
// - An empty broker snapshot is not proof of non-submission.
//
// This component never authorizes broker submission or trading.

enum class RecoveryStatus : std::uint8_t {
  Verified,
  BlockedIntegrity,
  BlockedMismatch,
  BlockedUnknownSubmission,
};

struct RecoveryEvidence {
  bool journal_integrity_verified = false;
  std::size_t unresolved_approvals = 0;
};

struct RecoveryGateReport {
  RecoveryStatus status;

  [[nodiscard]] bool verified() const noexcept {
    return status == RecoveryStatus::Verified;
  }
};

[[nodiscard]] inline RecoveryGateReport evaluate_recovery(
    const RecoveryEvidence& evidence,
    const ReconcileReport& reconciliation) noexcept {

  // Integrity failure dominates every other condition.
  if (!evidence.journal_integrity_verified) {
    return {RecoveryStatus::BlockedIntegrity};
  }

  if (!reconciliation.trading_allowed()) {
    return {RecoveryStatus::BlockedMismatch};
  }

  // An approved decision is NOT evidence of broker submission.
  // Until a durable submission protocol exists, block recovery.
  if (evidence.unresolved_approvals != 0) {
    return {RecoveryStatus::BlockedUnknownSubmission};
  }

  return {RecoveryStatus::Verified};
}

}  // namespace ej::recovery
