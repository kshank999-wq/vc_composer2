vcc_add_library(vcc_engraving DEPS vcc_notation)

# Writes a SMuFL test font whose glyph list comes from the engine itself
# (tools/fonts/make-test-music-font.py drives it).
if(VCC_BUILD_TOOLS)
    add_executable(smufl-font-gen ${PROJECT_SOURCE_DIR}/tools/smufl-font-gen/main.cpp)
    target_link_libraries(smufl-font-gen PRIVATE vcc_engraving vcc_build_options)
endif()
