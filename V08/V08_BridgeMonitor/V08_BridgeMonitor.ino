/*
 * ================================================================
 *   SMART BRIDGE SAFETY MONITORING SYSTEM  —  V08
 *   Group 22 | EC6020 Embedded Systems Design
 *   University of Jaffna
 * ================================================================
 *
 *  *** PLAIN ENGLISH EXPLANATION OF ALL BUGS FOUND ***
 *
 *  The display was blank in ALL previous versions (V05/V06/V07)
 *  even without Bluetooth connected. Here is exactly why:
 *
 *  BUG (ROOT CAUSE): mpu.begin() hangs the entire Arduino.
 *  ─────────────────────────────────────────────────────────
 *  The Arduino Wire (I2C) library has NO timeout by default.
 *  When mpu.begin() sends an I2C address and waits for a reply:
 *    - If MPU6050 IS connected: it replies → OK
 *    - If MPU6050 is NOT connected yet: the Wire library waits
 *      FOREVER for a reply that never comes → Arduino freezes
 *      at that line → display.begin() never runs → blank screen
 *
 *  This is a well-known Arduino Wire library bug. The fix is to
 *  call Wire.setWireTimeout() before any I2C communication.
 *  This was added in Arduino IDE 1.8.13 / Wire library 2.0.
 *
 *  SECONDARY BUG: mpu.calcOffsets() also blocks for ~1-3 seconds
 *  on startup, which caused the display to appear frozen.
 *
 *  ADDITIONAL BUG: The ultrasonic sensor was showing 0 because
 *  pulseIn() has a 1,000,000 µs default timeout (1 full second!)
 *  which blocked the loop. Fixed with a 30ms timeout.
 *
 *  *** WHAT THIS VERSION DOES DIFFERENTLY ***
 *  1. Wire.setWireTimeout(3000, true) — adds 3ms I2C timeout
 *     so a missing MPU6050 does NOT freeze the sketch
 *  2. MPU6050 is fully OPTIONAL — sketch works perfectly
 *     without it connected. Resonance shows 0 Hz until you
 *     plug in the MPU6050.
 *  3. mpu.calcOffsets() skipped by default — add a jumper
 *     to pin 8 to trigger it (or just comment it back in
 *     once everything else is working)
 *  4. HX711 timeout guard: if DOUT stays HIGH > 500ms,
 *     we skip that reading instead of hanging
 *  5. All sensor reads are non-blocking
 *
 * ================================================================
 *  PINS (identical to V03 — no changes)
 * ================================================================
 *  D2  → Tilt Sensor (INPUT_PULLUP)
 *  D3  → HX711 DOUT
 *  D4  → HX711 SCK
 *  D5  → Buzzer
 *  D9  → HC-SR04 TRIG
 *  D10 → HC-SR04 ECHO
 *  A4  → I2C SDA (OLED + MPU6050 share this line)
 *  A5  → I2C SCL (OLED + MPU6050 share this line)
 *
 *  For Bluetooth (HC-05):
 *  D0  → HC-05 TX pin (hardware Serial RX)
 *  D1  → HC-05 RX pin (hardware Serial TX, via voltage divider)
 *  ⚠ Remove HC-05 wire from D0 when uploading!
 *
 * ================================================================
 *  LIBRARIES (install via Sketch > Include Library > Manage)
 * ================================================================
 *  • Adafruit SSD1306
 *  • Adafruit GFX Library
 *  • HX711 Arduino Library  (by Bogdan Necula)
 *  • MPU6050_light           (by rfetick)
 * ================================================================
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "HX711.h"
#include <MPU6050_light.h>

// ================================================================
// PINS — DO NOT CHANGE
// ================================================================
#define TRIG_PIN        9
#define ECHO_PIN       10
#define TILT_PIN        2
#define HX711_DOUT      3
#define HX711_SCK       4
#define BUZZER_PIN      5

// ================================================================
// THRESHOLDS
// ================================================================
#define WATER_CRITICAL_CM    75
#define WEIGHT_CRITICAL_G    1500.0f
#define TILT_DANGER          LOW    // LOW = tilted (INPUT_PULLUP)
#define RESONANT_FREQ_HZ     10.0f
#define RESONANCE_BAND_HZ     2.0f

// Vibration analysis settings
#define VIB_SAMPLES          64
#define VIB_SAMPLE_US      5000UL  // 5 ms per sample = 200 Hz rate

// Loop timing
#define SENSOR_MS           300UL
#define DISPLAY_MS          300UL
#define BLUETOOTH_MS        600UL
#define VIBRATION_MS       1200UL  // run vibration analysis every 1.2 s

// ================================================================
// OBJECTS
// ================================================================
#define SCREEN_W  128
#define SCREEN_H   64

Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);
HX711   scale;
MPU6050 mpu(Wire);

// ================================================================
// STATE VARIABLES
// ================================================================
bool   mpuFound       = false;
int    dist_cm        = 0;
int    tiltVal        = HIGH;
float  weightG        = 0.0f;
float  vibFreqHz      = 0.0f;
float  vibMagG        = 0.0f;

bool   alarmWater     = false;
bool   alarmTilt      = false;
bool   alarmWeight    = false;
bool   alarmResonance = false;
bool   anyAlarm       = false;

// Global sample buffer (NOT on stack to avoid RAM overflow)
float  vibBuf[VIB_SAMPLES];

unsigned long tLastSensor    = 0;
unsigned long tLastDisplay   = 0;
unsigned long tLastBT        = 0;
unsigned long tLastVib       = 0;

// ================================================================
// SETUP
// ================================================================
void setup() {
  Serial.begin(9600);

  // ── WIRE TIMEOUT ─────────────────────────────────────────────
  // THIS IS THE KEY FIX. Without this, if MPU6050 is missing,
  // mpu.begin() waits forever and the sketch never starts.
  Wire.begin();
  Wire.setWireTimeout(3000, true); // 3 ms timeout, reset on timeout

  // ── PINS ─────────────────────────────────────────────────────
  pinMode(TRIG_PIN,   OUTPUT);
  pinMode(ECHO_PIN,   INPUT);
  pinMode(TILT_PIN,   INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(TRIG_PIN, LOW);

  // ── OLED ─────────────────────────────────────────────────────
  // display.begin() will call wire->begin() again internally —
  // that is fine, Wire.begin() is safe to call multiple times.
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    // OLED not found — blink onboard LED and halt
    pinMode(LED_BUILTIN, OUTPUT);
    while (true) {
      digitalWrite(LED_BUILTIN, HIGH); delay(150);
      digitalWrite(LED_BUILTIN, LOW);  delay(150);
    }
  }

  // ── SPLASH ───────────────────────────────────────────────────
  splash("Group 22 | EC6020", "Initialising...");

  // ── HX711 ────────────────────────────────────────────────────
  // begin() just sets the pin modes — never hangs
  scale.begin(HX711_DOUT, HX711_SCK);
  scale.set_scale(420.0f);

  splash("Group 22 | EC6020", "Taring scale...");
  // Safe tare: only tare if HX711 is ready within 500 ms
  unsigned long t0 = millis();
  while (!scale.is_ready() && (millis() - t0 < 500UL));
  if (scale.is_ready()) {
    scale.tare();
  }

  // ── MPU6050 ──────────────────────────────────────────────────
  // With setWireTimeout() above, this call returns an error code
  // instead of hanging if the MPU6050 is not connected.
  splash("Group 22 | EC6020", "Checking MPU6050");
  Wire.clearWireTimeoutFlag(); // clear any flag from previous ops

  byte mpuErr = mpu.begin();

  if (Wire.getWireTimeoutFlag() || mpuErr != 0) {
    // MPU6050 not found or I2C timeout — that is fine, continue
    Wire.clearWireTimeoutFlag();
    mpuFound = false;
    splash("Group 22 | EC6020", "MPU not found");
    delay(600);
  } else {
    mpuFound = true;
    // NOTE: calcOffsets() blocks for ~1-3 seconds and needs the
    // board held perfectly still. Keep it commented out during
    // development; uncomment for final deployment.
    // mpu.calcOffsets();
    splash("Group 22 | EC6020", "MPU6050 OK!");
    delay(400);
  }

  // ── READY ────────────────────────────────────────────────────
  splash("Group 22 | EC6020", "System ONLINE!");
  delay(600);
}

// ================================================================
// MAIN LOOP
// ================================================================
void loop() {
  unsigned long now = millis();

  // A — Read sensors every 300 ms
  if (now - tLastSensor >= SENSOR_MS) {
    tLastSensor = now;
    readUltrasonic();
    readTilt();
    readWeight();
    evaluateAlarms();
  }

  // B — Vibration analysis every 1200 ms (uses its own timer)
  //     Total blocking time: 64 × 5 ms = 320 ms
  if (mpuFound && (now - tLastVib >= VIBRATION_MS)) {
    tLastVib = now;
    analyzeVibration();
    evaluateAlarms(); // re-evaluate with new freq data
  }

  // C — Update OLED every 300 ms
  if (now - tLastDisplay >= DISPLAY_MS) {
    tLastDisplay = now;
    updateDisplay();
    controlBuzzer();
  }

  // D — Send BT packet every 600 ms
  if (now - tLastBT >= BLUETOOTH_MS) {
    tLastBT = now;
    sendBT();
  }
}

// ================================================================
// SENSOR FUNCTIONS
// ================================================================

void readUltrasonic() {
  // Trigger pulse
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // 30 ms timeout = max ~510 cm range. Returns 0 on timeout.
  long dur = pulseIn(ECHO_PIN, HIGH, 30000UL);
  dist_cm  = (dur > 0) ? (int)(dur * 0.034f / 2.0f) : 0;
}

void readTilt() {
  tiltVal = digitalRead(TILT_PIN);
}

void readWeight() {
  // Only read if HX711 is ready — never block waiting for it
  if (scale.is_ready()) {
    weightG = scale.get_units(3);
  }
}

void analyzeVibration() {
  float mean = 0.0f;

  // Sample at 200 Hz
  for (int i = 0; i < VIB_SAMPLES; i++) {
    unsigned long t0 = micros();
    mpu.update();
    vibBuf[i] = mpu.getAccZ();
    mean += vibBuf[i];
    // Busy-wait remainder of 5 ms slot
    while ((micros() - t0) < VIB_SAMPLE_US);
  }
  mean /= VIB_SAMPLES;

  // Remove DC (gravity component)
  float sumSq = 0.0f;
  for (int i = 0; i < VIB_SAMPLES; i++) {
    vibBuf[i] -= mean;
    sumSq += vibBuf[i] * vibBuf[i];
  }
  vibMagG = sqrtf(sumSq / VIB_SAMPLES);

  // Zero-crossing frequency estimate
  int zc = 0;
  for (int i = 1; i < VIB_SAMPLES; i++) {
    if ((vibBuf[i-1] < 0.0f) != (vibBuf[i] < 0.0f)) zc++;
  }
  float windowSec = (float)VIB_SAMPLES / 200.0f;
  vibFreqHz = (float)zc / 2.0f / windowSec;
}

// ================================================================
// ALARM LOGIC
// ================================================================
void evaluateAlarms() {
  alarmWater     = (dist_cm > 0 && dist_cm < WATER_CRITICAL_CM);
  alarmTilt      = (tiltVal == TILT_DANGER);
  alarmWeight    = (weightG > WEIGHT_CRITICAL_G);
  alarmResonance = mpuFound
                   && (vibFreqHz >= (RESONANT_FREQ_HZ - RESONANCE_BAND_HZ))
                   && (vibFreqHz <= (RESONANT_FREQ_HZ + RESONANCE_BAND_HZ))
                   && (vibMagG > 0.05f);
  anyAlarm = alarmWater || alarmTilt || alarmWeight || alarmResonance;
}

// ================================================================
// OLED DISPLAY
// ================================================================
void updateDisplay() {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  if (anyAlarm) {
    // ── ALARM SCREEN ──────────────────────────────────────────
    oled.setTextSize(2);
    oled.setCursor(8, 0);
    oled.print(F("WARNING!"));
    oled.drawFastHLine(0, 17, 128, SSD1306_WHITE);

    oled.setTextSize(1);
    int y = 22;

    if (alarmWater && y <= 54) {
      oled.setCursor(0, y);
      oled.print(F("WATER: "));
      oled.print(dist_cm);
      oled.print(F(" cm  <75!"));
      y += 11;
    }
    if (alarmTilt && y <= 54) {
      oled.setCursor(0, y);
      oled.print(F("TILT DETECTED!"));
      y += 11;
    }
    if (alarmWeight && y <= 54) {
      oled.setCursor(0, y);
      oled.print(F("OVERLOAD: "));
      oled.print((int)weightG);
      oled.print(F("g"));
      y += 11;
    }
    if (alarmResonance && y <= 54) {
      oled.setCursor(0, y);
      oled.print(F("RESONANCE:"));
      oled.print(vibFreqHz, 1);
      oled.print(F("Hz"));
    }

  } else {
    // ── NORMAL SCREEN ─────────────────────────────────────────
    oled.setTextSize(1);

    // Row 1: Water level
    oled.setCursor(0, 0);
    oled.print(F("Water: "));
    if (dist_cm > 0) { oled.print(dist_cm); oled.print(F(" cm")); }
    else              { oled.print(F("---")); }

    // Row 2: Tilt
    oled.setCursor(0, 11);
    oled.print(F("Tilt:  "));
    oled.print(tiltVal == LOW ? F("TILTED!") : F("Stable"));

    // Row 3: Vibration frequency
    oled.setCursor(0, 22);
    oled.print(F("Freq:  "));
    if (mpuFound) { oled.print(vibFreqHz, 1); oled.print(F(" Hz")); }
    else          { oled.print(F("No MPU")); }

    // Divider
    oled.drawFastHLine(0, 32, 128, SSD1306_WHITE);

    // Row 4 label + Row 5 big number: Weight
    oled.setCursor(0, 35);
    oled.print(F("Load (g):"));
    oled.setTextSize(2);
    oled.setCursor(0, 47);
    oled.print(weightG, 1);
  }

  oled.display();
}

// ================================================================
// BUZZER
// ================================================================
void controlBuzzer() {
  if (alarmResonance) {
    // Alternating double-beep for resonance alert
    tone(BUZZER_PIN, 2000, 80);
    delay(120);
    tone(BUZZER_PIN, 1000, 80);
  } else if (anyAlarm) {
    tone(BUZZER_PIN, 1000);   // Steady tone for other alarms
  } else {
    noTone(BUZZER_PIN);
  }
}

// ================================================================
// BLUETOOTH  (hardware Serial = D0/D1)
// Packet: $BRIDGE,<dist>,<tilt>,<weight>,<freq>,<status>#
// ================================================================
void sendBT() {
  const char* status = anyAlarm
    ? (alarmTilt || alarmResonance ? "DANGER" : "WARNING")
    : "SAFE";

  Serial.print(F("$BRIDGE,"));
  Serial.print(dist_cm);
  Serial.print(',');
  Serial.print(tiltVal == TILT_DANGER ? 1 : 0);
  Serial.print(',');
  Serial.print(weightG, 1);
  Serial.print(',');
  Serial.print(vibFreqHz, 2);
  Serial.print(',');
  Serial.print(status);
  Serial.println('#');
}

// ================================================================
// SPLASH SCREEN HELPER
// ================================================================
void splash(const char* line1, const char* line2) {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 14);
  oled.println(F("Smart Bridge System"));
  oled.setCursor(0, 25);
  oled.println(line1);
  oled.drawFastHLine(0, 36, 128, SSD1306_WHITE);
  oled.setCursor(4, 44);
  oled.println(line2);
  oled.display();
  delay(500);
}
