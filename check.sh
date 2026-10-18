#!/usr/bin/env bash
# Reproducible offline C++ core verification.
# No broker connection, trading activity, or host-state cleanup.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$ROOT/build-public"

if [[ -x /opt/homebrew/bin/cmake ]]; then
  CMAKE=/opt/homebrew/bin/cmake
else
  CMAKE="$(command -v cmake)"
fi

echo "=== Configure: offline core ==="
env -u LDFLAGS -u CPPFLAGS "$CMAKE" \
  -S "$ROOT" \
  -B "$BUILD" \
  -DEJ_WITH_IBKR=OFF \
  -DSANITIZE=OFF \
  -DCMAKE_EXE_LINKER_FLAGS:STRING=

echo "=== Build ==="
"$CMAKE" --build "$BUILD" --parallel 4

echo "=== Tests ==="
"$CMAKE" -E chdir "$BUILD" ctest --output-on-failure

echo "PASS: offline C++ build and tests"
echo "Scope: local core tests only; IBKR integration not validated."
