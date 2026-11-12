# VC Composer 2 desktop application (JUCE).
#
# Layout:
#   vcc_juce          JUCE modules, compiled once, shared by the app and its tests
#   vcc_*_data        Bravura and the content packs, embedded in the binary
#   vcc_ui_notation   notation view and renderer (ported from the original VC Composer)
#   vcc_desktop       the application: audio engine, windows, dialogs
#   VCComposer2       the executable / app bundle
#   vcc_desktop_check headless smoke test: builds the whole UI offscreen and
#                     writes screenshots (registered with CTest)
#
# JUCE code is compiled with JUCE's recommended flags rather than this
# project's stricter set, which JUCE's own headers are not written for.

add_library(vcc_juce STATIC)
target_link_libraries(vcc_juce
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
target_compile_definitions(vcc_juce PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=$<BOOL:${APPLE}>
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_STANDALONE_APPLICATION=1
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_APPLICATION_NAME_STRING="VC Composer 2"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")
target_compile_definitions(vcc_juce INTERFACE $<TARGET_PROPERTY:vcc_juce,COMPILE_DEFINITIONS>)
target_include_directories(vcc_juce INTERFACE $<TARGET_PROPERTY:vcc_juce,INCLUDE_DIRECTORIES>)
set_target_properties(vcc_juce PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- Embedded resources -------------------------------------------------------
set(vcc_font_dir "${PROJECT_SOURCE_DIR}/resources/fonts/Bravura")
juce_add_binary_data(vcc_music_font_data
    HEADER_NAME "VccMusicFontData.h"
    NAMESPACE vcc_music_font_data
    SOURCES "${vcc_font_dir}/Bravura.otf" "${vcc_font_dir}/bravura_metadata.json")

# One target per content folder: file names repeat between folders
# (instruments/historical.json, templates/historical.json).
file(GLOB vcc_content_root CONFIGURE_DEPENDS "${VCC_CONTENT_DIR}/*.json")
file(GLOB vcc_content_instruments CONFIGURE_DEPENDS "${VCC_CONTENT_DIR}/instruments/*.json")
file(GLOB vcc_content_templates CONFIGURE_DEPENDS "${VCC_CONTENT_DIR}/templates/*.json")
juce_add_binary_data(vcc_content_root_data HEADER_NAME "VccContentRoot.h"
    NAMESPACE vcc_content_root SOURCES ${vcc_content_root})
juce_add_binary_data(vcc_content_instruments_data HEADER_NAME "VccContentInstruments.h"
    NAMESPACE vcc_content_instruments SOURCES ${vcc_content_instruments})
juce_add_binary_data(vcc_content_templates_data HEADER_NAME "VccContentTemplates.h"
    NAMESPACE vcc_content_templates SOURCES ${vcc_content_templates})
foreach(t vcc_music_font_data vcc_content_root_data vcc_content_instruments_data vcc_content_templates_data)
    set_target_properties(${t} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endforeach()

# --- Notation UI (ported) ---------------------------------------------------------
file(GLOB vcc_ui_notation_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/notation/src/*.cpp")
add_library(vcc_ui_notation STATIC ${vcc_ui_notation_sources})
target_include_directories(vcc_ui_notation PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/notation/include")
target_link_libraries(vcc_ui_notation PUBLIC vcc_juce vcc_engraving vcc_editing vcc_composer vcc_music
                                      PRIVATE vcc_music_font_data)

# --- Application ------------------------------------------------------------------
file(GLOB_RECURSE vcc_desktop_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(vcc_desktop STATIC ${vcc_desktop_sources})
target_include_directories(vcc_desktop PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# JUCE's flag set includes -Wswitch-enum, which flags every switch over the
# shortcut-command enum even with a default case. Per-source so it lands after
# JUCE's own flags on the command line.
if(NOT MSVC)
    set_source_files_properties(${vcc_desktop_sources} ${vcc_ui_notation_sources}
                                PROPERTIES COMPILE_OPTIONS "-Wno-switch-enum")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(X11 REQUIRED)
    target_link_libraries(vcc_desktop PRIVATE X11::X11)
endif()
target_link_libraries(vcc_desktop
    PUBLIC vcc_ui_notation vcc_session vcc_media vcc_picture vcc_cues vcc_templates vcc_project vcc_theory vcc_ostinato vcc_engine vcc_audio vcc_juce
    PRIVATE vcc_content_root_data vcc_content_instruments_data vcc_content_templates_data)

juce_add_gui_app(VCComposer2
    PRODUCT_NAME "VC Composer 2"
    COMPANY_NAME "VC Composer"
    BUNDLE_ID "com.vccomposer.vccomposer2"
    VERSION "${PROJECT_VERSION}"
    DOCUMENT_EXTENSIONS vcc2
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "VC Composer 2 records audio and MIDI input.")
target_sources(VCComposer2 PRIVATE app/Main.cpp)
target_link_libraries(VCComposer2 PRIVATE vcc_desktop)

# --- Test instrument plug-in (used by the headless check only) ---------------------
juce_add_plugin(VccTestInstrument
    FORMATS VST3
    PRODUCT_NAME "VCC Test Instrument"
    COMPANY_NAME "VC Composer"
    PLUGIN_MANUFACTURER_CODE Vccm
    PLUGIN_CODE Vcct
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_AUTO_MANIFEST FALSE)
target_sources(VccTestInstrument PRIVATE testplugin/TestInstrument.cpp)
target_compile_definitions(VccTestInstrument PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
                                                    JUCE_DISPLAY_SPLASH_SCREEN=0)
target_link_libraries(VccTestInstrument PRIVATE juce::juce_audio_utils
                                        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# --- Headless check ----------------------------------------------------------------
add_executable(vcc_desktop_check check/DesktopCheck.cpp)
target_link_libraries(vcc_desktop_check PRIVATE vcc_desktop)
add_dependencies(vcc_desktop_check VccTestInstrument_VST3)
target_compile_definitions(vcc_desktop_check PRIVATE
    VCC_TEST_PLUGIN="$<GENEX_EVAL:$<TARGET_PROPERTY:VccTestInstrument_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>")
if(VCC_BUILD_TESTS)
    add_test(NAME desktop_check COMMAND vcc_desktop_check "${CMAKE_BINARY_DIR}/screenshots")
endif()
