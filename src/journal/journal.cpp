#include "journal/journal.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "journal/sha256.hpp"

namespace ej::journal {
namespace {

std::string hash_line(std::uint64_t seq, const std::string& prev_hash,
                      const std::string& payload) {
  std::ostringstream os;
  os << seq << '|' << prev_hash << '|' << payload;
  return sha256::hex_digest(os.str());
}

// Parse "<seq>|<prev>|<hash>|<payload>". nullopt = unparseable.
struct ParsedLine {
  std::uint64_t seq;
  std::string prev_hash;
  std::string record_hash;
  std::string payload;
};

std::optional<ParsedLine> parse_line(const std::string& line) {
  const auto p1 = line.find('|');
  if (p1 == std::string::npos) return std::nullopt;
  const auto p2 = line.find('|', p1 + 1);
  if (p2 == std::string::npos) return std::nullopt;
  const auto p3 = line.find('|', p2 + 1);
  if (p3 == std::string::npos) return std::nullopt;

  ParsedLine out;
  const std::string seq_s = line.substr(0, p1);
  if (seq_s.empty()) return std::nullopt;
  errno = 0;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(seq_s.c_str(), &end, 10);
  if (errno != 0 || end == nullptr || *end != '\0') return std::nullopt;
  out.seq = static_cast<std::uint64_t>(v);
  out.prev_hash = line.substr(p1 + 1, p2 - p1 - 1);
  out.record_hash = line.substr(p2 + 1, p3 - p2 - 1);
  out.payload = line.substr(p3 + 1);
  if (out.prev_hash.size() != 64 || out.record_hash.size() != 64) {
    return std::nullopt;
  }
  return out;
}

std::optional<JournalError> heal_to_prefix(const std::string& path,
                                           std::size_t keep_lines) {
  std::ifstream in(path);
  if (!in) {
    return JournalError{JournalErrorKind::IoFailure, 0, "open for heal"};
  }
  std::ostringstream kept;
  std::string line;
  for (std::size_t i = 0; i < keep_lines && std::getline(in, line); ++i) {
    kept << line << '\n';
  }
  in.close();

  const std::string tmp = path + ".tmp_recover";
  {
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      return JournalError{JournalErrorKind::IoFailure, 0, "create tmp"};
    }
    const std::string data = kept.str();
    std::size_t written = 0;
    while (written < data.size()) {
      const ssize_t n =
          ::write(fd, data.data() + written, data.size() - written);
      if (n < 0) {
        ::close(fd);
        return JournalError{JournalErrorKind::IoFailure, 0, "write tmp"};
      }
      written += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
      ::close(fd);
      return JournalError{JournalErrorKind::IoFailure, 0, "fsync tmp"};
    }
    ::close(fd);
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    return JournalError{JournalErrorKind::IoFailure, 0, "rename tmp"};
  }
  return std::nullopt;
}

}  // namespace

