cmake_minimum_required(VERSION 3.20)
project(ffmpeg VERSION 1.0.0)

include(ExternalProject)

set(FFMPEG_SOURCE_DIR  "${CMAKE_BINARY_DIR}/_deps/ffmpeg_src")
set(FFMPEG_BUILD_DIR   "${CMAKE_BINARY_DIR}/_deps/ffmpeg_build")
set(FFMPEG_LIB_DIR     "${CMAKE_BINARY_DIR}/bin")

set(FFMPEG_CROSS_COMPILE_ARGS "")
set(FFMPEG_CONFIGURE_LAUNCHER "")
set(FFMPEG_MAKE_LAUNCHER make)

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
    # through a Windows-native bash even when the compiler is cl.exe. Git
    # for Windows' bash is used here rather than MSYS2 - it's already
    # present on GitHub's windows-2022 runners and on most Windows dev
    # machines that have Git installed, so there's one fewer toolchain to
    # provision. A bare "bash" name is unsafe here: if WSL is installed,
    # Windows also has a C:\Windows\System32\bash.exe launcher stub, and it
    # typically wins PATH resolution inside MSBuild's process environment
    # over Git's bash. That stub runs bash *inside* the WSL VM, which then
    # can't resolve native Windows paths like C:/.../configure (fails with
    # "No such file or directory"). So look for Git's own bash explicitly
    # before falling back to PATH search - rather than guessing its install
    # location (which varies: custom drive, portable install, scoop/winget,
    # ...), let CMake's own FindGit module locate git.exe (registry keys,
    # standard dirs, PATH) and derive bash's location from it: git.exe sits
    # in <root>/cmd or <root>/bin, and bash.exe lives in <root>/usr/bin (or
    # <root>/bin on older layouts).
    find_package(Git QUIET)
    if(GIT_EXECUTABLE)
      get_filename_component(_ffmpeg_git_bin_dir "${GIT_EXECUTABLE}" DIRECTORY)
      get_filename_component(_ffmpeg_git_root_dir "${_ffmpeg_git_bin_dir}" DIRECTORY)
    endif()
    find_program(FFMPEG_BASH_EXECUTABLE
      NAMES bash.exe bash
      HINTS
        "${_ffmpeg_git_root_dir}/usr/bin"
        "${_ffmpeg_git_root_dir}/bin"
    )
    if(NOT FFMPEG_BASH_EXECUTABLE)
      message(FATAL_ERROR
        "ffmpeg_build needs Git for Windows' bash to run FFmpeg's "
        "configure/make. None was found in the usual install locations "
        "(Git's usr/bin or bin) or on PATH. Install Git for Windows, or "
        "set -DFFMPEG_BASH_EXECUTABLE=<path to bash.exe>.")
    endif()
    get_filename_component(FFMPEG_BASH_DIR "${FFMPEG_BASH_EXECUTABLE}" DIRECTORY)

    # Git for Windows doesn't bundle 'make', and it can't just be "any make
    # that runs on Windows": the common standalone Windows ports (ezwinports
    # - what "choco install make" gives you - and GnuWin32) build their
    # child command line by re-parsing each recipe with their own
    # DOS-oriented quoting logic, which corrupts "\\" into "\" even inside
    # single-quoted text. FFmpeg's own build (ffbuild/common.mak) depends on
    # exactly that surviving intact - its dependency-file generation runs
    # recipes like awk '{ gsub(/\\/, "/"); ... }', and the collapsed regex
    # is what fails as an "unterminated string" deep into the build. Builds
    # of make linked against a POSIX runtime (MSYS2, Cygwin) hand recipe
    # text to the shell untouched and don't have this bug - hence the HINT
    # below at a conventional cache path where such a make.exe can be
    # placed; it doesn't need to sit next to bash, since it resolves
    # msys-2.0.dll via bash's directory already being on PATH through the
    # wrapper below.
    # NO_DEFAULT_PATH: this must NOT fall back to searching PATH/the system -
    # GitHub's windows-2022 runner image (and many dev machines with "choco
    # install make" or GnuWin32 installed) already has a make.exe on PATH,
    # and it's the broken DOS-quoting kind described above. Restricting the
    # search to only this cache dir means the sole way FFMPEG_MAKE_EXECUTABLE
    # gets set is either the verified-good download below, or a make.exe a
    # human deliberately placed at this exact path (per the FATAL_ERROR
    # message further down).
    set(FFMPEG_MAKE_CACHE_DIR "$ENV{LOCALAPPDATA}/msys2-make")
    find_program(FFMPEG_MAKE_EXECUTABLE
      NAMES make.exe make
      HINTS "${FFMPEG_MAKE_CACHE_DIR}"
      NO_DEFAULT_PATH
    )
    if(NOT FFMPEG_MAKE_EXECUTABLE)
      # Nothing usable found - fetch the same POSIX-runtime make.exe CI
      # uses (pulled straight from MSYS2's package repo, not a full MSYS2
      # install) into the cache dir above, so a plain "cmake --build" works
      # here without anyone having to hunt this down by hand first.
      message(STATUS "ffmpeg_build: no POSIX-runtime 'make' found, downloading one from MSYS2's repo into ${FFMPEG_MAKE_CACHE_DIR}...")
      set(FFMPEG_MAKE_PKG "${CMAKE_CURRENT_BINARY_DIR}/msys2-make.pkg.tar.zst")
      file(DOWNLOAD
        "https://repo.msys2.org/msys/x86_64/make-4.4.1-3-x86_64.pkg.tar.zst"
        "${FFMPEG_MAKE_PKG}"
        EXPECTED_HASH SHA256=af0bdba17f06fe037f0194069adaa31a8fe45f1a11381501896aea1fae37bd5d
        STATUS FFMPEG_MAKE_DOWNLOAD_STATUS
      )
      list(GET FFMPEG_MAKE_DOWNLOAD_STATUS 0 FFMPEG_MAKE_DOWNLOAD_CODE)
      if(NOT FFMPEG_MAKE_DOWNLOAD_CODE EQUAL 0)
        list(GET FFMPEG_MAKE_DOWNLOAD_STATUS 1 FFMPEG_MAKE_DOWNLOAD_MESSAGE)
        message(FATAL_ERROR
          "ffmpeg_build needs a POSIX-runtime 'make' (MSYS2 or Cygwin "
          "build - NOT the native ezwinports/GnuWin32 'make' that 'choco "
          "install make' gives you, which corrupts backslashes in recipe "
          "text and breaks FFmpeg's build), and the automatic download of "
          "one failed: ${FFMPEG_MAKE_DOWNLOAD_MESSAGE}. Check your network "
          "connection, or manually place a POSIX-runtime make.exe at "
          "${FFMPEG_MAKE_CACHE_DIR}/make.exe, or set "
          "-DFFMPEG_MAKE_EXECUTABLE=<path to one>.")
      endif()
      file(ARCHIVE_EXTRACT
        INPUT "${FFMPEG_MAKE_PKG}"
        DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/msys2-make-extracted"
        PATTERNS "usr/bin/make.exe"
      )
      # COPY, not RENAME: RENAME maps to Windows' MoveFile, which refuses to
      # move a file across drives - and on GitHub's windows-2022 runners the
      # checkout sits on D: while %LOCALAPPDATA% resolves to a path on C:,
      # so a same-drive-only RENAME fails there.
      file(MAKE_DIRECTORY "${FFMPEG_MAKE_CACHE_DIR}")
      file(COPY
        "${CMAKE_CURRENT_BINARY_DIR}/msys2-make-extracted/usr/bin/make.exe"
        DESTINATION "${FFMPEG_MAKE_CACHE_DIR}"
      )
      find_program(FFMPEG_MAKE_EXECUTABLE
        NAMES make.exe
        HINTS "${FFMPEG_MAKE_CACHE_DIR}"
        NO_DEFAULT_PATH
      )
      if(NOT FFMPEG_MAKE_EXECUTABLE)
        message(FATAL_ERROR "ffmpeg_build: downloaded make.exe but find_program still can't locate it at ${FFMPEG_MAKE_CACHE_DIR}.")
      endif()
    endif()
    get_filename_component(FFMPEG_MAKE_DIR "${FFMPEG_MAKE_EXECUTABLE}" DIRECTORY)

    set(FFMPEG_TARGET_OS win64)
    if(FFMPEG_ARCH STREQUAL "x86")
      set(FFMPEG_TARGET_OS win32)
    endif()
    list(APPEND FFMPEG_CROSS_COMPILE_ARGS --toolchain=msvc)

    # PATH is itself a ;-separated string, and ExternalProject_Add's
    # CONFIGURE_COMMAND/BUILD_COMMAND/INSTALL_COMMAND are plain CMake lists -
    # splicing a "PATH=..." value into one via an unquoted variable
    # reference makes CMake re-split it into many separate argv entries on
    # every embedded ";" (breaking "PATH=..." into a bunch of bare paths,
    # the first of which then gets mistaken for the command to run).
    # Escaping each ";" as "\;" only survives a single level of list
    # expansion, not the several CONFIGURE_LAUNCHER/CONFIGURE_COMMAND
    # indirections here, so fix PATH inside a tiny generated wrapper
    # script instead - entirely outside CMake's list machinery. bash's own
    # directory goes on PATH too: launched bare (no -l/--login) it doesn't
    # source /etc/profile, so it wouldn't otherwise find Git's own coreutils
    # (sed, tr, mkdir, uname, ...) that FFmpeg's configure needs.
    set(FFMPEG_BASH_WRAPPER "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg_bash_wrapper.cmd")
    file(WRITE "${FFMPEG_BASH_WRAPPER}"
"@echo off\r\nset \"PATH=${FFMPEG_BASH_DIR};${FFMPEG_MAKE_DIR};%PATH%\"\r\n%*\r\n")
    set(FFMPEG_CONFIGURE_LAUNCHER "${FFMPEG_BASH_WRAPPER}" "${FFMPEG_BASH_EXECUTABLE}")
    set(FFMPEG_MAKE_LAUNCHER "${FFMPEG_BASH_WRAPPER}" "${FFMPEG_MAKE_EXECUTABLE}")
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
      --disable-libdrm
      --disable-x86asm

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

  BUILD_COMMAND ${FFMPEG_MAKE_LAUNCHER} -j4 V=1
  INSTALL_COMMAND ${FFMPEG_MAKE_LAUNCHER} install VERBOSE=1

  BUILD_IN_SOURCE 1
  UPDATE_DISCONNECTED TRUE

  BUILD_BYPRODUCTS
    ${FFMPEG_LIB_DIR}/libavdevice.a
    ${FFMPEG_LIB_DIR}/libavcodec.a
    ${FFMPEG_LIB_DIR}/libavformat.a
    ${FFMPEG_LIB_DIR}/libavutil.a
    ${FFMPEG_LIB_DIR}/libswscale.a
    ${FFMPEG_LIB_DIR}/libswresample.a
)

