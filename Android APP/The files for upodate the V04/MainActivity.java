package com.example.bridge_monitoring;

import android.Manifest;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothSocket;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.media.AudioAttributes;
import android.media.AudioManager;
import android.media.ToneGenerator;
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

    // ─────────────────────────────────────────────────────────────
    //  CONSTANTS  –  keep in sync with V05_BridgeMonitor.ino
    // ─────────────────────────────────────────────────────────────
    private static final UUID  BT_UUID             = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB");
    private static final int   REQUEST_BT          = 1;
    private static final int   REQUEST_PERM        = 2;

    private static final int   WATER_DANGER_CM     = 75;    // below this = DANGER
    private static final int   WATER_WARNING_CM    = 100;   // below this = WARNING
    private static final float WEIGHT_DANGER_G     = 1500f; // above this = DANGER
    private static final float WEIGHT_WARNING_G    = 1000f; // above this = WARNING
    private static final float RESONANT_FREQ_HZ    = 10.0f; // must match Arduino constant
    private static final float RESONANCE_BAND_HZ   = 2.0f;  // ±Hz tolerance band

    // Alarm sound config
    private static final int ALARM_BEEP_MS         = 800;   // ms per beep
    private static final int ALARM_INTERVAL_MS     = 1500;  // ms between beep cycles

    // ─────────────────────────────────────────────────────────────
    //  BLUETOOTH FIELDS
    // ─────────────────────────────────────────────────────────────
    private BluetoothAdapter btAdapter;
    private BluetoothSocket  btSocket;
    private InputStream      btInputStream;
    private volatile boolean isConnected = false;

    // ─────────────────────────────────────────────────────────────
    //  ALARM STATE
    // ─────────────────────────────────────────────────────────────
    private ToneGenerator    toneGen;
    private boolean          alarmSilenced   = false;
    private boolean          alarmActive     = false;
    private Runnable         alarmRunnable;
    private final Handler    alarmHandler    = new Handler(Looper.getMainLooper());

    // ─────────────────────────────────────────────────────────────
    //  UI REFERENCES
    // ─────────────────────────────────────────────────────────────
    private TextView     tvLiveDot, tvLiveLabel;
    private TextView     tvStatusIcon, tvStatusLabel, tvStatusSub;
    private LinearLayout layoutStatusBanner;

    // Alarm panel
    private LinearLayout layoutAlarmPanel;
    private TextView     tvAlarmWater, tvAlarmWeight, tvAlarmTilt, tvAlarmFreq;
    private Button       btnSilenceAlarm;

    // Sensor cards + value TextViews + status sub-labels
    private LinearLayout layoutWaterCard,  layoutWeightCard,
                         layoutTiltCard,   layoutFreqCard;
    private TextView     tvWater,   tvWaterStatus;
    private TextView     tvWeight,  tvWeightStatus;
    private TextView     tvTilt,    tvTiltStatus;
    private TextView     tvFreq,    tvFreqStatus;

    // Connection controls
    private TextView     tvConnectionStatus;
    private Button       btnConnect, btnDisconnect;
    private TextView     tvRawData;

    // ─────────────────────────────────────────────────────────────
    //  DATA BUFFER
    // ─────────────────────────────────────────────────────────────
    private final StringBuilder dataBuffer = new StringBuilder();
    private final Handler       uiHandler  = new Handler(Looper.getMainLooper());

    // ═════════════════════════════════════════════════════════════
    //  LIFECYCLE
    // ═════════════════════════════════════════════════════════════
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        ViewCompat.setOnApplyWindowInsetsListener(findViewById(R.id.main), (v, insets) -> {
            Insets sys = insets.getInsets(WindowInsetsCompat.Type.systemBars());
            v.setPadding(sys.left, sys.top, sys.right, sys.bottom);
            return insets;
        });

        bindViews();
        setupButtons();
        initToneGenerator();
        checkBluetooth();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        disconnect();
        stopAlarm();
        if (toneGen != null) { toneGen.release(); toneGen = null; }
    }

    // ═════════════════════════════════════════════════════════════
    //  VIEW BINDING
    // ═════════════════════════════════════════════════════════════
    private void bindViews() {
        tvLiveDot            = findViewById(R.id.tvLiveDot);
        tvLiveLabel          = findViewById(R.id.tvLiveLabel);

        layoutStatusBanner   = findViewById(R.id.layoutStatusBanner);
        tvStatusIcon         = findViewById(R.id.tvStatusIcon);
        tvStatusLabel        = findViewById(R.id.tvStatusLabel);
        tvStatusSub          = findViewById(R.id.tvStatusSub);

        layoutAlarmPanel     = findViewById(R.id.layoutAlarmPanel);
        tvAlarmWater         = findViewById(R.id.tvAlarmWater);
        tvAlarmWeight        = findViewById(R.id.tvAlarmWeight);
        tvAlarmTilt          = findViewById(R.id.tvAlarmTilt);
        tvAlarmFreq          = findViewById(R.id.tvAlarmFreq);
        btnSilenceAlarm      = findViewById(R.id.btnSilenceAlarm);

        layoutWaterCard      = findViewById(R.id.layoutWaterCard);
        layoutWeightCard     = findViewById(R.id.layoutWeightCard);
        layoutTiltCard       = findViewById(R.id.layoutTiltCard);
        layoutFreqCard       = findViewById(R.id.layoutFreqCard);

        tvWater              = findViewById(R.id.tvWater);
        tvWaterStatus        = findViewById(R.id.tvWaterStatus);
        tvWeight             = findViewById(R.id.tvWeight);
        tvWeightStatus       = findViewById(R.id.tvWeightStatus);
        tvTilt               = findViewById(R.id.tvTilt);
        tvTiltStatus         = findViewById(R.id.tvTiltStatus);
        tvFreq               = findViewById(R.id.tvFreq);
        tvFreqStatus         = findViewById(R.id.tvFreqStatus);

        tvConnectionStatus   = findViewById(R.id.tvConnectionStatus);
        btnConnect           = findViewById(R.id.btnConnect);
        btnDisconnect        = findViewById(R.id.btnDisconnect);
        tvRawData            = findViewById(R.id.tvRawData);
    }

    private void setupButtons() {
        btnConnect.setOnClickListener(v -> showDevicePicker());
        btnDisconnect.setOnClickListener(v -> disconnect());
        btnSilenceAlarm.setOnClickListener(v -> silenceAlarm());
    }

    // ═════════════════════════════════════════════════════════════
    //  BLUETOOTH
    // ═════════════════════════════════════════════════════════════
    private void checkBluetooth() {
        btAdapter = BluetoothAdapter.getDefaultAdapter();
        if (btAdapter == null) {
            toast("Bluetooth not supported");
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
            toast("Grant Bluetooth permission first");
            checkBluetooth();
            return;
        }
        Set<BluetoothDevice> paired = btAdapter.getBondedDevices();
        if (paired == null || paired.isEmpty()) {
            toast("No paired devices — pair HC-05 in Settings → Bluetooth (PIN: 1234)");
            return;
        }
        ArrayList<String> names     = new ArrayList<>();
        ArrayList<BluetoothDevice> devices = new ArrayList<>();
        for (BluetoothDevice d : paired) {
            names.add((d.getName() != null ? d.getName() : "Unknown") + "\n" + d.getAddress());
            devices.add(d);
        }
        ListView lv = new ListView(this);
        lv.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_list_item_1, names));
        AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle("Select HC-05 Device")
                .setView(lv).setNegativeButton("Cancel", null).create();
        lv.setOnItemClickListener((p, v, pos, id) -> { dialog.dismiss(); connectTo(devices.get(pos)); });
        dialog.show();
    }

    private void connectTo(BluetoothDevice device) {
        setConnectionStatus("Connecting...", "#FFA500");
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
                    setConnectionStatus("Connected · " + device.getName(), "#00C853");
                    btnConnect.setEnabled(false);
                    btnDisconnect.setEnabled(true);
                    setLiveIndicator(true);
                    toast("Connected!");
                });
                readLoop();
            } catch (IOException e) {
                isConnected = false;
                uiHandler.post(() -> {
                    setConnectionStatus("Failed — try again", "#F44336");
                    btnConnect.setEnabled(true);
                    setLiveIndicator(false);
                    toast("Failed: " + e.getMessage());
                });
            }
        }).start();
    }

    private void readLoop() {
        byte[] buf = new byte[256];
        int bytes;
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
                        setConnectionStatus("Connection lost", "#F44336");
                        btnConnect.setEnabled(true);
                        btnDisconnect.setEnabled(false);
                        setLiveIndicator(false);
                        stopAlarm();
                        resetUI();
                    });
                }
                break;
            }
        }
    }

    // ═════════════════════════════════════════════════════════════
    //  PACKET PARSING
    //  Format: $BRIDGE,<dist>,<tilt>,<weight>,<freq>,<status>#
    //           [0]     [1]    [2]    [3]      [4]    [5]
    // ═════════════════════════════════════════════════════════════
    private void processBuffer() {
        String s = dataBuffer.toString();
        int start = s.indexOf('$');
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
            tvRawData.setText(packet);

            String[] parts = packet.replace("$", "").replace("#", "").split(",");
            if (parts.length < 6 || !parts[0].trim().equals("BRIDGE")) return;

            int    dist   = Integer.parseInt(parts[1].trim());
            int    tilt   = Integer.parseInt(parts[2].trim());
            float  weight = Float.parseFloat(parts[3].trim());
            float  freq   = Float.parseFloat(parts[4].trim());
            String status = parts[5].trim();

            // ── Evaluate each sensor independently ──
            boolean waterDanger   = (dist > 0 && dist < WATER_DANGER_CM);
            boolean waterWarning  = (dist > 0 && dist >= WATER_DANGER_CM && dist < WATER_WARNING_CM);
            boolean weightDanger  = (weight > WEIGHT_DANGER_G);
            boolean weightWarning = (weight > WEIGHT_WARNING_G && weight <= WEIGHT_DANGER_G);
            boolean tiltDanger    = (tilt == 1);
            boolean inResonance   = (freq >= (RESONANT_FREQ_HZ - RESONANCE_BAND_HZ))
                                 && (freq <= (RESONANT_FREQ_HZ + RESONANCE_BAND_HZ))
                                 && (freq > 0.5f);
            boolean anyDanger  = waterDanger  || weightDanger  || tiltDanger || inResonance;
            boolean anyWarning = waterWarning || weightWarning;

            // ─── 1. UPDATE SENSOR CARDS ───────────────────────────────
            updateWaterCard(dist, waterDanger, waterWarning);
            updateWeightCard(weight, weightDanger, weightWarning);
            updateTiltCard(tilt == 1);
            updateFreqCard(freq, inResonance);

            // ─── 2. UPDATE STATUS BANNER ──────────────────────────────
            updateStatusBanner(anyDanger, anyWarning, status);

            // ─── 3. UPDATE ALARM PANEL ────────────────────────────────
            updateAlarmPanel(dist, weight, freq,
                             waterDanger, weightDanger, tiltDanger, inResonance, anyDanger);

            // ─── 4. TRIGGER / STOP SOUND ALARM ───────────────────────
            if (anyDanger && !alarmSilenced) {
                if (!alarmActive) startAlarm();
            } else if (!anyDanger) {
                alarmSilenced = false;   // auto-reset silence when condition clears
                stopAlarm();
            }

        } catch (Exception e) {
            tvRawData.setText("Parse error: " + packet);
        }
    }

    // ═════════════════════════════════════════════════════════════
    //  SENSOR CARD UPDATERS
    // ═════════════════════════════════════════════════════════════
    private void updateWaterCard(int dist, boolean danger, boolean warning) {
        tvWater.setText(dist + " cm");
        if (danger) {
            applyCardDanger(layoutWaterCard);
            tvWater.setTextColor(Color.parseColor("#FF1744"));
            tvWaterStatus.setText("⚠ CRITICAL — TOO HIGH");
            tvWaterStatus.setTextColor(Color.parseColor("#FF1744"));
        } else if (warning) {
            applyCardWarning(layoutWaterCard);
            tvWater.setTextColor(Color.parseColor("#FFAB00"));
            tvWaterStatus.setText("⚡ WARNING — RISING");
            tvWaterStatus.setTextColor(Color.parseColor("#FFAB00"));
        } else {
            applyCardNormal(layoutWaterCard);
            tvWater.setTextColor(Color.parseColor("#00E676"));
            tvWaterStatus.setText("✓ Safe");
            tvWaterStatus.setTextColor(Color.parseColor("#00897B"));
        }
    }

    private void updateWeightCard(float weight, boolean danger, boolean warning) {
        tvWeight.setText(String.format("%.0f g", weight));
        if (danger) {
            applyCardDanger(layoutWeightCard);
            tvWeight.setTextColor(Color.parseColor("#FF1744"));
            tvWeightStatus.setText("⚠ OVERLOAD!");
            tvWeightStatus.setTextColor(Color.parseColor("#FF1744"));
        } else if (warning) {
            applyCardWarning(layoutWeightCard);
            tvWeight.setTextColor(Color.parseColor("#FFAB00"));
            tvWeightStatus.setText("⚡ HIGH LOAD");
            tvWeightStatus.setTextColor(Color.parseColor("#FFAB00"));
        } else {
            applyCardNormal(layoutWeightCard);
            tvWeight.setTextColor(Color.parseColor("#00E676"));
            tvWeightStatus.setText("✓ Normal");
            tvWeightStatus.setTextColor(Color.parseColor("#00897B"));
        }
    }

    private void updateTiltCard(boolean tilted) {
        if (tilted) {
            applyCardDanger(layoutTiltCard);
            tvTilt.setText("TILTED ⚠");
            tvTilt.setTextColor(Color.parseColor("#FF1744"));
            tvTiltStatus.setText("⚠ STRUCTURAL ALERT");
            tvTiltStatus.setTextColor(Color.parseColor("#FF1744"));
        } else {
            applyCardNormal(layoutTiltCard);
            tvTilt.setText("Stable ✓");
            tvTilt.setTextColor(Color.parseColor("#00E676"));
            tvTiltStatus.setText("✓ No tilt detected");
            tvTiltStatus.setTextColor(Color.parseColor("#00897B"));
        }
    }

    private void updateFreqCard(float freq, boolean inResonance) {
        tvFreq.setText(String.format("%.1f Hz", freq));
        if (inResonance) {
            applyCardDanger(layoutFreqCard);
            tvFreq.setTextColor(Color.parseColor("#FF1744"));
            tvFreqStatus.setText("⚠ RESONANCE!");
            tvFreqStatus.setTextColor(Color.parseColor("#FF1744"));
        } else if (freq > 0.5f) {
            applyCardWarning(layoutFreqCard);
            tvFreq.setTextColor(Color.parseColor("#FFAB00"));
            tvFreqStatus.setText("⚡ Vibrating");
            tvFreqStatus.setTextColor(Color.parseColor("#FFAB00"));
        } else {
            applyCardNormal(layoutFreqCard);
            tvFreq.setTextColor(Color.parseColor("#00E676"));
            tvFreqStatus.setText("✓ Calm");
            tvFreqStatus.setTextColor(Color.parseColor("#00897B"));
        }
    }

    // ═════════════════════════════════════════════════════════════
    //  STATUS BANNER UPDATER
    // ═════════════════════════════════════════════════════════════
    private void updateStatusBanner(boolean danger, boolean warning, String status) {
        if (danger || "DANGER".equals(status)) {
            layoutStatusBanner.setBackgroundColor(Color.parseColor("#B71C1C"));
            tvStatusIcon.setText("🚨");
            tvStatusLabel.setText("DANGER — BRIDGE UNSAFE");
            tvStatusSub.setText("Immediate action required — evacuate area");
            tvStatusSub.setTextColor(Color.parseColor("#EF9A9A"));
        } else if (warning || "WARNING".equals(status)) {
            layoutStatusBanner.setBackgroundColor(Color.parseColor("#E65100"));
            tvStatusIcon.setText("⚠️");
            tvStatusLabel.setText("WARNING — Monitor Closely");
            tvStatusSub.setText("One or more parameters approaching critical levels");
            tvStatusSub.setTextColor(Color.parseColor("#FFCC80"));
        } else {
            layoutStatusBanner.setBackgroundColor(Color.parseColor("#1B5E20"));
            tvStatusIcon.setText("✅");
            tvStatusLabel.setText("SAFE — All Systems Normal");
            tvStatusSub.setText("All sensor readings within safe limits");
            tvStatusSub.setTextColor(Color.parseColor("#A5D6A7"));
        }
    }

    // ═════════════════════════════════════════════════════════════
    //  ALARM PANEL UPDATER
    // ═════════════════════════════════════════════════════════════
    private void updateAlarmPanel(int dist, float weight, float freq,
                                  boolean waterDanger, boolean weightDanger,
                                  boolean tiltDanger, boolean freqDanger,
                                  boolean anyDanger) {
        if (!anyDanger) {
            layoutAlarmPanel.setVisibility(View.GONE);
            tvAlarmWater.setVisibility(View.GONE);
            tvAlarmWeight.setVisibility(View.GONE);
            tvAlarmTilt.setVisibility(View.GONE);
            tvAlarmFreq.setVisibility(View.GONE);
            return;
        }

        layoutAlarmPanel.setVisibility(View.VISIBLE);

        if (waterDanger) {
            tvAlarmWater.setText("💧  HIGH WATER LEVEL — " + dist + " cm  (Critical < " + WATER_DANGER_CM + " cm)");
            tvAlarmWater.setVisibility(View.VISIBLE);
        } else {
            tvAlarmWater.setVisibility(View.GONE);
        }

        if (weightDanger) {
            tvAlarmWeight.setText("⚖️  OVERLOAD — " + String.format("%.0f", weight) + " g  (Critical > " + (int) WEIGHT_DANGER_G + " g)");
            tvAlarmWeight.setVisibility(View.VISIBLE);
        } else {
            tvAlarmWeight.setVisibility(View.GONE);
        }

        if (tiltDanger) {
            tvAlarmTilt.setVisibility(View.VISIBLE);
        } else {
            tvAlarmTilt.setVisibility(View.GONE);
        }

        if (freqDanger) {
            tvAlarmFreq.setText("〰️  RESONANT FREQUENCY — " + String.format("%.1f", freq) + " Hz  (Resonance ≈ " + (int) RESONANT_FREQ_HZ + " Hz)");
            tvAlarmFreq.setVisibility(View.VISIBLE);
        } else {
            tvAlarmFreq.setVisibility(View.GONE);
        }
    }

    // ═════════════════════════════════════════════════════════════
    //  SOUND ALARM
    // ═════════════════════════════════════════════════════════════
    private void initToneGenerator() {
        try {
            toneGen = new ToneGenerator(AudioManager.STREAM_ALARM, 100);
        } catch (Exception e) {
            toneGen = null;
        }
    }

    private void startAlarm() {
        if (alarmActive) return;
        alarmActive = true;

        alarmRunnable = new Runnable() {
            @Override
            public void run() {
                if (!alarmActive) return;
                if (toneGen != null) {
                    toneGen.startTone(ToneGenerator.TONE_CDMA_EMERGENCY_RINGBACK, ALARM_BEEP_MS);
                }
                alarmHandler.postDelayed(this, ALARM_INTERVAL_MS);
            }
        };
        alarmHandler.post(alarmRunnable);
    }

    private void stopAlarm() {
        alarmActive = false;
        alarmHandler.removeCallbacks(alarmRunnable);
        if (toneGen != null) toneGen.stopTone();
    }

    private void silenceAlarm() {
        alarmSilenced = true;
        stopAlarm();
        toast("Alarm silenced — will re-activate if new danger detected");
    }

    // ═════════════════════════════════════════════════════════════
    //  CARD BACKGROUND HELPERS
    // ═════════════════════════════════════════════════════════════
    private void applyCardDanger(LinearLayout card) {
        card.setBackground(getDrawable(R.drawable.alarm_border));
    }
    private void applyCardWarning(LinearLayout card) {
        card.setBackground(getDrawable(R.drawable.warning_border));
    }
    private void applyCardNormal(LinearLayout card) {
        card.setBackground(getDrawable(R.drawable.rounded_card));
    }

    // ═════════════════════════════════════════════════════════════
    //  UI HELPERS
    // ═════════════════════════════════════════════════════════════
    private void setLiveIndicator(boolean live) {
        if (live) {
            tvLiveDot.setTextColor(Color.parseColor("#00E676"));
            tvLiveLabel.setText("LIVE");
            tvLiveLabel.setTextColor(Color.parseColor("#00E676"));
        } else {
            tvLiveDot.setTextColor(Color.parseColor("#37474F"));
            tvLiveLabel.setText("OFFLINE");
            tvLiveLabel.setTextColor(Color.parseColor("#37474F"));
        }
    }

    private void setConnectionStatus(String msg, String hex) {
        tvConnectionStatus.setText(msg);
        tvConnectionStatus.setTextColor(Color.parseColor(hex));
    }

    private void disconnect() {
        isConnected = false;
        stopAlarm();
        try {
            if (btInputStream != null) btInputStream.close();
            if (btSocket      != null) btSocket.close();
        } catch (IOException ignored) {}
        setConnectionStatus("Disconnected", "#607D8B");
        btnConnect.setEnabled(true);
        btnDisconnect.setEnabled(false);
        setLiveIndicator(false);
        resetUI();
        toast("Disconnected");
    }

    private void resetUI() {
        // Status banner
        layoutStatusBanner.setBackgroundColor(Color.parseColor("#1C2833"));
        tvStatusIcon.setText("⚡");
        tvStatusLabel.setText("NOT CONNECTED");
        tvStatusSub.setText("Tap Connect to begin monitoring");
        tvStatusSub.setTextColor(Color.parseColor("#90A4AE"));

        // Alarm panel
        layoutAlarmPanel.setVisibility(View.GONE);

        // Cards
        applyCardNormal(layoutWaterCard);
        applyCardNormal(layoutWeightCard);
        applyCardNormal(layoutTiltCard);
        applyCardNormal(layoutFreqCard);

        tvWater.setText("-- cm");     tvWaterStatus.setText("—");
        tvWeight.setText("-- g");     tvWeightStatus.setText("—");
        tvTilt.setText("--");         tvTiltStatus.setText("—");
        tvFreq.setText("-- Hz");      tvFreqStatus.setText("—");

        String grey = "#37474F";
        tvWater.setTextColor(Color.parseColor(grey));
        tvWaterStatus.setTextColor(Color.parseColor(grey));
        tvWeight.setTextColor(Color.parseColor(grey));
        tvWeightStatus.setTextColor(Color.parseColor(grey));
        tvTilt.setTextColor(Color.parseColor(grey));
        tvTiltStatus.setTextColor(Color.parseColor(grey));
        tvFreq.setTextColor(Color.parseColor(grey));
        tvFreqStatus.setTextColor(Color.parseColor(grey));

        tvRawData.setText("Waiting for data...");
    }

    private void toast(String msg) { Toast.makeText(this, msg, Toast.LENGTH_SHORT).show(); }
}
