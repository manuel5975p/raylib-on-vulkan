# Setup the project and settings
project(examples)

# Sources are the upstream raylib examples with minimal porting deltas (SPIR-V
# shaders, rlgl.h -> compat shim) plus raylib-on-vulkan-specific additions such as
# others/vk_custom_commands.c. Excluded examples are listed with reasons.
#
# Vulkan shader sources live at <category>/resources/shaders/vk/*.vert|frag|comp
# and are compiled by glslc into the build-tree resources copy as <name>.spv
# (full extension kept: foo.vert -> foo.vert.spv). Binaries run from their
# build category dir, where resources/ is copied.

# Directories that contain examples
set(example_dirs
    audio
    core
    models
    others
    shaders
    shapes
    text
    textures
    )

set(RAYVK_EXAMPLES_EXCLUDE
    # OpenGL-specific by nature
    raylib_opengl_interop        # GL compute/PBO interop; see others/vk_custom_commands.c instead
    rlgl_standalone              # standalone rlgl+GLFW without raylib window
    # Web/platform-specific
    core_basic_window_web
    core_input_gestures_web
)

function(rayvk_add_example_category cat)
    # Per-category exclusions (owned by the category porter): a list named
    # RAYVK_EXAMPLES_EXCLUDE_EXTRA with one commented reason per entry
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${cat}/exclude.cmake)
        include(${CMAKE_CURRENT_SOURCE_DIR}/${cat}/exclude.cmake)
        list(APPEND RAYVK_EXAMPLES_EXCLUDE ${RAYVK_EXAMPLES_EXCLUDE_EXTRA})
    endif()
    set(outdir ${CMAKE_CURRENT_BINARY_DIR}/${cat})
    file(MAKE_DIRECTORY ${outdir})

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${cat}/resources)
        file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/${cat}/resources DESTINATION ${outdir})
    endif()

    # Compile Vulkan GLSL shaders to SPIR-V into the build resources copy
    file(GLOB vkshaders
        ${CMAKE_CURRENT_SOURCE_DIR}/${cat}/resources/shaders/vk/*.vert
        ${CMAKE_CURRENT_SOURCE_DIR}/${cat}/resources/shaders/vk/*.frag
        ${CMAKE_CURRENT_SOURCE_DIR}/${cat}/resources/shaders/vk/*.comp)
    set(spvfiles "")
    foreach(sh ${vkshaders})
        get_filename_component(shname ${sh} NAME)
        set(spv ${outdir}/resources/shaders/vk/${shname}.spv)
        add_custom_command(OUTPUT ${spv}
            COMMAND ${GLSLC} -O -g ${sh} -o ${spv}
            DEPENDS ${sh} COMMENT "glslc ${cat}/${shname}")
        list(APPEND spvfiles ${spv})
    endforeach()
    if(spvfiles)
        add_custom_target(${cat}_shaders ALL DEPENDS ${spvfiles})
    endif()

    file(GLOB example_sources ${CMAKE_CURRENT_SOURCE_DIR}/${cat}/*.c)
    foreach(example_source ${example_sources})
        get_filename_component(example_name ${example_source} NAME_WE)
        if(example_name IN_LIST RAYVK_EXAMPLES_EXCLUDE)
            continue()
        endif()
        add_executable(${example_name} ${example_source})
        target_link_libraries(${example_name} PRIVATE rayvulkan)
        set_target_properties(${example_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${outdir})
        if(TARGET ${cat}_shaders)
            add_dependencies(${example_name} ${cat}_shaders)
        endif()
    endforeach()
endfunction()

foreach(example_dir ${example_dirs})
    rayvk_add_example_category(${example_dir})
endforeach()

# The Vulkan interop example includes volk.h directly
target_include_directories(vk_custom_commands PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/external)
target_compile_definitions(vk_custom_commands PRIVATE VK_NO_PROTOTYPES)
