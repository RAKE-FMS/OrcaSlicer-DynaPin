get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests_main.cpp
    test_preview_capture.cpp
    )

if (MSVC)
    target_link_libraries(${_TEST_NAME}_tests Setupapi.lib)
endif ()

target_link_libraries(${_TEST_NAME}_tests test_common libslic3r_gui libslic3r Catch2::Catch2WithMain)
# GUI headers require the wxWidgets settings discovered in the src directory.
get_directory_property(_wx_includes DIRECTORY ${PROJECT_SOURCE_DIR}/src DEFINITION wxWidgets_INCLUDE_DIRS)
get_directory_property(_wx_definitions DIRECTORY ${PROJECT_SOURCE_DIR}/src DEFINITION wxWidgets_DEFINITIONS)
get_directory_property(_wx_cxx_flags DIRECTORY ${PROJECT_SOURCE_DIR}/src DEFINITION wxWidgets_CXX_FLAGS)
separate_arguments(_wx_cxx_flags NATIVE_COMMAND "${_wx_cxx_flags}")
target_include_directories(${_TEST_NAME}_tests SYSTEM PRIVATE ${_wx_includes})
target_compile_definitions(${_TEST_NAME}_tests PRIVATE ${_wx_definitions})
target_compile_options(${_TEST_NAME}_tests PRIVATE ${_wx_cxx_flags})
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

if (WIN32)
    if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
    orcaslicer_copy_dlls(COPY_DLLS "Debug" "d" output_dlls_Debug)
    elseif("${CMAKE_BUILD_TYPE}" STREQUAL "RelWithDebInfo")
    orcaslicer_copy_dlls(COPY_DLLS "RelWithDebInfo" "" output_dlls_Release)
    else()
    orcaslicer_copy_dlls(COPY_DLLS "Release" "" output_dlls_Release)
    endif()
endif()

catch_discover_tests(${_TEST_NAME}_tests)
