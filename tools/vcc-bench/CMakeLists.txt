add_executable(vcc-bench main.cpp)
target_link_libraries(vcc-bench PRIVATE vcc_session vcc_build_options)
target_compile_definitions(vcc-bench PRIVATE VCC_CONTENT_DIR="${VCC_CONTENT_DIR}")
