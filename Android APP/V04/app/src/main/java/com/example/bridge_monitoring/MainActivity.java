package com.example.bridge_monitoring;

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
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowInsetsCompat;

import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Set;
import java.util.UUID;

public class MainActivity extends AppCompatActivity {

    private static final UUID BT_UUID      = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB");
    private static final int  REQUEST_BT   = 1;
    private static final int  REQUEST_PERM = 2;

    private BluetoothAdapter btAdapter;
    private BluetoothSocket  btSocket;
    private InputStream      btInputStream;
    private volatile boolean isConnected = false;

    private TextView     tvConnectionStatus, tvWater, tvWeight, tvTilt, tvAlert, tvRawData, tvStatusLabel;
    private Button       btnConnect, btnDisconnect;
    private LinearLayout layoutStatusBanner, layoutWaterCard, layoutWeightCard, layoutTiltCard;

    private final StringBuilder dataBuffer = new StringBuilder();
    private final Handler       uiHandler  = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        ViewCompat.setOnApplyWindowInsetsListener(findViewById(R.id.main), (v, insets) -> {
            Insets systemBars = insets.getInsets(WindowInsetsCompat.Type.systemBars());
            v.setPadding(systemBars.left, systemBars.top, systemBars.right, systemBars.bottom);
            return insets;
        });

