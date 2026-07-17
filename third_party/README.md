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
