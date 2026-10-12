#!/bin/sh
#
# MCP 工具：设置扬声器音量（0-100）。
#
# 与界面走同一路混音器（见 app/config.c）：card 0 的 'DAC LINEOUT Volume'，硬件范围 0-30。
# 界面每次进设置页都重新 amixer 读一次，所以这里改了不会和界面显示打架。
#   QZDESK_MIXER_CARD / QZDESK_MIXER_CONTROL / QZDESK_MIXER_MAX 可覆盖（便于换板/测试）
#
# 入参：{"percent": 60}      也接受 level / value
# 出参：{"status":"ok","volume":60,"raw":18,"max":30}
set -u

json_num() {
	printf '%s' "$1" | sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\(-\{0,1\}[0-9][0-9]*\).*/\1/p" | head -1
}

body=$(cat)
percent=$(json_num "$body" percent)
[ -n "$percent" ] || percent=$(json_num "$body" level)
[ -n "$percent" ] || percent=$(json_num "$body" value)
if [ -z "$percent" ]; then
	printf '缺少 percent：需要 0-100 的整数，例如 {"percent":60}\n' >&2
	exit 1
fi

[ "$percent" -lt 0 ] && percent=0
[ "$percent" -gt 100 ] && percent=100

card=${QZDESK_MIXER_CARD:-0}
control=${QZDESK_MIXER_CONTROL:-DAC LINEOUT Volume}
max=${QZDESK_MIXER_MAX:-30}
case "$max" in
	''|*[!0-9]*) max=30 ;;
esac

command -v amixer >/dev/null 2>&1 || { printf '没有 amixer（缺 alsa-utils）\n' >&2; exit 1; }

raw=$(( (percent * max + 50) / 100 ))
amixer -c "$card" cset name="$control" "$raw" >/dev/null 2>&1 || {
	printf 'amixer 设置失败：card %s / %s / %s\n' "$card" "$control" "$raw" >&2
	exit 1
}

# 回读实际值（amixer cget 的范围在 "type=" 行，当前值在 ": values=" 行，只认后者）
now=$(amixer -c "$card" cget name="$control" 2>/dev/null \
	| sed -n 's/.*: values=\(-\{0,1\}[0-9][0-9]*\).*/\1/p' | head -1)
case "$now" in
	''|*[!0-9]*) now=$raw ;;
esac
level=$(( now * 100 / max ))

printf '{"status":"ok","volume":%s,"raw":%s,"max":%s}\n' "$level" "$now" "$max"
