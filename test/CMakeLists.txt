file(GLOB DBPLAYGROUND_TEST_SOURCES "${PROJECT_SOURCE_DIR}/test/*/*Test.cpp")

######################################################################################################################
# TEST TARGETS
#
# For each test/<Module>/<Name>Test.cpp this creates:
#   * an executable (output name "<Name>Test", in ${CMAKE_BINARY_DIR}/test)
#   * a CTest case "<Name>Test"
#   * a convenience target "<Name>Test" that builds *and runs* just that test:
#         ninja <Name>Test      /   make <Name>Test
#
# The aggregate "test" target builds every test and then runs the whole suite:
#         ninja test            /   make test
######################################################################################################################

# Collected executable targets, so the aggregate `test` target can depend on them.
set(DBPLAYGROUND_TEST_EXES "")

foreach (dbplayground_test_source ${DBPLAYGROUND_TEST_SOURCES})
    # Human readable name, e.g. "BufferPoolManagerTest".
    get_filename_component(dbplayground_test_filename ${dbplayground_test_source} NAME)
    string(REPLACE ".cpp" "" dbplayground_test_name ${dbplayground_test_filename})

    # The executable uses an internal target name so the friendly `${name}`
    # target below (build-and-run) is free for the user to invoke directly.
    set(dbplayground_test_exe "${dbplayground_test_name}_exe")

    add_executable(${dbplayground_test_exe} ${dbplayground_test_source})
    target_link_libraries(${dbplayground_test_exe} dbplayground_lib _gtest_all _gmock_all)
    set_target_properties(
        ${dbplayground_test_exe}
        PROPERTIES
            OUTPUT_NAME ${dbplayground_test_name}
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")

    # Register the test with CTest. Using the target as COMMAND lets CTest
    # resolve the built binary's location regardless of generator layout.
    add_test(NAME ${dbplayground_test_name}
             COMMAND ${dbplayground_test_exe}
                     --gtest_color=yes
                     --gtest_output=xml:${CMAKE_BINARY_DIR}/test/${dbplayground_test_name}.xml)

    # `ninja/make <Name>Test`: build only this test, then run only this test.
    add_custom_target(${dbplayground_test_name}
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R "^${dbplayground_test_name}$"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        USES_TERMINAL
        COMMENT "Running ${dbplayground_test_name}")
    add_dependencies(${dbplayground_test_name} ${dbplayground_test_exe})

    list(APPEND DBPLAYGROUND_TEST_EXES ${dbplayground_test_exe})
endforeach()

# `ninja/make test`: build every test, then run the whole suite.
add_custom_target(test
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
    COMMENT "Building and running all tests")
add_dependencies(test ${DBPLAYGROUND_TEST_EXES})

# `ninja/make build-tests`: build every test without running (kept for convenience).
add_custom_target(build-tests DEPENDS ${DBPLAYGROUND_TEST_EXES})
