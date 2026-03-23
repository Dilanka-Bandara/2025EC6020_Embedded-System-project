package com.bridgemonitor;

import android.Manifest;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothSocket;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Set;
import java.util.UUID;

public class MainActivity extends AppCompatActivity {

    // ── Bluetooth constants ──────────────────────────────────────────────────
    private static final UUID   BT_UUID       = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB");
    private static final int    REQUEST_BT     = 1;
    private static final int    REQUEST_PERMS  = 2;

    // ── Bluetooth objects ────────────────────────────────────────────────────
    private BluetoothAdapter  btAdapter;
    private BluetoothSocket   btSocket;
    private InputStream       btInputStream;
    private Thread            readThread;
    private volatile boolean  isConnected = false;

    // ── UI references ────────────────────────────────────────────────────────
    private TextView  tvStatus, tvWater, tvWeight, tvTilt, tvAlert, tvRawData;
    private TextView  tvStatusLabel;
    private Button    btnConnect, btnDisconnect;
    private LinearLayout layoutStatus;
    private ImageView imgWaterIcon, imgWeightIcon, imgTiltIcon;

    // ── Data buffer ──────────────────────────────────────────────────────────
    private final StringBuilder dataBuffer = new StringBuilder();
    private final Handler       uiHandler  = new Handler(Looper.getMainLooper());