add_library(${PROJECT_NAME} INTERFACE)
add_dependencies(${PROJECT_NAME} ffmpeg_build)

# SYSTEM: FFmpeg's own headers (e.g. libavutil/common.h's av_clip_* clipping
# macros) trip narrowing-conversion warnings - MSVC's C4244 in particular,
# which our own -Werror/-WX build flag would otherwise escalate to a hard
# error. Marking the include as SYSTEM tells the compiler these aren't our
# warnings to fix.
target_include_directories(${PROJECT_NAME} SYSTEM INTERFACE
  "${FFMPEG_BUILD_DIR}/include"
)

# Belt-and-suspenders: whether MSVC actually honors SYSTEM includes (i.e.
# emits /external:I + /external:W0) depends on toolset/CMake version
# specifics that aren't verified here, so also explicitly silence C4244 for
# consumers of this target on MSVC.
target_compile_options(${PROJECT_NAME} INTERFACE
  $<$<CXX_COMPILER_ID:MSVC>:/wd4244>
)

# Static archives are linked left-to-right, single-pass: each entry here
# must come before the archives that define the symbols it needs, with the
# shared base (avutil) last so it can satisfy every earlier library's wants.
target_link_libraries(${PROJECT_NAME} INTERFACE
  ${FFMPEG_LIB_DIR}/libavdevice.a
  ${FFMPEG_LIB_DIR}/libavformat.a
  ${FFMPEG_LIB_DIR}/libavcodec.a
  ${FFMPEG_LIB_DIR}/libswresample.a
  ${FFMPEG_LIB_DIR}/libswscale.a
  ${FFMPEG_LIB_DIR}/libavutil.a
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
# ws2_32 is Winsock2, needed by avformat's network protocols (tcp/udp/rtp,
# used for RTSP streams). bcrypt.lib backs avutil's av_get_random_seed on
# Windows (BCryptGenRandom) - referenced with its file extension, not the
# bare name, because this project already has its own CMake target called
# "bcrypt" (a vendored dependency of openDAQ), and target_link_libraries
# resolves a bare name to a same-named CMake target before treating it as
# a linker library.
if(WIN32)
  target_link_libraries(${PROJECT_NAME} INTERFACE
    ole32
    oleaut32
    strmiids
    uuid
    shlwapi
    ws2_32
    bcrypt.lib
  )
endif()

find_package(ZLIB QUIET)
if(ZLIB_FOUND)
  target_link_libraries(${PROJECT_NAME} INTERFACE ZLIB::ZLIB)
endif()