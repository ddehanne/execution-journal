#include "journal/journal.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <limits>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include "journal/sha256.hpp"

namespace ej::journal {

#ifdef EJ_JOURNAL_TEST_FAULTS
bool journal_test_fail_fsync = false;

int journal_test_fsync(int fd) {
  if (journal_test_fail_fsync) {
    errno = EIO;
    return -1;
  }
  return ::fsync(fd);
}
#endif

namespace {

// Production implementation always invokes the real system call.
// Test builds may provide a separate implementation.
int journal_fsync(int fd) {
#ifdef EJ_JOURNAL_TEST_FAULTS
  return journal_test_fsync(fd);
#else
  return ::fsync(fd);
#endif
}

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


JournalError io_error(const char* detail) {
  return JournalError{JournalErrorKind::IoFailure, 0, detail};
}

std::optional<JournalError> sync_parent_directory(
    const std::string& path) {
  const auto slash = path.find_last_of('/');
  const std::string directory =
      slash == std::string::npos ? "." :
      slash == 0 ? "/" : path.substr(0, slash);

  const int fd = ::open(directory.c_str(), O_RDONLY);
  if (fd < 0)
    return io_error("open journal directory");

  const int sync_result = ::fsync(fd);
  const int close_result = ::close(fd);

  if (sync_result != 0)
    return io_error("fsync journal directory");

  if (close_result != 0)
    return io_error("close journal directory");

  return std::nullopt;
}

bool same_file(int fd, const std::string& path) {
  struct stat a{};
  struct stat b{};
  return ::fstat(fd, &a) == 0 &&
         ::stat(path.c_str(), &b) == 0 &&
         S_ISREG(a.st_mode) &&
         S_ISREG(b.st_mode) &&
         a.st_dev == b.st_dev &&
         a.st_ino == b.st_ino;
}

std::variant<std::string, JournalError> read_from_fd(int fd) {
  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
    return io_error("journal is not a regular file");

  std::string bytes;
  char buffer[8192];
  off_t offset = 0;

  for (;;) {
    const ssize_t n = ::pread(fd, buffer, sizeof(buffer), offset);
    if (n > 0) {
      bytes.append(buffer, static_cast<std::size_t>(n));
      offset += static_cast<off_t>(n);
    } else if (n == 0) {
      return bytes;
    } else if (errno != EINTR) {
      return io_error("read journal");
    }
  }
}

std::optional<JournalError> heal_to_prefix(
    const std::string& path,
    int original_fd,
    const std::string& validated_bytes,
    std::size_t keep_lines,
    mode_t original_mode) {

  std::size_t end = 0;

  for (std::size_t i = 0; i < keep_lines; ++i) {
    const auto newline = validated_bytes.find('\n', end);
    if (newline == std::string::npos)
      return io_error("validated prefix incomplete");
    end = newline + 1;
  }

  const std::string data = validated_bytes.substr(0, end);

  std::string pattern = path + ".tmp_recover.XXXXXX";
  std::vector<char> name(pattern.begin(), pattern.end());
  name.push_back('\0');

  const int tmp_fd = ::mkstemp(name.data());
  if (tmp_fd < 0)
    return io_error("create recovery tmp");

  const std::string tmp_path(name.data());

  auto fail = [&](const char* detail)
      -> std::optional<JournalError> {
    ::close(tmp_fd);
    ::unlink(tmp_path.c_str());
    return io_error(detail);
  };

  if (::fchmod(tmp_fd, original_mode & 07777) != 0)
    return fail("chmod recovery tmp");

  std::size_t written = 0;

  while (written < data.size()) {
    const ssize_t n = ::write(
        tmp_fd, data.data() + written, data.size() - written);

    if (n < 0 && errno == EINTR)
      continue;

    if (n <= 0)
      return fail("write recovery tmp");

    written += static_cast<std::size_t>(n);
  }

  if (::fsync(tmp_fd) != 0)
    return fail("fsync recovery tmp");

  if (::close(tmp_fd) != 0) {
    ::unlink(tmp_path.c_str());
    return io_error("close recovery tmp");
  }

  // Refuse to replace a path that no longer names the validated inode.
  if (!same_file(original_fd, path)) {
    ::unlink(tmp_path.c_str());
    return io_error("journal changed before recovery rename");
  }

  if (::rename(tmp_path.c_str(), path.c_str()) != 0) {
    ::unlink(tmp_path.c_str());
    return io_error("rename recovery tmp");
  }

  const auto slash = path.find_last_of('/');
  const std::string directory =
      slash == std::string::npos ? "." :
      slash == 0 ? "/" : path.substr(0, slash);

  const int dir_fd = ::open(directory.c_str(), O_RDONLY);
  if (dir_fd < 0)
    return io_error("open recovery directory");

  const int sync_result = ::fsync(dir_fd);
  const int close_result = ::close(dir_fd);

  if (sync_result != 0)
    return io_error("fsync recovery directory");

  if (close_result != 0)
    return io_error("close recovery directory");

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
    if (!out.records.empty() &&
        out.records.back().sequence ==
            std::numeric_limits<std::uint64_t>::max()) {
      return JournalError{JournalErrorKind::SequenceViolation, line_no,
                          "sequence space exhausted"};
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
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    const int open_errno = errno;

    if (open_errno == ENOENT) {
      struct stat st{};

      if (::lstat(path.c_str(), &st) != 0 && errno == ENOENT) {
        return Recovered{{}, kGenesisHash, false};
      }
    }

    return JournalError{JournalErrorKind::IoFailure, 0,
                        "open journal for read"};
  }

  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return JournalError{JournalErrorKind::IoFailure, 0,
                        "journal is not a regular file"};
  }

