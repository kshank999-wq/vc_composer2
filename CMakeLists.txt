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
    LANGUAGES C CXX
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
option(VCC_BUILD_DESKTOP "Build the desktop application (needs JUCE)" OFF)
# JUCE is never vendored. Point VCC_JUCE_DIR at a checkout, or let the build
# fetch the pinned release. VC Composer 2 uses JUCE under the commercial
# JUCE 8 licence (docs/01-decisions-and-license-gates.md, D13).
set(VCC_JUCE_DIR "" CACHE PATH "Path to a JUCE source tree")
set(VCC_JUCE_VERSION "8.0.9" CACHE STRING "JUCE release fetched when VCC_JUCE_DIR is empty")
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
# Ported from the original VC Composer (docs/02-original-vc-composer-reuse-audit.md)
add_subdirectory(music)
add_subdirectory(audio)
add_subdirectory(notation)
add_subdirectory(engraving)
add_subdirectory(composer)
add_subdirectory(editing)
add_subdirectory(engine)
# VC Composer 2
add_subdirectory(theory)
add_subdirectory(ostinato)
add_subdirectory(project)
add_subdirectory(templates)
add_subdirectory(picture)
add_subdirectory(media)
add_subdirectory(cues)
add_subdirectory(session)

if(VCC_BUILD_TOOLS)
    add_subdirectory(tools/vcc-cli)
endif()

if(VCC_BUILD_DESKTOP)
    if(VCC_JUCE_DIR)
        if(NOT EXISTS "${VCC_JUCE_DIR}/CMakeLists.txt")
            message(FATAL_ERROR "VCC_JUCE_DIR does not look like a JUCE source tree: ${VCC_JUCE_DIR}")
        endif()
        add_subdirectory("${VCC_JUCE_DIR}" "${CMAKE_BINARY_DIR}/_juce" EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)
        FetchContent_Declare(juce
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG ${VCC_JUCE_VERSION}
            GIT_SHALLOW TRUE)
        FetchContent_GetProperties(juce)
        if(NOT juce_POPULATED)
            if(POLICY CMP0169)
                cmake_policy(SET CMP0169 OLD)  # keep EXCLUDE_FROM_ALL on CMake < 3.28
            endif()
            FetchContent_Populate(juce)
            add_subdirectory(${juce_SOURCE_DIR} ${juce_BINARY_DIR} EXCLUDE_FROM_ALL)
        endif()
    endif()
    add_subdirectory(desktop)
endif()

if(VCC_BUILD_TESTS)
    add_subdirectory(tests)
endif()
