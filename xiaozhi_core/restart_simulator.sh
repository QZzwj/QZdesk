#!/bin/sh
set -eu

flag=${QZDESK_SIMULATOR_RESTART_FLAG:-}
if [ -z "$flag" ]; then
    printf '{"status":"error","message":"当前不是由 QZdesk 模拟器监督运行，不能执行模拟器重启"}\n'
    exit 1
fi

: > "$flag"
printf '{"status":"ok","message":"模拟器将在当前回复结束后重启"}\n'
