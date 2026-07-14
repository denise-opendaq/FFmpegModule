cmake_minimum_required(VERSION 3.20)
project(ffmpeg VERSION 1.0.0)

include(ExternalProject)

set(FFMPEG_SOURCE_DIR  "${CMAKE_BINARY_DIR}/_deps/ffmpeg_src")
set(FFMPEG_BUILD_DIR   "${CMAKE_BINARY_DIR}/_deps/ffmpeg_build")
set(FFMPEG_LIB_DIR     "${CMAKE_BINARY_DIR}/bin")

set(FFMPEG_CROSS_COMPILE_ARGS "")
set(FFMPEG_CONFIGURE_LAUNCHER "")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(FFMPEG_TARGET_OS linux)
  set(FFMPEG_ARCH ${CMAKE_SYSTEM_PROCESSOR})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(FFMPEG_TARGET_OS darwin)
  set(FFMPEG_ARCH ${CMAKE_SYSTEM_PROCESSOR})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(FFMPEG_ARCH x86_64)
  else()
    set(FFMPEG_ARCH x86)
  endif()
  if(MSVC)
    # FFmpeg's configure/make are still shell scripts, so they need to run
    # through an MSYS2 bash even when the compiler is cl.exe. The CI job is
    # expected to put an MSYS2 bash on PATH and cl/link/lib on PATH too
    # (e.g. via ilammy/msvc-dev-cmd) before this ExternalProject step runs.
    set(FFMPEG_TARGET_OS win64)
    if(FFMPEG_ARCH STREQUAL "x86")
      set(FFMPEG_TARGET_OS win32)
    endif()
    list(APPEND FFMPEG_CROSS_COMPILE_ARGS --toolchain=msvc)
    set(FFMPEG_CONFIGURE_LAUNCHER bash)
  else()
    set(FFMPEG_TARGET_OS mingw32)
  endif()
else()
  string(TOLOWER "${CMAKE_SYSTEM_NAME}" FFMPEG_TARGET_OS)
  set(FFMPEG_ARCH ${CMAKE_SYSTEM_PROCESSOR})
endif()

list(APPEND FFMPEG_CROSS_COMPILE_ARGS
  --target-os=${FFMPEG_TARGET_OS}
  --arch=${FFMPEG_ARCH}
)

# FFmpeg's ./configure is its own build system and has no idea which
# compiler the outer CMake project picked (e.g. via a preset) - left alone
# it defaults to gcc. Forward the same compiler so the statically-linked
# FFmpeg code matches the toolchain building video_device_module.
if(NOT MSVC)
  if(CMAKE_C_COMPILER)
    list(APPEND FFMPEG_CROSS_COMPILE_ARGS --cc=${CMAKE_C_COMPILER})
  endif()
  if(CMAKE_CXX_COMPILER)
    list(APPEND FFMPEG_CROSS_COMPILE_ARGS --cxx=${CMAKE_CXX_COMPILER})
  endif()
endif()

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
elseif(FFMPEG_TARGET_OS MATCHES "^win")
  set(FFMPEG_INDEV_ARGS --enable-indev=dshow)
else()
  set(FFMPEG_INDEV_ARGS "")
endif()

ExternalProject_Add(ffmpeg_build
  GIT_REPOSITORY https://github.com/FFmpeg/FFmpeg.git
  GIT_TAG n7.1.4
  PREFIX "${FFMPEG_SOURCE_DIR}"

  CONFIGURE_COMMAND
    ${FFMPEG_CONFIGURE_LAUNCHER}
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

      # Static libs only: their code gets folded straight into
      # video_device_module's .dylib/.so/.dll, so the module is
      # self-contained and no separate libavcodec.*/avcodec-*.dll etc.
      # need to ship or be found on the loader path at runtime.
      --enable-static
      --disable-shared
      --enable-pic

  BUILD_COMMAND make -j4 V=1
  INSTALL_COMMAND make install VERBOSE=1

  BUILD_IN_SOURCE 1
  UPDATE_DISCONNECTED TRUE

  BUILD_BYPRODUCTS
    ${FFMPEG_LIB_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}avdevice${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${FFMPEG_LIB_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}avcodec${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${FFMPEG_LIB_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}avformat${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${FFMPEG_LIB_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}avutil${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${FFMPEG_LIB_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}swscale${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${FFMPEG_LIB_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}swresample${CMAKE_STATIC_LIBRARY_SUFFIX}
)

add_library(${PROJECT_NAME} INTERFACE)
add_dependencies(${PROJECT_NAME} ffmpeg_build)

target_include_directories(${PROJECT_NAME} INTERFACE
  "${FFMPEG_BUILD_DIR}/include"
)

target_link_directories(${PROJECT_NAME} INTERFACE
  "${FFMPEG_LIB_DIR}"
)

# Static archives are linked left-to-right, single-pass: each entry here
# must come before the archives that define the symbols it needs, with the
# shared base (avutil) last so it can satisfy every earlier library's wants.
target_link_libraries(${PROJECT_NAME} INTERFACE
  avdevice
  avformat
  avcodec
  swresample
  swscale
  avutil
)

# With shared FFmpeg libs, these were pulled in transitively via each
# .dylib's own LC_LOAD_DYLIB entries. Static archives carry no such linkage,
# so FFmpeg's system-framework/zlib dependencies must be listed explicitly
# here instead - avfoundation/videotoolbox (CoreMedia, CoreVideo,
# CoreGraphics) on Apple, and zlib (used by avformat/avcodec for http and
# png/mov chunk (de)compression) wherever it was found at FFmpeg configure
# time.
if(APPLE)
  target_link_libraries(${PROJECT_NAME} INTERFACE
    "-framework CoreFoundation"
    "-framework CoreGraphics"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework VideoToolbox"
  )
endif()

# dshow (Windows capture indev) is COM-based; not verified against a real
# Windows build yet, but these are the standard system import libs any
# DirectShow consumer needs (CoCreateInstance, IEnumMoniker, GUIDs, ...).
if(WIN32)
  target_link_libraries(${PROJECT_NAME} INTERFACE
    ole32
    oleaut32
    strmiids
    uuid
  )
endif()

find_package(ZLIB QUIET)
if(ZLIB_FOUND)
  target_link_libraries(${PROJECT_NAME} INTERFACE ZLIB::ZLIB)
endif()