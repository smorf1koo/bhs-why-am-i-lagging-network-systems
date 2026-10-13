cmake_minimum_required(VERSION 3.20)
project(wail_net LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# --- common: fixed wire protocol + clock helpers (ready-made) -------------------------------------
add_library(wail_common common/src/protocol.cpp)
target_include_directories(wail_common PUBLIC common/include)
target_compile_options(wail_common PUBLIC -Wall -Wextra -Wpedantic)

# --- server: READY-MADE authoritative server with the built-in network emulator --------------------
add_executable(wail_server server/main.cpp)
target_link_libraries(wail_server PRIVATE wail_common)

# --- client: YOUR TASK (C++ skeleton; a Python client is fine too, see python/) --------------------
add_executable(wail_client client/main.cpp)
target_link_libraries(wail_client PRIVATE wail_common)

# --- protocol round-trip self-test ------------------------------------------------------------------
add_executable(protocol_selftest tests/protocol_selftest.cpp)
target_link_libraries(protocol_selftest PRIVATE wail_common)

enable_testing()
add_test(NAME protocol_selftest COMMAND protocol_selftest)
