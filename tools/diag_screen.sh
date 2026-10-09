#!/usr/bin/env bash
#
# 屏幕白屏（/ 无信号）现场取证：白屏时 adb 还能用，跑这一条就把判定需要的信息全抓下来。
#
#   tools/diag_screen.sh                 抓一份到 /tmp/screen-diag-<时间>.txt
#   tools/diag_screen.sh --tag before    加个标签（对比"插相机前/后""开 adb 前/后"）
#
# 抓完直接看结论那几行：界面进程是否还活着、CMA 用了多少、显示与 USB 的时钟、
# 以及 dmesg 里有没有分配失败 / 显示控制器 / PHY 的报错。
#
# 用法建议（四象限对比，最省事）：
#   1) 不插相机、不插 USB        -> tools/diag_screen.sh --tag cam0-usb0
#   2) 插相机、不插 USB          -> tools/diag_screen.sh --tag cam1-usb0
#   3) 不插相机、插 USB（起 adb）-> tools/diag_screen.sh --tag cam0-usb1
#   4) 插相机、插 USB（白屏）    -> tools/diag_screen.sh --tag cam1-usb1
# 四份一对比就知道是"相机"、"USB/adb"，还是两者叠加（多半是供电）。

set -uo pipefail

ADB=${ADB:-adb}
DEVICE=${QZDESK_DEVICE:-}
TAG=now
[ "${1:-}" = "--tag" ] && TAG=${2:-now}
OUT="/tmp/screen-diag-$TAG.txt"

adb_() { if [ -n "$DEVICE" ]; then "$ADB" -s "$DEVICE" "$@"; else "$ADB" "$@"; fi; }

command -v "$ADB" >/dev/null || { echo "找不到 adb"; exit 1; }
adb_ wait-for-device || { echo "没有设备"; exit 1; }

{
    echo "===== QZdesk 屏幕诊断 [$TAG] $(date -Is) ====="
    echo
    echo "----- ① 界面进程还在吗（在 = 只有显示挂了；不在 = 崩了/被杀了）-----"
    adb_ shell 'echo -n "  qzdesk_screen: "; (pidof qzdesk_screen || echo 未运行)
                echo -n "  核心: "; (pidof xiaozhi_linux_rs || echo 未运行)
                echo -n "  adbd: "; (pidof adbd || echo 未运行)'
    echo
    echo "----- ② 界面日志尾部（崩了这里会露出来）-----"
    adb_ shell 'tail -25 /var/log/qzdesk.log 2>/dev/null || echo "  (没有日志)"'
    echo
    echo "----- ③ 内存与 CMA（相机/显示都从这里分配：这块板子只有 10MB 池）-----"
    adb_ shell 'head -3 /proc/meminfo; echo; \
                mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug 2>/dev/null; \
                for f in /sys/kernel/debug/cma/*/; do \
                    n=$(basename "$f"); \
                    printf "  %-22s used/%-10s total %s\n" "$n" \
                        "$(cat $f/used 2>/dev/null)" "$(cat $f/count 2>/dev/null)"; \
                done 2>/dev/null || echo "  (读不到 cma 信息)"'
    echo
    echo "----- ④ 显示状态（fb 是否还开着、分辨率对不对）-----"
    adb_ shell 'for f in /sys/class/graphics/fb0/name /sys/class/graphics/fb0/virtual_size \
                          /sys/class/graphics/fb0/bits_per_pixel /sys/class/graphics/fb0/blank; do
                    [ -e "$f" ] && { echo -n "  $f = "; cat "$f"; }
                done; echo -n "  backlight = "; cat /sys/class/backlight/*/brightness 2>/dev/null'
    echo
    echo "----- ⑤ 显示 / USB 相关时钟（对比时要看这几个：dclk_vop、dclk_rgb、usb、phy、pll）-----"
    adb_ shell 'cat /sys/kernel/debug/clk/clk_summary 2>/dev/null \
                | grep -iE "dclk|vop|rgb|dsi|usb|phy|cpll|gpll|hpll" | head -30 \
                || echo "  (读不到 clk_summary)"'
    echo
    echo "----- ⑥ dmesg：分配失败 / 显示 / 相机 / USB PHY / I2C 报错 -----"
    adb_ shell 'dmesg 2>/dev/null | grep -iE "cma|ion|alloc.*fail|out of memory|oom|vop|rgb|dclk|panel|rkcif|rkisp|sc3336|i2c.*(timeout|error)|usb|phy|dwc3" | tail -60'
    echo
    echo "----- ⑦ dmesg 最后 40 行（时间顺序，看白屏那一刻发生了什么）-----"
    adb_ shell 'dmesg 2>/dev/null | tail -40'
    echo
    echo "----- ⑧ 汇总提示 -----"
    adb_ shell 'echo "  相机相关内核对象（这套镜像里 rkcif/rkisp 是 =m 且没打模块，正常应该是空的）:";
                ls /sys/bus/platform/drivers/rkcif* /sys/bus/platform/drivers/rkisp* 2>/dev/null || echo "    (无)"
                echo "  USB 控制器绑定情况:"; ls /sys/class/udc/ 2>/dev/null | sed "s/^/    /"'
} | tee "$OUT"

echo
echo "===== 已保存: $OUT（把这个文件发我）====="
