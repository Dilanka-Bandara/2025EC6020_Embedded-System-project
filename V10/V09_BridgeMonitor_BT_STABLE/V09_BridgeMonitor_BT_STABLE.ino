/*
 * ================================================================
 *   SMART BRIDGE SAFETY MONITORING SYSTEM  —  V09 (BT-STABLE)
 *   Group 22 | EC6020 Embedded Systems Design
 *   University of Jaffna
 * ================================================================
 *
 *  *** FIXES FROM V08 (RAM) STILL INCLUDED ***
 *  - VIB_SAMPLES = 32, vibBuf is int16_t (saves ~190 bytes RAM)
 *  - splash() uses F() strings
 *
 *  *** NEW IN V09: BLUETOOTH STABILITY FIXES ***
 *
 *  PROBLEM 1: HC-05 auto-disconnects after a few seconds.
 *  CAUSE:  The USB-to-serial chip (CH340/FTDI) on the Arduino
 *          board shares D0/D1 with the HC-05. When both are
 *          connected, they fight over the bus. Also, when the
 *          phone first connects to HC-05, the serial line
 *          transition can trigger the Arduino's auto-reset.
 *  FIX:    >>> UNPLUG THE USB CABLE after uploading! <<<
 *          Power the Arduino from a 9V battery via the barrel
 *          jack (or VIN pin) instead. This removes the USB chip
 *          from the serial bus entirely.
 *
 *  PROBLEM 2: Real-time data doesn't update / connection drops.
 *  CAUSE:  analyzeVibration() blocked for 160-320ms, and
 *          controlBuzzer() had a delay(120). During these
 *          blocking periods, no BT data was sent. The phone
 *          app may interpret silence as a dead connection.
 *  FIX:    - Buzzer is now fully NON-BLOCKING (uses millis()
 *            timing instead of delay()).
 *          - BT send interval reduced from 600ms to 500ms for
 *            more responsive data updates.
 *          - Added a Serial.flush() after sending BT packet to
 *            ensure buffer is pushed out before blocking ops.
 *
 * ================================================================
 *  PINS — DO NOT CHANGE (identical to V03/V08)
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
 *
 *  ⚠ IMPORTANT: Remove HC-05 TX wire from D0 when uploading!
 *  ⚠ IMPORTANT: Unplug USB cable after uploading for BT to work!
 *
 * ================================================================
 *  LIBRARIES
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
#define VIB_SAMPLES          32
#define VIB_SAMPLE_US      5000UL  // 5 ms per sample = 200 Hz rate

// Loop timing
#define SENSOR_MS           300UL
#define DISPLAY_MS          300UL
#define BLUETOOTH_MS        500UL   // Reduced from 600 for faster updates
#define VIBRATION_MS       1200UL

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

// RAM-optimised vibration buffer (int16_t = 2 bytes each)
int16_t vibBuf[VIB_SAMPLES];

unsigned long tLastSensor    = 0;
unsigned long tLastDisplay   = 0;
unsigned long tLastBT        = 0;
unsigned long tLastVib       = 0;

// Non-blocking buzzer state
unsigned long buzzerStartMs  = 0;
uint8_t       buzzerPhase    = 0;  // 0=off, 1=tone1, 2=gap, 3=tone2

// ================================================================
// SETUP
// ================================================================
void setup() {
  Serial.begin(9600);

  // ── WIRE TIMEOUT ─────────────────────────────────────────────
  Wire.begin();
  Wire.setWireTimeout(3000, true);

  // ── PINS ─────────────────────────────────────────────────────
  pinMode(TRIG_PIN,   OUTPUT);
  pinMode(ECHO_PIN,   INPUT);
  pinMode(TILT_PIN,   INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(TRIG_PIN, LOW);

  // ── OLED ─────────────────────────────────────────────────────
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    pinMode(LED_BUILTIN, OUTPUT);
    while (true) {
      digitalWrite(LED_BUILTIN, HIGH); delay(150);
      digitalWrite(LED_BUILTIN, LOW);  delay(150);
    }
  }

  // ── SPLASH ───────────────────────────────────────────────────
  splash(F("Group 22 | EC6020"), F("Initialising..."));

  // ── HX711 ────────────────────────────────────────────────────
  scale.begin(HX711_DOUT, HX711_SCK);
  scale.set_scale(420.0f);

  splash(F("Group 22 | EC6020"), F("Taring scale..."));
  unsigned long t0 = millis();
  while (!scale.is_ready() && (millis() - t0 < 500UL));
  if (scale.is_ready()) {
    scale.tare();
  }

  // ── MPU6050 ──────────────────────────────────────────────────
  splash(F("Group 22 | EC6020"), F("Checking MPU6050"));
  Wire.clearWireTimeoutFlag();

  byte mpuErr = mpu.begin();

  if (Wire.getWireTimeoutFlag() || mpuErr != 0) {
    Wire.clearWireTimeoutFlag();
    mpuFound = false;
    splash(F("Group 22 | EC6020"), F("MPU not found"));
    delay(600);
  } else {
    mpuFound = true;
    // mpu.calcOffsets();  // Uncomment for final deployment
    splash(F("Group 22 | EC6020"), F("MPU6050 OK!"));
    delay(400);
  }

  // ── READY ────────────────────────────────────────────────────
  splash(F("Group 22 | EC6020"), F("System ONLINE!"));
  delay(600);

  // Send initial BT packet so the app knows we're alive
  sendBT();
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

  // B — Vibration analysis every 1200 ms
  //     Blocking time: 32 × 5 ms = 160 ms
  if (mpuFound && (now - tLastVib >= VIBRATION_MS)) {
    tLastVib = now;
    analyzeVibration();
    evaluateAlarms();
  }

  // C — Update OLED every 300 ms
  if (now - tLastDisplay >= DISPLAY_MS) {
    tLastDisplay = now;
    updateDisplay();
  }

  // D — Send BT packet every 500 ms
  if (now - tLastBT >= BLUETOOTH_MS) {
    tLastBT = now;
    sendBT();
  }

  // E — Non-blocking buzzer update (runs every loop iteration)
  updateBuzzer();
}

// ================================================================
// SENSOR FUNCTIONS
// ================================================================

void readUltrasonic() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long dur = pulseIn(ECHO_PIN, HIGH, 30000UL);
  dist_cm  = (dur > 0) ? (int)(dur * 0.034f / 2.0f) : 0;
}

void readTilt() {
  tiltVal = digitalRead(TILT_PIN);
}

void readWeight() {
  if (scale.is_ready()) {
    weightG = scale.get_units(3);
  }
}

void analyzeVibration() {
  long meanAcc = 0;

  for (int i = 0; i < VIB_SAMPLES; i++) {
    unsigned long t0 = micros();
    mpu.update();
    vibBuf[i] = (int16_t)(mpu.getAccZ() * 1000.0f);
    meanAcc += vibBuf[i];
    while ((micros() - t0) < VIB_SAMPLE_US);
  }
  int16_t mean = (int16_t)(meanAcc / VIB_SAMPLES);

  long sumSq = 0;
  for (int i = 0; i < VIB_SAMPLES; i++) {
    int16_t val = vibBuf[i] - mean;
    vibBuf[i] = val;
    sumSq += (long)val * val;
  }
  vibMagG = sqrtf((float)sumSq / VIB_SAMPLES) / 1000.0f;

  int zc = 0;
  for (int i = 1; i < VIB_SAMPLES; i++) {
    if ((vibBuf[i-1] < 0) != (vibBuf[i] < 0)) zc++;
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
    oled.setTextSize(1);

    oled.setCursor(0, 0);
    oled.print(F("Water: "));
    if (dist_cm > 0) { oled.print(dist_cm); oled.print(F(" cm")); }
    else              { oled.print(F("---")); }

    oled.setCursor(0, 11);
    oled.print(F("Tilt:  "));
    oled.print(tiltVal == LOW ? F("TILTED!") : F("Stable"));

    oled.setCursor(0, 22);
    oled.print(F("Freq:  "));
    if (mpuFound) { oled.print(vibFreqHz, 1); oled.print(F(" Hz")); }
    else          { oled.print(F("No MPU")); }

    oled.drawFastHLine(0, 32, 128, SSD1306_WHITE);

    oled.setCursor(0, 35);
    oled.print(F("Load (g):"));
    oled.setTextSize(2);
    oled.setCursor(0, 47);
    oled.print(weightG, 1);
  }

  oled.display();
}

// ================================================================
// NON-BLOCKING BUZZER (replaces the old controlBuzzer with delay)
// ================================================================
void updateBuzzer() {
  if (!anyAlarm) {
    // No alarm — silence
    if (buzzerPhase != 0) {
      noTone(BUZZER_PIN);
      buzzerPhase = 0;
    }
    return;
  }

  if (!alarmResonance) {
    // Non-resonance alarm — simple steady tone, no blocking
    if (buzzerPhase != 10) {
      tone(BUZZER_PIN, 1000);
      buzzerPhase = 10;  // special state for steady tone
    }
    return;
  }

  // Resonance alarm — double-beep pattern, fully non-blocking
  unsigned long elapsed = millis() - buzzerStartMs;

  switch (buzzerPhase) {
    case 0:  // Start new beep cycle
      tone(BUZZER_PIN, 2000);
      buzzerStartMs = millis();
      buzzerPhase = 1;
      break;

    case 1:  // First tone playing (80 ms)
      if (elapsed >= 80) {
        noTone(BUZZER_PIN);
        buzzerStartMs = millis();
        buzzerPhase = 2;
      }
      break;

    case 2:  // Gap between tones (40 ms)
      if (elapsed >= 40) {
        tone(BUZZER_PIN, 1000);
        buzzerStartMs = millis();
        buzzerPhase = 3;
      }
      break;

    case 3:  // Second tone playing (80 ms)
      if (elapsed >= 80) {
        noTone(BUZZER_PIN);
        buzzerStartMs = millis();
        buzzerPhase = 4;
      }
      break;

    case 4:  // Pause before next cycle (300 ms)
      if (elapsed >= 300) {
        buzzerPhase = 0;  // restart cycle
      }
      break;
  }
}

// ================================================================
// BLUETOOTH  (hardware Serial = D0/D1)
// Packet: $BRIDGE,<dist>,<tilt>,<weight>,<freq>,<mag>,<status>#
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

  // Flush ensures the data is fully transmitted before any
  // blocking operation (like vibration sampling) begins.
  // This prevents half-sent packets from corrupting the stream.
  Serial.flush();
}

// ================================================================
// SPLASH SCREEN HELPER
// ================================================================
void splash(const __FlashStringHelper* line1, const __FlashStringHelper* line2) {
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
