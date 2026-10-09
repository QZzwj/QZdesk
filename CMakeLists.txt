cmake_minimum_required(VERSION 3.12.4)
project(qzdesk_echo_demo C CXX ASM)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON)
set(LV_CONF_BUILD_DISABLE_DEMOS ON)
option(QZDESK_SIMULATOR "Build with the LVGL SDL simulator" OFF)

# LVGL's software renderer is a per-pixel loop, so an unoptimised build (-O0,
# what an empty CMAKE_BUILD_TYPE gives) is several times slower than -O2/-O3 and
# the UI visibly stutters while scrolling. Default to Release; pass
# -DCMAKE_BUILD_TYPE=Debug explicitly when a debug build is wanted.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo MinSizeRel)
endif()
option(QZDESK_BUILD_CORE "Build the bundled QZdesk Rust core with Cargo" ON)
set(QZDESK_CARGO_TARGET "" CACHE STRING "Optional Rust target triple for QZdesk core")

# 面板尺寸的**默认值**（编译期兜底）。界面按 480×320 的设计稿布局（见 include/scale.h），
# 实际尺寸是运行时取的：真机读 /dev/fb0，模拟器读 QZDESK_PANEL=320x240，所以这两个值
# 只在两者都取不到时起作用。320×240 是最小适配基准，较大面板自动放大布局。
set(QZDESK_PANEL_W 320 CACHE STRING "Panel width in pixels (compile-time fallback)")
set(QZDESK_PANEL_H 240 CACHE STRING "Panel height in pixels (compile-time fallback)")

# LVGL and its configuration are vendored inside the project under third_party/.
# They have to stay side by side: lv_conf.h does `#include "conf/dev_conf.h"`,
# so conf/ must be relative to the directory holding lv_conf.h.
set(LVGL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lvgl")
set(LVGL_CONF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
add_subdirectory("${LVGL_DIR}" lvgl)
target_include_directories(lvgl PUBLIC "${LVGL_CONF_DIR}")

if(QZDESK_SIMULATOR)
    find_package(SDL2 REQUIRED)
    target_compile_definitions(lvgl PUBLIC QZDESK_SIMULATOR=1)
endif()

# 面板尺寸传给所有编译单元（theme.h 会按它算缩放系数）。
target_compile_definitions(lvgl PUBLIC
    QZ_SCREEN_W=${QZDESK_PANEL_W}
    QZ_SCREEN_H=${QZDESK_PANEL_H})

add_executable(qzdesk_screen
    app/main.c
    app/scale.c
    app/theme.c
    app/ai_face.c
    app/otto_emoji.c
    app/icon_assets.c
    app/config.c
    app/wifi.c
    app/skills.c
    app/smarthome.c
    app/web_client.c
    app/qzdesk_core.c
    app/qzdesk_core_process.c
    pages/desktop.c
    pages/apps.c
    pages/assistant.c
    pages/settings.c
    pages/applets.c
    pages/skills_page.c
    pages/weather_card.c
    pages/weather_page.c
    pages/performance_page.c
)
target_include_directories(qzdesk_screen PRIVATE include app pages)
target_link_libraries(qzdesk_screen PRIVATE lvgl lvgl::thorvg m pthread)
if(QZDESK_SIMULATOR)
    target_link_libraries(qzdesk_screen PRIVATE SDL2::SDL2)
endif()

# MCP subprocess tools run with the bundled core's directory as their working
# directory. Keep the scripts beside that binary so relative paths in the
# persisted JSON configuration work in both the simulator and on the device.
add_custom_target(qzdesk_mcp_tools ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/xiaozhi_core/system_status.sh
            ${CMAKE_CURRENT_BINARY_DIR}/system_status.sh
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/xiaozhi_core/robot_move.sh
            ${CMAKE_CURRENT_BINARY_DIR}/robot_move.sh
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/xiaozhi_core/set_timer.py
            ${CMAKE_CURRENT_BINARY_DIR}/set_timer.py
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/xiaozhi_core/pomodoro.py
            ${CMAKE_CURRENT_BINARY_DIR}/pomodoro.py
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/xiaozhi_core/restart_simulator.sh
            ${CMAKE_CURRENT_BINARY_DIR}/restart_simulator.sh
    COMMENT "Installing QZdesk MCP tool scripts"
)
add_dependencies(qzdesk_screen qzdesk_mcp_tools)

if(QZDESK_BUILD_CORE)
    find_program(QZDESK_CARGO_EXECUTABLE cargo HINTS "$ENV{HOME}/.cargo/bin")
    if(NOT QZDESK_CARGO_EXECUTABLE)
        message(FATAL_ERROR "Cargo is required to build the bundled QZdesk core")
    endif()

    set(QZDESK_CARGO_ARGS build --locked --release)
    set(QZDESK_CORE_BINARY
        ${CMAKE_CURRENT_SOURCE_DIR}/xiaozhi_core/target/release/xiaozhi-linux-rs)
    if(QZDESK_CARGO_TARGET)
        list(APPEND QZDESK_CARGO_ARGS --target ${QZDESK_CARGO_TARGET})
        set(QZDESK_CORE_BINARY
            ${CMAKE_CURRENT_SOURCE_DIR}/xiaozhi_core/target/${QZDESK_CARGO_TARGET}/release/xiaozhi-linux-rs)
    endif()

    add_custom_target(qzdesk_core_build ALL
        COMMAND ${CMAKE_COMMAND} -E env --unset=MAKEFLAGS --unset=MFLAGS
                ${QZDESK_CARGO_EXECUTABLE} ${QZDESK_CARGO_ARGS}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${QZDESK_CORE_BINARY}
                ${CMAKE_CURRENT_BINARY_DIR}/xiaozhi_linux_rs
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/xiaozhi_core
        COMMENT "Building bundled QZdesk core"
    )
    add_dependencies(qzdesk_screen qzdesk_core_build)
endif()

add_custom_target(qzdesk_all DEPENDS qzdesk_screen)
