/*
 * ============================================================
 *   SMART BRIDGE SAFETY MONITORING SYSTEM  — V05
 *   Group 22 | EC6020 Embedded Systems Design
 *   University of Jaffna
 * ============================================================
 *
 * PIN CONFIGURATION (UNCHANGED FROM V03)
 * ────────────────────────────────────────
 *  D2  → Tilt Sensor (SW-420 or ball tilt)
 *  D3  → HX711 DOUT  (Strain gauge data)
 *  D4  → HX711 SCK   (Strain gauge clock)
 *  D5  → Buzzer
 *  D9  → Ultrasonic TRIG (HC-SR04)
 *  D10 → Ultrasonic ECHO (HC-SR04)
 *  A4  → I2C SDA  (OLED + MPU6050 share bus)
 *  A5  → I2C SCL  (OLED + MPU6050 share bus)
 *
 * NEW ADDITIONS
 * ────────────────────────────────────────
 *  D6  → HC-05 RX  (connect to HC-05 TX pin)  [SoftwareSerial]
 *  D7  → HC-05 TX  (connect to HC-05 RX pin)  [SoftwareSerial]
 *  MPU6050 → A4/A5 (I2C — shares bus with OLED, I2C addr 0x68)
 *
 * BLUETOOTH PACKET FORMAT (to Android App)
 * ────────────────────────────────────────
 *  $BRIDGE,<dist_cm>,<tilt_0/1>,<weight_g>,<freq_hz>,<status>#
 *  status: SAFE | WARNING | DANGER
 *
 * RESONANCE DETECTION CONCEPT
 * ────────────────────────────────────────
 *  Bridge resonance frequency is set in BRIDGE_RESONANT_FREQ_HZ.
 *  The MPU6050's Z-axis vibration is sampled at ~200 Hz for
 *  FFT_SAMPLES cycles. A lightweight zero-crossing counter
 *  estimates the dominant frequency. If it falls within
 *  ±RESONANCE_TOLERANCE_HZ of the resonant frequency, a
 *  resonance alarm is triggered.
 *
 * LIBRARIES REQUIRED (install via Arduino Library Manager)
 * ────────────────────────────────────────
 *  • Adafruit SSD1306   (OLED display)
 *  • Adafruit GFX       (graphics core)
 *  • HX711              (by bogde)
 *  • MPU6050_light      (by rfetick) — lightweight, no heavy DSP
 *  • SoftwareSerial     (built-in Arduino)
 * ============================================================
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "HX711.h"
#include <MPU6050_light.h>    // Lightweight MPU6050 library
#include <SoftwareSerial.h>

// ============================================================
// 1. PIN DEFINITIONS  (V03 pins PRESERVED — do not change)
// ============================================================
const int trigPin          = 9;   // HC-SR04 TRIG
const int echoPin          = 10;  // HC-SR04 ECHO
const int tiltPin          = 2;   // Tilt / Vibration sensor
const int LOADCELL_DOUT    = 3;   // HX711 data
const int LOADCELL_SCK     = 4;   // HX711 clock
const int buzzerPin        = 5;   // Buzzer

// NEW: HC-05 Bluetooth (SoftwareSerial — pins 6 & 7 are free)
const int BT_RX_PIN        = 6;   // Arduino D6 → HC-05 TX
const int BT_TX_PIN        = 7;   // Arduino D7 → HC-05 RX
// NOTE: MPU6050 uses I2C (A4=SDA, A5=SCL) — same bus as OLED

// ============================================================
// 2. THRESHOLDS & SETTINGS
// ============================================================
const int   CRITICAL_WATER_CM   = 40;    // cm — flood alarm
const float CRITICAL_WEIGHT_G   = 1500.0; // g  — overload alarm
const int   TILT_DANGER_STATE   = HIGH;  // change to LOW if sensor inverted

// Resonance Detection
// Set this to your bridge model's resonant frequency in Hz.
// For a real bridge: measure with FFT analyzer or engineering data.
// For lab demo with popsicle-stick model: typically 8–15 Hz.
const float BRIDGE_RESONANT_FREQ_HZ = 10.0; // Hz — ADJUST FOR YOUR BRIDGE
const float RESONANCE_TOLERANCE_HZ  = 2.0;  // ±Hz tolerance band

// FFT sampling
const int   FFT_SAMPLES      = 128;   // number of accel samples per analysis window
const float SAMPLE_RATE_HZ   = 200.0; // target sampling rate (Hz)
const long  SAMPLE_INTERVAL_US = (long)(1000000.0 / SAMPLE_RATE_HZ); // µs between samples

// Warning threshold for vibration magnitude (before resonance)
const float VIBRATION_WARN_G = 0.5;   // g — raw accel magnitude warning

// ============================================================
// 3. OBJECT DECLARATIONS
// ============================================================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET   -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

HX711 scale;
MPU6050 mpu(Wire);
SoftwareSerial btSerial(BT_RX_PIN, BT_TX_PIN); // RX, TX

// ============================================================
// 4. GLOBAL VARIABLES
// ============================================================
float calibration_factor = 420.0;  // HX711 calibration — adjust as needed

// Sensor readings
int   distance    = 0;
int   tiltState   = 0;
float weight      = 0.0;
float dominantFreq = 0.0;   // Hz — estimated from zero-crossing
float accelMag    = 0.0;    // g  — raw vibration magnitude

// Alarm flags
bool waterCritical    = false;
bool tiltCritical     = false;
bool weightCritical   = false;
bool resonanceCritical = false;
bool systemCritical   = false;

// Timing
unsigned long lastSensorRead = 0;
unsigned long lastBTSend     = 0;
const unsigned long SENSOR_INTERVAL = 250; // ms
const unsigned long BT_INTERVAL     = 500; // ms — send BT packet every 500ms

// ============================================================
// 5. SETUP
// ============================================================
void setup() {
  Serial.begin(9600);

  // Pin modes
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
  pinMode(tiltPin, INPUT);
  pinMode(buzzerPin, OUTPUT);

  // --- OLED Init ---
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;);
  }
  showBootScreen("Initializing...");

  // --- HX711 Init ---
  scale.begin(LOADCELL_DOUT, LOADCELL_SCK);
  scale.set_scale(calibration_factor);
  showBootScreen("Taring scale...");
  scale.tare();

  // --- MPU6050 Init ---
  Wire.begin();
  byte mpuStatus = mpu.begin();
  if (mpuStatus != 0) {
    showBootScreen("MPU6050 FAILED!");
    Serial.print(F("MPU6050 error: "));
    Serial.println(mpuStatus);
    delay(2000);
    // Continue anyway — system still works without resonance
  } else {
    showBootScreen("MPU6050 OK");
    delay(500);
    mpu.calcOffsets(); // Auto-calibrate gyro & accel offsets (keep board still!)
  }

  // --- HC-05 Bluetooth Init ---
  btSerial.begin(9600); // Default HC-05 baud rate
  showBootScreen("Bluetooth Ready");
  delay(500);

  showBootScreen("System ONLINE");
  delay(1000);
}

// ============================================================
// 6. MAIN LOOP
// ============================================================
void loop() {
  unsigned long now = millis();

  // --- A. Read sensors every SENSOR_INTERVAL ms ---
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;

    readUltrasonic();
    readTilt();
    readWeight();
    analyzeVibration();   // MPU6050 resonance analysis
    evaluateAlarms();
    updateOLED();
    controlBuzzer();
  }

  // --- B. Send Bluetooth packet every BT_INTERVAL ms ---
  if (now - lastBTSend >= BT_INTERVAL) {
    lastBTSend = now;
    sendBluetoothPacket();
  }
}

// ============================================================
// 7. SENSOR FUNCTIONS
// ============================================================

void readUltrasonic() {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  long dur = pulseIn(echoPin, HIGH, 30000); // 30ms timeout
  distance = (dur > 0) ? (int)(dur * 0.034 / 2) : 0;
}

void readTilt() {
  tiltState = digitalRead(tiltPin);
}

void readWeight() {
  if (scale.is_ready()) {
    weight = scale.get_units(3);
  }
}

/*
 * analyzeVibration()
 * ──────────────────
 * Samples MPU6050 acceleration at ~200 Hz for FFT_SAMPLES points.
 * Uses a zero-crossing counter on the Z-axis to estimate dominant
 * vibration frequency. This is a lightweight approach — suitable
 * for an Arduino Uno which cannot run a full FFT in real time.
 *
 * For higher accuracy: consider Arduino Due / ESP32 with arduinoFFT library.
 *
 * Formula:  freq = (crossings / 2) / time_window
 */
