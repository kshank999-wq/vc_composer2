add_library(vcc_test_main STATIC support/vcc_test_main.cpp)
target_include_directories(vcc_test_main PUBLIC support)

# One executable and one CTest entry per test file.
function(vcc_add_test name)
    cmake_parse_arguments(ARG "" "" "DEPS" ${ARGN})
    add_executable(${name} unit/${name}.cpp)
    target_link_libraries(${name} PRIVATE vcc_test_main ${ARG_DEPS} vcc_build_options)
    target_compile_definitions(${name} PRIVATE
        VCC_CONTENT_DIR="${VCC_CONTENT_DIR}"
        VCC_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")
    add_test(NAME ${name} COMMAND ${name})
endfunction()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tmp")

vcc_add_test(test_json DEPS vcc_core)
vcc_add_test(test_time DEPS vcc_core)
vcc_add_test(test_files DEPS vcc_core)
vcc_add_test(test_midi DEPS vcc_core)
vcc_add_test(test_pitch DEPS vcc_theory)
vcc_add_test(test_chords DEPS vcc_theory)
vcc_add_test(test_progressions DEPS vcc_theory)
vcc_add_test(test_ostinato DEPS vcc_ostinato)
vcc_add_test(test_project DEPS vcc_project)
vcc_add_test(test_templates DEPS vcc_templates)
vcc_add_test(test_workflow DEPS vcc_templates)
add_subdirectory(ported)
