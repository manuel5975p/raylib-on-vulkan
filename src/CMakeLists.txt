# Setup the project and settings
project(rayvulkan C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

message(STATUS "raylib-on-vulkan platform backend: ${PLATFORM}")

# Used as public API to be included into other projects
set(rayvulkan_public_headers
    raylib.h
    raymath.h
    rcamera.h
    rgestures.h
    rlgl.h
    rlvk.h
    rayvk.h
    )

# ---------------------------------------------------------------------------
# Default shaders: GLSL -> SPIR-V -> embedded C arrays
# ---------------------------------------------------------------------------
find_program(GLSLC glslc REQUIRED)
set(SHADER_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${SHADER_GEN_DIR})

function(embed_spirv OUT_C_LIST)
    set(genfiles "")
    foreach(shader ${ARGN})
        get_filename_component(name ${shader} NAME)
        string(REPLACE "." "_" sym ${name})
        set(spv ${SHADER_GEN_DIR}/${name}.spv)
        set(cfile ${SHADER_GEN_DIR}/${sym}_spv.c)
        add_custom_command(OUTPUT ${spv}
            COMMAND ${GLSLC} -O -g ${shader} -o ${spv}   # -g keeps OpName for SPIR-V reflection
            DEPENDS ${shader} COMMENT "glslc ${name}")
        add_custom_command(OUTPUT ${cfile}
            COMMAND ${CMAKE_COMMAND} -DSPV=${spv} -DSYM=${sym} -DOUT=${cfile}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/embed_spirv.cmake
            DEPENDS ${spv} COMMENT "embed ${name}")
        list(APPEND genfiles ${cfile})
    endforeach()
    set(${OUT_C_LIST} ${genfiles} PARENT_SCOPE)
endfunction()

embed_spirv(SHADER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/default.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/default.frag)

# Sources to be compiled
set(rayvulkan_sources
    rcore.c
    rlvk.c
    rshapes.c
    rtextures.c
    rtext.c
    rmodels.c
    raudio.c
    utils.c
    external/volk.c
    ${SHADER_SOURCES}
    )

if(PLATFORM STREQUAL "X11")
    list(APPEND rayvulkan_sources platforms/rcore_x11.c)
elseif(PLATFORM STREQUAL "WAYLAND")
    list(APPEND rayvulkan_sources platforms/rcore_wayland.c)
elseif(PLATFORM STREQUAL "WIN32")
    list(APPEND rayvulkan_sources platforms/rcore_win32.c)
elseif(PLATFORM STREQUAL "ANDROID")
    list(APPEND rayvulkan_sources platforms/rcore_android.c)
elseif(PLATFORM STREQUAL "MACOS")
    list(APPEND rayvulkan_sources platforms/rcore_macos.c)
endif()

add_library(rayvulkan STATIC ${rayvulkan_sources} ${rayvulkan_public_headers})

set_target_properties(rayvulkan PROPERTIES
                      PUBLIC_HEADER "${rayvulkan_public_headers}"
                      )

# Registering include directories
target_include_directories(rayvulkan
                           PUBLIC
                           $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                           PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/external
                           ${SHADER_GEN_DIR}
                           )
target_compile_definitions(rayvulkan PRIVATE PLATFORM_${PLATFORM} VK_NO_PROTOTYPES)

if(UNIX AND NOT APPLE)
    target_link_libraries(rayvulkan PUBLIC m pthread ${CMAKE_DL_LIBS})
endif()

if(PLATFORM STREQUAL "X11")
    find_package(X11 REQUIRED)
    target_link_libraries(rayvulkan PRIVATE X11::X11 X11::Xrandr X11::Xcursor X11::Xi X11::Xfixes)
elseif(PLATFORM STREQUAL "WAYLAND")
    find_package(PkgConfig REQUIRED)
    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
    pkg_check_modules(WL REQUIRED IMPORTED_TARGET wayland-client wayland-cursor xkbcommon)
    # xdg-shell + extras protocol glue generated at build time
    include(wayland_protocols)
    target_link_libraries(rayvulkan PRIVATE PkgConfig::WL)
elseif(PLATFORM STREQUAL "WIN32")
    # NOTE: XInput is loaded at runtime (xinput1_4 -> 1_3 -> 9_1_0), no import library needed
    target_link_libraries(rayvulkan PRIVATE user32 gdi32 shell32 winmm ole32)
elseif(PLATFORM STREQUAL "ANDROID")
    target_link_libraries(rayvulkan PRIVATE android log)
    # The NativeActivity entry point lives in a static library: keep the linker from dropping it
    target_link_options(rayvulkan INTERFACE "-Wl,-u,ANativeActivity_onCreate")
elseif(PLATFORM STREQUAL "MACOS")
    target_link_libraries(rayvulkan PRIVATE
        "-framework Cocoa" "-framework QuartzCore" "-framework CoreGraphics" objc)
endif()

# Warnings: keep the library clean under gcc and clang (MSVC has no such flags)
if(NOT MSVC)
    target_compile_options(rayvulkan PRIVATE -Wall -Wextra -Wno-unused-parameter)
    if(PLATFORM STREQUAL "MACOS")
        # objc_msgSend must be cast to the exact message signature at every call site
        target_compile_options(rayvulkan PRIVATE -Wno-cast-function-type)
    endif()
endif()
