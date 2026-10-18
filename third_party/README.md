# Third-party dependencies (not vendored)

These dependencies are not needed for the default offline core build (`EJ_WITH_IBKR=OFF`).

## IBKR TWS API (optional adapter only)

The TWS API is licensed by Interactive Brokers and may NOT be
redistributed with this repository. To build:

1. Download `twsapi_macunix.<version>.zip` from
   https://interactivebrokers.github.io/
2. Unzip into `third_party/twsapi/` so that
   `third_party/twsapi/IBJts/source/cppclient/client/` exists
3. Configure with `cmake -S . -B build-ibkr -DEJ_WITH_IBKR=ON`, then build with `cmake --build build-ibkr`.

Version used during development: 10.30.01.

## Intel Decimal Floating-Point Math Library (required by TWS Decimal.cpp)

Not redistributed here. Build per IBKR's own `Intel_lib_build.txt`:

1. Download `IntelRDFPMathLib20U2.tar.gz` (Intel site or netlib mirror)
2. Extract into `third_party/`, then in `IntelRDFPMathLib20U2/LIBRARY`:
   `make CC=gcc CALL_BY_REF=0 GLOBAL_RND=0 GLOBAL_FLAGS=0 UNCHANGED_BINARY_FLAGS=0`
3. `libbid.a` is produced there; CMake links it automatically.
