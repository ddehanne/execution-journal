# Journal Performance Benchmark

## Purpose

Reproducible C++20 microbenchmark of the execution journal.

Measures:
- Append latency: p50, p95, p99
- Append throughput
- Journal read and integrity verification
- Journal reopening

Each append includes an fsync call.
Payload size: 128 bytes.

## Build

    cmake -S . -B build-bench -DEJ_WITH_IBKR=OFF -DEJ_BUILD_BENCHMARKS=ON -DSANITIZE=OFF -DCMAKE_BUILD_TYPE=Release
    cmake --build build-bench --parallel 2

## Run

    ./build-bench/journal_bench 1000
    ./build-bench/journal_bench 10000

Each execution creates a fresh temporary journal and emits CSV.

## Methodology

Append latency is measured with std::chrono::steady_clock.
Percentiles use linear interpolation over sorted observations.
Throughput is based on total append-loop elapsed time.
Read/verify and reopen are measured separately.

## Initial test environment

- CPU: Apple M3 Pro
- Architecture: ARM64
- RAM: 18 GiB
- OS: macOS 27.0.1
- Compiler: Apple Clang 21.0.0
- Build: Release, sanitizers disabled
- Storage: macOS Data volume via /tmp

## Limitations

Results depend on hardware, filesystem, OS and background load.
fsync semantics do not guarantee identical power-loss durability
across operating systems and storage devices.

This benchmark does not measure live trading, broker connectivity,
market-data processing or HFT performance.

No CPU affinity, thermal stabilization or cache control is applied.
Read and reopen timings are single observations per execution.

These measurements do not establish production readiness.
