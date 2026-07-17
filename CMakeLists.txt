cmake_minimum_required(VERSION 3.22)
project(execution_journal CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SANITIZE "Enable ASan+UBSan" ON)

add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow)
if(SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(ej_core
  src/core/types.cpp
  src/lifecycle/order_state.cpp
)
target_include_directories(ej_core PUBLIC src)

# --- tests ---
include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(ej_tests tests/test_types.cpp tests/test_lifecycle.cpp)
target_link_libraries(ej_tests PRIVATE ej_core gtest_main)
include(GoogleTest)
gtest_discover_tests(ej_tests)
