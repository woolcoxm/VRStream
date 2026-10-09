# Bring-up guide

One-time headset-side setup (only you can do these):

1. **Power the Quest 3 on** and keep it on (USB debugging requires an awake,
   logged-in headset).
2. **Enable Developer Mode**: the Meta Horizon phone app → Devices → select
   the headset → Headset Settings → Developer Mode → on. (Requires a Meta
   developer organization — creating one is free and instant at
   developer.oculus.com.) The headset reboots.
3. **USB debugging**: on the headset, Settings → System → Developer
   settings → USB debugging → on. Plug into the PC (a **data** USB-C cable —
   charge-only cables won't enumerate) and accept the "Allow USB debugging"
   prompt inside the headset.
4. Verify from the PC: `adb devices` must list the headset as `device`
   (not `unauthorized`).

Then run the whole chain:

```powershell
cd <repo>
tools\bringup.ps1                 # test-pattern stream to the headset
tools\bringup.ps1 -WithSteamVR    # game capture via the SteamVR driver
adb logcat -s VRStream            # watch the pipeline stages
```

For wireless sessions later (after one USB pairing): `adb tcpip 5555`,
then `adb connect <headset-ip>:5555`.

SteamVR path additionally needs Steam installed and logged in, with SteamVR
(free, app 250820) installed — `bringup.ps1 -WithSteamVR` registers our
driver via `vrpathreg` and the host consumes game frames on `--feed-port`.
