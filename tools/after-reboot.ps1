# VRStream post-reboot bring-up: SteamVR -> chaperone -> host -> Quest.
# Run from the repo root after the PC has rebooted and Steam auto-logged-in.
param(
    [string]$QuestIp = "10.0.0.162",
    [string]$PcIp = "10.0.0.31",
    [int]$Port = 9944,
    [int]$FeedPort = 9955
)
$ErrorActionPreference = "Continue"
$repo = Split-Path -Parent $PSScriptRoot
$hostExe = Join-Path $repo "build\host\Release\vrstream_host.exe"
$chaperone = Join-Path $repo "host\tools\tt\Release\chaperone_commit.exe"
$apk = Join-Path $repo "quest\app\build\outputs\apk\debug\app-debug.apk"
$steam = "C:\Program Files (x86)\Steam\steam.exe"
$vrserverLog = "C:\Program Files (x86)\Steam\logs\vrserver.txt"

Write-Host "== 1. Launch SteamVR ==" -ForegroundColor Cyan
Start-Process $steam -ArgumentList "-applaunch","250820"

Write-Host "   waiting for vrserver + our HMD (up to 120s)..." -ForegroundColor DarkGray
$deadline = (Get-Date).AddSeconds(120)
while ((Get-Date) -lt $deadline) {
    Start-Sleep 5
    $srv = Get-Process vrserver -ErrorAction SilentlyContinue
    $hmd = Select-String -Path $vrserverLog -Pattern "Using existing HMD vrstream" -ErrorAction SilentlyContinue |
           Select-Object -Last 1
    if ($srv -and $hmd -and $hmd.Line -match "$(Get-Date -Format 'HH:mm')") { break }
}
Write-Host ("   vrserver: {0}" -f ((Get-Process vrserver -ErrorAction SilentlyContinue) -ne $null))

Write-Host "== 2. Commit chaperone (clears first-setup) ==" -ForegroundColor Cyan
& $chaperone

Write-Host "== 3. Start VRStream host (driver feed + network) ==" -ForegroundColor Cyan
Start-Process cmd -ArgumentList "/k `"`"$hostExe`" --port $Port --feed-port $FeedPort --fps 90 --duration 14400`""
Start-Sleep 3

Write-Host "== 4. Quest: install + launch client ==" -ForegroundColor Cyan
adb connect ${QuestIp}:5555 2>$null | Out-Null
adb -s ${QuestIp}:5555 install -r $apk
adb -s ${QuestIp}:5555 shell am start -n com.vrstream.client/.MainActivity --es host $PcIp --es port $Port
Write-Host "   put the headset on -- test pattern expected" -ForegroundColor Yellow

Write-Host "== 5. Watch ==" -ForegroundColor Cyan
Write-Host "   host stats:  tail host-run.log in the host console window"
Write-Host "   headset log: adb -s ${QuestIp}:5555 logcat -s VRStream"
Write-Host "   vrserver:    Select-String vrstream '$vrserverLog' | Select -Last 10"
