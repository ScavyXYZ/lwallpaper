add_executable(lwallpaper_performance_tests performance_tests.cpp)
set_target_properties(lwallpaper_performance_tests PROPERTIES
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_include_directories(lwallpaper_performance_tests PRIVATE "${LWALLPAPER_CORE_DIR}/src")
target_include_directories(lwallpaper_performance_tests SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/rang")
target_compile_definitions(lwallpaper_performance_tests PRIVATE UNICODE _UNICODE)
target_compile_options(lwallpaper_performance_tests PRIVATE /W4 /utf-8)
target_link_libraries(lwallpaper_performance_tests PRIVATE lwallpaper_dependencies
    d3d11 dxgi d3dcompiler dwmapi shcore winmm)
add_dependencies(lwallpaper_performance_tests lwallpaper)

add_executable(lwallpaper_import_worker_tests import_worker_tests.cpp
    "${PROJECT_SOURCE_DIR}/src/wallpaper/wallpaper_api.hpp")
set_target_properties(lwallpaper_import_worker_tests PROPERTIES
    AUTOUIC OFF AUTORCC OFF RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_include_directories(lwallpaper_import_worker_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_include_directories(lwallpaper_import_worker_tests SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/rang")
target_compile_options(lwallpaper_import_worker_tests PRIVATE /W4 /utf-8)
target_compile_definitions(lwallpaper_import_worker_tests PRIVATE UNICODE _UNICODE)
target_link_libraries(lwallpaper_import_worker_tests PRIVATE Qt6::Core livewallpaper::livewallpaper lwallpaper_dependencies
    d3d11 dxgi d3dcompiler dwmapi shcore winmm)
add_dependencies(lwallpaper_import_worker_tests lwallpaper)

add_test(NAME performance_unit COMMAND lwallpaper_performance_tests --unit)
set_tests_properties(performance_unit PROPERTIES TIMEOUT 15)

find_program(FFMPEG_EXECUTABLE ffmpeg)
find_program(POWERSHELL_EXECUTABLE NAMES pwsh powershell)
if(FFMPEG_EXECUTABLE AND POWERSHELL_EXECUTABLE)
    set(_media "${CMAKE_BINARY_DIR}/test-data/$<CONFIG>")
    add_test(NAME prepare_media COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/prepare_media.ps1"
        -FFmpeg "${FFMPEG_EXECUTABLE}" -Directory "${_media}")
    set_tests_properties(prepare_media PROPERTIES FIXTURES_SETUP media TIMEOUT 60)
    add_test(NAME import_and_decoder COMMAND lwallpaper_performance_tests --media
        "${_media}/h264.mp4" "${_media}/ffv1.mkv" "${_media}")
    add_test(NAME d3d11_gpu_smoke COMMAND lwallpaper_performance_tests --gpu "${_media}/h264.mp4")
    add_test(NAME atomic_import_worker COMMAND lwallpaper_import_worker_tests
        "${_media}/h264.mp4" "${_media}")
    set_tests_properties(import_and_decoder d3d11_gpu_smoke atomic_import_worker PROPERTIES
        FIXTURES_REQUIRED media TIMEOUT 90)
    set_tests_properties(d3d11_gpu_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20)
else()
    message(STATUS "FFmpeg CLI/PowerShell not found: media/GPU integration tests are disabled")
endif()
