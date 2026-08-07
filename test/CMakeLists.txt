file(GLOB DBPLAYGROUND_TEST_SOURCES "${PROJECT_SOURCE_DIR}/test/*/*Test.cpp")

######################################################################################################################
# MAKE TARGETS
######################################################################################################################

##########################################
# "make check-tests"
##########################################
add_custom_target(build-tests COMMAND ${CMAKE_CTEST_COMMAND} --show-only)
add_custom_target(check-tests COMMAND ${CMAKE_CTEST_COMMAND} --verbose)

##########################################
# "make XYZ_test"
##########################################
foreach (dbplayground_test_source ${DBPLAYGROUND_TEST_SOURCES})
    # Create a human readable name.
    get_filename_component(dbplayground_test_filename ${dbplayground_test_source} NAME)
    string(REPLACE ".cpp" "" dbplayground_test_name ${dbplayground_test_filename})

    # Add the test target separately and as part of "make check-tests".
    # EXCLUDE_FROM_ALL 表示不直接编译当前的可执行文件，需要后续通过 make XXX 来手动指定编译
    # 最后的参数表示不将 EXCLUDE_FROM_ALL 应用在该文件上，即 xxx.cpp 一定会被编译
    add_executable(${dbplayground_test_name} ${dbplayground_test_source})
    add_dependencies(build-tests ${dbplayground_test_name})
    add_dependencies(check-tests ${dbplayground_test_name})
    target_link_libraries(${dbplayground_test_name} dbplayground_lib _gtest_all _gmock_all)

    # Set test target properties and dependencies.
    set_target_properties(
        ${dbplayground_test_name}
        PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test"
            COMMAND ${dbplayground_test_name})

    # Add the test under CTest.
    add_test(NAME ${dbplayground_test_name}
             COMMAND ${CMAKE_BINARY_DIR}/test/${dbplayground_test_name}
                     --gtest_color=yes
                     --gtest_output=xml:${CMAKE_BINARY_DIR}/test/${dbplayground_test_name}.xml)

endforeach()
