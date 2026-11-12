# VC Composer 2 - notation-first scoring workstation.
#
# The libraries here hold every piece of musical and project logic. They
# depend on nothing but the C++17 standard library, so they build and test
# on any machine. The desktop shell (JUCE: audio devices, VST3 hosting,
# windows) is a separate layer on top - see docs/00-architecture-and-roadmap.md.
cmake_minimum_required(VERSION 3.22)

project(VCComposer2
    VERSION 0.1.0
    DESCRIPTION "VC Composer 2 - scoring workstation"
    LANGUAGES CXX
)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported. Use: cmake -B build")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build configuration" FORCE)
endif()

option(VCC_BUILD_TESTS "Build the test suites" ON)
option(VCC_BUILD_TOOLS "Build the vcc command-line tool" ON)
option(VCC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(VCC_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. address,undefined")

include(BuildOptions)

# Shipped content (templates, instruments, progressions) is found here by the
# tests and the CLI. The desktop app copies it into its bundle.
set(VCC_CONTENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/content")

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(VCC_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(core)
add_subdirectory(theory)
add_subdirectory(ostinato)
add_subdirectory(project)
add_subdirectory(templates)

if(VCC_BUILD_TOOLS)
    add_subdirectory(tools/vcc-cli)
endif()

if(VCC_BUILD_TESTS)
    add_subdirectory(tests)
endif()
