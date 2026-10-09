# deploy-via-adb.ps1 -- deploy camera modules + UI to the Luckfox board via Windows adb
#
# Files are read from the WSL filesystem through \\wsl$ (no usbipd, no ssh needed).
#
# Usage (admin not required):
#   powershell -ExecutionPolicy Bypass -File .\deploy-via-adb.ps1
#
# Prereq: "adb devices" (run in this same PowerShell) lists the board as "device".

$ErrorActionPreference = "Continue"
$WSL = "\\wsl$\Ubuntu-22.04"
$KO_SRC = "$WSL\home\jn\QZdesk\SDK\sysdrv\out\kernel_drv_ko"
$UI_SRC = "$WSL\home\jn\QZdesk\SDK\project\app\qzdesk\out\bin\qzdesk_screen"
$MODULES = "rk_dvbm","video_rkcif","video_rkisp","phy-rockchip-csi2-dphy-hw","phy-rockchip-csi2-dphy","sc3336"

function Run($cmd) {
    Write-Host ("> " + $cmd) -ForegroundColor DarkGray
    cmd /c $cmd
    Write-Host ""
}

# ---- 0) adb present + device present ----
$dev = adb devices 2>&1 | Select-String -Pattern "\tdevice$"
if (-not $dev) {
    Write-Host "No adb device. Run 'adb devices' -- the board must show as 'device'." -ForegroundColor Red
    Read-Host "Press ENTER to exit"; exit 1
}
Write-Host "Board found: $dev" -ForegroundColor Green

# ---- 1) stage files from WSL into %TEMP% (adb cannot push UNC paths directly) ----
$stage = "$env:TEMP\luckfox_deploy"
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $stage | Out-Null
foreach ($m in $MODULES) {
    $src = "$KO_SRC\$m.ko"
    if (-not (Test-Path $src)) { Write-Host "missing: $src" -ForegroundColor Red; Read-Host "Press ENTER"; exit 1 }
    Copy-Item $src "$stage\$m.ko"
}
if (Test-Path $UI_SRC) { Copy-Item $UI_SRC "$stage\qzdesk_screen" } else { Write-Host "UI binary missing: $UI_SRC (skip UI)" -ForegroundColor Yellow }

# ---- 2) push camera modules ----
Run "adb shell mkdir -p /oem/usr/lib/camera /data/local/tmp"
foreach ($m in $MODULES) {
    Run ("adb push `"$stage\$m.ko`" /data/local/tmp/$m.ko")
    Run "adb shell mv -f /data/local/tmp/$m.ko /oem/usr/lib/camera/$m.ko"
}

# ---- 3) load modules (order matters) + the unready-dev workaround ----
Run "adb shell cd /oem/usr/lib/camera; for m in rk_dvbm video_rkcif video_rkisp phy-rockchip-csi2-dphy-hw phy-rockchip-csi2-dphy sc3336; do grep -q `"^`$m `" /proc/modules || insmod ./`$m.ko 2>&1 | sed s/^/`$m:\ /; done"
Run "adb shell echo 1 > /sys/module/video_rkcif/parameters/clr_unready_dev; echo 1 > /sys/module/video_rkisp/parameters/clr_unready_dev"
Run "adb shell sleep 2; ls /dev/video* 2>/dev/null; echo ---; lsmod | grep -E 'sc3336|rkcif|rkisp'"

# ---- 4) push the new UI binary and restart it ----
if (Test-Path "$stage\qzdesk_screen") {
    Run "adb push `"$stage\qzdesk_screen`" /data/local/tmp/qzdesk_screen"
    Run "adb shell mv -f /data/local/tmp/qzdesk_screen /oem/usr/bin/qzdesk_screen; chmod 755 /oem/usr/bin/qzdesk_screen"
    Run "adb shell killall qzdesk_screen 2>/dev/null; sleep 1; cd /oem/usr/bin; QZDESK_FONT=/oem/usr/share/fonts/NanoTikBazHei-Bold.ttf QZDESK_FB=/dev/fb0 QZDESK_TOUCH=/dev/input/event0 nohup ./qzdesk_screen >/dev/null 2>&1 &"
}

Write-Host ""
Write-Host "If 'ls /dev/video*' printed nothing above, run:" -ForegroundColor Yellow
Write-Host "  adb shell `"dmesg | grep -iE 'sc3336|rkisp|rkcif' | tail -20`""
Write-Host "and send the output back."
Read-Host "Press ENTER to exit"
