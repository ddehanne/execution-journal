// IBKR adapter — implements BrokerPort against the official TWS API.
// The ONLY impure module: sockets, threads, broker session.
//
// Concurrency model (TSan-checked):
// - EReader runs on its own thread, invoking EWrapper callbacks
// - snapshot() issues reqPositions/reqAllOpenOrders and blocks (bounded)
//   on a condition_variable until the *End callbacks fire
// - all shared state behind one mutex; no lock-free cleverness
#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

#include "broker/broker_port.hpp"

namespace ej::ibkr {

struct ConnectParams {
  std::string host = "127.0.0.1";
  int port = 4002;          // paper gateway
  int client_id = 71;       // distinct from pipeline writers (44/45…)
  int timeout_ms = 15000;
};

class IbkrBroker final : public ej::broker::BrokerPort {
 public:
  // Factory: connect + start EReader. nullopt on failure (typed logs).
  static std::unique_ptr<IbkrBroker> connect(const ConnectParams& params);
  ~IbkrBroker() override;

  IbkrBroker(const IbkrBroker&) = delete;
  IbkrBroker& operator=(const IbkrBroker&) = delete;

  // Bounded-blocking fresh snapshot. Throws nothing; on timeout returns
  // what arrived (caller sees partial => reconcile mismatches => refuse).
  [[nodiscard]] ej::broker::BrokerSnapshot snapshot() override;

 private:
  IbkrBroker();
  struct Impl;                    // hides all TWS API types from our headers
  std::unique_ptr<Impl> impl_;
};

}  // namespace ej::ibkr
