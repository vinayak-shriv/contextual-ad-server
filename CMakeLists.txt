cmake_minimum_required(VERSION 3.16)
project(contextual_ad_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(ADSERVE_SANITIZE "" CACHE STRING "Build with a sanitizer: address, thread, or undefined")
option(ADSERVE_BUILD_SERVER "Build the HTTP server" ON)

find_package(Threads REQUIRED)

# Sanitizers apply to every target, so they are set before any target is defined.
if(ADSERVE_SANITIZE)
  if(MSVC)
    message(FATAL_ERROR "ADSERVE_SANITIZE is only supported with GCC/Clang")
  endif()
  message(STATUS "Sanitizer: ${ADSERVE_SANITIZE}")
  add_compile_options(-fsanitize=${ADSERVE_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${ADSERVE_SANITIZE})
endif()

add_library(adserve_core
  src/tokenizer.cpp
  src/context_index.cpp
  src/auction.cpp
  src/budget.cpp
  src/frequency_cap.cpp
  src/ctr_estimator.cpp
  src/metrics.cpp
  src/ad_server.cpp
  src/inventory_loader.cpp
)
target_include_directories(adserve_core PUBLIC include)
target_link_libraries(adserve_core PUBLIC Threads::Threads)

if(MSVC)
  target_compile_options(adserve_core PRIVATE /W4)
else()
  target_compile_options(adserve_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()

# ---- tests
enable_testing()
add_executable(adserve_tests tests/test_main.cpp tests/test_components.cpp tests/test_ad_server.cpp)
target_include_directories(adserve_tests PRIVATE tests bench)
target_link_libraries(adserve_tests PRIVATE adserve_core)
add_test(NAME adserve_tests COMMAND adserve_tests)

# ---- benchmark
add_executable(adserve_bench bench/bench.cpp)
target_include_directories(adserve_bench PRIVATE bench)
target_link_libraries(adserve_bench PRIVATE adserve_core)

# ---- HTTP server (cpp-httplib, vendored single header, MIT licence)
if(ADSERVE_BUILD_SERVER)
  add_executable(adserve_server src/main_server.cpp)
  target_include_directories(adserve_server SYSTEM PRIVATE third_party/httplib)
  target_link_libraries(adserve_server PRIVATE adserve_core)
  if(WIN32)
    target_link_libraries(adserve_server PRIVATE ws2_32)
  endif()
endif()
