#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir=${QZDESK_BUILD_DIR:-"$script_dir/build"}


# The SDL simulator has no ALSA capture/playback devices. Keep the core
# network and GUI paths active without starting failing ALSA threads.
export QZDESK_AUDIO_DISABLED=1

# The simulator can reuse a previously built core. Set QZDESK_BUILD_CORE=ON
# when testing Rust changes; this avoids competing Cargo cache locks during
# normal UI-only simulator runs.
if [[ -z "${QZDESK_BUILD_CORE:-}" &&
      -x "$script_dir/xiaozhi_core/target/release/xiaozhi-linux-rs" ]]; then
    export QZDESK_BUILD_CORE=OFF

    cmake -E copy_if_different \
        "$script_dir/xiaozhi_core/target/release/xiaozhi-linux-rs" \
        "$build_dir/xiaozhi_linux_rs" >/dev/null 2>&1 || true
    printf '模拟器复用已有 Rust 核心；需要重编译时设置 QZDESK_BUILD_CORE=ON\n'
fi

# Cloud TTS remains the default and is used as the audio fallback.

# QZdesk owns the Skill Manager port. Close only the process listening on
# TCP 8080; the GUI UDP ports are not touched.
if [[ -z "${QZDESK_SKILL_WEB_PORT:-}" && -z "${XIAOZHI_SKILL_WEB_PORT:-}" ]]; then
    export QZDESK_SKILL_WEB_PORT=8080
    export XIAOZHI_SKILL_WEB_PORT=8080
elif [[ -n "${QZDESK_SKILL_WEB_PORT:-}" && -z "${XIAOZHI_SKILL_WEB_PORT:-}" ]]; then
    export XIAOZHI_SKILL_WEB_PORT="$QZDESK_SKILL_WEB_PORT"
fi
release_skill_port() {
    [[ "${QZDESK_REPLACE_PORT_8080:-1}" == "0" ]] && return 0
    command -v fuser >/dev/null 2>&1 || return 0

    local skill_pids
    skill_pids=$(fuser -n tcp 8080 2>/dev/null || true)
    if [[ -n "$skill_pids" ]]; then
        printf '关闭占用 TCP 8080 的进程: %s\n' "$skill_pids"
        kill $skill_pids 2>/dev/null || true
        for _ in {1..30}; do
            fuser -n tcp 8080 >/dev/null 2>&1 || break
            sleep 0.1
        done
    fi
}

stop_previous_qzdesk() {
    local pids
    pids=$(pgrep -f "$build_dir/qzdesk_screen$" 2>/dev/null || true)
    if [[ -z "$pids" ]]; then
        return 0
    fi
    printf '关闭已运行的 QZdesk UI: %s\n' "$pids"
    kill $pids 2>/dev/null || true
    for _ in {1..30}; do
        pgrep -f "$build_dir/qzdesk_screen$" >/dev/null 2>&1 || return 0
        sleep 0.1
    done
    # A stale simulator must not keep UDP 5679 from the new instance.
    pids=$(pgrep -f "$build_dir/qzdesk_screen$" 2>/dev/null || true)
    [[ -z "$pids" ]] || kill -KILL $pids 2>/dev/null || true
}

# Clear it once before the build and again after the build. A system OTA
# service can restart while Cargo/CMake is compiling, so the second pass must
# happen immediately before qzdesk_screen starts its bundled core.
release_skill_port
stop_previous_qzdesk

# 面板尺寸可选：界面按 480×320 的设计稿布局，换成同比例的小屏只是换个系数
# （见 include/scale.h）。  例：QZDESK_PANEL=320x240 ./run.sh
panel_args=()
if [[ -n "${QZDESK_PANEL:-}" ]]; then
    panel_args=(-DQZDESK_PANEL_W="${QZDESK_PANEL%x*}" -DQZDESK_PANEL_H="${QZDESK_PANEL#*x}")
    printf '面板尺寸：%s\n' "$QZDESK_PANEL"
fi

cmake -S "$script_dir" -B "$build_dir" \
    -DQZDESK_SIMULATOR=ON \
    "${panel_args[@]}" \
    -DQZDESK_BUILD_CORE="${QZDESK_BUILD_CORE:-ON}"
cmake --build "$build_dir" --target qzdesk_screen -j"${QZDESK_JOBS:-2}"

release_skill_port
stop_previous_qzdesk

# Keep the simulator under a small supervisor so the MCP "restart device"
# action can restart the UI/core pair without rebooting the host.
restart_flag="$build_dir/.qzdesk-restart"
while :; do
    rm -f "$restart_flag"
    QZDESK_SIMULATOR_RESTART_FLAG="$restart_flag" "$build_dir/qzdesk_screen"
    if [[ ! -f "$restart_flag" ]]; then
        exit 0
    fi
    printf '模拟器收到重启请求，正在重新启动 UI 和核心...\n'
done
