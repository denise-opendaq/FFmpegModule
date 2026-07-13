cmake_minimum_required(VERSION 3.10)
set_cmake_folder_context(TARGET_FOLDER_NAME)

if(APPLE)
    project(VideoDeviceModule VERSION 1.0.0 LANGUAGES CXX OBJCXX)
else()
    project(VideoDeviceModule VERSION 1.0.0 LANGUAGES CXX)
endif()

add_subdirectory(src)

if(${REPO_OPTION_PREFIX}_ENABLE_TESTS)
    add_subdirectory(tests)
endif()