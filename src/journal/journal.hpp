// Durable append-only execution journal — mirror of verified-ledger
// storage contract, with the corrected submission semantics:
//
//   append record → durability barrier (fsync) → dispatch eligibility
//
// An intent EXISTS from creation; durability gates its ELIGIBILITY for
// broker submission. The local durability confirmation is distinct from
// any broker acknowledgement.
//
// Line format (one line = one record):
//   <seq>|<prev_hash>|<record_hash>|<payload>\n
//   record_hash = SHA256(<seq>|<prev_hash>|<payload>)
//
// Recovery contract (frozen, parity with Rust):
// - torn/unparseable FINAL line = crash artifact → recover longest valid
//   prefix, heal file (atomic tmp+rename)
// - any integrity failure BEFORE the final line = fatal; the cause
//   (tampering, bug, storage fault) is not inferable and never guessed
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ej::journal {

inline constexpr const char* kGenesisHash =
    "0000000000000000000000000000000000000000000000000000000000000000";

struct Record {
  std::uint64_t sequence;
  std::string payload;  // opaque to the journal; no '\n' allowed
};

enum class JournalErrorKind : std::uint8_t {
  IoFailure,
  InteriorCorruption,   // unparseable or hash-invalid non-final line
  ChainBreak,           // prev_hash mismatch on a valid line
  SequenceViolation,    // non-contiguous sequence
  InvalidPayload,       // payload contains '\n'
};

struct JournalError {
  JournalErrorKind kind;
  std::size_t line;  // 1-based; 0 = not line-specific
  std::string detail;
};

struct Recovered {
  std::vector<Record> records;
  std::string last_hash;
  bool recovered_truncated_tail;
};

using ReadResult = std::variant<Recovered, JournalError>;

// Pure parse over bytes (fuzz-friendly, parity with Rust refactor).
[[nodiscard]] ReadResult parse_journal_bytes(const std::string& bytes);

// IO wrapper: read + verify a journal file (missing file = empty journal).
[[nodiscard]] ReadResult read_journal(const std::string& path);

class JournalWriter {
 public:
  // Open for append. Verifies the whole file first; heals a torn tail.
  // A writer never appends to a journal it has not verified.
  static std::variant<JournalWriter, JournalError> open(const std::string& path);

  JournalWriter(JournalWriter&&) noexcept;
  JournalWriter& operator=(JournalWriter&&) noexcept;
  JournalWriter(const JournalWriter&) = delete;
  JournalWriter& operator=(const JournalWriter&) = delete;
  ~JournalWriter();

  // Durably append: write line, then fsync. Returns only after the
  // durability barrier — success here IS dispatch eligibility.
  [[nodiscard]] std::optional<JournalError> append(const Record& record);

  [[nodiscard]] const std::string& last_hash() const noexcept { return last_hash_; }
  [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_seq_; }

  // Read while this writer holds the exclusive journal lock.
  [[nodiscard]] ReadResult snapshot() const;

  // True if this open repaired the journal OR a durable
  // recovery quarantine marker is present.
  // A marker survives process restarts and is not auto-cleared.
  [[nodiscard]] bool recovery_repaired() const noexcept {
    return recovery_repaired_;
  }

 private:
  JournalWriter(std::string path, int fd, int lock_fd,
                std::string last_hash, std::uint64_t next_seq,
                bool recovery_repaired) noexcept;
  std::string path_;
  int fd_;
  int lock_fd_;
  bool poisoned_ = false;
  bool recovery_repaired_ = false;
  std::string last_hash_;
  std::uint64_t next_seq_;
};

}  // namespace ej::journal
