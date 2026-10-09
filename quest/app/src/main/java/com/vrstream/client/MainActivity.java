package com.vrstream.client;

import android.app.Activity;
import android.content.Context;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.util.Log;

/**
 * Thin VR trampoline activity: acquires the low-latency Wi-Fi lock (so the
 * radio never enters power-save during a session) and hands the host address
 * to the native client, which owns OpenXR, MediaCodec and the network.
 *
 * Launch with the host PC address:
 *   adb shell am start -n com.vrstream.client/.MainActivity --es host 192.168.1.50
 */
public class MainActivity extends Activity {
    private static final String TAG = "VRStream";
    private WifiManager.WifiLock wifiLock;
    private WifiManager.MulticastLock multicastLock;
    private volatile boolean nativeRunning;

    static {
        System.loadLibrary("vrstream_client");
    }

    private native void nativeStart(String host, int port);
    private native void nativeStop();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (nativeRunning) return;

        WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wifi != null) {
            wifiLock = wifi.createWifiLock(4 /* WIFI_MODE_FULL_LOW_LATENCY */, "VRStream");
            wifiLock.setReferenceCounted(false);
            wifiLock.acquire();
            multicastLock = wifi.createMulticastLock("VRStream");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
        }

        String host = getIntent().getStringExtra("host");
        int port = 9944;
        String portStr = getIntent().getStringExtra("port");
        if (portStr != null) {
            try { port = Integer.parseInt(portStr); } catch (NumberFormatException ignored) {}
        }
        if (host == null || host.isEmpty()) {
            host = "255.255.255.255";  // broadcast discovery handshake
            Log.i(TAG, "no host extra; broadcasting discovery on port " + port);
        }

        final String finalHost = host;
        final int finalPort = port;
        nativeRunning = true;
        new Thread(() -> {
            Log.i(TAG, "starting native client -> " + finalHost + ":" + finalPort);
            nativeStart(finalHost, finalPort);
            Log.i(TAG, "native client exited");
            runOnUiThread(() -> {
                nativeRunning = false;
                finish();
            });
        }, "vrstream-native").start();
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (nativeRunning) {
            nativeStop();
            nativeRunning = false;
        }
        releaseLocks();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        releaseLocks();
    }

    private void releaseLocks() {
        if (wifiLock != null && wifiLock.isHeld()) wifiLock.release();
        if (multicastLock != null && multicastLock.isHeld()) multicastLock.release();
    }
}
