set(LIB_NAME video_device_module)
set(MODULE_HEADERS_DIR ../include/${TARGET_FOLDER_NAME})
set(MODULE_BINARY_HEADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/../include/${TARGET_FOLDER_NAME})

set(SRC_Include common.h
                module_dll.h
                fallback_timer.h
                time_accumulator.h
                video_device_module_impl.h
                camera_channel_impl.h
                camera_device_impl.h
                camera_driver.h
                camera_platform.h
                video_frame_encoder_fb_impl.h
)

set(SRC_Srcs module_dll.cpp
             fallback_timer.cpp
             time_accumulator.cpp
             video_device_module_impl.cpp
             camera_device_impl.cpp
             camera_channel_impl.cpp
             camera_driver.cpp
             camera_platform.cpp
             video_frame_encoder_fb_impl.cpp
)

if (${REPO_OPTION_PREFIX}_ENABLE_VIDEO_DISPLAY)
    opendaq_embed_resource("${CMAKE_CURRENT_SOURCE_DIR}/arial.ttf"
                           ${MODULE_BINARY_HEADER_DIR}/arial.ttf.h
                           "ARIAL_TTF")
    list(APPEND SRC_Include video_display_fb_impl.h)
    list(APPEND SRC_Srcs video_display_fb_impl.cpp
                         ${MODULE_BINARY_HEADER_DIR}/arial.ttf.h)
endif()

if(APPLE)
    list(APPEND SRC_Srcs camera_platform_macos.mm)
endif()

opendaq_prepend_include(${TARGET_FOLDER_NAME} SRC_Include)

source_group("module" FILES ${MODULE_HEADERS_DIR}/module_dll.h
                            ${MODULE_HEADERS_DIR}/video_device_module_impl.h
                            ${MODULE_HEADERS_DIR}/camera_device_impl.h
                            ${MODULE_HEADERS_DIR}/camera_channel_impl.h
                            ${MODULE_HEADERS_DIR}/camera_driver.h
                            ${MODULE_HEADERS_DIR}/camera_platform.h
                            ${MODULE_HEADERS_DIR}/video_frame_encoder_fb_impl.h
                            $<$<BOOL:${REPO_OPTION_PREFIX}_ENABLE_VIDEO_DISPLAY>:${MODULE_HEADERS_DIR}/video_display_fb_impl.h>
                            module_dll.cpp
                            video_device_module_impl.cpp
                            camera_device_impl.cpp
                            camera_channel_impl.cpp
                            camera_driver.cpp
                            camera_platform.cpp
                            video_frame_encoder_fb_impl.cpp
                            $<$<BOOL:${REPO_OPTION_PREFIX}_ENABLE_VIDEO_DISPLAY>:video_display_fb_impl.cpp>
                            $<$<BOOL:${REPO_OPTION_PREFIX}_ENABLE_VIDEO_DISPLAY>:${MODULE_BINARY_HEADER_DIR}/arial.ttf.h>
                            $<$<BOOL:${APPLE}>:camera_platform_macos.mm>
)

add_library(${LIB_NAME} SHARED ${SRC_Include}
                               ${SRC_Srcs}
)

add_library(${SDK_TARGET_NAMESPACE}::${LIB_NAME} ALIAS ${LIB_NAME})

if("${Boost_LIBRARIES}")
    set(BOOST_LIBS ${Boost_LIBRARIES})
endif()

target_link_libraries(${LIB_NAME}
        PUBLIC
            daq::opendaq
        PRIVATE
            ${BOOST_LIBS}
            ffmpeg
)

# Should link only what you need $<BUILD_INTERFACE:Boost::locale>
target_link_libraries(${LIB_NAME} PRIVATE $<BUILD_INTERFACE:Boost::locale>)

if (${REPO_OPTION_PREFIX}_ENABLE_VIDEO_DISPLAY)
    target_compile_definitions(${LIB_NAME} PRIVATE VIDEO_DEVICE_MODULE_ENABLE_VIDEO_DISPLAY)
    target_link_libraries(${LIB_NAME} PRIVATE SFML::Graphics)
endif()

if(APPLE)
    target_link_libraries(${LIB_NAME} PRIVATE "-framework AVFoundation" "-framework Foundation")
endif()

if (CMAKE_COMPILER_IS_GNUCXX AND MINGW)
    target_compile_options(${LIB_NAME} PRIVATE -Wno-error)
endif()

target_include_directories(${LIB_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
                                              $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/../include>
                                              $<INSTALL_INTERFACE:include>
                                              ${Boost_INCLUDE_DIRS}
)

opendaq_set_module_properties(${LIB_NAME} ${PROJECT_VERSION_MAJOR})
opendaq_generate_version_header(${LIB_NAME})