void analyzeVibration() {
  float zSamples[FFT_SAMPLES];
  float zMean = 0.0;

  // Collect samples at target rate
  for (int i = 0; i < FFT_SAMPLES; i++) {
    unsigned long t0 = micros();
    mpu.update();
    zSamples[i] = mpu.getAccZ();
    zMean += zSamples[i];

    // Busy-wait for next sample slot
    while ((micros() - t0) < (unsigned long)SAMPLE_INTERVAL_US);
  }

  zMean /= FFT_SAMPLES;

  // Remove DC offset (mean subtraction)
  for (int i = 0; i < FFT_SAMPLES; i++) {
    zSamples[i] -= zMean;
  }

  // Compute RMS magnitude for alarm threshold
  float sumSq = 0.0;
  for (int i = 0; i < FFT_SAMPLES; i++) {
    sumSq += zSamples[i] * zSamples[i];
  }
  accelMag = sqrt(sumSq / FFT_SAMPLES);

  // Zero-crossing frequency estimation
  int crossings = 0;
  for (int i = 1; i < FFT_SAMPLES; i++) {
    if ((zSamples[i - 1] < 0.0 && zSamples[i] >= 0.0) ||
        (zSamples[i - 1] >= 0.0 && zSamples[i] < 0.0)) {
      crossings++;
    }
  }

  // Time for the full window in seconds
  float windowSec = (float)FFT_SAMPLES / SAMPLE_RATE_HZ;
  // Each full cycle = 2 crossings
  dominantFreq = (float)crossings / 2.0f / windowSec;

  // Resonance check: only alarm if there is also significant vibration
  bool inResonanceBand = (dominantFreq >= (BRIDGE_RESONANT_FREQ_HZ - RESONANCE_TOLERANCE_HZ)) &&
                         (dominantFreq <= (BRIDGE_RESONANT_FREQ_HZ + RESONANCE_TOLERANCE_HZ));
  resonanceCritical = inResonanceBand && (accelMag > 0.1f); // must have real vibration

  // Debug to serial
  Serial.print(F("Freq:")); Serial.print(dominantFreq, 2);
  Serial.print(F("Hz | Mag:")); Serial.print(accelMag, 3);
  Serial.print(F("g | Resonance:"));
  Serial.println(resonanceCritical ? F("YES!") : F("no"));
}

