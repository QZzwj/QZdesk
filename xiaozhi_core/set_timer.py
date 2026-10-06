#!/usr/bin/env python3
"""MCP 工具：为 QZdesk 桌面助手新建定时提醒。

调用方式（与 xiaozhi_config.json 中的 set_timer 工具对应）：
    executable = "python3"
    args       = ["./set_timer.py"]

输入（stdin，JSON）：
    {"hour": 7, "minute": 30, "label": "喝水", "daily": true}

输出（stdout，JSON）：
    {"status": "ok", "message": "已新建提醒 每天 07:30 喝水"}

提醒文件与 QZdesk 图形界面共用：
    ~/.local/share/qzdesk/timers.json
"""

import json
import os
import sys
import tempfile
from datetime import date

def default_dir():
    """与核心（src/user_data.rs）同一套规则：设备上优先 /userdata/qzdesk。

    核心从 0.2 起自己就是提醒的权威（`/api/timers`），这个脚本只是备用入口；
    目录对齐了，脚本与界面读写的才是同一个文件。
    """
    device = "/userdata/qzdesk"
    try:
        os.makedirs(device, exist_ok=True)
        return device
    except OSError:
        return os.path.join(os.path.expanduser("~"), ".local", "share", "qzdesk")


TIMER_DIR = os.environ.get(
    "QZDESK_TIMER_DIR",
    os.environ.get("DESKBOT_TIMER_DIR", default_dir()),
)
TIMER_FILE = os.path.join(TIMER_DIR, "timers.json")
MAX_TIMERS = 8


def load_timers():
    try:
        with open(TIMER_FILE, "r", encoding="utf-8") as fp:
            data = json.load(fp)
    except (OSError, ValueError):
        return []
    timers = data.get("timers") if isinstance(data, dict) else None
    return timers if isinstance(timers, list) else []


def save_timers(timers):
    os.makedirs(TIMER_DIR, exist_ok=True)
    # 先写临时文件再原子替换，避免界面读到只写了一半的 JSON
    fd, tmp_path = tempfile.mkstemp(dir=TIMER_DIR, prefix=".timers-", suffix=".tmp")
    with os.fdopen(fd, "w", encoding="utf-8") as fp:
        json.dump({"timers": timers}, fp, ensure_ascii=False, indent=2)
        fp.flush()
        os.fsync(fp.fileno())
    os.replace(tmp_path, TIMER_FILE)


def reply(payload):
    print(json.dumps(payload, ensure_ascii=False))
    return 0


def main():
    raw = sys.stdin.read().strip()
    try:
        params = json.loads(raw) if raw else {}
    except ValueError as exc:
        return reply({"status": "error", "message": "参数不是合法 JSON: %s" % exc})
    if not isinstance(params, dict):
        return reply({"status": "error", "message": "参数必须是 JSON 对象"})

    try:
        hour = int(params.get("hour"))
        minute = int(params.get("minute"))
    except (TypeError, ValueError):
        return reply({"status": "error", "message": "hour 和 minute 必须是整数"})

    if not (0 <= hour <= 23) or not (0 <= minute <= 59):
        return reply({"status": "error", "message": "时间超出范围，hour 0-23，minute 0-59"})

    label = str(params.get("label") or "提醒").strip() or "提醒"
    daily = bool(params.get("daily", True))
    one_shot_date = "" if daily else str(params.get("date") or "").strip()

    timers = load_timers()
    for item in timers:
        if (item.get("hour") == hour and item.get("minute") == minute
                and item.get("label") == label and bool(item.get("daily", True)) == daily):
            return reply({
                "status": "exists",
                "message": "已存在 %02d:%02d %s 的提醒" % (hour, minute, label),
            })

    if len(timers) >= MAX_TIMERS:
        return reply({"status": "error", "message": "提醒数量已达上限 %d 条" % MAX_TIMERS})

    timers.append({
        "hour": hour,
        "minute": minute,
        "label": label,
        "daily": daily,
        "date": one_shot_date,
        # 每天重复的提醒把 last_date 记为今天，避免"刚建好就立刻触发"
        "last_date": date.today().isoformat() if daily else "",
        "enabled": True,
    })
    save_timers(timers)

    repeat = "每天" if daily else "仅一次"
    return reply({
        "status": "ok",
        "message": "已新建提醒 %s %02d:%02d %s" % (repeat, hour, minute, label),
    })


if __name__ == "__main__":
    sys.exit(main())
