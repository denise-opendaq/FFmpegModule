opendaq_set_cmake_folder_context(TARGET_FOLDER_NAME)

if (MSVC)
    add_compile_options(/wd4100)
endif()

add_subdirectory(video_device_module)