        bindViews();
        setupButtons();
        checkBluetooth();
    }

    private void bindViews() {
        tvConnectionStatus = findViewById(R.id.tvConnectionStatus);
        tvWater            = findViewById(R.id.tvWater);
        tvWeight           = findViewById(R.id.tvWeight);
        tvTilt             = findViewById(R.id.tvTilt);
        tvAlert            = findViewById(R.id.tvAlert);
        tvRawData          = findViewById(R.id.tvRawData);
        tvStatusLabel      = findViewById(R.id.tvStatusLabel);
        btnConnect         = findViewById(R.id.btnConnect);
        btnDisconnect      = findViewById(R.id.btnDisconnect);
        layoutStatusBanner = findViewById(R.id.layoutStatusBanner);
        layoutWaterCard    = findViewById(R.id.layoutWaterCard);
        layoutWeightCard   = findViewById(R.id.layoutWeightCard);
        layoutTiltCard     = findViewById(R.id.layoutTiltCard);
    }

    private void setupButtons() {
        btnConnect.setOnClickListener(v -> showDevicePicker());
        btnDisconnect.setOnClickListener(v -> disconnect());
    }

    private void checkBluetooth() {
        btAdapter = BluetoothAdapter.getDefaultAdapter();
        if (btAdapter == null) {
            toast("Bluetooth not supported on this device");
            btnConnect.setEnabled(false);
            return;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (ActivityCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT)
                    != PackageManager.PERMISSION_GRANTED) {
                ActivityCompat.requestPermissions(this,
                        new String[]{Manifest.permission.BLUETOOTH_SCAN,
                                     Manifest.permission.BLUETOOTH_CONNECT}, REQUEST_PERM);
            }
        } else {
            if (ActivityCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION)
                    != PackageManager.PERMISSION_GRANTED) {
                ActivityCompat.requestPermissions(this,
                        new String[]{Manifest.permission.ACCESS_FINE_LOCATION}, REQUEST_PERM);
            }
        }
        if (!btAdapter.isEnabled()) {
            startActivityForResult(new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE), REQUEST_BT);
        }
    }

    private void showDevicePicker() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
                ActivityCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT)
                        != PackageManager.PERMISSION_GRANTED) {
            toast("Please grant Bluetooth permission first");
            checkBluetooth();
            return;
        }

        Set<BluetoothDevice> paired = btAdapter.getBondedDevices();
        if (paired == null || paired.isEmpty()) {
            toast("No paired devices. Pair HC-05 in Settings → Bluetooth (PIN: 1234)");
            return;
        }

        ArrayList<String>          names   = new ArrayList<>();
        ArrayList<BluetoothDevice> devices = new ArrayList<>();
        for (BluetoothDevice d : paired) {
            names.add((d.getName() != null ? d.getName() : "Unknown") + "\n" + d.getAddress());
            devices.add(d);
        }

        ListView lv = new ListView(this);
        lv.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_list_item_1, names));

        AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle("Select HC-05 Device")
                .setView(lv)
                .setNegativeButton("Cancel", null)
                .create();

        lv.setOnItemClickListener((p, v, pos, id) -> {
            dialog.dismiss();
            connectTo(devices.get(pos));
        });
        dialog.show();
    }

    private void connectTo(BluetoothDevice device) {
        setStatus("Connecting...", "#FFA500");
        btnConnect.setEnabled(false);

        new Thread(() -> {
            try {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
                        ActivityCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT)
                                != PackageManager.PERMISSION_GRANTED) {
                    uiHandler.post(() -> toast("Bluetooth permission denied"));
                    return;
                }
                btSocket      = device.createRfcommSocketToServiceRecord(BT_UUID);
                btSocket.connect();
                btInputStream = btSocket.getInputStream();
                isConnected   = true;

                uiHandler.post(() -> {
                    setStatus("Connected to " + device.getName(), "#00C853");
                    btnConnect.setEnabled(false);
                    btnDisconnect.setEnabled(true);
                    toast("Connected!");
                });

                readLoop();

            } catch (IOException e) {
                isConnected = false;
                uiHandler.post(() -> {
                    setStatus("Connection Failed — try again", "#F44336");
                    btnConnect.setEnabled(true);
                    toast("Failed: " + e.getMessage());
                });
            }
        }).start();
    }

    private void readLoop() {
        byte[] buf = new byte[256];
        int    bytes;
        while (isConnected) {
            try {
                bytes = btInputStream.read(buf);
                if (bytes > 0) {
                    dataBuffer.append(new String(buf, 0, bytes));
                    processBuffer();
                }
            } catch (IOException e) {
                if (isConnected) {
                    isConnected = false;
                    uiHandler.post(() -> {
                        setStatus("Connection lost", "#F44336");
                        btnConnect.setEnabled(true);
                        btnDisconnect.setEnabled(false);
                    });
                }
                break;
            }
        }
    }

    // Packet format from ATmega328P: $BRIDGE,<dist>,<tilt>,<weight>,<status>#
    private void processBuffer() {
        String s     = dataBuffer.toString();
        int    start = s.indexOf('$');
        if (start > 0) { dataBuffer.delete(0, start); s = dataBuffer.toString(); start = 0; }
        if (start < 0) { dataBuffer.setLength(0); return; }
        int end = s.indexOf('#');
        if (end > 0) {
            String packet = s.substring(0, end + 1);
            dataBuffer.delete(0, end + 1);
            uiHandler.post(() -> parseAndDisplay(packet));
        }
    }

    private void parseAndDisplay(String packet) {
        try {
            tvRawData.setText("Raw: " + packet);
            String[] parts = packet.replace("$", "").replace("#", "").split(",");
            if (parts.length < 5 || !parts[0].trim().equals("BRIDGE")) return;

            int    dist   = Integer.parseInt(parts[1].trim());
            int    tilt   = Integer.parseInt(parts[2].trim());
            String weight = parts[3].trim();
            String status = parts[4].trim();

            // Water Level
            tvWater.setText(dist + " cm");
            if (dist > 0 && dist < 75) {
                tvWater.setTextColor(Color.parseColor("#FF5252"));
                layoutWaterCard.setBackgroundColor(Color.parseColor("#2C1010"));
            } else if (dist < 100) {
                tvWater.setTextColor(Color.parseColor("#FFB300"));
                layoutWaterCard.setBackgroundColor(Color.parseColor("#2C2010"));
            } else {
                tvWater.setTextColor(Color.parseColor("#00E676"));
                layoutWaterCard.setBackgroundColor(Color.parseColor("#0D2016"));
            }

            // Tilt
            if (tilt == 1) {
                tvTilt.setText("TILTED  ⚠");
                tvTilt.setTextColor(Color.parseColor("#FF5252"));
                layoutTiltCard.setBackgroundColor(Color.parseColor("#2C1010"));
            } else {
                tvTilt.setText("Stable  ✓");
                tvTilt.setTextColor(Color.parseColor("#00E676"));
                layoutTiltCard.setBackgroundColor(Color.parseColor("#0D2016"));
            }

            // Weight
            tvWeight.setText(weight + " g");
            float w = Float.parseFloat(weight);
            if (w > 1500) {
                tvWeight.setTextColor(Color.parseColor("#FF5252"));
                layoutWeightCard.setBackgroundColor(Color.parseColor("#2C1010"));
            } else if (w > 1000) {
                tvWeight.setTextColor(Color.parseColor("#FFB300"));
                layoutWeightCard.setBackgroundColor(Color.parseColor("#2C2010"));
            } else {
                tvWeight.setTextColor(Color.parseColor("#00E676"));
                layoutWeightCard.setBackgroundColor(Color.parseColor("#0D2016"));
            }

            // Status banner
            switch (status) {
                case "DANGER":
                    layoutStatusBanner.setBackgroundColor(Color.parseColor("#B71C1C"));
                    tvStatusLabel.setText("🚨  DANGER — BRIDGE UNSAFE!");
                    tvAlert.setText("CRITICAL ALERT: Evacuate area immediately!");
                    tvAlert.setVisibility(View.VISIBLE);
                    break;
                case "WARNING":
                    layoutStatusBanner.setBackgroundColor(Color.parseColor("#E65100"));
                    tvStatusLabel.setText("⚠  WARNING — Monitor Closely");
                    tvAlert.setText("WARNING: Approaching critical safety levels");
                    tvAlert.setVisibility(View.VISIBLE);
                    break;
                default:
                    layoutStatusBanner.setBackgroundColor(Color.parseColor("#1B5E20"));
                    tvStatusLabel.setText("✅  SAFE — All Systems Normal");
                    tvAlert.setVisibility(View.GONE);
                    break;
            }

        } catch (Exception e) {
            tvRawData.setText("Parse error: " + packet);
        }
    }

    private void disconnect() {
        isConnected = false;
        try {
            if (btInputStream != null) btInputStream.close();
            if (btSocket      != null) btSocket.close();
        } catch (IOException ignored) {}
        setStatus("Disconnected", "#9E9E9E");
        btnConnect.setEnabled(true);
        btnDisconnect.setEnabled(false);
        resetUI();
        toast("Disconnected");
    }

    private void resetUI() {
        tvWater.setText("-- cm");
        tvWeight.setText("-- g");
        tvTilt.setText("--");
        tvRawData.setText("Waiting for data...");
        layoutStatusBanner.setBackgroundColor(Color.parseColor("#1C2833"));
        tvStatusLabel.setText("⚡  Not Connected");
        tvAlert.setVisibility(View.GONE);
        layoutWaterCard.setBackgroundColor(Color.parseColor("#0D1B24"));
        layoutWeightCard.setBackgroundColor(Color.parseColor("#0D1B24"));
        layoutTiltCard.setBackgroundColor(Color.parseColor("#0D1B24"));
    }

    private void setStatus(String msg, String hex) {
        tvConnectionStatus.setText(msg);
        tvConnectionStatus.setTextColor(Color.parseColor(hex));
    }

    private void toast(String msg) { Toast.makeText(this, msg, Toast.LENGTH_SHORT).show(); }

    @Override
    protected void onDestroy() { super.onDestroy(); disconnect(); }
}
