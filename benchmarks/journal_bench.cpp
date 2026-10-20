#include "journal/journal.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

double microseconds(Clock::duration duration) {
  return std::chrono::duration<double, std::micro>(duration).count();
}

double percentile(const std::vector<double>& sorted, double p) {
  if (sorted.empty()) return 0.0;

  const double position = p * static_cast<double>(sorted.size() - 1);
  const auto lower = static_cast<std::size_t>(position);
  const auto upper = std::min(lower + 1, sorted.size() - 1);
  const double fraction = position - static_cast<double>(lower);

  return sorted[lower] * (1.0 - fraction) + sorted[upper] * fraction;
}

std::uint64_t parse_count(const char* text) {
  const std::string input(text);

  if (input.empty() ||
      !std::all_of(input.begin(), input.end(),
                   [](unsigned char c) { return c >= '0' && c <= '9'; })) {
    throw std::invalid_argument("record count must be a positive integer");
  }

  const auto count = std::stoull(input);

  if (count == 0 || count > 1000000) {
    throw std::invalid_argument("record count must be between 1 and 1000000");
  }

  return static_cast<std::uint64_t>(count);
}

int main(int argc, char** argv) {
  try {
    if (argc != 2) {
      std::cerr << "Usage: " << argv[0] << " <record-count>\n";
      return 2;
    }

    const std::uint64_t count = parse_count(argv[1]);

    char directory[] = "/tmp/ej-bench-XXXXXX";

    if (::mkdtemp(directory) == nullptr) {
      std::cerr << "Failed to create temporary directory\n";
      return 1;
    }

    const fs::path root(directory);

    struct Cleanup {
      fs::path path;
      ~Cleanup() {
        std::error_code error;
        fs::remove_all(path, error);
      }
    } cleanup{root};

    const std::string journal_path = (root / "journal.log").string();

    constexpr std::size_t payload_size = 128;
    const std::string payload(payload_size, 'X');

    std::vector<double> latencies;
    latencies.reserve(static_cast<std::size_t>(count));

    double total_append_us = 0.0;

    {
      auto opened = ej::journal::JournalWriter::open(journal_path);

      if (!std::holds_alternative<ej::journal::JournalWriter>(opened)) {
        std::cerr << "Initial journal open failed\n";
        return 1;
      }

      auto writer =
          std::move(std::get<ej::journal::JournalWriter>(opened));

      const auto total_start = Clock::now();

      for (std::uint64_t sequence = 1; sequence <= count; ++sequence) {
        const ej::journal::Record record{sequence, payload};

        const auto start = Clock::now();
        const auto error = writer.append(record);
        const auto end = Clock::now();

        if (error) {
          std::cerr << "Append failed at sequence "
                    << sequence << ": " << error->detail << '\n';
          return 1;
        }

        latencies.push_back(microseconds(end - start));
      }

      total_append_us = microseconds(Clock::now() - total_start);
    }

    const auto read_start = Clock::now();
    auto result = ej::journal::read_journal(journal_path);
    const auto read_end = Clock::now();

    if (!std::holds_alternative<ej::journal::Recovered>(result)) {
      std::cerr << "Journal read/verification failed\n";
      return 1;
    }

    const auto& recovered = std::get<ej::journal::Recovered>(result);

    if (recovered.records.size() != count ||
        recovered.recovered_truncated_tail ||
        recovered.records.front().sequence != 1 ||
        recovered.records.back().sequence != count) {
      std::cerr << "Unexpected recovered state\n";
      return 1;
    }

    const double read_us = microseconds(read_end - read_start);

    const auto reopen_start = Clock::now();

    {
      auto opened = ej::journal::JournalWriter::open(journal_path);

      if (!std::holds_alternative<ej::journal::JournalWriter>(opened)) {
        std::cerr << "Journal reopen failed\n";
        return 1;
      }

      auto writer =
          std::move(std::get<ej::journal::JournalWriter>(opened));

      if (writer.next_sequence() != count + 1 ||
          writer.recovery_repaired()) {
        std::cerr << "Unexpected reopen state\n";
        return 1;
      }
    }

    const double reopen_us = microseconds(Clock::now() - reopen_start);

    std::sort(latencies.begin(), latencies.end());

    const double throughput =
        static_cast<double>(count) / (total_append_us / 1000000.0);

    std::cout << std::fixed << std::setprecision(3);

    std::cout
        << "records,payload_bytes,append_p50_us,append_p95_us,"
        << "append_p99_us,throughput_records_s,read_verify_us,reopen_us\n";

    std::cout
        << count << ','
        << payload_size << ','
        << percentile(latencies, 0.50) << ','
        << percentile(latencies, 0.95) << ','
        << percentile(latencies, 0.99) << ','
        << throughput << ','
        << read_us << ','
        << reopen_us << '\n';

    return 0;

  } catch (const std::exception& error) {
    std::cerr << "Benchmark error: " << error.what() << '\n';
    return 2;
  }
}
