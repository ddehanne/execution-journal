// Journal contract tests — parity with verified-ledger storage tests,
// plus FIPS 180-4 vectors for the local SHA-256.
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

#include "journal/journal.hpp"
#include "journal/sha256.hpp"

using namespace ej::journal;

namespace {

class TmpFile {
 public:
  TmpFile() : path_("/tmp/ej_journal_test_" + std::to_string(counter_++)) {
    std::remove(path_.c_str());
  }
  ~TmpFile() { std::remove(path_.c_str()); }
  const std::string& path() const { return path_; }

 private:
  static inline int counter_ = 0;
  std::string path_;
};

const Recovered& ok(const ReadResult& r) { return std::get<Recovered>(r); }
const JournalError& err(const ReadResult& r) {
  return std::get<JournalError>(r);
}

JournalWriter open_ok(const std::string& path) {
  auto w = JournalWriter::open(path);
  return std::move(std::get<JournalWriter>(w));
}

void append_n(JournalWriter& w, std::uint64_t n) {
  for (std::uint64_t s = w.next_sequence(); n > 0; --n, ++s) {
    ASSERT_FALSE(w.append({s, "payload-" + std::to_string(s)}).has_value());
  }
}

std::string slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST(Sha256, FipsVectors) {
  EXPECT_EQ(ej::sha256::hex_digest(""),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(ej::sha256::hex_digest("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(ej::sha256::hex_digest(
                "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}


TEST(Journal, ExclusiveCreationAndReopenPreservesRecords) {
  TmpFile f;

  struct stat before{};
  ASSERT_NE(::stat(f.path().c_str(), &before), 0);

  {
    auto w = open_ok(f.path());

    struct stat created{};
    ASSERT_EQ(::stat(f.path().c_str(), &created), 0);
    ASSERT_TRUE(S_ISREG(created.st_mode));

    ASSERT_FALSE(w.append({1, "first"}).has_value());
  }

  {
    auto w = open_ok(f.path());
    ASSERT_EQ(w.next_sequence(), 2u);
    ASSERT_FALSE(w.append({2, "second"}).has_value());
  }

  const auto result = read_journal(f.path());

  ASSERT_TRUE(std::holds_alternative<Recovered>(result));
  const auto& recovered = std::get<Recovered>(result);

  ASSERT_EQ(recovered.records.size(), 2u);
  EXPECT_EQ(recovered.records[0].payload, "first");
  EXPECT_EQ(recovered.records[1].payload, "second");
  EXPECT_FALSE(recovered.recovered_truncated_tail);
}

TEST(Journal, SequenceOverflowIsRejected) {
  TmpFile f;
  auto w = open_ok(f.path());

  // An invalid sequence must not mutate the journal.
  const auto error = w.append({
      std::numeric_limits<std::uint64_t>::max(),
      "overflow"
  });

  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->kind, JournalErrorKind::SequenceViolation);

  EXPECT_EQ(w.next_sequence(), 1u);
  EXPECT_TRUE(slurp(f.path()).empty());

  // The writer must remain usable after rejecting invalid input.
  ASSERT_FALSE(w.append({1, "valid"}).has_value());

  const auto result = read_journal(f.path());
  ASSERT_TRUE(std::holds_alternative<Recovered>(result));
  ASSERT_EQ(std::get<Recovered>(result).records.size(), 1u);
}

TEST(Journal, RoundTrip) {
  TmpFile f;
  auto w = open_ok(f.path());
  append_n(w, 5);
  const auto back = read_journal(f.path());
  ASSERT_TRUE(std::holds_alternative<Recovered>(back));
  EXPECT_FALSE(ok(back).recovered_truncated_tail);
  ASSERT_EQ(ok(back).records.size(), 5u);
  EXPECT_EQ(ok(back).records[4].payload, "payload-5");
}

TEST(Journal, TornFinalLineRecovered) {
  TmpFile f;
  {
    auto w = open_ok(f.path());
    append_n(w, 4);
  }
  // Crash simulation: torn write without newline.
  {
    std::ofstream app(f.path(), std::ios::app | std::ios::binary);
    app << "5|deadbeef";  // unparseable fragment
  }
  const auto back = read_journal(f.path());
  ASSERT_TRUE(std::holds_alternative<Recovered>(back));
  EXPECT_TRUE(ok(back).recovered_truncated_tail);
  EXPECT_EQ(ok(back).records.size(), 4u);

  // Reopen heals and appending resumes at the right sequence.
  auto w2 = open_ok(f.path());
  EXPECT_TRUE(w2.recovery_repaired());
  EXPECT_EQ(w2.next_sequence(), 5u);
  ASSERT_FALSE(w2.append({5, "after-heal"}).has_value());
  const auto final_read = read_journal(f.path());
  EXPECT_FALSE(ok(final_read).recovered_truncated_tail);
  EXPECT_EQ(ok(final_read).records.size(), 5u);
}


TEST(Journal, RecoveryQuarantineSurvivesTwoRestarts) {
  TmpFile f;

  {
    auto writer = open_ok(f.path());
    append_n(writer, 4);
  }

  {
    std::ofstream out(f.path(), std::ios::binary | std::ios::app);
    ASSERT_TRUE(out.is_open());
    out << "5|incomplete";
    out.flush();
    ASSERT_TRUE(out.good());
  }

  for (int restart = 0; restart < 3; ++restart) {
    auto writer = open_ok(f.path());

    EXPECT_TRUE(writer.recovery_repaired());
    EXPECT_EQ(writer.next_sequence(), 5u);
  }

  const auto recovered = read_journal(f.path());

  ASSERT_TRUE(std::holds_alternative<Recovered>(recovered));

  const auto& state = std::get<Recovered>(recovered);

  EXPECT_FALSE(state.recovered_truncated_tail);
  EXPECT_EQ(state.records.size(), 4u);
}

TEST(Journal, MissingTrailingNewlineOnValidLastLineIsFine) {
  TmpFile f;
  {
    auto w = open_ok(f.path());
    append_n(w, 3);
  }
  // Strip the final newline — the last line is complete, just unterminated.
  auto content = slurp(f.path());
  ASSERT_EQ(content.back(), '\n');
  content.pop_back();
  std::ofstream(f.path(), std::ios::trunc | std::ios::binary) << content;

  const auto back = read_journal(f.path());
  ASSERT_TRUE(std::holds_alternative<Recovered>(back));
  EXPECT_FALSE(ok(back).recovered_truncated_tail);
  EXPECT_EQ(ok(back).records.size(), 3u);
}

TEST(Journal, InteriorCorruptionFatal) {
  TmpFile f;
  {
    auto w = open_ok(f.path());
    append_n(w, 5);
  }
  auto content = slurp(f.path());
  // Corrupt payload of line 2 (flip a char in "payload-2").
  const auto pos = content.find("payload-2");
  ASSERT_NE(pos, std::string::npos);
  content[pos] = 'X';
  std::ofstream(f.path(), std::ios::trunc | std::ios::binary) << content;

  const auto back = read_journal(f.path());
  ASSERT_TRUE(std::holds_alternative<JournalError>(back));
  EXPECT_EQ(err(back).kind, JournalErrorKind::InteriorCorruption);
  EXPECT_EQ(err(back).line, 2u);
}

TEST(Journal, RemovedInteriorLineBreaksChain) {
  TmpFile f;
  {
    auto w = open_ok(f.path());
    append_n(w, 5);
  }
  const auto content = slurp(f.path());
  // Drop line 2 entirely.
  std::string pruned;
  std::size_t line_no = 0, start = 0;
  for (std::size_t i = 0; i <= content.size(); ++i) {
    if (i == content.size() || content[i] == '\n') {
      ++line_no;
      if (line_no != 2 && i > start) {
        pruned.append(content, start, i - start);
        pruned.push_back('\n');
      }
      start = i + 1;
    }
  }
  std::ofstream(f.path(), std::ios::trunc | std::ios::binary) << pruned;

  const auto back = read_journal(f.path());
  ASSERT_TRUE(std::holds_alternative<JournalError>(back));
  EXPECT_EQ(err(back).kind, JournalErrorKind::ChainBreak);
}

TEST(Journal, AppendRefusesNonContiguousSequenceAndNewlinePayload) {
  TmpFile f;
  auto w = open_ok(f.path());
  append_n(w, 2);
  auto e1 = w.append({7, "gap"});
  ASSERT_TRUE(e1.has_value());
  EXPECT_EQ(e1->kind, JournalErrorKind::SequenceViolation);
  auto e2 = w.append({3, "bad\npayload"});
  ASSERT_TRUE(e2.has_value());
  EXPECT_EQ(e2->kind, JournalErrorKind::InvalidPayload);
}


TEST(Journal, CompleteCorruptedFinalRecordIsFatal) {
  TmpFile f;
  {
    auto w = open_ok(f.path());
    append_n(w, 2);
  }

  auto content = slurp(f.path());
  const auto pos = content.find("payload-2");
  ASSERT_NE(pos, std::string::npos);

  content[pos] = 'X';

  // Preserve the final newline: this is a complete,
  // corrupted record, not an unterminated write.
  ASSERT_EQ(content.back(), '\n');

  std::ofstream(f.path(), std::ios::trunc | std::ios::binary)
      << content;

  const auto result = read_journal(f.path());

  ASSERT_TRUE(std::holds_alternative<JournalError>(result))
      << "A complete corrupted final record must not be "
         "silently discarded";

  EXPECT_EQ(
      std::get<JournalError>(result).kind,
      JournalErrorKind::InteriorCorruption);
}


TEST(Journal, ReopenRepairsMissingFinalNewlineBeforeAppend) {
  TmpFile f;

  {
    auto w = open_ok(f.path());
    append_n(w, 2);
  }

  auto content = slurp(f.path());
  ASSERT_FALSE(content.empty());
  ASSERT_EQ(content.back(), '\n');

  content.pop_back();

  {
    std::ofstream out(f.path(), std::ios::binary | std::ios::trunc);
    out << content;
  }

  {
    auto w = open_ok(f.path());
    ASSERT_EQ(w.next_sequence(), 3u);
    ASSERT_FALSE(w.append({3, "third"}).has_value());
  }

  const auto result = read_journal(f.path());

  ASSERT_TRUE(std::holds_alternative<Recovered>(result))
      << "Appending after an unterminated valid record "
         "must preserve journal integrity";

  const auto& recovered = std::get<Recovered>(result);

  ASSERT_EQ(recovered.records.size(), 3u);
  EXPECT_EQ(recovered.records[2].sequence, 3u);
  EXPECT_EQ(recovered.records[2].payload, "third");
}

TEST(Journal, MissingFileIsEmpty) {
  TmpFile f;
  const auto result = read_journal(f.path());
  ASSERT_TRUE(std::holds_alternative<Recovered>(result));
  EXPECT_TRUE(ok(result).records.empty());
}

TEST(Journal, DirectoryIsNotAnEmptyJournal) {
  TmpFile f;
  ASSERT_EQ(::mkdir(f.path().c_str(), 0700), 0);

  const auto result = read_journal(f.path());
  ASSERT_TRUE(std::holds_alternative<JournalError>(result));
  EXPECT_EQ(err(result).kind, JournalErrorKind::IoFailure);

  EXPECT_EQ(::rmdir(f.path().c_str()), 0);
}

TEST(Journal, BrokenSymlinkMustNotBeTreatedAsMissing) {
  TmpFile f;
  const std::string missing_target = f.path() + ".nonexistent";

  ASSERT_EQ(::symlink(missing_target.c_str(), f.path().c_str()), 0);

  const auto result = read_journal(f.path());

  ASSERT_TRUE(std::holds_alternative<JournalError>(result))
      << "A dangling symlink must not initialize an empty journal";

  EXPECT_EQ(err(result).kind, JournalErrorKind::IoFailure);
}
