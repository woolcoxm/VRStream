#!/usr/bin/env bash
# v2: watches for the Quest on USB; when it appears: enable tcpip ADB, get IP,
# connect wireless, install the APK, start the host, launch the app.
APK="/c/Users/Mark/Desktop/Projects/VRStream/quest/app/build/outputs/apk/debug/app-debug.apk"
HOSTEXE="/c/Users/Mark/Desktop/Projects/VRStream/build/host/Release/vrstream_host.exe"
HOSTLOG="/c/Users/Mark/Desktop/Projects/VRStream/host-run.log"
PC_IP="10.0.0.31"
DEADLINE=$(( $(date +%s) + 3000 ))

while [ "$(date +%s)" -lt "$DEADLINE" ]; do
    # Reconnect to known wireless device first (if it rebooted, this fails).
    if adb connect 10.0.0.162:5555 >/dev/null 2>&1; then
        if adb devices | grep -q "10.0.0.162:5555.*device"; then
            echo "$(date +%T) wireless device already up"
            break
        fi
    fi
    LINE=$(adb devices 2>/dev/null | awk 'NR>1 && NF>1 {print}')
    [ -z "$LINE" ] && { sleep 5; continue; }
    STATUS=$(echo "$LINE" | awk '{print $2}')
    [ "$STATUS" != "device" ] && { sleep 4; continue; }

    echo "$(date +%T) headset on USB"
    adb tcpip 5555 >/dev/null 2>&1
    sleep 2
    IP=$(adb shell ip -f inet addr show wlan0 2>/dev/null | grep -oE 'inet [0-9.]+' | awk '{print $2}')
    [ -z "$IP" ] && { echo "$(date +%T) no wlan IP"; sleep 5; continue; }
    echo "$(date +%T) headset IP: $IP"
    for i in $(seq 1 12); do
        adb connect "$IP:5555" >/dev/null 2>&1
        if adb devices | grep -q "$IP:5555.*device"; then
            echo "$(date +%T) WIRELESS ADB UP at $IP:5555"
            echo "$IP" > /tmp/vrs-ip
            break 2
        fi
        sleep 4
    done
    sleep 4
done

IP=$(cat /tmp/vrs-ip 2>/dev/null || echo 10.0.0.162)
if ! adb devices | grep -q "$IP:5555.*device"; then echo "no wireless device"; exit 1; fi

adb -s "$IP:5555" install -r "$APK" && echo "$(date +%T) APK installed"
sleep 3
# Host: fresh instance if none running
if ! tasklist 2>/dev/null | grep -qi vrstream_host; then
    ( "$HOSTEXE" --port 9944 --fps 90 --duration 1800 > "$HOSTLOG" 2>&1 & )
    sleep 3
fi
adb -s "$IP:5555" shell am start -n com.vrstream.client/.MainActivity --es host "$PC_IP" --es port 9944
echo "$(date +%T) LAUNCHED on headset -> $PC_IP:9944"
sleep 10
adb -s "$IP:5555" logcat -d -s VRStream:* | tail -12
echo "=== host ==="; tail -5 "$HOSTLOG" 2>/dev/null
