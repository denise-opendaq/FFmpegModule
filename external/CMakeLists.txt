opendaq_get_cmake_mode(_FFMPEG_CMAKE_MODERN_MODE_SAVED)
opendaq_set_cmake_mode(ANCIENT)
set(CMAKE_FOLDER external)
list(APPEND CMAKE_MESSAGE_CONTEXT external)

opendaq_ignore_compiler_warnings()

if (${REPO_OPTION_PREFIX}_ENABLE_TESTS AND (NOT TARGET gtest OR NOT TARGET gmock))
    set(BUILD_GMOCK ON)
    opendaq_module_setup_gtest("1.12.1")

    # GMock somehow doesn't have proper include paths set for GTest dependency
    get_target_property(GTEST_INCLUDE_DIRS gtest INTERFACE_INCLUDE_DIRECTORIES)
    target_include_directories(gmock PRIVATE ${GTEST_INCLUDE_DIRS})
    target_include_directories(gmock_main PRIVATE ${GTEST_INCLUDE_DIRS})
endif()

message(STATUS "Including ffmpeg...")
add_subdirectory(ffmpeg EXCLUDE_FROM_ALL)

opendaq_set_cmake_mode(${_FFMPEG_CMAKE_MODERN_MODE_SAVED})

