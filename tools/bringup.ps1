# VRStream end-to-end bring-up (run from the repo root after building).
# Prereqs (headset-side, once):
#   - Quest on, Developer Mode enabled, USB debugging allowed
#   - Steam + SteamVR installed (for the game-capture path only)
param(
    [string]$HostIp = "",       # defaults to this PC's LAN IPv4
    [int]$Port = 9944,
    [switch]$WithSteamVR        # register the driver + use --feed-port
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$apk = Join-Path $repo "quest\app\build\outputs\apk\debug\app-debug.apk"
$hostExe = Join-Path $repo "build\host\Release\vrstream_host.exe"
$driverRoot = Join-Path $repo "host\driver\driver-root\vrstream"

if (-not $HostIp) {
    $HostIp = (Get-NetIPAddress -AddressFamily IPv4 |
        Where-Object { $_.IPAddress -notlike "127.*" -and $_.IPAddress -notlike "169.254.*" } |
        Select-Object -First 1).IPAddress
}
Write-Host "PC IP: $HostIp" -ForegroundColor Cyan

# 1. Headset
adb devices | Out-Null
$device = (adb devices | Select-String "device\s*$")
if (-not $device) { Write-Error "No adb device. On the headset: enable Developer Mode + USB debugging, accept the prompt." }
adb install -r $apk
Write-Host "APK installed." -ForegroundColor Green

# 2. PC host (background console)
$hostArgs = @("--port", $Port, "--fps", "90")
if ($WithSteamVR) {
    $vrpathreg = "C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\vrpathreg.exe"
    if (Test-Path $vrpathreg) {
        & $vrpathreg adddriver $driverRoot
        Write-Host "SteamVR driver registered." -ForegroundColor Green
    } else {
        Write-Warning "SteamVR not found at $vrpathreg; streaming the built-in test pattern."
    }
    $hostArgs += @("--feed-port", "9955")
}
Start-Process -FilePath "cmd" -ArgumentList "/k `"`"$hostExe`" $($hostArgs -join ' ')`"" 
Start-Sleep -Seconds 3

# 3. Launch on headset (extra: -e logcat to watch)
adb shell am start -n com.vrstream.client/.MainActivity --es host $HostIp --es port $Port
Write-Host "Client launched on headset -> ${HostIp}:$Port" -ForegroundColor Green
Write-Host "Watch: adb logcat -s VRStream" -ForegroundColor Cyan