// ============================================================
// 8. ALARM EVALUATION
// ============================================================
void evaluateAlarms() {
  waterCritical  = (distance > 0 && distance < CRITICAL_WATER_CM);
  tiltCritical   = (tiltState == TILT_DANGER_STATE);
  weightCritical = (weight > CRITICAL_WEIGHT_G);
  // resonanceCritical is set inside analyzeVibration()

  systemCritical = waterCritical || tiltCritical || weightCritical || resonanceCritical;
}

// ============================================================
// 9. OLED DISPLAY
// ============================================================
void updateOLED() {
  display.clearDisplay();

  if (systemCritical) {
    // ── WARNING SCREEN ──
    display.setTextSize(2);
    display.setCursor(10, 0);
    display.print(F("WARNING!"));
    display.drawLine(0, 17, 128, 17, WHITE);

    display.setTextSize(1);
    int y = 22;

    if (waterCritical) {
      display.setCursor(0, y);
      display.print(F("WATER HI: "));
      display.print(distance);
      display.print(F("cm"));
      y += 10;
    }
    if (tiltCritical) {
      display.setCursor(0, y);
      display.print(F("TILT DETECTED!"));
      y += 10;
    }
    if (weightCritical) {
      display.setCursor(0, y);
      display.print(F("OVERLOAD: "));
      display.print(weight, 0);
      display.print(F("g"));
      y += 10;
    }
    if (resonanceCritical) {
      display.setCursor(0, y);
      display.print(F("RESONANCE:"));
      display.print(dominantFreq, 1);
      display.print(F("Hz!"));
    }

  } else {
    // ── NORMAL SCREEN ──
    display.drawLine(0, 32, 128, 32, WHITE);

    // TOP: water & tilt
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print(F("Water Lvl: "));
    display.print(distance);
    display.print(F(" cm"));

    display.setCursor(0, 12);
    display.print(F("Bridge: "));
    display.print(tiltState == LOW ? F("Stable") : F("CHECK!"));

    display.setCursor(0, 22);
    display.print(F("Freq: "));
    display.print(dominantFreq, 1);
    display.print(F("Hz "));
    display.print(accelMag < 0.1f ? F("OK") : F("Vib"));

    // BOTTOM: weight
    display.setTextSize(1);
    display.setCursor(0, 36);
    display.print(F("Load (g):"));

    display.setTextSize(2);
    display.setCursor(0, 48);
    display.print(weight, 1);
  }

  display.display();
}

// ============================================================
// 10. BUZZER CONTROL
// ============================================================
void controlBuzzer() {
  if (resonanceCritical) {
    // Rapid alternating tone for resonance — distinctive from other alarms
    tone(buzzerPin, 2000, 100);
    delay(150);
    tone(buzzerPin, 1000, 100);
  } else if (systemCritical) {
    tone(buzzerPin, 1000); // Steady 1kHz for other critical conditions
  } else {
    noTone(buzzerPin);
  }
}

// ============================================================
// 11. BLUETOOTH PACKET TRANSMISSION
// ============================================================
/*
 * Packet format (matches Android app parser):
 *   $BRIDGE,<dist>,<tilt>,<weight>,<freq>,<status>#
 *
 *   dist   = integer cm
 *   tilt   = 0 (stable) or 1 (tilted)
 *   weight = float with 1 decimal
 *   freq   = dominant vibration frequency (float, 2 decimals)
 *   status = SAFE | WARNING | DANGER
 *
 * The Android app's parseAndDisplay() currently expects 5 fields.
 * After adding <freq>, it will have 6 fields — update app accordingly
 * (see notes at the end of this file).
 */
