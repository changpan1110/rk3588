cmake_minimum_required(VERSION 3.16)
project(rk3588_video_app C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(_rk_media_default "${CMAKE_SOURCE_DIR}/install")
if(DEFINED ENV{RK_FFMPEG_PREFIX} AND NOT "$ENV{RK_FFMPEG_PREFIX}" STREQUAL "")
    set(_rk_media_default "$ENV{RK_FFMPEG_PREFIX}")
endif()

set(RK_MEDIA_PREFIX
    "${_rk_media_default}"
    CACHE PATH
    "FFmpeg, MPP and RGA installation prefix"
)
get_filename_component(RK_MEDIA_PREFIX
    "${RK_MEDIA_PREFIX}"
    ABSOLUTE
    BASE_DIR "${CMAKE_SOURCE_DIR}"
)

set(RK_MEDIA_INCLUDE_DIR "${RK_MEDIA_PREFIX}/include")
set(RK_MEDIA_LIB_DIR "${RK_MEDIA_PREFIX}/lib")
set(RK_MEDIA_PKGCONFIG_DIR "${RK_MEDIA_LIB_DIR}/pkgconfig")

if(NOT EXISTS "${RK_MEDIA_PKGCONFIG_DIR}/libavcodec.pc")
    message(FATAL_ERROR
        "Rockchip FFmpeg was not found under ${RK_MEDIA_PREFIX}. "
        "Set -DRK_MEDIA_PREFIX=/path/to/install or RK_FFMPEG_PREFIX."
    )
endif()

message(STATUS "RK media prefix: ${RK_MEDIA_PREFIX}")

list(PREPEND CMAKE_PREFIX_PATH "${RK_MEDIA_PREFIX}")
set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH ON)
list(APPEND CMAKE_BUILD_RPATH "${RK_MEDIA_LIB_DIR}")
list(APPEND CMAKE_INSTALL_RPATH "${RK_MEDIA_LIB_DIR}")

if(CMAKE_HOST_WIN32)
    set(_pkg_config_path_separator ";")
else()
    set(_pkg_config_path_separator ":")
endif()

