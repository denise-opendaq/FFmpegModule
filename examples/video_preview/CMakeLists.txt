set(APP_NAME video_preview)

add_executable(${APP_NAME} main.cpp)

target_compile_definitions(${APP_NAME} PRIVATE MODULE_PATH="${CMAKE_BINARY_DIR}/bin")

target_link_libraries(${APP_NAME} PRIVATE daq::opendaq)

add_dependencies(${APP_NAME} video_device_module)

if (APPLE)
    # SFML static build expects OpenGL at link time for the executable too.
    target_link_libraries(${APP_NAME} PRIVATE "-framework OpenGL")
endif()
