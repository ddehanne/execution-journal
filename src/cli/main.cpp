// audit-cli — operational façade. Zero business logic lives here.
//
// Commands:
//   ej-audit verify <journal>     integrity check (exit 0/2)
//   ej-audit status <journal>     replay + summary (exit 0/2)
//   ej-audit replay <journal>     full record dump (exit 0/2)
//
// Exit codes: 0 = healthy · 2 = journal integrity failure · 1 = usage
#include <cstdio>
#include <string>
#include <variant>

#include "journal/journal.hpp"

namespace {

using ej::journal::JournalError;
using ej::journal::JournalErrorKind;
using ej::journal::read_journal;
using ej::journal::Recovered;

const char* kind_name(JournalErrorKind k) {
  switch (k) {
    case JournalErrorKind::IoFailure: return "io-failure";
    case JournalErrorKind::InteriorCorruption: return "interior-corruption";
    case JournalErrorKind::ChainBreak: return "chain-break";
    case JournalErrorKind::SequenceViolation: return "sequence-violation";
    case JournalErrorKind::InvalidPayload: return "invalid-payload";
  }
  return "unknown";
}

int fail(const JournalError& e) {
  std::fprintf(stderr, "INTEGRITY FAILURE line=%zu kind=%s detail=%s\n",
               e.line, kind_name(e.kind), e.detail.c_str());
  return 2;
}

int cmd_verify(const std::string& path) {
  const auto r = read_journal(path);
  if (const auto* e = std::get_if<JournalError>(&r)) return fail(*e);
  const auto& rec = std::get<Recovered>(r);
  std::printf("OK records=%zu last_hash=%.16s… truncated_tail_recovered=%s\n",
              rec.records.size(), rec.last_hash.c_str(),
              rec.recovered_truncated_tail ? "yes" : "no");
  return 0;
}

int cmd_status(const std::string& path) {
  const auto r = read_journal(path);
  if (const auto* e = std::get_if<JournalError>(&r)) return fail(*e);
  const auto& rec = std::get<Recovered>(r);
  const std::uint64_t next_seq =
      rec.records.empty() ? 1 : rec.records.back().sequence + 1;
  std::printf("journal=%s\nrecords=%zu\nnext_sequence=%llu\n"
              "truncated_tail_recovered=%s\nchain_head=%s\n",
              path.c_str(), rec.records.size(),
              static_cast<unsigned long long>(next_seq),
              rec.recovered_truncated_tail ? "yes" : "no",
              rec.last_hash.c_str());
  return 0;
}

int cmd_replay(const std::string& path) {
  const auto r = read_journal(path);
  if (const auto* e = std::get_if<JournalError>(&r)) return fail(*e);
  for (const auto& rec : std::get<Recovered>(r).records) {
    std::printf("%llu\t%s\n", static_cast<unsigned long long>(rec.sequence),
                rec.payload.c_str());
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: ej-audit <verify|status|replay> <journal>\n");
    return 1;
  }
  const std::string cmd = argv[1];
  const std::string path = argv[2];
  if (cmd == "verify") return cmd_verify(path);
  if (cmd == "status") return cmd_status(path);
  if (cmd == "replay") return cmd_replay(path);
  std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
  return 1;
}
