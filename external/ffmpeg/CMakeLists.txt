cmake_minimum_required(VERSION 3.20)
project(ffmpeg VERSION 1.0.0)

include(ExternalProject)

set(FFMPEG_SOURCE_DIR  "${CMAKE_BINARY_DIR}/_deps/ffmpeg_src")
set(FFMPEG_BUILD_DIR   "${CMAKE_BINARY_DIR}/_deps/ffmpeg_build")
set(FFMPEG_LIB_DIR     "${CMAKE_BINARY_DIR}/bin")

set(FFMPEG_CROSS_COMPILE_ARGS "")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(FFMPEG_TARGET_OS linux)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(FFMPEG_TARGET_OS darwin)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(FFMPEG_TARGET_OS mingw32)
else()
  string(TOLOWER "${CMAKE_SYSTEM_NAME}" FFMPEG_TARGET_OS)
endif()

list(APPEND FFMPEG_CROSS_COMPILE_ARGS
  --target-os=${FFMPEG_TARGET_OS}
  --arch=${CMAKE_SYSTEM_PROCESSOR}
)

if(CMAKE_CROSSCOMPILING)
  list(APPEND FFMPEG_CROSS_COMPILE_ARGS --enable-cross-compile)
  if(CMAKE_C_COMPILER)
    get_filename_component(_cc_name ${CMAKE_C_COMPILER} NAME)
    string(REGEX REPLACE "-?g?cc$" "" CROSS_PREFIX "${_cc_name}")
    list(APPEND FFMPEG_CROSS_COMPILE_ARGS
      --cross-prefix=${CROSS_PREFIX}-
    )
  endif()
endif()

if(FFMPEG_TARGET_OS STREQUAL "linux")
  set(FFMPEG_INDEV_ARGS --enable-indev=v4l2)
elseif(FFMPEG_TARGET_OS STREQUAL "darwin")
  set(FFMPEG_INDEV_ARGS --enable-indev=avfoundation)
else()
  set(FFMPEG_INDEV_ARGS "")
endif()

ExternalProject_Add(ffmpeg_build
  GIT_REPOSITORY https://github.com/FFmpeg/FFmpeg.git
  GIT_TAG n7.1.4
  PREFIX "${FFMPEG_SOURCE_DIR}"

  CONFIGURE_COMMAND
    ${FFMPEG_SOURCE_DIR}/src/ffmpeg_build/configure
      --prefix=${FFMPEG_BUILD_DIR}
      --libdir=${FFMPEG_LIB_DIR}
      ${FFMPEG_CROSS_COMPILE_ARGS}
      --disable-debug
      --enable-gpl
      --disable-programs
      --disable-doc
      --disable-everything

      --enable-avdevice
      --enable-avformat
      --enable-avcodec
      --enable-avutil
      --enable-swscale
      --enable-swresample

      ${FFMPEG_INDEV_ARGS}

      --enable-encoder=mjpeg
      --enable-decoder=mjpeg
      --enable-parser=mjpeg
      --enable-encoder=rawvideo
      --enable-decoder=rawvideo
      --enable-encoder=png
      --enable-encoder=bmp
      --enable-encoder=targa
      --enable-encoder=gif
      --enable-encoder=hdr

      # IP camera support (RTSP streams): the demuxer plus the transport protocols it needs,
      # and h264/hevc since that's what most IP cameras/RTSP servers actually send.
      --enable-demuxer=rtsp
      --enable-protocol=tcp,udp,rtp,http,file
      --enable-parser=h264,hevc
      --enable-decoder=h264,hevc

      --enable-shared
      --disable-static
      --enable-pic

  BUILD_COMMAND make -j4 V=1
  INSTALL_COMMAND make install VERBOSE=1

  BUILD_IN_SOURCE 1
  UPDATE_DISCONNECTED TRUE

  BUILD_BYPRODUCTS
    ${FFMPEG_LIB_DIR}/libavcodec${CMAKE_SHARED_LIBRARY_SUFFIX}
    ${FFMPEG_LIB_DIR}/libavformat${CMAKE_SHARED_LIBRARY_SUFFIX}
    ${FFMPEG_LIB_DIR}/libavutil${CMAKE_SHARED_LIBRARY_SUFFIX}
    ${FFMPEG_LIB_DIR}/libswscale${CMAKE_SHARED_LIBRARY_SUFFIX}
    ${FFMPEG_LIB_DIR}/libswresample${CMAKE_SHARED_LIBRARY_SUFFIX}
)

add_library(${PROJECT_NAME} INTERFACE)
add_dependencies(${PROJECT_NAME} ffmpeg_build)

target_include_directories(${PROJECT_NAME} INTERFACE
  "${FFMPEG_BUILD_DIR}/include"
)

target_link_directories(${PROJECT_NAME} INTERFACE 
  "${FFMPEG_LIB_DIR}"
)

target_link_libraries(${PROJECT_NAME} INTERFACE
  avdevice
  avformat
  avcodec
  avutil
  swscale
)

set_property(TARGET ${PROJECT_NAME} PROPERTY
  INTERFACE_LINK_OPTIONS "-Wl,-rpath,${FFMPEG_LIB_DIR}"
)