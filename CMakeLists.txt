cmake_minimum_required(VERSION 3.20)
project(open_hsdu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

option(OPENHSDU_SANITIZE "Build the tests with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(OPENHSDU_COVERAGE "Build the tests with coverage instrumentation" OFF)

set(OPENHSDU_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror)

# The core: everything that is not the ESP32. No Arduino, no heap in the hot path.
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS core/src/*.cpp)
add_library(openhsdu_core STATIC ${CORE_SOURCES})
target_include_directories(openhsdu_core PUBLIC core/include)
target_compile_options(openhsdu_core PRIVATE ${OPENHSDU_WARNINGS})

# The chip model and fakes the tests use.
add_library(openhsdu_sim STATIC sim/mock_cc1101.cpp)
target_include_directories(openhsdu_sim PUBLIC sim)
target_link_libraries(openhsdu_sim PUBLIC openhsdu_core)
target_compile_options(openhsdu_sim PRIVATE ${OPENHSDU_WARNINGS})

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS test/*.cpp)
add_executable(openhsdu_tests ${TEST_SOURCES})
target_include_directories(openhsdu_tests PRIVATE third_party/doctest)
target_link_libraries(openhsdu_tests PRIVATE openhsdu_sim)
target_compile_options(openhsdu_tests PRIVATE ${OPENHSDU_WARNINGS})

if(OPENHSDU_SANITIZE)
  foreach(t openhsdu_core openhsdu_sim openhsdu_tests)
    target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    target_link_options(${t} PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()

if(OPENHSDU_COVERAGE)
  foreach(t openhsdu_core openhsdu_sim openhsdu_tests)
    target_compile_options(${t} PRIVATE --coverage -O0)
    target_link_options(${t} PRIVATE --coverage)
  endforeach()
endif()

enable_testing()
foreach(suite math profiles power ring_buffer driver modem console host_mux hardening mutation-kills integration fuzz wiring)
  add_test(NAME "${suite}" COMMAND openhsdu_tests "--test-suite=${suite}")
endforeach()
