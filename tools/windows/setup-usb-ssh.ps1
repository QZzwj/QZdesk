# setup-usb-ssh.ps1 -- prepare the Windows side for SSH-over-USB to the Luckfox board
#
# What it does (admin PowerShell, safe to run repeatedly):
#   1. release the board from usbipd sharing (if it was shared to WSL)
#   2. re-enable the board's "Remote NDIS" USB network adapter
#   3. set that adapter's IPv4 to 172.32.0.100 (board is 172.32.0.93)
#   4. add firewall allow-rules for the board address
#   5. ping the board so you can see whether the channel is up
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\setup-usb-ssh.ps1
#
# After "PING OK", tell the assistant in WSL -- it will deploy over ssh
# (root@172.32.0.93) without adb and without usbipd.

# PS 5.1 turns native-command stderr into exceptions under "Stop".
$ErrorActionPreference = "Continue"

# ---- 1) admin check ----
$identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Admin rights required, relaunching elevated..." -ForegroundColor Yellow
    Start-Process powershell "-ExecutionPolicy Bypass -File `"$PSCommandPath`"" -Verb RunAs
    exit
}

# ---- 2) release the board from usbipd sharing (if shared) ----
if (Get-Command usbipd -ErrorAction SilentlyContinue) {
    $listing = usbipd list
    $hit = $listing | Select-String -Pattern "2207|luckfox" | Select-Object -First 1
    if ($hit -and ($hit.Line -match '([0-9]+-[0-9]+(?:\.[0-9]+)*)')) {
        Write-Host ("Releasing usbipd share for BUSID " + $Matches[1])
        usbipd unbind --busid $Matches[1] 2>&1 | Out-Null
    }
}

# ---- 3) re-enable the RNDIS USB network adapter ----
Get-PnpDevice -PresentOnly |
    Where-Object { $_.FriendlyName -like "*Remote NDIS*" -and $_.InstanceId -like "*VID_2207*" } |
    ForEach-Object {
        Write-Host ("Enabling device: " + $_.FriendlyName)
        Enable-PnpDevice -InstanceId $_.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
    }
Start-Sleep -Seconds 2

# ---- 4) set the adapter IPv4 to 172.32.0.100 ----
$adapter = Get-NetAdapter | Where-Object { $_.InterfaceDescription -like "*Remote NDIS*" } |
    Select-Object -First 1
if (-not $adapter) {
    Write-Host "RNDIS adapter not found. Re-plug the USB cable (data cable, OTG port)" -ForegroundColor Red
    Read-Host "Press ENTER to exit"
    exit 1
}
Write-Host ("Configuring adapter: " + $adapter.Name)
netsh interface ip set address name="$($adapter.Name)" static 172.32.0.100 255.255.255.0 2>&1 | Out-Null

# ---- 5) firewall allow-rules for the board address ----
New-NetFirewallRule -DisplayName "Luckfox USB (in)"  -Direction Inbound  -RemoteAddress 172.32.0.93 -Action Allow -ErrorAction SilentlyContinue | Out-Null
New-NetFirewallRule -DisplayName "Luckfox USB (out)" -Direction Outbound -RemoteAddress 172.32.0.93 -Action Allow -ErrorAction SilentlyContinue | Out-Null

# ---- 6) reachability test ----
Write-Host ""
Write-Host "Pinging the board (172.32.0.93)..."
ping -n 2 -w 1000 172.32.0.93
if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "PING OK -- channel is up. Tell the assistant in WSL to deploy." -ForegroundColor Green
} else {
    Write-Host ""
    Write-Host "PING FAILED. Checklist:" -ForegroundColor Red
    Write-Host "  - re-plug the USB cable (must be a DATA cable, OTG port)"
    Write-Host "  - temporarily turn OFF Windows firewall for all profiles, then retry"
    Write-Host "    (the tutorial says this is needed for first-time setup)"
    Write-Host "  - board powered on and booted"
}
Read-Host "Press ENTER to exit"
