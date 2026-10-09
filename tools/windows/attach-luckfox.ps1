# attach-luckfox.ps1 -- attach the Luckfox/Rockchip board USB into WSL via usbipd
#
# Usage (admin PowerShell, safe to run repeatedly):
#   powershell -ExecutionPolicy Bypass -File .\attach-luckfox.ps1
#
# Notes:
#   - "bind" is only needed once (requires admin); after re-plugging the board,
#     just run this script again, it will re-attach.
#   - The board is located by Rockchip USB VID 2207 (Luckfox adb) or the text
#     "Luckfox" in the device description.

# PS 5.1 turns native-command stderr (e.g. usbipd warnings) into exceptions
# under "Stop", which aborts the script mid-way. Warnings are harmless here.
$ErrorActionPreference = "Continue"

# ---- 1) admin check: bind needs admin, re-launch self elevated if needed ----
$identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Admin rights required, relaunching elevated..." -ForegroundColor Yellow
    Start-Process powershell "-ExecutionPolicy Bypass -File `"$PSCommandPath`"" -Verb RunAs
    exit
}

# ---- 2) is usbipd installed ----
if (-not (Get-Command usbipd -ErrorAction SilentlyContinue)) {
    Write-Host "usbipd not found. Please install it first:" -ForegroundColor Red
    Write-Host "  open https://github.com/dorssel/usbipd-win/releases/latest"
    Write-Host "  download usbipd-win_x.x.x_x64.msi, double-click to install,"
    Write-Host "  then re-open an admin PowerShell and run this script again."
    Read-Host "Press ENTER to exit"
    exit 1
}

# ---- 3) find the board ----
$listing = usbipd list
$hit = $listing | Select-String -Pattern "2207|luckfox" | Select-Object -First 1
if (-not $hit) {
    Write-Host "Board not found. Please check:" -ForegroundColor Red
    Write-Host "  - use a DATA cable (many charge-only cables carry no data)"
    Write-Host "  - plugged into the board's OTG/USB port, board powered on"
    Write-Host "All USB devices right now:"
    $listing
    Read-Host "Press ENTER to exit"
    exit 1
}

if ($hit.Line -match '([0-9]+-[0-9]+(?:\.[0-9]+)*)') {
    $busid = $Matches[1]
} else {
    Write-Host "Cannot parse BUSID from this line, do it manually:" -ForegroundColor Red
    Write-Host $hit.Line
    Write-Host '  usbipd bind --busid <BUSID>; usbipd attach --wsl --busid <BUSID>'
    Read-Host "Press ENTER to exit"
    exit 1
}

Write-Host "Found board: BUSID=$busid" -ForegroundColor Green
Write-Host $hit.Line

# ---- 4) bind (ignore "already shared" errors) + attach into WSL ----
# --force is required: Windows holds the board's RNDIS network adapter, so the
# first attach always reports "Device busy (exported)". Detaching it from
# Windows is exactly what we want (we only need adb inside WSL).
# usbipd-win 5.x: "force" lives on BIND (it detaches the device from its Windows
# driver, e.g. the RNDIS network adapter Windows grabs). attach has no --force.
# But bind --force has no effect on a device that is ALREADY bound, so unbind
# first, then force-bind, then attach.
usbipd unbind --busid $busid 2>$null | Out-Null
usbipd bind --force --busid $busid 2>$null | Out-Null
$attachOut = usbipd attach --wsl --busid $busid 2>&1
$attachOut | ForEach-Object { Write-Host $_ }
if ("$attachOut" -match "Device busy") {
    Write-Host ""
    Write-Host "Windows still holds the device. Do this once by hand:" -ForegroundColor Yellow
    Write-Host "  1. Device Manager -> Network adapters -> 'Remote NDIS based Internet Sharing Device'"
    Write-Host "  2. right-click -> Disable device"
    Write-Host "  3. re-run this script"
}

Write-Host ""
Write-Host "Done. Verify inside WSL:" -ForegroundColor Green
Write-Host "  adb devices"
Write-Host "After re-plugging the board, just run this script again."
Read-Host "Press ENTER to exit"
