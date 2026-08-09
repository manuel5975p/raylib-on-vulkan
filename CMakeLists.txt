cmake_minimum_required(VERSION 3.22)
project(rayvulkan C)

# Avoid excessive expansion of variables in conditionals
# See https://cmake.org/cmake/help/latest/policy/CMP0054.html
cmake_policy(SET CMP0054 NEW)

# Directory for easier includes
# Anywhere you see include(...) you can check <root>/cmake for that file
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# Registers build options that are exposed to cmake
include(CMakeOptions.txt)

# Main sources directory (the second parameter sets the output directory name to raylib-on-vulkan)
add_subdirectory(src rayvulkan)

if (${BUILD_EXAMPLES})
    message(STATUS "Building examples is enabled")
    add_subdirectory(examples)
endif()

if (${BUILD_TESTING})
    enable_testing()
    add_subdirectory(tests)
endif()