ReadResult parse_journal_bytes(const std::string& bytes) {
  std::vector<std::string> lines;
  {
    std::string cur;
    for (const char c : bytes) {
      if (c == '\n') {
        lines.push_back(cur);
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
    if (!cur.empty()) lines.push_back(cur);  // final line without newline
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
  }

  Recovered out;
  out.last_hash = kGenesisHash;
  out.recovered_truncated_tail = false;

  for (std::size_t i = 0; i < lines.size(); ++i) {
    const bool is_last = (i + 1 == lines.size());
    const bool unterminated_last =
        is_last && !bytes.empty() && bytes.back() != '\n';
    const std::size_t line_no = i + 1;

    const auto parsed = parse_line(lines[i]);
    if (!parsed.has_value()) {
      if (unterminated_last) {
        out.recovered_truncated_tail = true;
        break;
      }
      return JournalError{JournalErrorKind::InteriorCorruption, line_no,
                          "unparseable interior line"};
    }

    const auto computed =
        hash_line(parsed->seq, parsed->prev_hash, parsed->payload);
    if (computed != parsed->record_hash) {
      if (unterminated_last) {
        out.recovered_truncated_tail = true;
        break;
      }
      return JournalError{JournalErrorKind::InteriorCorruption, line_no,
                          "record_hash mismatch"};
    }
    if (parsed->prev_hash != out.last_hash) {
      return JournalError{JournalErrorKind::ChainBreak, line_no,
                          "prev_hash chain broken"};
    }
    const std::uint64_t expected =
        out.records.empty() ? 1 : out.records.back().sequence + 1;
    if (parsed->seq != expected) {
      return JournalError{JournalErrorKind::SequenceViolation, line_no,
                          "non-contiguous sequence"};
    }

    out.last_hash = parsed->record_hash;
    out.records.push_back(Record{parsed->seq, parsed->payload});
  }
  return out;
}

ReadResult read_journal(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return Recovered{{}, kGenesisHash, false};  // missing = empty journal
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return parse_journal_bytes(ss.str());
}

JournalWriter::JournalWriter(std::string path, int fd, int lock_fd,
                             std::string last_hash,
                             std::uint64_t next_seq) noexcept
    : path_(std::move(path)),
      fd_(fd),
      lock_fd_(lock_fd),
      last_hash_(std::move(last_hash)),
      next_seq_(next_seq) {}

JournalWriter::JournalWriter(JournalWriter&& o) noexcept
    : path_(std::move(o.path_)),
      fd_(o.fd_),
      lock_fd_(o.lock_fd_),
      poisoned_(o.poisoned_),
      last_hash_(std::move(o.last_hash_)),
      next_seq_(o.next_seq_) {
  o.fd_ = -1;
  o.lock_fd_ = -1;
}

JournalWriter& JournalWriter::operator=(JournalWriter&& o) noexcept {
  if (this != &o) {
    if (fd_ >= 0) ::close(fd_);
    if (lock_fd_ >= 0) ::close(lock_fd_);
    path_ = std::move(o.path_);
    fd_ = o.fd_;
    lock_fd_ = o.lock_fd_;
    poisoned_ = o.poisoned_;
    last_hash_ = std::move(o.last_hash_);
    next_seq_ = o.next_seq_;
    o.fd_ = -1;
    o.lock_fd_ = -1;
  }
  return *this;
}

JournalWriter::~JournalWriter() {
  if (fd_ >= 0) ::close(fd_);
  if (lock_fd_ >= 0) ::close(lock_fd_);
}

std::variant<JournalWriter, JournalError> JournalWriter::open(
    const std::string& path) {
  const std::string lock_path = path + ".lock";
  const int lock_fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT, 0644);
  if (lock_fd < 0) {
    return JournalError{JournalErrorKind::IoFailure, 0, "open lock"};
  }

  if (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(lock_fd);
    return JournalError{JournalErrorKind::IoFailure, 0,
                        "writer lock unavailable"};
  }

  // Own the lock through every early-return path until ownership
  // transfers to JournalWriter.
  struct LockGuard {
    int fd;
    ~LockGuard() { if (fd >= 0) ::close(fd); }
  } guard{lock_fd};

  auto read = read_journal(path);
  if (const auto* err = std::get_if<JournalError>(&read)) {
    return *err;
  }
  auto& rec = std::get<Recovered>(read);
  if (rec.recovered_truncated_tail) {
    if (auto heal_err = heal_to_prefix(path, rec.records.size())) {
      return *heal_err;
    }
  }
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    return JournalError{JournalErrorKind::IoFailure, 0, "open append"};
  }

  // A valid final record may lack its newline. Repair the delimiter
  // before appending, otherwise the next record would be concatenated.
  const off_t end = ::lseek(fd, 0, SEEK_END);
  if (end < 0) {
    ::close(fd);
    return JournalError{JournalErrorKind::IoFailure, 0, "seek end"};
  }

  if (end > 0) {
    char last{};
    if (::pread(fd, &last, 1, end - 1) != 1) {
      ::close(fd);
      return JournalError{JournalErrorKind::IoFailure, 0, "read final byte"};
    }

    if (last != '\n') {
      if (::write(fd, "\n", 1) != 1 || ::fsync(fd) != 0) {
        ::close(fd);
        return JournalError{JournalErrorKind::IoFailure, 0,
                            "repair final newline"};
      }
    }
  }

  const std::uint64_t next_seq =
      rec.records.empty() ? 1 : rec.records.back().sequence + 1;
  guard.fd = -1;
  return JournalWriter(path, fd, lock_fd, rec.last_hash, next_seq);
}

std::optional<JournalError> JournalWriter::append(const Record& record) {
  if (poisoned_) {
    return JournalError{JournalErrorKind::IoFailure, 0, "writer poisoned"};
  }
  if (record.payload.find('\n') != std::string::npos) {
    return JournalError{JournalErrorKind::InvalidPayload, 0,
                        "payload contains newline"};
  }
  if (record.sequence != next_seq_) {
    return JournalError{JournalErrorKind::SequenceViolation, 0,
                        "append sequence must be contiguous"};
  }
  const auto record_hash =
      hash_line(record.sequence, last_hash_, record.payload);
  std::ostringstream os;
  os << record.sequence << '|' << last_hash_ << '|' << record_hash << '|'
     << record.payload << '\n';
  const std::string line = os.str();

  std::size_t written = 0;
  while (written < line.size()) {
    const ssize_t n = ::write(
        fd_, line.data() + written, line.size() - written);
    if (n <= 0) {
      poisoned_ = true;
      return JournalError{JournalErrorKind::IoFailure, 0, "write"};
    }
    written += static_cast<std::size_t>(n);
  }
  // Durability barrier — success past this point IS dispatch eligibility.
  if (::fsync(fd_) != 0) {
    poisoned_ = true;
    return JournalError{JournalErrorKind::IoFailure, 0, "fsync"};
  }
  last_hash_ = record_hash;
  next_seq_ += 1;
  return std::nullopt;
}

}  // namespace ej::journal