  std::string bytes;
  char buffer[8192];

  for (;;) {
    const ssize_t n = ::read(fd, buffer, sizeof(buffer));
    if (n > 0) {
      bytes.append(buffer, static_cast<std::size_t>(n));
    } else if (n == 0) {
      break;
    } else if (errno != EINTR) {
      ::close(fd);
      return JournalError{JournalErrorKind::IoFailure, 0,
                          "read journal"};
    }
  }

  if (::close(fd) != 0) {
    return JournalError{JournalErrorKind::IoFailure, 0,
                        "close journal after read"};
  }

  return parse_journal_bytes(bytes);
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

ReadResult JournalWriter::snapshot() const {
  if (fd_ < 0 || poisoned_ || !same_file(fd_, path_)) {
    return JournalError{
        JournalErrorKind::IoFailure,
        0,
        "Writer unavailable or journal identity changed"};
  }

  auto bytes = read_from_fd(fd_);
  if (const auto* error = std::get_if<JournalError>(&bytes)) {
    return *error;
  }

  return parse_journal_bytes(std::get<std::string>(bytes));
}

std::variant<JournalWriter, JournalError> JournalWriter::open(
    const std::string& path) {

  const std::string lock_path = path + ".lock";
  const int lock_fd =
      ::open(lock_path.c_str(), O_RDWR | O_CREAT, 0644);

  if (lock_fd < 0)
    return io_error("open lock");

  if (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(lock_fd);
    return io_error("writer lock unavailable");
  }

  struct LockGuard {
    int fd;
    ~LockGuard() {
      if (fd >= 0) ::close(fd);
    }
  } guard{lock_fd};

  const int existing_flags = O_RDWR | O_APPEND | O_NOFOLLOW;
  const int create_flags = existing_flags | O_CREAT | O_EXCL;

  bool created = false;
  int fd = ::open(path.c_str(), create_flags, 0644);

  if (fd >= 0) {
    created = true;
  } else if (errno == EEXIST) {
    fd = ::open(path.c_str(), existing_flags);
  }

  if (fd < 0)
    return io_error("open journal");

  auto close_and_fail = [&](const char* detail)
      -> std::variant<JournalWriter, JournalError> {
    ::close(fd);
    return io_error(detail);
  };

  if (!same_file(fd, path))
    return close_and_fail("journal path identity changed");

  if (created) {
    if (auto error = sync_parent_directory(path)) {
      ::close(fd);
      return *error;
    }
  }

  auto snapshot = read_from_fd(fd);

  if (const auto* error = std::get_if<JournalError>(&snapshot)) {
    ::close(fd);
    return *error;
  }

  const std::string& bytes = std::get<std::string>(snapshot);
  auto parsed = parse_journal_bytes(bytes);

  if (const auto* error = std::get_if<JournalError>(&parsed)) {
    ::close(fd);
    return *error;
  }

  Recovered recovered = std::get<Recovered>(std::move(parsed));

  if (!same_file(fd, path))
    return close_and_fail("journal changed during validation");

  if (recovered.recovered_truncated_tail) {
    struct stat st{};

    if (::fstat(fd, &st) != 0)
      return close_and_fail("stat before recovery");

    const auto error = heal_to_prefix(
        path, fd, bytes, recovered.records.size(), st.st_mode);

    ::close(fd);
    fd = -1;

    if (error)
      return *error;

    fd = ::open(path.c_str(), existing_flags);
    if (fd < 0)
      return io_error("reopen recovered journal");

    if (!same_file(fd, path))
      return close_and_fail("recovered journal identity changed");

    auto verification = read_from_fd(fd);

    if (const auto* e = std::get_if<JournalError>(&verification)) {
      ::close(fd);
      return *e;
    }

    auto verified = parse_journal_bytes(
        std::get<std::string>(verification));

    if (const auto* e = std::get_if<JournalError>(&verified)) {
      ::close(fd);
      return *e;
    }

    const auto& after = std::get<Recovered>(verified);

    if (after.recovered_truncated_tail ||
        after.records.size() != recovered.records.size() ||
        after.last_hash != recovered.last_hash)
      return close_and_fail("recovery verification mismatch");

    if (!same_file(fd, path))
      return close_and_fail("journal changed after recovery");
  }

  const off_t end = ::lseek(fd, 0, SEEK_END);

  if (end < 0)
    return close_and_fail("seek end");

  if (end > 0) {
    char last{};

    if (::pread(fd, &last, 1, end - 1) != 1)
      return close_and_fail("read final byte");

    if (last != '\n') {
      if (!same_file(fd, path))
        return close_and_fail("journal changed before newline repair");

      if (::write(fd, "\n", 1) != 1 || ::fsync(fd) != 0)
        return close_and_fail("repair final newline");
    }
  }

  if (!same_file(fd, path))
    return close_and_fail("journal changed before writer activation");

  if (!recovered.records.empty() &&
      recovered.records.back().sequence ==
          std::numeric_limits<std::uint64_t>::max()) {
    return close_and_fail("sequence space exhausted");
  }

  const std::uint64_t next_seq =
      recovered.records.empty()
          ? 1
          : recovered.records.back().sequence + 1;

  guard.fd = -1;

  return JournalWriter(
      path, fd, lock_fd, recovered.last_hash, next_seq);
}

std::optional<JournalError> JournalWriter::append(const Record& record) {
  if (poisoned_) {
    return JournalError{JournalErrorKind::IoFailure, 0, "writer poisoned"};
  }

  struct stat fd_stat{};
  struct stat path_stat{};

  if (::fstat(fd_, &fd_stat) != 0 ||
      ::stat(path_.c_str(), &path_stat) != 0 ||
      !S_ISREG(fd_stat.st_mode) ||
      !S_ISREG(path_stat.st_mode) ||
      fd_stat.st_dev != path_stat.st_dev ||
      fd_stat.st_ino != path_stat.st_ino) {
    poisoned_ = true;
    return JournalError{JournalErrorKind::IoFailure, 0,
                        "journal path identity changed"};
  }
  if (record.payload.find('\n') != std::string::npos) {
    return JournalError{JournalErrorKind::InvalidPayload, 0,
                        "payload contains newline"};
  }
  if (next_seq_ == 0 ||
      next_seq_ == std::numeric_limits<std::uint64_t>::max()) {
    return JournalError{JournalErrorKind::SequenceViolation, 0,
                        "sequence space exhausted"};
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
  if (journal_fsync(fd_) != 0) {
    poisoned_ = true;
    return JournalError{JournalErrorKind::IoFailure, 0, "fsync"};
  }
  last_hash_ = record_hash;
  next_seq_ += 1;
  return std::nullopt;
}

}  // namespace ej::journal
