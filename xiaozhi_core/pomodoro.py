#!/usr/bin/env python3
"""MCP 工具：控制 QZdesk 桌面助手的番茄钟。

调用方式（与 xiaozhi_config.json 中的 pomodoro 工具对应）：
    executable = "python3"
    args       = ["./pomodoro.py"]

输入（stdin，JSON）：
    {"action": "start", "focus_minutes": 25, "break_minutes": 5, "cycles": 4}
    {"action": "pause"} / {"action": "resume"} / {"action": "stop"} / {"action": "status"}

输出（stdout，JSON）：
    {"status": "ok", "message": "已开始番茄钟：专注 25 分钟 / 休息 5 分钟 / 共 4 轮"}

交互方式：脚本把命令写入命令文件，QZdesk 图形界面可轮询并消费该文件；查询状态时读取界面写出的状态文件。
"""

import json
import os
import sys
import tempfile
import time

QZDESK_DATA_DIR = os.path.join(os.path.expanduser("~"), ".local", "share", "qzdesk")
POMODORO_DIR = os.environ.get(
    "QZDESK_POMODORO_DIR",
    os.environ.get("DESKBOT_POMODORO_DIR", QZDESK_DATA_DIR),
)
STATE_FILE = os.path.join(POMODORO_DIR, "pomodoro.json")
CMD_FILE = os.path.join(POMODORO_DIR, "pomodoro_cmd.json")

DEFAULT_FOCUS = 25
DEFAULT_BREAK = 5
DEFAULT_CYCLES = 4
MAX_CYCLES = 12

VALID_ACTIONS = ("start", "pause", "resume", "stop", "status")


def write_command(payload):
    os.makedirs(POMODORO_DIR, exist_ok=True)
    # 原子替换，避免界面读到半份文件
    fd, tmp_path = tempfile.mkstemp(dir=POMODORO_DIR, prefix=".pomodoro-", suffix=".tmp")
    with os.fdopen(fd, "w", encoding="utf-8") as fp:
        json.dump(payload, fp, ensure_ascii=False)
        fp.flush()
        os.fsync(fp.fileno())
    os.replace(tmp_path, CMD_FILE)


def write_state(state):
    os.makedirs(POMODORO_DIR, exist_ok=True)
    fd, tmp_path = tempfile.mkstemp(dir=POMODORO_DIR, prefix=".pomodoro-state-", suffix=".tmp")
    with os.fdopen(fd, "w", encoding="utf-8") as fp:
        json.dump(state, fp, ensure_ascii=False)
        fp.flush()
        os.fsync(fp.fileno())
    os.replace(tmp_path, STATE_FILE)


def read_state():
    try:
        with open(STATE_FILE, "r", encoding="utf-8") as fp:
            state = json.load(fp)
    except (OSError, ValueError):
        return None
    return state if isinstance(state, dict) else None


def advance_state(state, now=None):
    """Advance elapsed focus/break time without requiring a GUI worker."""
    if not state.get("active") or state.get("paused"):
        return False

    now = int(now if now is not None else time.time())
    updated_at = int(state.get("updated_at") or now)
    elapsed = max(0, now - updated_at)
    remaining = max(0, int(state.get("remaining_seconds") or 0))
    changed = False

    while remaining > 0 and elapsed >= remaining:
        elapsed -= remaining
        if state.get("phase") == "focus":
            completed = int(state.get("completed_cycles") or 0) + 1
            state["completed_cycles"] = completed
            if completed >= int(state.get("total_cycles") or DEFAULT_CYCLES):
                state["active"] = False
                state["remaining_seconds"] = 0
                state["updated_at"] = now
                return True
            state["phase"] = "break"
            remaining = max(1, int(state.get("break_minutes") or DEFAULT_BREAK) * 60)
        else:
            state["phase"] = "focus"
            remaining = max(1, int(state.get("focus_minutes") or DEFAULT_FOCUS) * 60)
        changed = True

    new_remaining = max(0, remaining - elapsed)
    if new_remaining != int(state.get("remaining_seconds") or 0):
        changed = True
    state["remaining_seconds"] = new_remaining
    state["updated_at"] = now
    return changed


