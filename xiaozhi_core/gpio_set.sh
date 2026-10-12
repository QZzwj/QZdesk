#!/bin/sh
#
# MCP 工具：通用 GPIO 输出（继电器、灯、风扇、舵机使能这类只有开/关的外设）。
#
# 与 robot_move.sh 同一套 sysfs 写法，区别是引脚由入参给，不用预先写死：
#   {"pin": 32, "value": 1}      pin = 内核全局 GPIO 编号（32 即 GPIO1_A0）
#   value 认 0/1，也认 on/off/high/low/true/false
# 出参：{"status":"ok","pin":32,"value":1}
#
# 只做输出；读输入（按钮/门磁）用不上这层，留着以后按需加。
set -u

json_str() {
	printf '%s' "$1" | sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" | head -1
}

json_num() {
	printf '%s' "$1" | sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\(-\{0,1\}[0-9][0-9]*\).*/\1/p" | head -1
}

body=$(cat)
pin=$(json_num "$body" pin)
case "$pin" in
	''|*[!0-9]*) printf '缺少 pin：需要内核全局 GPIO 编号，例如 {"pin":32,"value":1}\n' >&2; exit 1 ;;
esac

value=$(json_num "$body" value)
[ -n "$value" ] || value=$(json_str "$body" value)
case "$value" in
	1|on|ON|high|HIGH|true) value=1 ;;
	0|off|OFF|low|LOW|false) value=0 ;;
	*) printf 'value 只能是 0/1（也认 on/off/high/low），收到：%s\n' "${value:-空}" >&2; exit 1 ;;
esac

dir=/sys/class/gpio/gpio$pin
if [ ! -d "$dir" ]; then
	printf '%s' "$pin" > /sys/class/gpio/export 2>/dev/null || true
fi
[ -d "$dir" ] || { printf '导出 GPIO %s 失败（引脚号不对或被别的驱动占用）\n' "$pin" >&2; exit 1; }

printf 'out' > "$dir/direction" 2>/dev/null || true
printf '%s' "$value" > "$dir/value" || { printf '写 GPIO %s 失败\n' "$pin" >&2; exit 1; }

# 回读确认（导出成功但引脚被复用的情况）
now=$(cat "$dir/value" 2>/dev/null || printf '%s' "$value")
case "$now" in ''|*[!0-9]*) now=$value ;; esac

printf '{"status":"ok","pin":%s,"value":%s}\n' "$pin" "$now"
