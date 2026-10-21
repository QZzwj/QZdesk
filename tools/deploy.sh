#!/usr/bin/env bash
#
# QZdesk 界面 / 核心：用 adb 直接推上设备，省掉每次打包 update.img 再烧录。
#
# 设备上的落点（与 SDK/project/app/qzdesk/Makefile 的安装规则一一对应）：
#   SDK .../out/bin/qzdesk_screen      -> /oem/usr/bin/qzdesk_screen     界面
#   SDK .../out/bin/xiaozhi_linux_rs   -> /oem/usr/bin/xiaozhi_linux_rs  核心（界面自己拉起）
#   SDK .../out/usr/share/fonts/*.ttf  -> /oem/usr/share/fonts/          中文字体
#   SDK .../out/etc/init.d/S*qzdesk    -> /oem/usr/etc/init.d/           开机自启（RkLunch 跑）
#
# 用法：
#   tools/deploy.sh                 推界面 + 核心，然后重启界面（起效最快）
#   tools/deploy.sh --ui-only       只推界面
#   tools/deploy.sh --core-only     只推核心与唤醒词进程
#   tools/deploy.sh --kws-only      只推唤醒词进程
#   tools/deploy.sh --init-only     只推开机脚本
#   tools/deploy.sh --with-fonts    连中文字体一起推
#   tools/deploy.sh --init-script   连开机脚本一起推（同时更新两份）
#   tools/deploy.sh --s30           设备上把开机脚本从 S99 改名为 S30（提前启动，先试这个）
#   tools/deploy.sh --wifi          推 WiFi 驱动模块 + 固件并加载（镜像里缺这两样，见下）
#   tools/deploy.sh --camera        推相机模块（SC3336/rkcif/rkisp）并加载（含 unready 兜底）
#   tools/deploy.sh --build         先让 SDK 重新交叉编译（QZDESK_SDK_BUILD_CMD）
#   tools/deploy.sh --no-restart    只推，不重启
#   tools/deploy.sh --log [秒]      部署完盯一段设备日志（默认 20 秒）
#   tools/deploy.sh --status        只看设备状态（进程、日志尾部、开机耗时）
#
# 环境变量：
#   QZDESK_SDK             SDK 根目录（默认 <仓库>/../SDK）
#   QZDESK_DEVICE          adb 目标（多设备时用，例如 192.168.1.20:5555）
#   QZDESK_SDK_BUILD_CMD   --build 时执行的命令（默认 "./build.sh app"）
#
# 注意：推上去的必须是交叉编译产物（ARM）。脚本会挡掉 x86 的模拟器二进制 ——
# 那东西推上去只会 "cannot execute binary file"。

set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
SDK=${QZDESK_SDK:-$(CDPATH= cd -- "$repo_dir/../SDK" && pwd)}
if [ -n "${ADB:-}" ]; then
    :
elif [ -x /mnt/d/adb/adb.exe ]; then
    ADB=/mnt/d/adb/adb.exe
else
    ADB=adb
fi
DEVICE=${QZDESK_DEVICE:-}
BUILD_CMD=${QZDESK_SDK_BUILD_CMD:-./build.sh app}

OUT="$SDK/project/app/qzdesk/out"      # 与 oem 分区 /oem/usr 同构
# /oem 只剩几 MB，4.6 MB 的核心无法再在目标分区暂存一份。先在 /tmp 完整
# 暂存并校验；涉及运行中程序时先停服务释放旧 inode，再整批切换。
STAGE=/tmp/.qzdesk-deploy
TX_STAGE="$STAGE/txn.$$"
tx_src=()
tx_dst=()
tx_mode=()
tx_hash=()
tx_staged=()
tx_backup=()
tx_backed=()
tx_install=()
tx_touched=0
tx_finished=0
tx_runtime_stopped=0
tx_init=""

push_ui=1 push_core=1 push_kws=1 push_fonts=0 push_init=0 do_build=0 do_restart=1 s30=0
wifi=0
camera=0
log_secs=0

usage() { sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
    case $1 in
        --ui-only)     push_core=0; push_kws=0 ;;
        --core-only)   push_ui=0 ;;
        --kws-only)    push_ui=0; push_core=0; push_kws=1 ;;
        --init-only)   push_ui=0; push_core=0; push_kws=0; push_init=1 ;;
        --with-fonts)  push_fonts=1 ;;
        --init-script) push_init=1 ;;
        --s30)         s30=1 ;;
        --wifi)        wifi=1 ;;
        --camera)      camera=1 ;;
        --build)       do_build=1 ;;
        --no-restart)  do_restart=0 ;;
        --log)         log_secs=${2:-20}; [ $# -gt 1 ] && shift ;;
        --status)      log_secs=0; do_restart=0; push_ui=0; push_core=0; push_kws=0 ;;
        -h|--help)     usage; exit 0 ;;
        *) echo "未知参数: $1（--help 看用法）" >&2; exit 2 ;;
    esac
    shift
