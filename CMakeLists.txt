cmake_minimum_required(VERSION 3.21)
project(lwallpaper VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "LWallpaper requires Windows and a 64-bit MSVC toolchain.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
include(cmake/Dependencies.cmake)

add_executable(lwallpaper WIN32
    src/app/main.cpp
    src/gui/lwallpaperGUI.cpp
    src/gui/lwallpaperGUI.h
    src/gui/lwallpaperGUI.ui
    src/wallpaper/wallpaper_api.hpp
    src/wallpaper/wallpaper_core.hpp
    src/render/d3d11_zero_copy.hpp
    resources/lwallpaperGUI.qrc
    resources/lwallpaper.rc)

target_include_directories(lwallpaper PRIVATE src resources)
target_include_directories(lwallpaper SYSTEM PRIVATE third_party/rang)
target_compile_options(lwallpaper PRIVATE /W3 /utf-8 /permissive-)
target_compile_definitions(lwallpaper PRIVATE UNICODE _UNICODE QT_NO_DEPRECATED_WARNINGS)
option(LWALLPAPER_ENABLE_LOGGING "Enable playback diagnostics" OFF)
target_compile_definitions(lwallpaper PRIVATE DEBUG_ENABLED=$<BOOL:${LWALLPAPER_ENABLE_LOGGING}>)
target_link_libraries(lwallpaper PRIVATE Qt6::Widgets lwallpaper_dependencies
    d3d11 dxgi d3dcompiler dwmapi shcore winmm)
set_target_properties(lwallpaper PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(LWALLPAPER_DEPLOY_RUNTIME "Copy Qt, FFmpeg and SDL3 runtime files after building" ON)
if(LWALLPAPER_DEPLOY_RUNTIME)
    get_target_property(_qt_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin "${_qt_qmake}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_bin}" REQUIRED)
    add_custom_command(TARGET lwallpaper POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${LWALLPAPER_RUNTIME_DLLS} "$<TARGET_FILE_DIR:lwallpaper>"
        COMMAND "${WINDEPLOYQT_EXECUTABLE}"
            "$<IF:$<CONFIG:Debug>,--debug,--release>"
            --no-translations --no-compiler-runtime
            "$<TARGET_FILE:lwallpaper>"
        COMMENT "Deploying FFmpeg, SDL3 and Qt runtime files"
        VERBATIM)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
