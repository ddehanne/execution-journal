cmake_minimum_required(VERSION 3.22)
project(execution_journal CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SANITIZE "Enable ASan+UBSan" ON)
option(EJ_WITH_IBKR "Build the optional IBKR adapter" OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow)
if(SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(ej_core
  src/core/types.cpp
  src/lifecycle/order_state.cpp
  src/journal/journal.cpp
  src/risk/risk_gate.cpp
  src/recovery/reconcile.cpp
)
target_include_directories(ej_core PUBLIC src)
if(EJ_WITH_IBKR)
  set(EJ_TWS_CLIENT_DIR
      "${CMAKE_SOURCE_DIR}/third_party/twsapi/IBJts/source/cppclient/client")
  set(EJ_INTEL_DECIMAL_LIB
      "${CMAKE_SOURCE_DIR}/third_party/IntelRDFPMathLib20U2/LIBRARY/libbid.a")

  if(NOT EXISTS "${EJ_TWS_CLIENT_DIR}/EClientSocket.cpp")
    message(FATAL_ERROR
      "EJ_WITH_IBKR=ON requires the licensed IBKR TWS API. "
      "See third_party/README.md.")
  endif()

  if(NOT EXISTS "${EJ_INTEL_DECIMAL_LIB}")
    message(FATAL_ERROR
      "EJ_WITH_IBKR=ON requires Intel Decimal libbid.a. "
      "See third_party/README.md.")
  endif()

  # ── IBKR TWS API (third-party: their code, their warnings suppressed) ──
  file(GLOB IBKR_SOURCES third_party/twsapi/IBJts/source/cppclient/client/*.cpp)
  add_library(ibkr_client STATIC ${IBKR_SOURCES})
  target_include_directories(ibkr_client SYSTEM PUBLIC
    third_party/twsapi/IBJts/source/cppclient/client)
  # Their code will never pass our strict flags — isolate completely.
  target_compile_options(ibkr_client PRIVATE -w)
  if(SANITIZE)
    target_compile_options(ibkr_client PRIVATE -fsanitize=address,undefined)
  endif()
  find_package(Threads REQUIRED)
  # Intel Decimal FP lib — required by TWS Decimal.cpp (bid64 symbols).
  # Not vendored (see third_party/README.md); built per Intel_lib_build.txt.
  target_link_libraries(ibkr_client PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/IntelRDFPMathLib20U2/LIBRARY/libbid.a
    Threads::Threads)
  add_library(ej_ibkr src/ibkr/ibkr_broker.cpp)
  target_include_directories(ej_ibkr PUBLIC src)
  target_link_libraries(ej_ibkr PUBLIC ej_core ibkr_client)
endif()

# --- tests ---
include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(ej_tests tests/test_types.cpp tests/test_lifecycle.cpp tests/test_journal.cpp tests/test_risk.cpp tests/test_recovery.cpp tests/test_fault_injection.cpp)
target_link_libraries(ej_tests PRIVATE ej_core gtest_main)
include(GoogleTest)
gtest_discover_tests(ej_tests)


add_executable(ej-audit src/cli/main.cpp)
target_link_libraries(ej-audit PRIVATE ej_core)