if(DEFINED ENV{PKG_CONFIG_PATH} AND NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
    set(ENV{PKG_CONFIG_PATH}
        "${RK_MEDIA_PKGCONFIG_DIR}${_pkg_config_path_separator}$ENV{PKG_CONFIG_PATH}"
    )
else()
    set(ENV{PKG_CONFIG_PATH} "${RK_MEDIA_PKGCONFIG_DIR}")
endif()

function(find_shared_lib out_var base_name)
    set(_candidates
        "/usr/lib/aarch64-linux-gnu/lib${base_name}.so"
        "/usr/lib/aarch64-linux-gnu/lib${base_name}.so.2"
        "/usr/lib/aarch64-linux-gnu/lib${base_name}.so.1"
        "/lib/aarch64-linux-gnu/lib${base_name}.so"
        "/lib/aarch64-linux-gnu/lib${base_name}.so.2"
        "/lib/aarch64-linux-gnu/lib${base_name}.so.1"
    )

    foreach(_cand IN LISTS _candidates)
        if(EXISTS "${_cand}")
            set(${out_var} "${_cand}" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    find_library(_fallback
        NAMES ${base_name}
        PATHS /usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu
        NO_DEFAULT_PATH
    )
    set(${out_var} "${_fallback}" PARENT_SCOPE)
endfunction()

find_package(PkgConfig REQUIRED)

foreach(_rk_media_package libavcodec rockchip_mpp librga)
    execute_process(
        COMMAND "${PKG_CONFIG_EXECUTABLE}" --variable=prefix "${_rk_media_package}"
        RESULT_VARIABLE _rk_media_pkg_result
        OUTPUT_VARIABLE _rk_media_pkg_prefix
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    if(NOT _rk_media_pkg_result EQUAL 0)
        message(FATAL_ERROR
            "pkg-config package ${_rk_media_package} was not found under "
            "${RK_MEDIA_PKGCONFIG_DIR}."
        )
    endif()

    get_filename_component(_rk_media_pkg_prefix
        "${_rk_media_pkg_prefix}"
        ABSOLUTE
    )
    if(NOT "${_rk_media_pkg_prefix}" STREQUAL "${RK_MEDIA_PREFIX}")
        message(FATAL_ERROR
            "${_rk_media_package}.pc still points to ${_rk_media_pkg_prefix}, "
            "but RK_MEDIA_PREFIX is ${RK_MEDIA_PREFIX}. Reinstall the media "
            "libraries with the new prefix or update the copied .pc files."
        )
    endif()
endforeach()

pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavdevice
    libavcodec
    libavformat
    libavfilter
    libavutil
    libswscale
)
set(FFMPEG_LINK_TARGET PkgConfig::FFMPEG)

pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
set(FREETYPE_LINK_TARGET PkgConfig::FREETYPE)

find_path(SDL2_INCLUDE_DIR
    NAMES SDL2/SDL.h
    PATHS
        /usr/include
        /usr/local/include
)
find_shared_lib(SDL2_LIB SDL2)

find_path(RGA_INCLUDE_DIR
    NAMES rga/RgaApi.h
    PATHS "${RK_MEDIA_INCLUDE_DIR}"
    NO_DEFAULT_PATH
)
find_library(RGA_LIB
    NAMES rga
    PATHS "${RK_MEDIA_LIB_DIR}"
    NO_DEFAULT_PATH
)

if(RGA_INCLUDE_DIR AND RGA_LIB)
    set(HAVE_LIBRGA ON)
    message(STATUS "librga found: ${RGA_LIB} - RGA hardware scale/convert enabled")
else()
    set(HAVE_LIBRGA OFF)
    message(STATUS "librga not found - scale/convert falls back to swscale")
endif()

file(GLOB RK_COMMON_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/*.c"
)
file(GLOB RK_INPUT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/input/*.c"
)
file(GLOB RK_VIDEO_PIPELINE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/video/*.c"
)

set(RK_OUTPUT_CONVERT_SOURCES
    src/output/output_encode_common.c
    src/output/output_encode_rkrga_filter.c
    src/output/output_osd_rkrga.c
)
list(REMOVE_ITEM RK_VIDEO_PIPELINE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/video/video_pipeline_main.c"
)

file(GLOB RK3588_VIDEO_APP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/input/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/process/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/output/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/serial/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.c"
)
list(REMOVE_ITEM RK3588_VIDEO_APP_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/sbus_rc8_command.c"
)
list(SORT RK_COMMON_SOURCES)
list(SORT RK_INPUT_SOURCES)
list(SORT RK_VIDEO_PIPELINE_SOURCES)
list(SORT RK3588_VIDEO_APP_SOURCES)

add_executable(rk3588_video_app ${RK3588_VIDEO_APP_SOURCES})

add_executable(rk3588_gui_smoke_test
    src/tools/gui_smoke_test.c
    ${RK_COMMON_SOURCES}
    src/output/output_display_gui.c
)

add_executable(rk3588_usb_mjpeg_hw_preview
    src/tools/usb_mjpeg_hw_preview.c
    ${RK_COMMON_SOURCES}
    src/output/output_display_gui.c
)

add_executable(rk3588_preview_3cam_gui
    src/tools/preview_3cam_gui.c
    ${RK_COMMON_SOURCES}
    ${RK_INPUT_SOURCES}
)

add_executable(rk3588_hdmi_color_compare
    src/tools/hdmi_color_compare.c
    ${RK_COMMON_SOURCES}
    ${RK_INPUT_SOURCES}
    ${RK_OUTPUT_CONVERT_SOURCES}
)

add_executable(rk3588_rga_color_probe
    src/tools/rga_color_probe.c
    ${RK_COMMON_SOURCES}
    ${RK_OUTPUT_CONVERT_SOURCES}
)

add_executable(rk3588_pipeline_switch_test
    src/tools/pipeline_switch_test.c
    ${RK_COMMON_SOURCES}
    ${RK_INPUT_SOURCES}
    ${RK_OUTPUT_CONVERT_SOURCES}
)

add_executable(rk3588_hw_encode_test
    src/tools/hw_encode_test.c
    ${RK_COMMON_SOURCES}
    ${RK_INPUT_SOURCES}
    ${RK_OUTPUT_CONVERT_SOURCES}
)

add_executable(rk3588_mp4_encode_test
    src/tools/mp4_encode_test.c
    ${RK_COMMON_SOURCES}
    ${RK_INPUT_SOURCES}
    ${RK_OUTPUT_CONVERT_SOURCES}
    src/output/output_recorder.c
)

add_executable(rk3588_video_pipeline_main
    src/video/video_pipeline_main.c
    src/app/sbus_rc8_command.c
    src/serial/sbus.c
    src/serial/sbus_rc8.c
    ${RK_COMMON_SOURCES}
    ${RK_INPUT_SOURCES}
    ${RK_OUTPUT_CONVERT_SOURCES}
    src/output/output_recorder.c
    ${RK_VIDEO_PIPELINE_SOURCES}
)

add_executable(rk3588_sbus_test
    src/tools/sbus_test.c
    src/serial/sbus.c
)
target_include_directories(rk3588_sbus_test PRIVATE include)

add_executable(rk3588_v4l2_capture_bench
    src/tools/v4l2_capture_bench.c
)

enable_testing()
add_test(NAME rk3588_sbus_parser_test COMMAND rk3588_sbus_test)

set(RK3588_ALL_TARGETS
    rk3588_video_app
    rk3588_gui_smoke_test
    rk3588_usb_mjpeg_hw_preview
    rk3588_preview_3cam_gui
    rk3588_hdmi_color_compare
    rk3588_rga_color_probe
    rk3588_pipeline_switch_test
    rk3588_hw_encode_test
    rk3588_mp4_encode_test
    rk3588_video_pipeline_main
)

foreach(_target IN LISTS RK3588_ALL_TARGETS)
    target_include_directories(${_target} PRIVATE include)
    target_link_libraries(${_target} PRIVATE
        ${FFMPEG_LINK_TARGET}
        ${FREETYPE_LINK_TARGET}
        pthread
    )
endforeach()

set(RK3588_AVDEVICE_TARGETS
    rk3588_video_app
    rk3588_usb_mjpeg_hw_preview
    rk3588_preview_3cam_gui
    rk3588_hdmi_color_compare
    rk3588_pipeline_switch_test
    rk3588_hw_encode_test
    rk3588_mp4_encode_test
    rk3588_video_pipeline_main
)
foreach(_target IN LISTS RK3588_AVDEVICE_TARGETS)
    target_compile_definitions(${_target} PRIVATE HAVE_LIBAVDEVICE=1)
endforeach()

if(SDL2_LIB)
    foreach(_target IN LISTS RK3588_ALL_TARGETS)
        if(SDL2_INCLUDE_DIR)
            target_include_directories(${_target} PRIVATE ${SDL2_INCLUDE_DIR})
        endif()
        target_link_libraries(${_target} PRIVATE ${SDL2_LIB})
    endforeach()
else()
    message(FATAL_ERROR "SDL2 shared library not found. Install libsdl2-dev.")
endif()

if(HAVE_LIBRGA)
    foreach(_rga_target
            rk3588_video_app
            rk3588_hdmi_color_compare
            rk3588_rga_color_probe
            rk3588_pipeline_switch_test
            rk3588_hw_encode_test
            rk3588_mp4_encode_test
            rk3588_video_pipeline_main)
        target_compile_definitions(${_rga_target} PRIVATE HAVE_LIBRGA=1)
        target_include_directories(${_rga_target} PRIVATE ${RGA_INCLUDE_DIR})
        target_link_libraries(${_rga_target} PRIVATE ${RGA_LIB})
    endforeach()
endif()
