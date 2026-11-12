add_executable(vcc main.cpp)
target_link_libraries(vcc PRIVATE vcc_templates vcc_build_options)
target_compile_definitions(vcc PRIVATE VCC_DEFAULT_CONTENT_DIR="${VCC_CONTENT_DIR}")
