#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>
#include <sys/wait.h>
#include <variant>

#include "execution/coordinator.hpp"

using namespace ej;
using namespace ej::execution;

namespace {

struct Paths {
  std::string journal;
  std::string latch;

  explicit Paths(const std::string& name) {
    const auto base = "/tmp/ej_m7_" +
                      std::to_string(::getpid()) + "_" + name;
    journal = base + ".journal";
    latch = base + ".latch";
    std::remove(journal.c_str());
    std::remove((journal + ".lock").c_str());
    std::remove(latch.c_str());
  }

  ~Paths() {
    std::remove(journal.c_str());
    std::remove((journal + ".lock").c_str());
    std::remove(latch.c_str());
  }
};

risk::RiskConfig config() {
  return {
      1,
      UsdCents{10'000'000},
      1'000,
      UsdCents{100},
      UsdCents{1'000'000}
  };
}

risk::OrderIntent order(const std::string& id) {
  return {
      id,
      risk::Side::Buy,
      *OrderQuantity::make(10),
      UsdCents{1'000}
  };
}

Coordinator open_coordinator(const Paths& paths) {
  auto result = Coordinator::open(paths.journal, paths.latch, config());
  if (auto* error = std::get_if<journal::JournalError>(&result)) {
    ADD_FAILURE() << error->detail;
    throw std::runtime_error("Coordinator open failed");
  }
  return std::move(std::get<Coordinator>(result));
}

}  // namespace

TEST(Coordinator, ApprovedDecisionIsDurable) {
  Paths paths("approved");

  {
    auto coordinator = open_coordinator(paths);
    const auto decision = coordinator.evaluate(order("ord-1"), 0);
    ASSERT_TRUE(std::holds_alternative<Approved>(decision));
  }

  const auto recovered = journal::read_journal(paths.journal);
  ASSERT_TRUE(std::holds_alternative<journal::Recovered>(recovered));

  const auto& records =
      std::get<journal::Recovered>(recovered).records;

  ASSERT_EQ(records.size(), 1U);
  EXPECT_NE(records[0].payload.find("DECISION|ord-1|APPROVED"),
            std::string::npos);
}

TEST(Coordinator, RefusalIsDurableAndNotApproved) {
  Paths paths("refused");

  {
    auto coordinator = open_coordinator(paths);
    const auto decision = coordinator.evaluate(order("ord-2"), 2'000);
    ASSERT_TRUE(std::holds_alternative<Refused>(decision));
  }

  const auto recovered = journal::read_journal(paths.journal);
  ASSERT_TRUE(std::holds_alternative<journal::Recovered>(recovered));

  const auto& records =
      std::get<journal::Recovered>(recovered).records;

  ASSERT_EQ(records.size(), 1U);
  EXPECT_NE(records[0].payload.find("DECISION|ord-2|REFUSED"),
            std::string::npos);
}

TEST(Coordinator, DuplicateBlockedAfterRestart) {
  Paths paths("restart");

  {
    auto coordinator = open_coordinator(paths);
    ASSERT_TRUE(std::holds_alternative<Approved>(
        coordinator.evaluate(order("ord-3"), 0)));
  }

  {
    auto coordinator = open_coordinator(paths);
    EXPECT_EQ(coordinator.evaluate(order("ord-3"), 0).index(), 2U);
  }

  const auto recovered = journal::read_journal(paths.journal);
  ASSERT_TRUE(std::holds_alternative<journal::Recovered>(recovered));
  EXPECT_EQ(std::get<journal::Recovered>(recovered).records.size(), 1U);
}


TEST(Coordinator, RejectsMalformedDecisionOnRecovery) {
  Paths paths("malformed");

  {
    auto opened = journal::JournalWriter::open(paths.journal);
    ASSERT_TRUE(std::holds_alternative<journal::JournalWriter>(opened));

    auto writer =
        std::move(std::get<journal::JournalWriter>(opened));

    ASSERT_FALSE(writer.append({
        writer.next_sequence(),
        "DECISION|ord-bad|APPROVED"
    }).has_value());
  }

  auto result = Coordinator::open(
      paths.journal, paths.latch, config());

  ASSERT_TRUE(std::holds_alternative<journal::JournalError>(result));
  EXPECT_EQ(
      std::get<journal::JournalError>(result).kind,
      journal::JournalErrorKind::InteriorCorruption);
}

TEST(Coordinator, RejectsInvalidRefusalReasonOnRecovery) {
  Paths paths("invalid_reason");

  {
    auto opened = journal::JournalWriter::open(paths.journal);
    ASSERT_TRUE(std::holds_alternative<journal::JournalWriter>(opened));

    auto writer =
        std::move(std::get<journal::JournalWriter>(opened));

    const auto payload =
        "DECISION|ord-bad|REFUSED"
        "|config_version=1"
        "|config_hash=" + config().content_hash() +
        "|side=BUY"
        "|quantity=10"
        "|price_cents=1000"
        "|reason=999";

    ASSERT_FALSE(writer.append({
        writer.next_sequence(),
        payload
    }).has_value());
  }

  auto result = Coordinator::open(
      paths.journal, paths.latch, config());

  ASSERT_TRUE(std::holds_alternative<journal::JournalError>(result));
  EXPECT_EQ(
      std::get<journal::JournalError>(result).kind,
      journal::JournalErrorKind::InteriorCorruption);
}


TEST(Coordinator, RejectsUnknownRecordOnRecovery) {
  Paths paths("unknown_record");

  {
    auto opened = journal::JournalWriter::open(paths.journal);
    ASSERT_TRUE(std::holds_alternative<journal::JournalWriter>(opened));

    auto writer =
        std::move(std::get<journal::JournalWriter>(opened));

    ASSERT_FALSE(writer.append({
        writer.next_sequence(),
        "UNKNOWN|ord-unknown"
    }).has_value());
  }

  auto result = Coordinator::open(
      paths.journal, paths.latch, config());

  ASSERT_TRUE(std::holds_alternative<journal::JournalError>(result));
  EXPECT_EQ(
      std::get<journal::JournalError>(result).kind,
      journal::JournalErrorKind::InteriorCorruption);
}

TEST(Coordinator, DurableDecisionBlocksDuplicateAfterCrash) {
  Paths paths("crash_duplicate");

  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);

  if (pid == 0) {
    auto opened = Coordinator::open(
        paths.journal, paths.latch, config());

    if (!std::holds_alternative<Coordinator>(opened))
      ::_exit(10);

    auto coordinator =
        std::move(std::get<Coordinator>(opened));

    const auto decision =
        coordinator.evaluate(order("ord-crash"), 0);

    if (!std::holds_alternative<Approved>(decision))
      ::_exit(11);

    // Terminate without Coordinator destruction.
    ::_exit(0);
  }

  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  auto coordinator = open_coordinator(paths);

  const auto duplicate =
      coordinator.evaluate(order("ord-crash"), 0);

  ASSERT_TRUE(
      std::holds_alternative<DecisionError>(duplicate));

  EXPECT_EQ(
      std::get<DecisionError>(duplicate),
      DecisionError::DuplicateOrderId);

  const auto recovered =
      journal::read_journal(paths.journal);

  ASSERT_TRUE(
      std::holds_alternative<journal::Recovered>(recovered));

  EXPECT_EQ(
      std::get<journal::Recovered>(recovered).records.size(),
      1U);
}


TEST(Coordinator, RejectsRepairedJournalBeforeResuming) {
  Paths paths("repaired_journal");

  // Persist a legitimate approved decision.
  {
    auto coordinator = open_coordinator(paths);

    const auto decision =
        coordinator.evaluate(order("ord-durable"), 0);

    ASSERT_TRUE(std::holds_alternative<Approved>(decision));
  }

  // Simulate an interrupted final journal record.
  {
    std::ofstream out(
        paths.journal,
        std::ios::binary | std::ios::app);

    ASSERT_TRUE(out.is_open());

    out << "2|incomplete-record";
    out.flush();

    ASSERT_TRUE(out.good());
  }

  // JournalWriter may repair the torn tail, but the
  // Coordinator must refuse to resume silently.
  auto result = Coordinator::open(
      paths.journal,
      paths.latch,
      config());

  ASSERT_TRUE(
      std::holds_alternative<journal::JournalError>(result));

  EXPECT_EQ(
      std::get<journal::JournalError>(result).kind,
      journal::JournalErrorKind::InteriorCorruption);
}
