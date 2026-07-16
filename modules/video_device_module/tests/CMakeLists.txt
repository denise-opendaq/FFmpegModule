set(TEST_APP test_video_device_module)

if(NOT TARGET ${OPENDAQ_SDK_TARGET_NAMESPACE}::opendaq_test_utils)
    message(FATAL_ERROR "VIDEO_DEVICE_MODULE tests require OPENDAQ_ENABLE_TEST_UTILS=ON")
endif()

set(TEST_SOURCES
    test_app.cpp
    video_frame_encoder_fb_impl_test.cpp
    video_device_module_test.cpp
)

if (${REPO_OPTION_PREFIX}_ENABLE_VIDEO_DISPLAY)
    list(APPEND TEST_SOURCES video_display_fb_impl_test.cpp)
endif()

add_executable(${TEST_APP} ${TEST_SOURCES})

target_include_directories(${TEST_APP} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(${TEST_APP} PRIVATE
    ${OPENDAQ_SDK_TARGET_NAMESPACE}::opendaq_test_utils
    gtest
    video_device_module
    ffmpeg
)

add_test(NAME ${TEST_APP}
         COMMAND $<TARGET_FILE_NAME:${TEST_APP}>
         WORKING_DIRECTORY $<TARGET_FILE_DIR:${TEST_APP}>
)
