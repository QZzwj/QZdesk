#!/bin/sh
#
# MCP 工具：设置屏幕亮度（1-100）。
#
# 直接写背光 sysfs，和界面共用同一套约定（见 app/config.c 的 backlight_path）：
#   QZDESK_BACKLIGHT   覆盖背光节点，默认 /sys/class/backlight/backlight/brightness
# 界面每次进设置页都重新读硬件，所以这里改了不会和界面显示打架（反向也一样）。
#
# 入参：{"percent": 60}      也接受 level / value / brightness
# 出参：{"status":"ok","brightness":60,"raw":153,"max":255}
#
# 下限取 1 而不是 0：全黑的屏看起来跟死机一样，界面里也是这么兜的（config.c 注释）。
set -u

json_num() {
	printf '%s' "$1" | sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\(-\{0,1\}[0-9][0-9]*\).*/\1/p" | head -1
}

body=$(cat)
percent=$(json_num "$body" percent)
[ -n "$percent" ] || percent=$(json_num "$body" level)
[ -n "$percent" ] || percent=$(json_num "$body" value)
[ -n "$percent" ] || percent=$(json_num "$body" brightness)
if [ -z "$percent" ]; then
	printf '缺少 percent：需要 1-100 的整数，例如 {"percent":60}\n' >&2
	exit 1
fi

[ "$percent" -lt 1 ] && percent=1
[ "$percent" -gt 100 ] && percent=100

node=${QZDESK_BACKLIGHT:-/sys/class/backlight/backlight/brightness}
if [ ! -w "$node" ]; then
	printf '背光节点不可写：%s（可用 QZDESK_BACKLIGHT 指定）\n' "$node" >&2
	exit 1
fi

max=$(cat "${node%/*}/max_brightness" 2>/dev/null)
case "$max" in
	''|*[!0-9]*) max=255 ;;
esac

raw=$(( (percent * max + 50) / 100 ))
[ "$raw" -lt 1 ] && raw=1

printf '%s' "$raw" > "$node" || { printf '写背光失败：%s\n' "$node" >&2; exit 1; }

# 回读一遍：节点被别的驱动接管、或写进去被忽略时，这里能立刻发现
now=$(cat "$node" 2>/dev/null)
case "$now" in
	''|*[!0-9]*) now=$raw ;;
esac
level=$(( (now * 100 + max / 2) / max ))

printf '{"status":"ok","brightness":%s,"raw":%s,"max":%s}\n' "$level" "$now" "$max"
