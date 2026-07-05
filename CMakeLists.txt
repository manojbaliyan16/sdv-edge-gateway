cmake_minimum_required(VERSION 3.16)
project(sdv_edge_gateway VERSION 1.0.0 LANGUAGES CXX)

# ─── C++ Standard ─────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)   # hard fail if compiler doesn't support C++17
set(CMAKE_CXX_EXTENSIONS OFF)         # ISO C++17 only — no GCC-specific extensions

# ─── Warnings + Build Type ────────────────────────────────────────────────────
add_compile_options(-Wall -Wextra -Wpedantic)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)     # default to optimised build on Pi
endif()

# ─── DLT — enabled on Linux (Pi target) only ──────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_definitions(USE_DLT)
    find_package(automotive-dlt QUIET)   # QUIET — don't error if not found on cross-compile host
endif()

# ─── Dependencies ─────────────────────────────────────────────────────────────

# OpenSSL and Threads ship CMake config files — use find_package (gives named targets)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# SQLite3 — no CMake config, find pieces manually
find_library(SQLITE3_LIB     sqlite3    REQUIRED)  # finds libsqlite3.so
find_path   (SQLITE3_INCLUDE sqlite3.h  REQUIRED)  # finds folder containing sqlite3.h

# Paho MQTT — no CMake config, find both layers manually
find_library(PAHO_MQTT_C_LIB   paho-mqtt3as  REQUIRED)  # C core
find_library(PAHO_MQTT_CPP_LIB paho-mqttpp3  REQUIRED)  # C++ wrapper (depends on C core)
find_path   (PAHO_INCLUDE      mqtt/async_client.h REQUIRED)  # folder containing mqtt/

# libcurl — HTTPS download for OTA firmware (ota_agent.cpp)
find_package(CURL REQUIRED)

# yaml-cpp — ships a CMake config, use find_package for named target
find_package(yaml-cpp REQUIRED)

# ONNX Runtime — no CMake config, find pieces manually (same pattern as SQLite3/Paho)
# Install on Pi:  pip install onnxruntime  (or apt install libonnxruntime-dev)
# Install on Mac: pip install onnxruntime  (headers + lib land in Python site-packages)
find_library(ONNXRUNTIME_LIB     onnxruntime           REQUIRED)
find_path   (ONNXRUNTIME_INCLUDE onnxruntime_cxx_api.h  REQUIRED)

# ─── Modules ──────────────────────────────────────────────────────────────────
# Order reflects dependency hierarchy — can_layer has no deps, gateway uses it
add_subdirectory(src/can_layer)
add_subdirectory(src/gateway)
add_subdirectory(src/security)
add_subdirectory(src/analytics)

# ─── Main executable ──────────────────────────────────────────────────────────
add_executable(sdv_gateway src/main.cpp)

target_include_directories(sdv_gateway PRIVATE
    ${CMAKE_SOURCE_DIR}/include   # our headers: common/, gateway/, can_layer/ etc.
    ${PAHO_INCLUDE}               # mqtt/async_client.h — main.cpp wires up the client
)

target_link_libraries(sdv_gateway PRIVATE
    gateway
    can_layer
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    yaml-cpp::yaml-cpp            # main.cpp loads config.yaml at startup
    # security, analytics — added as each module is implemented
)
