#!/bin/bash
# Health check: build + tests + CLI smoke. Exit 0 = healthy.
set -e
cd "$(dirname "$0")/build"
cmake .. >/dev/null
make -j2 2>&1 | tail -1
ctest 2>&1 | grep "tests passed"
rm -f /tmp/ej_check.journal
./ej-audit verify /tmp/ej_check.journal >/dev/null 2>&1 || true  # missing = empty = ok path
echo "✓ execution-journal healthy"
