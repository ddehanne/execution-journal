#include "recovery/reconcile.hpp"

namespace ej::recovery {
namespace {

void diff_maps(const std::map<std::string, std::int64_t>& local,
               const std::map<std::string, std::int64_t>& broker,
               MismatchKind phantom, MismatchKind missing,
               MismatchKind divergence, std::vector<Mismatch>& out) {
  for (const auto& [key, bval] : broker) {
    const auto it = local.find(key);
    if (it == local.end() || it->second == 0) {
      if (bval != 0) out.push_back({phantom, key, 0, bval});
    } else if (it->second != bval) {
      out.push_back({divergence, key, it->second, bval});
    }
  }
  for (const auto& [key, lval] : local) {
    if (lval == 0) continue;
    const auto it = broker.find(key);
    if (it == broker.end() || it->second == 0) {
      out.push_back({missing, key, lval, 0});
    }
  }
}

}  // namespace

ReconcileReport reconcile(const LocalState& local,
                          const ej::broker::BrokerSnapshot& broker) {
  ReconcileReport report;
  report.broker_snapshot_complete = broker.complete;
  diff_maps(local.positions, broker.positions,
            MismatchKind::PhantomBrokerPosition,
            MismatchKind::MissingBrokerPosition,
            MismatchKind::QuantityDivergence, report.mismatches);
  diff_maps(local.open_orders, broker.open_orders,
            MismatchKind::PhantomBrokerOrder, MismatchKind::MissingBrokerOrder,
            MismatchKind::QuantityDivergence, report.mismatches);
  return report;
}

}  // namespace ej::recovery