done

adb_() {
    if [ -n "$DEVICE" ]; then "$ADB" -s "$DEVICE" "$@"; else "$ADB" "$@"; fi
}
say() { printf '\033[36m==>\033[0m %s\n' "$*"; }
die() { printf '\033[31m错误:\033[0m %s\n' "$*" >&2; exit 1; }

find_init_script() {
    adb_ shell 'ls /oem/usr/etc/init.d/S*qzdesk /etc/init.d/S*qzdesk 2>/dev/null | head -1' \
        | tr -d '\r'
}

remote_ok() {
    local command=$1
    local marker
    marker=$(adb_ shell "{ $command; } >/dev/null 2>&1; printf 'QZDESK_REMOTE_RC=%s\\n' \"\$?\"" \
        | tr -d '\r' | tail -n1)
    [ "$marker" = "QZDESK_REMOTE_RC=0" ]
}

runtime_pids() {
    adb_ shell '
        for name in qzdesk_screen qzdesk-kws xiaozhi_linux_rs; do
            for pid in $(pidof "$name" 2>/dev/null); do
                state=$(awk "/^State:/{print \$2}" "/proc/$pid/status" 2>/dev/null)
                [ "$state" = Z ] || echo "$pid"
            done
        done
    ' | tr -d '\r'
}

runtime_present() {
    [ -n "$(runtime_pids | tr -d '[:space:]')" ]
}

stop_runtime() {
    [ -n "$tx_init" ] || tx_init=$(find_init_script)
    if runtime_present; then
        tx_runtime_stopped=1
        say "停止界面、核心与唤醒词，释放 /oem 旧 inode"
        if [ -n "$tx_init" ]; then
            adb_ shell "sh $tx_init stop" >/dev/null 2>&1 || true
        else
            adb_ shell 'killall qzdesk_screen qzdesk-kws xiaozhi_linux_rs 2>/dev/null || true'
        fi
        local i=0
        while runtime_present && [ "$i" -lt 40 ]; do
            sleep 0.1
            i=$((i + 1))
        done
        if runtime_present; then
            adb_ shell 'killall qzdesk_screen 2>/dev/null; killall qzdesk-kws 2>/dev/null; killall xiaozhi_linux_rs 2>/dev/null' \
                >/dev/null 2>&1 || true
            sleep 0.5
        fi
        if runtime_present; then
            die "旧进程无法停止，未改动 /oem 文件"
        fi
    fi
}

start_runtime() {
    [ -n "$tx_init" ] || tx_init=$(find_init_script)
    if [ -n "$tx_init" ]; then
        say "重新启动：sh $tx_init start"
        adb_ shell "sh $tx_init start" || true
    else
        say "重新启动界面（未找到开机脚本）"
        adb_ shell 'cd /oem/usr/bin && setsid ./qzdesk_screen >>/var/log/qzdesk.log 2>&1 </dev/null &' || true
    fi
}

