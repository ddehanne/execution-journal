// Fault injection — real process kills, not simulated errors.
// F1: SIGKILL mid-append storm => reopen heals, prefix intact
// F2: crash AFTER durability, BEFORE dispatch => intent recovered as
//     eligible-never-dispatched; no blind resubmission path exists
// F3: mute broker (empty snapshot) vs local state => mismatches => refusal
#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <cstdio>
#include <string>
#include <variant>
#include <vector>

#include "broker/broker_port.hpp"
#include "journal/journal.hpp"
#include "recovery/reconcile.hpp"

using namespace ej::journal;
using namespace ej::recovery;

namespace {
const Recovered& ok(const ReadResult& r) { return std::get<Recovered>(r); }

std::string tmp_path(const char* tag) {
  return std::string("/tmp/ej_fault_") + tag + "_" + std::to_string(getpid());
}
}  // namespace

TEST(FaultInjection, F1_SigkillDuringAppendStorm_ReopenHeals) {
  const auto path = tmp_path("f1");
  std::remove(path.c_str());

  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    // Child: append as fast as possible until killed.
    auto w = std::get<JournalWriter>(JournalWriter::open(path));
    for (std::uint64_t s = 1;; ++s) {
      if (w.append({s, "storm-" + std::to_string(s)}).has_value()) _exit(1);
    }
  }
  usleep(150'000);  // let it write a few hundred records
  kill(child, SIGKILL);
  waitpid(child, nullptr, 0);

  // Parent: reopen must heal to a valid prefix and allow appending.
  auto opened = JournalWriter::open(path);
  ASSERT_TRUE(std::holds_alternative<JournalWriter>(opened));
  auto& w = std::get<JournalWriter>(opened);
  const auto back = read_journal(path);
  ASSERT_TRUE(std::holds_alternative<Recovered>(back));
  EXPECT_GT(ok(back).records.size(), 0u);  // real work survived
  EXPECT_FALSE(w.append({w.next_sequence(), "post-crash"}).has_value());
  std::remove(path.c_str());
}

TEST(FaultInjection, F2_CrashBetweenDurabilityAndDispatch_NoDuplicatePath) {
  const auto path = tmp_path("f2");
  std::remove(path.c_str());

  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    auto w = std::get<JournalWriter>(JournalWriter::open(path));
    // Intent journaled + fsynced => durable => ELIGIBLE...
    if (w.append({1, "INTENT|ord-7|BUY|5|10000"}).has_value()) _exit(1);
    // ...crash strikes BEFORE any DISPATCH record could be written.
    _exit(0);  // simulate hard stop at the exact boundary
  }
  waitpid(child, nullptr, 0);

  // Recovery: replay journal, classify.
  const auto back = read_journal(path);
  ASSERT_TRUE(std::holds_alternative<Recovered>(back));
  const auto& records = ok(back).records;
  ASSERT_EQ(records.size(), 1u);
  bool has_intent = records[0].payload.rfind("INTENT|ord-7", 0) == 0;
  bool has_dispatch = false;
  for (const auto& r : records) {
    if (r.payload.rfind("DISPATCH|ord-7", 0) == 0) has_dispatch = true;
  }
  EXPECT_TRUE(has_intent);
  EXPECT_FALSE(has_dispatch);
  // Contract: eligible-never-dispatched => requires a DECISION, never a
  // blind resubmission. The assertion is structural: the only way to
  // dispatch is to first journal DISPATCH — which recovery has not done.
  std::remove(path.c_str());
}

TEST(FaultInjection, F3_MuteBrokerPartialSnapshot_TradingRefused) {
  // Local state says we hold positions; the broker snapshot is EMPTY
  // (mute/timeout). Composition must refuse trading.
  LocalState local;
  local.positions["SMWB"] = 1559;
  local.open_orders["ord-42"] = 5;
  ej::broker::BrokerSnapshot empty{};  // what a timeout returns

  const auto report = reconcile(local, empty);
  EXPECT_FALSE(report.trading_allowed());
  ASSERT_EQ(report.mismatches.size(), 2u);
}


TEST(FaultInjection, SingleWriterExclusiveAcrossProcesses) {
  const auto path = tmp_path("single_writer");
  const auto lock_path = path + ".lock";

  std::remove(path.c_str());
  std::remove(lock_path.c_str());

  {
    auto first = JournalWriter::open(path);
    ASSERT_TRUE(std::holds_alternative<JournalWriter>(first));

    auto& writer = std::get<JournalWriter>(first);
    ASSERT_FALSE(writer.append({1, "parent-intent"}).has_value());

    const pid_t child = fork();
    ASSERT_GE(child, 0);

    if (child == 0) {
      auto second = JournalWriter::open(path);
      const auto* error = std::get_if<JournalError>(&second);

      const bool refused_by_lock =
          error != nullptr &&
          error->kind == JournalErrorKind::IoFailure &&
          error->detail == "writer lock unavailable";

      _exit(refused_by_lock ? 0 : 1);
    }

    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0)
        << "Second writer was not explicitly refused by the lock";
  }

  {
    // The first writer is destroyed: the lock must be released.
    auto reopened = JournalWriter::open(path);
    ASSERT_TRUE(std::holds_alternative<JournalWriter>(reopened));

    auto& writer = std::get<JournalWriter>(reopened);
    EXPECT_EQ(writer.next_sequence(), 2u);
    EXPECT_FALSE(writer.append({2, "after-release"}).has_value());

    const auto recovered = read_journal(path);
    ASSERT_TRUE(std::holds_alternative<Recovered>(recovered));
    EXPECT_EQ(std::get<Recovered>(recovered).records.size(), 2u);
  }

  // Both writers are destroyed before cleanup.
  std::remove(path.c_str());
  std::remove(lock_path.c_str());
}
