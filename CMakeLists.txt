cmake_minimum_required(VERSION 3.28)
project(Ulticast VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/utf-8 /W3 /permissive-)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
endif()

set(FFMPEG_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg")
set(FFNVCODEC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nv-codec-headers")

# ---------------------------------------------------------------- CEF (browser overlays)

set(CEF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cef")
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
# The wrapper must use the dynamic CRT (/MD) to match this project and FFmpeg.
set(CEF_RUNTIME_LIBRARY_FLAG "/MD" CACHE STRING "CEF wrapper runtime library" FORCE)
find_package(CEF REQUIRED)
add_subdirectory("${CEF_ROOT}/libcef_dll" "${CMAKE_BINARY_DIR}/libcef_dll")

# ---------------------------------------------------------------- core engine

add_library(ulticast_core STATIC
    src/session.cpp
    src/core/d3d.cpp
    src/core/capture_wgc.cpp
    src/core/capture_dxgi.cpp
    src/core/nvenc_encoder.cpp
    src/core/ffmuxer.cpp
    src/core/wasapi_capture.cpp
    src/core/aac_encoder.cpp
    src/core/audio_mixer.cpp
    src/core/compositor.cpp
    src/core/webcam_mf.cpp
    src/core/replay_buffer.cpp
    src/core/browser_cef.cpp
    src/core/diag.cpp
)

target_include_directories(ulticast_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${FFNVCODEC_DIR}/include"
    "${FFMPEG_DIR}/include"
    "${CEF_ROOT}"
)

target_link_directories(ulticast_core PUBLIC "${FFMPEG_DIR}/lib")

target_link_libraries(ulticast_core PUBLIC
    avformat
    avcodec
    avutil
    swresample
    d3d11
    d3dcompiler
    dxgi
    windowsapp
    mfplat
    mfreadwrite
    mfuuid
    mf
    ole32
    oleaut32
    user32
    version
    libcef_dll_wrapper
    "${CEF_ROOT}/$<CONFIG>/libcef.lib"
    ${CEF_STANDARD_LIBS}
)

# ---------------------------------------------------------------- executables

add_executable(ulticast src/main.cpp)
target_link_libraries(ulticast PRIVATE ulticast_core)

add_executable(ulticast-ui WIN32
    src/ui/ui_main.cpp
    src/ui/ui_app.cpp
    src/ui/settings.cpp
)
target_link_libraries(ulticast-ui PRIVATE
    ulticast_core
    d2d1
    dwrite
    dwmapi
    crypt32
    shell32
    comdlg32
    gdi32
)

foreach (target ulticast ulticast-ui)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FFMPEG_DIR}/bin" "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Copying FFmpeg runtime DLLs next to ${target}"
    )
endforeach()

# CEF runtime (libcef.dll + resources) next to the executables.
add_custom_target(cef_runtime ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CEF_ROOT}/Release" "$<TARGET_FILE_DIR:ulticast>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CEF_ROOT}/Resources" "$<TARGET_FILE_DIR:ulticast>"
    COMMENT "Deploying CEF runtime (libcef.dll, resources)"
)
add_dependencies(cef_runtime ulticast)
