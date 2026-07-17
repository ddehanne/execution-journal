#include "ibkr/ibkr_broker.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"
#include "Decimal.h"
#include "Order.h"

namespace ej::ibkr {

using ej::broker::BrokerSnapshot;

// ── Impl: wrapper + socket + reader thread + snapshot sync ───────────
struct IbkrBroker::Impl final : public DefaultEWrapper {
  // TWS plumbing
  EReaderOSSignal signal{2000 /*ms*/};
  std::unique_ptr<EClientSocket> client;
  std::unique_ptr<EReader> reader;
  std::thread reader_thread;
  std::atomic<bool> running{false};

  // snapshot assembly (guarded by m)
  std::mutex m;
  std::condition_variable cv;
  BrokerSnapshot building;
  bool positions_done = false;
  bool orders_done = false;
  bool connected = false;

  // ── EWrapper callbacks we care about ───────────────────────────────
  void nextValidId(OrderId) override {
    std::lock_guard<std::mutex> lk(m);
    connected = true;
    cv.notify_all();
  }

  void position(const std::string& /*account*/, const Contract& contract,
                Decimal pos, double /*avgCost*/) override {
    std::lock_guard<std::mutex> lk(m);
    // Decimal -> long long via IBKR's own conversion helper
    const long long units =
        static_cast<long long>(DecimalFunctions::decimalToDouble(pos));
    building.positions[contract.symbol] = units;
  }

  void positionEnd() override {
    std::lock_guard<std::mutex> lk(m);
    positions_done = true;
    cv.notify_all();
  }

  void openOrder(OrderId orderId, const Contract& /*c*/, const Order& order,
                 const OrderState& /*state*/) override {
    std::lock_guard<std::mutex> lk(m);
    const long long remaining =
        static_cast<long long>(DecimalFunctions::decimalToDouble(order.totalQuantity)) -
        static_cast<long long>(DecimalFunctions::decimalToDouble(order.filledQuantity));
    building.open_orders[std::to_string(orderId)] = remaining;
  }

  void openOrderEnd() override {
    std::lock_guard<std::mutex> lk(m);
    orders_done = true;
    cv.notify_all();
  }

  void error(int id, int code, const std::string& msg,
             const std::string& /*advancedOrderRejectJson*/) override {
    // Informational codes are frequent; log-and-continue is correct here.
    std::fprintf(stderr, "[ibkr] id=%d code=%d %s\n", id, code, msg.c_str());
  }

  // ── reader loop ────────────────────────────────────────────────────
  void run_reader() {
    while (running.load() && client && client->isConnected()) {
      signal.waitForSignal();
      reader->processMsgs();
    }
  }
};

IbkrBroker::IbkrBroker() : impl_(std::make_unique<Impl>()) {}

IbkrBroker::~IbkrBroker() {
  impl_->running.store(false);
  if (impl_->client && impl_->client->isConnected()) {
    impl_->client->eDisconnect();
  }
  impl_->signal.issueSignal();  // unblock waitForSignal for clean join
  if (impl_->reader_thread.joinable()) {
    impl_->reader_thread.join();
  }
}

std::unique_ptr<IbkrBroker> IbkrBroker::connect(const ConnectParams& p) {
  auto broker = std::unique_ptr<IbkrBroker>(new IbkrBroker());
  auto& i = *broker->impl_;

  i.client = std::make_unique<EClientSocket>(&i, &i.signal);
  if (!i.client->eConnect(p.host.c_str(), p.port, p.client_id)) {
    std::fprintf(stderr, "[ibkr] eConnect failed %s:%d\n", p.host.c_str(),
                 p.port);
    return nullptr;
  }
  i.reader = std::make_unique<EReader>(i.client.get(), &i.signal);
  i.reader->start();
  i.running.store(true);
  i.reader_thread = std::thread([&i] { i.run_reader(); });

  // Wait (bounded) for nextValidId — the session-ready signal.
  std::unique_lock<std::mutex> lk(i.m);
  const bool ok = i.cv.wait_for(lk, std::chrono::milliseconds(p.timeout_ms),
                                [&i] { return i.connected; });
  if (!ok) {
    std::fprintf(stderr, "[ibkr] session-ready timeout\n");
    lk.unlock();
    return nullptr;
  }
  return broker;
}

BrokerSnapshot IbkrBroker::snapshot() {
  auto& i = *impl_;
  {
    std::lock_guard<std::mutex> lk(i.m);
    i.building = BrokerSnapshot{};
    i.positions_done = false;
    i.orders_done = false;
  }
  i.client->reqPositions();
  i.client->reqAllOpenOrders();

  std::unique_lock<std::mutex> lk(i.m);
  i.cv.wait_for(lk, std::chrono::seconds(10),
                [&i] { return i.positions_done && i.orders_done; });
  // On timeout: partial snapshot => reconcile reports mismatches =>
  // trading refused. Fail-closed by composition, not by exception.
  return i.building;
}

}  // namespace ej::ibkr
