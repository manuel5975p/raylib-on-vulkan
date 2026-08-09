# ---------------------------------------------------------------------------
# wayland_protocols.cmake - generate the Wayland protocol glue used by
# src/platforms/rcore_wayland.c and add it to the raylib-on-vulkan target.
#
# Requires: wayland-scanner (WAYLAND_SCANNER), the wayland-protocols package
# and an already declared `raylib-on-vulkan` target.
# ---------------------------------------------------------------------------

if(NOT TARGET rayvulkan)
    message(FATAL_ERROR "wayland_protocols.cmake must be included after add_library(rayvulkan ...)")
endif()

if(NOT WAYLAND_SCANNER)
    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
if(NOT WAYLAND_PROTOCOLS_DIR)
    message(FATAL_ERROR "Could not determine the wayland-protocols pkgdatadir")
endif()

set(WAYLAND_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/wayland)
file(MAKE_DIRECTORY ${WAYLAND_GEN_DIR})

# Generate <name>-client-protocol.h and <name>-protocol.c from a protocol XML and
# append both to WAYLAND_PROTOCOL_SOURCES in the caller scope
function(wayland_generate_protocol XML_PATH)
    if(NOT EXISTS ${XML_PATH})
        message(FATAL_ERROR "Wayland protocol XML not found: ${XML_PATH}")
    endif()

    get_filename_component(name ${XML_PATH} NAME_WE)
    set(header ${WAYLAND_GEN_DIR}/${name}-client-protocol.h)
    set(source ${WAYLAND_GEN_DIR}/${name}-protocol.c)

    add_custom_command(OUTPUT ${header}
        COMMAND ${WAYLAND_SCANNER} client-header ${XML_PATH} ${header}
        DEPENDS ${XML_PATH}
        COMMENT "wayland-scanner client-header ${name}")
    add_custom_command(OUTPUT ${source}
        COMMAND ${WAYLAND_SCANNER} private-code ${XML_PATH} ${source}
        DEPENDS ${XML_PATH}
        COMMENT "wayland-scanner private-code ${name}")

    set(WAYLAND_PROTOCOL_SOURCES ${WAYLAND_PROTOCOL_SOURCES} ${header} ${source} PARENT_SCOPE)
endfunction()

set(WAYLAND_PROTOCOL_SOURCES "")
wayland_generate_protocol(${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml)
wayland_generate_protocol(${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml)
wayland_generate_protocol(${WAYLAND_PROTOCOLS_DIR}/unstable/relative-pointer/relative-pointer-unstable-v1.xml)
wayland_generate_protocol(${WAYLAND_PROTOCOLS_DIR}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml)

# Explicit target so the headers always exist before rcore_wayland.c is compiled
add_custom_target(rayvulkan_wayland_protocols DEPENDS ${WAYLAND_PROTOCOL_SOURCES})
add_dependencies(rayvulkan rayvulkan_wayland_protocols)

target_sources(rayvulkan PRIVATE ${WAYLAND_PROTOCOL_SOURCES})
target_include_directories(rayvulkan PRIVATE ${WAYLAND_GEN_DIR})

# wayland-scanner output triggers warnings we do not control
set_source_files_properties(${WAYLAND_PROTOCOL_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