def reply(payload):
    print(json.dumps(payload, ensure_ascii=False))
    return 0


def clamp_int(value, default, low, high):
    try:
        number = int(value)
    except (TypeError, ValueError):
        number = default
    return max(low, min(high, number))


def format_mmss(seconds):
    seconds = max(0, int(seconds))
    return "%02d:%02d" % (seconds // 60, seconds % 60)


def status_text():
    state = read_state()
    if state is None:
        return {"status": "ok", "message": "番茄钟当前未运行"}

    if not state.get("active"):
        completed = int(state.get("completed_cycles") or 0)
        if completed:
            return {"status": "ok",
                    "message": "番茄钟已结束，本次完成 %d 轮专注" % completed}
        return {"status": "ok", "message": "番茄钟当前未运行"}

    if advance_state(state):
        write_state(state)
    remaining = int(state.get("remaining_seconds") or 0)

    phase = "专注" if state.get("phase") == "focus" else "休息"
    if state.get("paused"):
        return {"status": "ok",
                "message": "番茄钟已暂停，%s剩余 %s（第 %s/%s 轮）" % (
                    phase, format_mmss(remaining),
                    state.get("completed_cycles"), state.get("total_cycles"))}
    return {"status": "ok",
            "message": "番茄钟进行中，%s剩余 %s（第 %s/%s 轮）" % (
                phase, format_mmss(remaining),
                state.get("completed_cycles"), state.get("total_cycles"))}


def main():
    raw = sys.stdin.read().strip()
    try:
        params = json.loads(raw) if raw else {}
    except ValueError as exc:
        return reply({"status": "error", "message": "参数不是合法 JSON: %s" % exc})
    if not isinstance(params, dict):
        return reply({"status": "error", "message": "参数必须是 JSON 对象"})

    action = str(params.get("action") or "status").strip().lower()
    if action not in VALID_ACTIONS:
        return reply({"status": "error",
                      "message": "action 只能是 start/pause/resume/stop/status"})

    if action == "status":
        return reply(status_text())

    if action == "start":
        focus_min = clamp_int(params.get("focus_minutes"), DEFAULT_FOCUS, 1, 180)
        break_min = clamp_int(params.get("break_minutes"), DEFAULT_BREAK, 1, 60)
        cycles = clamp_int(params.get("cycles"), DEFAULT_CYCLES, 1, MAX_CYCLES)
        write_command({
            "command": "start",
            "focus_minutes": focus_min,
            "break_minutes": break_min,
            "cycles": cycles,
        })
        write_state({
            "active": True,
            "paused": False,
            "phase": "focus",
            "remaining_seconds": focus_min * 60,
            "focus_minutes": focus_min,
            "break_minutes": break_min,
            "total_cycles": cycles,
            "completed_cycles": 0,
            "updated_at": int(time.time()),
        })
        return reply({"status": "ok",
                      "message": "已开始番茄钟：专注 %d 分钟 / 休息 %d 分钟 / 共 %d 轮"
                                 % (focus_min, break_min, cycles)})

    write_command({"command": action})
    state = read_state()
    if state is not None:
        advance_state(state)
        if action == "pause" and state.get("active"):
            state["paused"] = True
            state["updated_at"] = int(time.time())
        elif action == "resume" and state.get("active"):
            state["paused"] = False
            state["updated_at"] = int(time.time())
        elif action == "stop":
            state["active"] = False
            state["paused"] = False
            state["remaining_seconds"] = 0
            state["updated_at"] = int(time.time())
        write_state(state)
    text = {"pause": "番茄钟已暂停",
            "resume": "番茄钟已继续",
            "stop": "番茄钟已停止"}[action]
    return reply({"status": "ok", "message": text})


if __name__ == "__main__":
    sys.exit(main())
