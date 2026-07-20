# Third-party dependencies (not vendored)

## IBKR TWS API (required for the M6 broker adapter)

The TWS API is licensed by Interactive Brokers and may NOT be
redistributed with this repository. To build:

1. Download `twsapi_macunix.<version>.zip` from
   https://interactivebrokers.github.io/
2. Unzip into `third_party/twsapi/` so that
   `third_party/twsapi/IBJts/source/cppclient/client/` exists
3. Build as usual (`cmake .. && make`)

Version used during development: 10.30.01.

## Intel Decimal Floating-Point Math Library (required by TWS Decimal.cpp)

Not redistributed here. Build per IBKR's own `Intel_lib_build.txt`:

1. Download `IntelRDFPMathLib20U2.tar.gz` (Intel site or netlib mirror)
2. Extract into `third_party/`, then in `IntelRDFPMathLib20U2/LIBRARY`:
   `make CC=gcc CALL_BY_REF=0 GLOBAL_RND=0 GLOBAL_FLAGS=0 UNCHANGED_BINARY_FLAGS=0`
3. `libbid.a` is produced there; CMake links it automatically.