rollback_transaction() {
    local i
    say "部署未完成，正在整批回滚"
    for ((i = ${#tx_dst[@]} - 1; i >= 0; i--)); do
        if [ "${tx_backed[$i]:-0}" = 1 ]; then
            adb_ shell "rm -f ${tx_dst[$i]}; mv -f ${tx_backup[$i]} ${tx_dst[$i]} && chmod ${tx_mode[$i]} ${tx_dst[$i]}" \
                >/dev/null 2>&1 || true
        elif [ "${tx_install[$i]:-0}" = 1 ]; then
            adb_ shell "rm -f ${tx_dst[$i]}" >/dev/null 2>&1 || true
        fi
    done
    adb_ shell sync >/dev/null 2>&1 || true
}

cleanup_transaction() {
    local status=$?
    set +e
    if [ "$tx_touched" = 1 ] && [ "$tx_finished" = 0 ]; then
        rollback_transaction
    fi
    adb_ shell "rm -rf $TX_STAGE" >/dev/null 2>&1 || true
    if [ "$tx_runtime_stopped" = 1 ] && [ "$do_restart" = 1 ]; then
        start_runtime
        tx_runtime_stopped=0
    fi
    trap - EXIT
    exit "$status"
}

trap cleanup_transaction EXIT

# ------------------------------------------------------------------ 0) 设备就绪
command -v "$ADB" >/dev/null 2>&1 || die "没找到 adb（可显式设置 ADB=/mnt/d/adb/adb.exe）"
say "等待设备（adb wait-for-device，最多 20 秒）"
if ! timeout 20 "$ADB" ${DEVICE:+-s "$DEVICE"} wait-for-device 2>/dev/null; then
    die "没有设备响应。插好 USB / 打开 'adb tcpip 5555' 后用 QZDESK_DEVICE=ip:5555 再来"
fi
[ "$(adb_ shell 'echo QZDESK_SHELL_OK' 2>/dev/null | tr -d '\r\n')" = "QZDESK_SHELL_OK" ] \
    || die "adb 连上了但 shell 不通（试试 adb root / adb kill-server）"
say "设备: $(adb_ shell 'cat /proc/device-tree/model 2>/dev/null || uname -m' | tr -d '\r')"

# ------------------------------------------------------------------ 1) 交叉编译产物
if [ "$do_build" = 1 ]; then
    say "先重新交叉编译：cd $SDK && $BUILD_CMD"
    (cd "$SDK" && eval "$BUILD_CMD")
fi

check_arm() {   # 挡掉 x86 模拟器二进制：推上去只会 cannot execute binary file
    local f=$1
    file -b "$f" | grep -qi 'ARM' || die "$f 不是 ARM 产物（是模拟器构建？先跑 $BUILD_CMD）"
}

want_ui=0; want_core=0; want_kws=0
[ "$push_ui" = 1 ] && want_ui=1
[ "$push_core" = 1 ] && want_core=1
[ "$push_kws" = 1 ] && want_kws=1

if [ "$want_ui" = 1 ] || [ "$want_core" = 1 ] || [ "$want_kws" = 1 ]; then
    command -v file >/dev/null 2>&1 || say "提示: 本机没有 file，跳过架构检查"
fi

# ------------------------------------------------------------------ 2) 可写性
if [ "$want_ui" = 1 ] || [ "$want_core" = 1 ] || [ "$want_kws" = 1 ] || [ "$push_init" = 1 ] || [ "$s30" = 1 ] \
   || [ "$wifi" = 1 ] || [ "$camera" = 1 ]; then
    say "确认 /oem 可写"
    remote_ok "mount -o remount,rw /oem 2>/dev/null; touch /oem/.qzdesk-wtest && rm -f /oem/.qzdesk-wtest" \
        || die "/oem 不可写（先 adb root && adb remount，或确认 oem 分区已挂载）"
    adb_ shell "rm -rf $STAGE; mkdir -p $TX_STAGE /oem/usr/bin /oem/usr/etc/init.d /oem/usr/etc/qzdesk /oem/usr/share/fonts /oem/usr/share/licenses/aora-bot" >/dev/null

    stage_file() {   # stage_file <本地> <设备路径> [权限]
        local src=$1 dst=$2 mode=${3:-0755}
        [ -f "$src" ] || die "找不到 $src"
        local local_hash=""
        if command -v sha256sum >/dev/null 2>&1 \
           && remote_ok "command -v sha256sum >/dev/null 2>&1"; then
            local_hash=$(sha256sum "$src" | awk '{print $1}')
        fi
        local index=${#tx_src[@]}
        local name; name=$(basename "$dst")
        local staged="$TX_STAGE/$index-$name.new"
        local backup="$TX_STAGE/$index-$name.old"
        tx_src[$index]=$src
        tx_dst[$index]=$dst
        tx_mode[$index]=$mode
        tx_hash[$index]=$local_hash
        tx_staged[$index]=$staged
        tx_backup[$index]=$backup
        tx_backed[$index]=0
        tx_install[$index]=0
        say "推 $(basename "$src") -> $dst ($(du -h "$src" | cut -f1))"
        local adb_src=$src
        if [[ "$ADB" == *.exe ]]; then adb_src=$(wslpath -w "$src"); fi
        adb_ push "$adb_src" "$staged" >/dev/null
        if [ -n "$local_hash" ]; then
            local remote_hash
            remote_hash=$(adb_ shell "sha256sum $staged" | awk '{print $1}' | tr -d '\r')
            [ "$remote_hash" = "$local_hash" ] \
                || die "$name 传输校验失败（本地 $local_hash，设备 $remote_hash）"
        fi
    }

    commit_staged() {
        local count=${#tx_src[@]}
        local i name installed_hash
        [ "$count" -gt 0 ] || return 0

        # 只停一次，且必须在移动旧二进制之前。替换路径不会释放正在运行程序
        # 占用的 inode；低空间时正是这一点导致核心写入被截断。
        if [ "$want_ui" = 1 ] || [ "$want_core" = 1 ] || [ "$want_kws" = 1 ]; then
            stop_runtime
        fi

        say "备份目标分区旧文件到 /tmp（$count 个）"
        for ((i = 0; i < count; i++)); do
            name=$(basename "${tx_dst[$i]}")
            if remote_ok "[ -e ${tx_dst[$i]} ]"; then
                tx_touched=1
                remote_ok "mv -f ${tx_dst[$i]} ${tx_backup[$i]}" \
                    || die "$name 无法移到备份，整批安装尚未开始"
                tx_backed[$i]=1
            fi
        done

        say "安装到 /oem（全部校验通过后才删除备份）"
        for ((i = 0; i < count; i++)); do
            name=$(basename "${tx_dst[$i]}")
            tx_touched=1
            tx_install[$i]=1
            if ! remote_ok "mv -f ${tx_staged[$i]} ${tx_dst[$i]} && chmod ${tx_mode[$i]} ${tx_dst[$i]} && sync"; then
                die "$name 安装失败"
            fi
            if [ -n "${tx_hash[$i]}" ]; then
                installed_hash=$(adb_ shell "sha256sum ${tx_dst[$i]}" | awk '{print $1}' | tr -d '\r')
                [ "$installed_hash" = "${tx_hash[$i]}" ] \
                    || die "$name 安装后校验失败（设备 $installed_hash）"
            fi
        done
        tx_finished=1
        adb_ shell "rm -rf $TX_STAGE" >/dev/null
        say "全部文件校验通过"
    }
fi

# ------------------------------------------------------------------ 3) 推文件
if [ "$want_ui" = 1 ]; then
    command -v file >/dev/null 2>&1 && check_arm "$OUT/bin/qzdesk_screen"
    stage_file "$OUT/bin/qzdesk_screen" /oem/usr/bin/qzdesk_screen
    for name in LICENSE NOTICE.md README.md; do
        stage_file "$OUT/usr/share/licenses/aora-bot/$name" "/oem/usr/share/licenses/aora-bot/$name" 0644
    done
fi
if [ "$want_core" = 1 ]; then
    command -v file >/dev/null 2>&1 && check_arm "$OUT/bin/xiaozhi_linux_rs"
    stage_file "$OUT/bin/xiaozhi_linux_rs" /oem/usr/bin/xiaozhi_linux_rs
    # 核心里的小脚本（系统状态 / 定时器 / 番茄钟 / 音量 / 播报等）跟核心是配套的
    for s in system_status.sh set_timer.py pomodoro.py robot_move.sh \
             set_volume.sh set_brightness.sh speak.sh screenshot.sh \
             power_action.sh gpio_set.sh; do
        [ -f "$OUT/bin/$s" ] && stage_file "$OUT/bin/$s" "/oem/usr/bin/$s"
    done
fi
if [ "$want_kws" = 1 ]; then
    command -v file >/dev/null 2>&1 && check_arm "$OUT/bin/qzdesk-kws"
    stage_file "$OUT/bin/qzdesk-kws" /oem/usr/bin/qzdesk-kws
fi
if [ "$push_fonts" = 1 ]; then
    for f in "$OUT"/usr/share/fonts/*; do
        [ -f "$f" ] && stage_file "$f" "/oem/usr/share/fonts/$(basename "$f")" 0644
    done
fi
if [ "$push_init" = 1 ]; then
    # 两份都要更新：rootfs /etc/init.d（rcS 跑）与 oem /oem/usr/etc/init.d（RkLunch 跑）
    init_local="$script_dir/device/S30qzdesk"
    [ -f "$init_local" ] || die "找不到 $init_local"
    stage_file "$init_local" /oem/usr/etc/init.d/S30qzdesk
    stage_file "$script_dir/device/wpa_supplicant.default.conf" \
        /oem/usr/etc/qzdesk/wpa_supplicant.default.conf 0600
    if remote_ok "touch /etc/init.d/.wtest && rm -f /etc/init.d/.wtest"; then
        stage_file "$init_local" /etc/init.d/S30qzdesk
        adb_ shell 'rm -f /etc/init.d/S99qzdesk' >/dev/null
    else
        say "rootfs 只读：/etc/init.d 那份跳过（只更新了 oem 那份）"
    fi
    adb_ shell 'rm -f /oem/usr/etc/init.d/S99qzdesk' >/dev/null
fi

if [ "$want_ui" = 1 ] || [ "$want_core" = 1 ] || [ "$want_kws" = 1 ] || [ "$push_fonts" = 1 ] || [ "$push_init" = 1 ]; then
    commit_staged
fi

# ------------------------------------------------------------------ 4) S30：提前启动
if [ "$s30" = 1 ]; then
    say "把开机脚本从 S99 改名成 S30（排在挂载之后、网络之前）"
    for d in /oem/usr/etc/init.d /etc/init.d; do
        if remote_ok "[ -f $d/S99qzdesk ]"; then
            remote_ok "mv -f $d/S99qzdesk $d/S30qzdesk" && say "  $d -> S30qzdesk"
        fi
    done
    adb_ shell "ls /oem/usr/etc/init.d/ /etc/init.d/ | grep qzdesk || true"
fi

# ------------------------------------------------------------------ 4.5) WiFi 驱动 + 固件
# 这块板子（Luckfox Pico 86Panel / RV1106G）用的是 RTL8723BS（SDIO WiFi+BT）：
#   - 驱动模块编译出来了，但镜像里没打进 rootfs（/lib/modules 都不存在）
#   - 固件在 SDK 的 overlay-luckfox-wifibt-firmware 里，板级配置没引用它
#   - 也没有任何脚本去 insmod，所以设备上根本不会出现 wlan0
# 这个开关把模块与固件推上去并当场加载，用来验证界面里的无线页 —— 不改镜像就能用。
if [ "$wifi" = 1 ]; then
    KO_SRC="$SDK/sysdrv/out/kernel_drv_ko"
    FW_SRC="$SDK/project/cfg/BoardConfig_IPC/overlay/overlay-luckfox-wifibt-firmware/lib/firmware"
    MODULES="cfg80211.ko mac80211.ko r8723bs.ko"      # 顺序不能换：后两个依赖前一个
    for m in $MODULES; do
        [ -f "$KO_SRC/$m" ] || die "找不到模块 $KO_SRC/$m（先编译内核）"
    done

    say "推驱动模块到 /oem/usr/lib/wifi"
    adb_ shell "mkdir -p /oem/usr/lib/wifi /lib/firmware/rtlwifi /lib/firmware/rtlbt" >/dev/null
    for m in $MODULES; do
        adb_ push -q "$KO_SRC/$m" "$STAGE/$m" >/dev/null
        adb_ shell "mv -f $STAGE/$m /oem/usr/lib/wifi/$m" >/dev/null
    done

    # 固件路径由驱动写死（request_firmware("rtlwifi/rtl8723bs_nic.bin")），只能放 /lib/firmware
    if remote_ok "touch /lib/firmware/.wtest && rm -f /lib/firmware/.wtest"; then
        for f in rtlwifi/rtl8723bs_nic.bin rtlbt/rtl8723b_config rtlbt/rtl8723b_fw; do
            [ -f "$FW_SRC/$f" ] || continue
            adb_ push -q "$FW_SRC/$f" "$STAGE/$(basename "$f")" >/dev/null
            adb_ shell "mv -f $STAGE/$(basename "$f") /lib/firmware/$f" >/dev/null
        done
        say "固件已就位: /lib/firmware/rtlwifi|rtlbt"
    else
        say "警告: /lib/firmware 不可写（rootfs 只读），固件推不进去 —— 驱动会 request_firmware 失败"
    fi

    say "加载模块（cfg80211 -> mac80211 -> r8723bs）"
    adb_ shell '
        cd /oem/usr/lib/wifi || exit 1
        for m in cfg80211 mac80211 r8723bs; do
            if grep -q "^$m " /proc/modules 2>/dev/null; then echo "  $m 已在运行"; continue; fi
            out=$(insmod ./$m.ko 2>&1)
            [ -n "$out" ] && echo "  $m: $out" || echo "  $m 已加载"
        done
        echo -n "  网卡: "; ls /sys/class/net | tr "\n" " "; echo
        echo -n "  无线网卡: "; ls -d /sys/class/net/*/wireless 2>/dev/null | cut -d/ -f5 | tr "\n" " "; echo
    '
    say "提示: 无线页若已打开过，退出重进即可（界面会重新探测网卡）；连接走 wpa_cli + udhcpc"
fi

# ------------------------------------------------------------------ 4.6) 相机模块
# 与 --wifi 同理：SC3336 + rkcif/rkisp 的驱动模块镜像里没带，推到 /oem/usr/lib/camera
# 并当场加载，验证界面里的相机页 —— 不改镜像就能用。IQ 文件随整包走（Makefile 装）。
if [ "$camera" = 1 ]; then
    KO_SRC="$SDK/sysdrv/out/kernel_drv_ko"
    MODULES="rk_dvbm video_rkcif video_rkisp phy-rockchip-csi2-dphy-hw phy-rockchip-csi2-dphy sc3336"
    for m in $MODULES; do
        [ -f "$KO_SRC/$m.ko" ] || die "找不到模块 $KO_SRC/$m.ko（先编译内核）"
    done

    say "推相机模块到 /oem/usr/lib/camera"
    adb_ shell "mkdir -p /oem/usr/lib/camera" >/dev/null
    for m in $MODULES; do
        adb_ push -q "$KO_SRC/$m" "$STAGE/$m" >/dev/null
        adb_ shell "mv -f $STAGE/$m /oem/usr/lib/camera/$m" >/dev/null
    done

    say "加载相机模块（含 clr_unready_dev 兜底）"
    adb_ shell '
        cd /oem/usr/lib/camera || exit 1
        for m in rk_dvbm video_rkcif video_rkisp phy-rockchip-csi2-dphy-hw phy-rockchip-csi2-dphy sc3336; do
            grep -q "^$m " /proc/modules 2>/dev/null && continue
            insmod ./$m.ko 2>&1 | sed "s/^/  $m: /"
        done
        # 传感器比 cif/isp 晚挂上时，video 节点不会注册；写 1 强制补注册
        echo 1 > /sys/module/video_rkcif/parameters/clr_unready_dev 2>/dev/null
        echo 1 > /sys/module/video_rkisp/parameters/clr_unready_dev 2>/dev/null
        sleep 2
        echo -n "  video 节点: "; ls /dev/video* 2>/dev/null | tr "\n" " "; echo
        echo -n "  sc3336 已加载: "; lsmod | grep -c sc3336
    '
    say "提示: 重启界面后进相机页；仍无节点则 adb shell \"dmesg | grep -iE 'sc3336|rkisp|rkcif'\""
fi

# ------------------------------------------------------------------ 5) 重启界面
if [ "$do_restart" = 1 ] && { [ "$want_ui" = 1 ] || [ "$want_core" = 1 ] || [ "$want_kws" = 1 ] || [ "$push_init" = 1 ]; }; then
    [ -n "$tx_init" ] || tx_init=$(find_init_script)
    if [ "$tx_runtime_stopped" = 1 ]; then
        start_runtime
        tx_runtime_stopped=0
    elif [ -n "$tx_init" ]; then
        say "重启界面：sh $tx_init restart"
        adb_ shell "sh $tx_init restart" || true
    else
        say "设备上没找到开机脚本，直接重启进程"
        adb_ shell 'killall qzdesk_screen 2>/dev/null; sleep 1; cd /oem/usr/bin && (./qzdesk_screen >>/var/log/qzdesk.log 2>&1 &)'
    fi
    sleep 2
elif [ "$tx_runtime_stopped" = 1 ]; then
    say "提示：--no-restart 已让设备保持停机；手动启动用 sh $tx_init start"
fi

# ------------------------------------------------------------------ 6) 状态 / 日志
if [ "$log_secs" -gt 0 ]; then
    say "盯 $log_secs 秒日志（/var/log/qzdesk.log）"
    adb_ shell "timeout $log_secs tail -f /var/log/qzdesk.log" || true
fi

say "设备现状："
adb_ shell 'echo -n "  UI: "; (pidof qzdesk_screen || echo 未运行); \
            echo -n "  核心: "; (pidof xiaozhi_linux_rs || echo 未运行); \
            echo -n "  唤醒词: "; (pidof qzdesk-kws || echo 未运行); \
            echo -n "  开机脚本: "; (ls /etc/init.d/S*qzdesk /oem/usr/etc/init.d/S*qzdesk 2>/dev/null | tr "\n" " "); echo; \
            echo -n "  开机耗时(秒): "; grep -o "uptime=[0-9.]*" /var/log/qzdesk.log 2>/dev/null | tail -1' | tr -d '\r'

say "完成。看界面效果: adb shell \"tail -20 /var/log/qzdesk.log\""
