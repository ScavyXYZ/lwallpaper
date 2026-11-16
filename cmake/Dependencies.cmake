# Keep third-party headers and import libraries separate from application sources.
set(FFMPEG_ROOT "${PROJECT_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH "FFmpeg development package")
set(SDL3_ROOT "${PROJECT_SOURCE_DIR}/third_party/SDL3" CACHE PATH "SDL3 Visual C++ development package")

foreach(_header IN ITEMS "${FFMPEG_ROOT}/include/libavcodec/avcodec.h" "${SDL3_ROOT}/include/SDL3/SDL.h")
    if(NOT EXISTS "${_header}")
        message(FATAL_ERROR "Missing ${_header}. See third_party/README.md.")
    endif()
endforeach()

add_library(lwallpaper_dependencies INTERFACE)
target_include_directories(lwallpaper_dependencies SYSTEM INTERFACE
    "${FFMPEG_ROOT}/include" "${SDL3_ROOT}/include")

set(LWALLPAPER_RUNTIME_DLLS "")
foreach(_component IN ITEMS avcodec avdevice avfilter avformat avutil swresample swscale)
    find_library(FFMPEG_${_component}_LIBRARY NAMES ${_component}
        PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(lwallpaper_dependencies INTERFACE "${FFMPEG_${_component}_LIBRARY}")
    file(GLOB _dlls "${FFMPEG_ROOT}/bin/${_component}-*.dll")
    if(NOT _dlls)
        message(FATAL_ERROR "Missing ${_component} runtime DLL under ${FFMPEG_ROOT}/bin")
    endif()
    list(APPEND LWALLPAPER_RUNTIME_DLLS ${_dlls})
endforeach()

find_library(SDL3_LIBRARY NAMES SDL3 PATHS "${SDL3_ROOT}/lib/x64" NO_DEFAULT_PATH REQUIRED)
target_link_libraries(lwallpaper_dependencies INTERFACE "${SDL3_LIBRARY}")
if(NOT EXISTS "${SDL3_ROOT}/lib/x64/SDL3.dll")
    message(FATAL_ERROR "Missing SDL3 x64 runtime. See third_party/README.md.")
endif()
list(APPEND LWALLPAPER_RUNTIME_DLLS "${SDL3_ROOT}/lib/x64/SDL3.dll")
