#!/bin/sh
#
# MCP 工具：重启 / 关机 / 只重启界面。
#
#   {"action":"reboot"}       重启整机
#   {"action":"poweroff"}     关机
#   {"action":"restart_gui"}  只重启界面与核心（不重启系统；界面卡住、改完配置时用）
#
# 为什么要延迟：先让 MCP 的回执发出去（用户能听到"好的，正在重启"），delay 秒后再
# 真的断电。否则回执还没送出，进程已经被自己干掉了。delay 默认 2 秒、上限 30 秒。
#
# 注意 restart_gui 会把核心自己也杀掉（界面重新拉起一个新的），所以动作一律放在
# 后台子进程里延迟执行，保证 stdout 先被上层读走。
set -u

json_str() {
	printf '%s' "$1" | sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" | head -1
}

json_num() {
	printf '%s' "$1" | sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\(-\{0,1\}[0-9][0-9]*\).*/\1/p" | head -1
}

body=$(cat)
action=$(json_str "$body" action)
[ -n "$action" ] || action=$(json_str "$body" command)

delay=$(json_num "$body" delay)
case "$delay" in
	''|*[!0-9]*) delay=2 ;;
esac
[ "$delay" -gt 30 ] && delay=30

case "$action" in
	reboot|reboot_device|restart_device)
		printf '{"status":"ok","action":"reboot","delay":%s}\n' "$delay"
		( sleep "$delay"; sync; reboot ) >/dev/null 2>&1 &
		exit 0
		;;
	poweroff|shutdown|power_off)
		printf '{"status":"ok","action":"poweroff","delay":%s}\n' "$delay"
		( sleep "$delay"; sync; poweroff ) >/dev/null 2>&1 &
		exit 0
		;;
	restart_gui|restart_ui|restart_screen|restart_desktop)
		init=/etc/init.d/S30qzdesk
		[ -x "$init" ] || init=/oem/usr/etc/init.d/S30qzdesk
		printf '{"status":"ok","action":"restart_gui","delay":%s}\n' "$delay"
		# 先杀掉核心（也就是本工具的上层）再重启，否则旧核心会占着 :8080 不放
		( sleep "$delay"; killall qzdesk_screen xiaozhi_linux_rs 2>/dev/null; \
		  sleep 1; \
		  if [ -x "$init" ]; then "$init" restart >/dev/null 2>&1; fi ) >/dev/null 2>&1 &
		exit 0
		;;
esac

printf 'action 只能是 reboot（重启整机）/ poweroff（关机）/ restart_gui（只重启界面），收到：%s\n' \
	"${action:-空}" >&2
exit 1
