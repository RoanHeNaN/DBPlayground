file(GLOB DBPLAYGROUND_TEST_SOURCES "${PROJECT_SOURCE_DIR}/test/*/*Test.cpp")

######################################################################################################################
# TEST TARGETS
#
# For each test/<Module>/<Name>Test.cpp this creates:
#   * an executable "<Name>Test_exe" in ${CMAKE_BINARY_DIR}/test
#   * a convenience target "<Name>Test" that builds *and runs* just that test:
#         ninja <Name>Test      /   make <Name>Test
#
# The aggregate "test" target builds every test and then runs them all in turn:
#         ninja test            /   make test
#
# We drive the gtest binaries directly rather than through CTest: CTest reserves
# the target name "test" and its built-in target never rebuilds the binaries.
######################################################################################################################

set(DBPLAYGROUND_TEST_EXES "")

foreach (dbplayground_test_source ${DBPLAYGROUND_TEST_SOURCES})
    # Human readable name, e.g. "BufferPoolManagerTest".
    get_filename_component(dbplayground_test_filename ${dbplayground_test_source} NAME)
    string(REPLACE ".cpp" "" dbplayground_test_name ${dbplayground_test_filename})

    # The executable uses a distinct "<Name>Test_exe" name (both as target and
    # output file) so the friendly "<Name>Test" name stays free for the
    # build-and-run target below. Giving the executable OUTPUT_NAME "<Name>Test"
    # would create a top-level alias that shadows that run target.
    set(dbplayground_test_exe "${dbplayground_test_name}_exe")

    add_executable(${dbplayground_test_exe} ${dbplayground_test_source})
    target_link_libraries(${dbplayground_test_exe} dbplayground_lib _gtest_all _gmock_all)
    set_target_properties(
        ${dbplayground_test_exe}
        PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")

    # `ninja/make <Name>Test`: build only this test, then run only this test.
    add_custom_target(${dbplayground_test_name}
        COMMAND ${dbplayground_test_exe}
                --gtest_color=yes
                --gtest_output=xml:${CMAKE_BINARY_DIR}/test/${dbplayground_test_name}.xml
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/test
        USES_TERMINAL
        COMMENT "Running ${dbplayground_test_name}")

    list(APPEND DBPLAYGROUND_TEST_EXES ${dbplayground_test_exe})
endforeach()

# `ninja/make test`: build every test, then run them all (sequentially, so the
# gtest output stays readable). Any non-zero exit fails the build.
add_custom_target(test COMMENT "Building and running all tests")
add_dependencies(test ${DBPLAYGROUND_TEST_EXES})
foreach (dbplayground_test_exe ${DBPLAYGROUND_TEST_EXES})
    add_custom_command(
        TARGET test POST_BUILD
        COMMAND ${dbplayground_test_exe} --gtest_color=yes
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/test
        USES_TERMINAL
        VERBATIM)
endforeach()

# `ninja/make build-tests`: build every test without running (kept for convenience).
add_custom_target(build-tests)
add_dependencies(build-tests ${DBPLAYGROUND_TEST_EXES})
