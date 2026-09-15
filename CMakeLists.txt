cmake_minimum_required(VERSION 3.16)
project(rk3588_video_app C)

# ============================================================
#  路径配置 (直接在此指定, 不用 -D 参数或环境变量)
# ============================================================
set(MAVLINK_C_ROOT  "/home/cat/ffmpeg/c_library_v2")    # mavlink C 头文件目录
set(RK_MEDIA_PREFIX "/home/cat/ffmpeg/install")  # FFmpeg/MPP/RGA 安装前缀
set(ENABLE_MAVLINK_CONTROL ON)                    # 是否启用 mavlink 控制
# ============================================================

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 默认 Release 构建（命令行传 -DCMAKE_BUILD_TYPE=xxx 可覆盖）
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 关闭编译警告输出
add_compile_options(-w)

set(RK_MEDIA_INCLUDE_DIR "${RK_MEDIA_PREFIX}/include")
set(RK_MEDIA_LIB_DIR "${RK_MEDIA_PREFIX}/lib")
set(RK_MEDIA_PKGCONFIG_DIR "${RK_MEDIA_LIB_DIR}/pkgconfig")

if(NOT EXISTS "${RK_MEDIA_PKGCONFIG_DIR}/libavcodec.pc")
    message(FATAL_ERROR
        "Rockchip FFmpeg was not found under ${RK_MEDIA_PREFIX}. "
        "请修改 CMakeLists.txt 顶部的 RK_MEDIA_PREFIX 路径."
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

# SDL2 已禁用（命令行环境，暂不需要 GUI 显示）
# find_path(SDL2_INCLUDE_DIR
#     NAMES SDL2/SDL.h
#     PATHS
#         /usr/include
#         /usr/local/include
# )
# find_shared_lib(SDL2_LIB SDL2)

include_directories(/usr/include/libdrm)
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
file(GLOB RK_INPUT_VIDEO_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/input/video/*.c"
)
set(RK_INPUT_SOURCES ${RK_INPUT_VIDEO_SOURCES})
set(RK_VIDEO_CONVERT_SOURCES
    src/process/video/video_frame_convert.c
    src/process/video/video_rkrga_filter.c
)
set(RK_VIDEO_OSD_SOURCES
    src/process/osd/osd_rkrga.c
)
set(RK_OUTPUT_STREAM_SOURCES
    src/output/stream/rtsp/rtsp_output.c
    src/output/stream/rtp/rtp_output.c
    src/output/stream/srt/srt_output.c
    src/output/stream/udp/udp_output.c
)
set(RK_VIDEO_PIPELINE_SOURCES
    src/process/video/video_pipeline.c
    src/process/video/video_pipeline_input.c
    src/process/video/video_pipeline_queue.c
    src/process/video/video_pipeline_util.c
    src/process/codec/video_pipeline_encode.c
    src/process/osd/video_pipeline_osd.c
    src/output/file/mp4/video_pipeline_record.c
    src/output/stream/video_pipeline_stream.c
    src/control/video_pipeline/video_pipeline_control.c
)

add_library(rk3588_dji_rsdk STATIC
    src/common/debug.c
    src/input/can/can_socket.c
    src/control/dji_rsdk/dji_rsdk_crc16_adapter.c
    src/control/dji_rsdk/dji_rsdk_crc32_adapter.c
    src/control/dji_rsdk/dji_rsdk_crc.c
    src/control/dji_rsdk/dji_rsdk_protocol.c
    src/control/dji_rsdk/dji_rsdk_commands.c
)
target_include_directories(rk3588_dji_rsdk PUBLIC include)
target_include_directories(rk3588_dji_rsdk PRIVATE
    include/control/dji_rsdk
)
target_link_libraries(rk3588_dji_rsdk PUBLIC pthread)

set(RK3588_VIDEO_APP_SOURCES
    src/main.c
    src/app/app_manager.c
    ${RK_COMMON_SOURCES}
    ${RK_INPUT_SOURCES}
    src/process/video/process_common.c
    src/output/display/display_gui.c
    ${RK_VIDEO_CONVERT_SOURCES}
)
list(SORT RK_COMMON_SOURCES)
list(SORT RK_INPUT_SOURCES)
list(SORT RK_VIDEO_PIPELINE_SOURCES)
list(SORT RK3588_VIDEO_APP_SOURCES)

# add_executable(rk3588_video_app ${RK3588_VIDEO_APP_SOURCES})  # 依赖 SDL2

# add_executable(rk3588_gui_smoke_test  # 依赖 SDL2
#     src/tools/gui_smoke_test.c
#     ${RK_COMMON_SOURCES}
#     src/output/display/display_gui.c
# )

# add_executable(rk3588_usb_mjpeg_hw_preview  # 依赖 SDL2
#     src/tools/usb_mjpeg_hw_preview.c
#     ${RK_COMMON_SOURCES}
#     src/output/display/display_gui.c
# )

# add_executable(rk3588_preview_3cam_gui  # 依赖 SDL2
#     src/tools/preview_3cam_gui.c
#     ${RK_COMMON_SOURCES}
#     ${RK_INPUT_SOURCES}
# )

# add_executable(rk3588_hdmi_color_compare  # 依赖 SDL2
#     src/tools/hdmi_color_compare.c
#     ${RK_COMMON_SOURCES}
#     ${RK_INPUT_SOURCES}
#     ${RK_VIDEO_CONVERT_SOURCES}
# )

add_executable(rk3588_video_pipeline_main
    src/app/video_pipeline_main.c
    src/app/video_pipeline_config.c
    src/app/sbus_rc8_command.c
    src/input/serial/uart_base.c
    src/control/sbus/sbus.c
    src/control/sbus/sbus_rc8.c
    src/control/high_speed_camera/high_speed_camera.c
    src/control/sfl0603_laser/sfl0603_laser.c
    src/control/thermal_camera/thermal_camera.c
    src/control/visca_camera/visca_camera.c
    src/control/visca_4k_camera/visca_4k_camera.c
    src/service/dji_rsdk_service.c
    src/service/high_speed_camera_service.c
    src/service/sfl0603_laser_service.c
    src/service/thermal_camera_service.c
    src/service/motor_service.c
    src/control/modbus/modbus_rtu.c
    src/control/modbus/agile_modbus/agile_modbus.c
    src/control/modbus/agile_modbus/agile_modbus_rtu.c
    src/control/modbus/agile_modbus/agile_modbus_tcp.c
    src/control/modbus/agile_modbus/agile_modbus_slave_util.c
    ${RK_COMMON_SOURCES}
    ${RK_INPUT_SOURCES}
    ${RK_VIDEO_CONVERT_SOURCES}
    ${RK_VIDEO_OSD_SOURCES}
    ${RK_OUTPUT_STREAM_SOURCES}
    src/output/file/mp4/mp4_recorder.c
    ${RK_VIDEO_PIPELINE_SOURCES}
)
target_include_directories(rk3588_video_pipeline_main PRIVATE
    src/process/video
    src/control/modbus
)
target_compile_definitions(rk3588_video_pipeline_main PRIVATE
    DJI_RSDK_OSD_SERVICE=1
)
target_link_libraries(rk3588_video_pipeline_main PRIVATE
    m
    rk3588_dji_rsdk
)

if(ENABLE_MAVLINK_CONTROL)
    if(NOT EXISTS "${MAVLINK_C_ROOT}/mavlink/common/mavlink.h")
        message(FATAL_ERROR
            "MAVLink C headers were not found at ${MAVLINK_C_ROOT}. "
            "请修改 CMakeLists.txt 顶部的 MAVLINK_C_ROOT 路径, 或设 ENABLE_MAVLINK_CONTROL=OFF."
        )
    endif()
    target_sources(rk3588_video_pipeline_main PRIVATE
        src/input/network/network_endpoint.c
        src/control/mavlink/mavlink_web_control.c
        src/control/mavlink/mavlink_control_actions.c
        src/control/mavlink/mavlink_control_config.c
        src/control/mavlink/mavlink_control_service.c
        src/control/mavlink/mavlink_transport.c
        src/control/mavlink/mavlink_transport_udp.c
        src/control/mavlink/mavlink_transport_tcp.c
        src/control/mavlink/mavlink_transport_serial.c
    )
    target_include_directories(rk3588_video_pipeline_main PRIVATE
        "${MAVLINK_C_ROOT}"
    )
    target_compile_definitions(rk3588_video_pipeline_main PRIVATE
        HAVE_MAVLINK_CONTROL=1
    )
    target_link_libraries(rk3588_video_pipeline_main PRIVATE m)
endif()

# SBUS 在本板上已整体注释掉（sbus_test.c / sbus.c 均无实现），禁用对应测试目标。
# add_executable(rk3588_sbus_test
#     src/tools/sbus_test.c
#     src/input/serial/uart_base.c
#     src/control/sbus/sbus.c
# )
# target_include_directories(rk3588_sbus_test PRIVATE include)
set(RK3588_ALL_TARGETS
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
    rk3588_video_pipeline_main
)
foreach(_target IN LISTS RK3588_AVDEVICE_TARGETS)
    target_compile_definitions(${_target} PRIVATE HAVE_LIBAVDEVICE=1)
endforeach()

# SDL2 已禁用
# if(SDL2_LIB)
#     foreach(_target IN LISTS RK3588_ALL_TARGETS)
#         if(SDL2_INCLUDE_DIR)
#             target_include_directories(${_target} PRIVATE ${SDL2_INCLUDE_DIR})
#         endif()
#         target_link_libraries(${_target} PRIVATE ${SDL2_LIB})
#     endforeach()
# else()
#     message(FATAL_ERROR "SDL2 shared library not found. Install libsdl2-dev.")
# endif()

if(HAVE_LIBRGA)
    foreach(_rga_target
            rk3588_video_pipeline_main)
        target_compile_definitions(${_rga_target} PRIVATE HAVE_LIBRGA=1)
        target_include_directories(${_rga_target} PRIVATE ${RGA_INCLUDE_DIR})
        target_link_libraries(${_rga_target} PRIVATE ${RGA_LIB})
    endforeach()
endif()
