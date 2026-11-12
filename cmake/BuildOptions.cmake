# Shared compile settings, applied by linking vcc_build_options PRIVATE.
add_library(vcc_build_options INTERFACE)

if(MSVC)
    target_compile_options(vcc_build_options INTERFACE /W4 /permissive- /utf-8)
    if(VCC_WARNINGS_AS_ERRORS)
        target_compile_options(vcc_build_options INTERFACE /WX)
    endif()
else()
    target_compile_options(vcc_build_options INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
        -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual)
    if(VCC_WARNINGS_AS_ERRORS)
        target_compile_options(vcc_build_options INTERFACE -Werror)
    endif()
    if(VCC_SANITIZE)
        target_compile_options(vcc_build_options INTERFACE -fsanitize=${VCC_SANITIZE} -fno-omit-frame-pointer)
        target_link_options(vcc_build_options INTERFACE -fsanitize=${VCC_SANITIZE})
    endif()
endif()

# Adds a library from <dir>/src/*.cpp with public headers in <dir>/include.
function(vcc_add_library name)
    cmake_parse_arguments(ARG "" "" "DEPS" ${ARGN})
    file(GLOB _srcs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
    add_library(${name} STATIC ${_srcs})
    target_include_directories(${name} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(${name} PUBLIC ${ARG_DEPS} PRIVATE vcc_build_options)
    set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()
