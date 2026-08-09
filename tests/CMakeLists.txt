# Tests run with a timeout: they probe edge cases, crashes/hangs are expected findings.
set(TESTS_NOWINDOW
    test_text
    test_colors
    test_images
    test_compression
    test_files
    test_collision)

set(TESTS_WINDOW
    test_window_smoke
    test_render_readback)

foreach(t ${TESTS_NOWINDOW} ${TESTS_WINDOW})
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE rayvulkan)
    add_test(NAME ${t} COMMAND ${t})
    set_tests_properties(${t} PROPERTIES TIMEOUT 60)
endforeach()

foreach(t ${TESTS_WINDOW})
    set_tests_properties(${t} PROPERTIES
        DISABLED $<NOT:$<BOOL:$ENV{DISPLAY}>>)
endforeach()