    // ────────────────────────────────────────────────────────────────────────
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        bindViews();
        setupButtons();
        checkBluetoothSupport();
    }

    // ── Bind all UI views ────────────────────────────────────────────────────
    private void bindViews() {
        tvStatus      = findViewById(R.id.tvStatus);
        tvWater       = findViewById(R.id.tvWater);
        tvWeight      = findViewById(R.id.tvWeight);
        tvTilt        = findViewById(R.id.tvTilt);
        tvAlert       = findViewById(R.id.tvAlert);
        tvRawData     = findViewById(R.id.tvRawData);
        tvStatusLabel = findViewById(R.id.tvStatusLabel);
        btnConnect    = findViewById(R.id.btnConnect);
        btnDisconnect = findViewById(R.id.btnDisconnect);
        layoutStatus  = findViewById(R.id.layoutStatus);
    }

    // ── Button click handlers ────────────────────────────────────────────────
    private void setupButtons() {
        btnConnect.setOnClickListener(v -> showPairedDevices());
        btnDisconnect.setOnClickListener(v -> disconnectBluetooth());
    }

    // ── Check if Bluetooth is available on this phone ────────────────────────
    private void checkBluetoothSupport() {
        btAdapter = BluetoothAdapter.getDefaultAdapter();
        if (btAdapter == null) {
            showToast("This device does not support Bluetooth!");
            btnConnect.setEnabled(false);
            return;
        }
        requestPermissions();
    }

    // ── Request Bluetooth permissions (Android 12+ needs new permissions) ────
    private void requestPermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            // Android 12+
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT)
                    != PackageManager.PERMISSION_GRANTED) {
                ActivityCompat.requestPermissions(this,
                        new String[]{
                                Manifest.permission.BLUETOOTH_SCAN,
                                Manifest.permission.BLUETOOTH_CONNECT
                        }, REQUEST_PERMS);
            }
        } else {
            // Android 11 and below
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION)
                    != PackageManager.PERMISSION_GRANTED) {
                ActivityCompat.requestPermissions(this,
                        new String[]{Manifest.permission.ACCESS_FINE_LOCATION},
                        REQUEST_PERMS);
            }
        }

        // Enable Bluetooth if it's off
        if (!btAdapter.isEnabled()) {
            Intent enableBt = new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE);
            startActivityForResult(enableBt, REQUEST_BT);
        }
    }

    // ── Show list of already-paired Bluetooth devices ────────────────────────
    private void showPairedDevices() {
        if (ActivityCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT)
                != PackageManager.PERMISSION_GRANTED
                && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            showToast("Please grant Bluetooth permission first");
            requestPermissions();
            return;
        }

        Set<BluetoothDevice> pairedDevices = btAdapter.getBondedDevices();

        if (pairedDevices == null || pairedDevices.isEmpty()) {
            showToast("No paired devices found. Please pair HC-05 first in Settings → Bluetooth");
            return;
        }

        // Build list of device names + addresses
        ArrayList<String> deviceNames    = new ArrayList<>();
        ArrayList<BluetoothDevice> deviceList = new ArrayList<>();

        for (BluetoothDevice device : pairedDevices) {
            String name = device.getName() != null ? device.getName() : "Unknown";
            deviceNames.add(name + "\n" + device.getAddress());
            deviceList.add(device);
        }

        // Show picker dialog
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this,
                android.R.layout.simple_list_item_1, deviceNames);
        ListView listView = new ListView(this);
        listView.setAdapter(adapter);

        AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle("Select HC-05 Device")
                .setView(listView)
                .setNegativeButton("Cancel", null)
                .create();

        listView.setOnItemClickListener((parent, view, position, id) -> {
            dialog.dismiss();
            connectToDevice(deviceList.get(position));
        });

        dialog.show();
    }

    // ── Connect to the selected HC-05 device ─────────────────────────────────
    private void connectToDevice(BluetoothDevice device) {
        updateConnectionStatus("Connecting...", "#FFA500");
        btnConnect.setEnabled(false);

        new Thread(() -> {
            try {
                if (ActivityCompat.checkSelfPermission(this,
                        Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED
                        && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    uiHandler.post(() -> showToast("Bluetooth permission denied"));
                    return;
                }

                btSocket = device.createRfcommSocketToServiceRecord(BT_UUID);
                btSocket.connect();
                btInputStream = btSocket.getInputStream();
                isConnected = true;

                uiHandler.post(() -> {
                    updateConnectionStatus("Connected to " + device.getName(), "#00C853");
                    btnConnect.setEnabled(false);
                    btnDisconnect.setEnabled(true);
                    showToast("Connected to " + device.getName());
                });

                startReadingData();

            } catch (IOException e) {
                isConnected = false;
                uiHandler.post(() -> {
                    updateConnectionStatus("Connection Failed", "#F44336");
                    btnConnect.setEnabled(true);
                    showToast("Failed to connect: " + e.getMessage());
                });
            }
        }).start();
    }

    // ── Continuously read data from HC-05 in a background thread ─────────────
    private void startReadingData() {
        readThread = new Thread(() -> {
            byte[] buffer = new byte[256];
            int bytes;

            while (isConnected) {
                try {
                    bytes = btInputStream.read(buffer);
                    if (bytes > 0) {
                        String received = new String(buffer, 0, bytes);
                        dataBuffer.append(received);
                        processBuffer();
                    }
                } catch (IOException e) {
                    if (isConnected) {
                        isConnected = false;
                        uiHandler.post(() -> {
                            updateConnectionStatus("Disconnected", "#F44336");
                            btnConnect.setEnabled(true);
                            btnDisconnect.setEnabled(false);
                            showToast("Connection lost");
                        });
                    }
                    break;
                }
            }
        });
        readThread.start();
    }

    // ── Parse incoming data packets: $BRIDGE,dist,tilt,weight,status# ────────
    private void processBuffer() {
        String buf = dataBuffer.toString();
        int start = buf.indexOf('$');
        int end   = buf.indexOf('#');

        // Keep buffer clean — remove junk before the $ marker
        if (start > 0) {
            dataBuffer.delete(0, start);
            buf = dataBuffer.toString();
            start = 0;
            end = buf.indexOf('#');
        }

        // We have a complete packet
        if (start == 0 && end > 0) {
            String packet = buf.substring(start, end + 1);
            dataBuffer.delete(0, end + 1);

            // Expected format: $BRIDGE,45,0,320.5,SAFE#
            uiHandler.post(() -> parseAndDisplay(packet));
        }
    }

    // ── Extract each sensor value and update the UI ───────────────────────────
    private void parseAndDisplay(String packet) {
        try {
            // Show raw data for debugging
            tvRawData.setText("Raw: " + packet);

            // Remove $ and # then split by comma
            // Result: ["BRIDGE", "45", "0", "320.5", "SAFE"]
            String clean  = packet.replace("$", "").replace("#", "");
            String[] parts = clean.split(",");

            if (parts.length < 5 || !parts[0].equals("BRIDGE")) return;

            int    distance = Integer.parseInt(parts[1].trim());
            int    tilt     = Integer.parseInt(parts[2].trim());
            String weight   = parts[3].trim();
            String status   = parts[4].trim();

            // ── Water Level ───────────────────────────────────────────────
            tvWater.setText(distance + " cm");
            if (distance < 75) {
                tvWater.setTextColor(Color.parseColor("#F44336")); // Red - danger
            } else if (distance < 100) {
                tvWater.setTextColor(Color.parseColor("#FFA500")); // Orange - warning
            } else {
                tvWater.setTextColor(Color.parseColor("#00C853")); // Green - safe
            }

            // ── Tilt / Vibration ──────────────────────────────────────────
            if (tilt == 1) {
                tvTilt.setText("TILTED ⚠");
                tvTilt.setTextColor(Color.parseColor("#F44336"));
            } else {
                tvTilt.setText("Stable ✓");
                tvTilt.setTextColor(Color.parseColor("#00C853"));
            }

            // ── Weight / Strain ───────────────────────────────────────────
            tvWeight.setText(weight + " g");
            float w = Float.parseFloat(weight);
            if (w > 1500) {
                tvWeight.setTextColor(Color.parseColor("#F44336"));
            } else if (w > 1000) {
                tvWeight.setTextColor(Color.parseColor("#FFA500"));
            } else {
                tvWeight.setTextColor(Color.parseColor("#00C853"));
            }

            // ── Overall Status Banner ─────────────────────────────────────
            updateBridgeStatus(status);

        } catch (Exception e) {
            tvRawData.setText("Parse error: " + packet);
        }
    }

    // ── Update the big status banner at the top ───────────────────────────────
    private void updateBridgeStatus(String status) {
        switch (status) {
            case "DANGER":
                layoutStatus.setBackgroundColor(Color.parseColor("#F44336"));
                tvStatusLabel.setText("🚨  DANGER — BRIDGE UNSAFE!");
                tvAlert.setText("⚠ CRITICAL ALERT: Evacuate immediately!");
                tvAlert.setVisibility(View.VISIBLE);
                break;
            case "WARNING":
                layoutStatus.setBackgroundColor(Color.parseColor("#FFA500"));
                tvStatusLabel.setText("⚠  WARNING — Monitor Closely");
                tvAlert.setText("⚠ WARNING: Approaching critical levels");
                tvAlert.setVisibility(View.VISIBLE);
                break;
            default: // SAFE
                layoutStatus.setBackgroundColor(Color.parseColor("#00C853"));
                tvStatusLabel.setText("✅  SAFE — All Systems Normal");
                tvAlert.setVisibility(View.GONE);
                break;
        }
    }

    // ── Disconnect from HC-05 ─────────────────────────────────────────────────
    private void disconnectBluetooth() {
        isConnected = false;
        try {
            if (btInputStream != null) btInputStream.close();
            if (btSocket     != null) btSocket.close();
        } catch (IOException e) {
            e.printStackTrace();
        }
        updateConnectionStatus("Disconnected", "#9E9E9E");
        btnConnect.setEnabled(true);
        btnDisconnect.setEnabled(false);
        resetDisplay();
        showToast("Disconnected");
    }

    // ── Reset all display values ──────────────────────────────────────────────
    private void resetDisplay() {
        tvWater.setText("-- cm");
        tvWeight.setText("-- g");
        tvTilt.setText("--");
        tvRawData.setText("Waiting for data...");
        layoutStatus.setBackgroundColor(Color.parseColor("#607D8B"));
        tvStatusLabel.setText("Not Connected");
        tvAlert.setVisibility(View.GONE);
    }

    // ── Helper to update the connection status bar ────────────────────────────
    private void updateConnectionStatus(String message, String hexColor) {
        tvStatus.setText(message);
        tvStatus.setTextColor(Color.parseColor(hexColor));
    }

    private void showToast(String message) {
        Toast.makeText(this, message, Toast.LENGTH_SHORT).show();
    }

    // ── Clean up when app is closed ───────────────────────────────────────────
    @Override
    protected void onDestroy() {
        super.onDestroy();
        disconnectBluetooth();
    }
}