void sendBluetoothPacket() {
  // Determine status string
  String status;
  if (resonanceCritical || tiltCritical) {
    status = "DANGER";
  } else if (waterCritical || weightCritical) {
    status = "WARNING";
  } else {
    status = "SAFE";
  }

  // Build packet
  btSerial.print('$');
  btSerial.print("BRIDGE,");
  btSerial.print(distance);
  btSerial.print(',');
  btSerial.print(tiltState == TILT_DANGER_STATE ? 1 : 0);
  btSerial.print(',');
  btSerial.print(weight, 1);
  btSerial.print(',');
  btSerial.print(dominantFreq, 2);  // NEW field
  btSerial.print(',');
  btSerial.print(status);
  btSerial.print('#');
  btSerial.println(); // optional newline for readability

  // Mirror to USB serial for debugging
  Serial.print(F("BT> $BRIDGE,"));
  Serial.print(distance);  Serial.print(',');
  Serial.print(tiltState == TILT_DANGER_STATE ? 1 : 0); Serial.print(',');
  Serial.print(weight, 1); Serial.print(',');
  Serial.print(dominantFreq, 2); Serial.print(',');
  Serial.println(status + "#");
}

// ============================================================
// 12. UTILITY
// ============================================================
void showBootScreen(const char* msg) {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.println(F("Smart Bridge System"));
  display.println(F("Group 22 | EC6020"));
  display.drawLine(0, 38, 128, 38, WHITE);
  display.setCursor(10, 44);
  display.println(msg);
  display.display();
  delay(600);
}

/*
 * ============================================================
 * ANDROID APP UPDATE NOTES
 * ============================================================
 *
 * The new BT packet has 6 comma-separated fields (was 5).
 * In MainActivity.java, update parseAndDisplay() as follows:
 *
 *  OLD check:  if (parts.length < 5 || ...)
 *  NEW check:  if (parts.length < 6 || ...)
 *
 *  OLD fields:  parts[1]=dist, parts[2]=tilt, parts[3]=weight, parts[4]=status
 *  NEW fields:  parts[1]=dist, parts[2]=tilt, parts[3]=weight,
 *               parts[4]=freq,  parts[5]=status
 *
 *  Add these lines to parseAndDisplay() after the weight block:
 *
 *    // --- Vibration / Resonance ---
 *    float freq = Float.parseFloat(parts[4].trim());
 *    String freqStr = String.format("%.1f Hz", freq);
 *    tvFreq.setText(freqStr);  // add a tvFreq TextView to your layout
 *    if (freq >= 8.0f && freq <= 12.0f) {  // resonance band
 *        tvFreq.setTextColor(Color.parseColor("#FF5252"));
 *    } else {
 *        tvFreq.setTextColor(Color.parseColor("#00E676"));
 *    }
 *
 *    // Update status — now at parts[5] instead of parts[4]
 *    String status = parts[5].trim();
 *
 * ============================================================
 *
 * HC-05 WIRING GUIDE
 * ============================================================
 *  HC-05 VCC  → 5V
 *  HC-05 GND  → GND
 *  HC-05 TX   → Arduino D6 (our BT_RX_PIN)
 *  HC-05 RX   → Arduino D7 via 1kΩ–2kΩ voltage divider to GND
 *                (HC-05 RX is 3.3V logic — protect with divider)
 *  HC-05 STATE & KEY → not needed for basic operation
 *
 *  Default HC-05 pairing PIN: 1234  (or 0000)
 *  Baud rate: 9600 bps
 *
 * MPU6050 WIRING GUIDE
 * ============================================================
 *  MPU6050 VCC → 3.3V (some modules have onboard regulator: use 5V)
 *  MPU6050 GND → GND
 *  MPU6050 SDA → A4  (shared with OLED — I2C bus)
 *  MPU6050 SCL → A5  (shared with OLED — I2C bus)
 *  MPU6050 AD0 → GND (sets I2C address to 0x68)
 *  MPU6050 INT → not used (polling mode)
 *
 *  I2C addresses on bus:
 *    OLED    → 0x3C
 *    MPU6050 → 0x68
 *    (no conflict)
 *
 * RESONANT FREQUENCY CALIBRATION
 * ============================================================
 *  1. Build your bridge model and tap it gently.
 *  2. Open Serial Monitor at 9600 baud.
 *  3. Observe the "Freq:" output — the number after tapping
 *     is close to the natural frequency.
 *  4. Set BRIDGE_RESONANT_FREQ_HZ to that value.
 *  5. Adjust RESONANCE_TOLERANCE_HZ if you want a wider/narrower band.
 * ============================================================
 */
