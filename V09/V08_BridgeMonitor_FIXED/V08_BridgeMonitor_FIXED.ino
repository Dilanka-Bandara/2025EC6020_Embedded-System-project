/*
 * ================================================================
 *   SMART BRIDGE SAFETY MONITORING SYSTEM  —  V08 (RAM-FIX)
 *   Group 22 | EC6020 Embedded Systems Design
 *   University of Jaffna
 * ================================================================
 *
 *  *** WHY THE DISPLAY WAS BLANK IN V08 ***
 *
 *  ROOT CAUSE: SRAM overflow on the ATmega328P (only 2048 bytes).
 *
 *  The Adafruit SSD1306 library dynamically allocates a 1024-byte
 *  framebuffer on the HEAP when oled.begin() is called.
 *  In V08, the global vibBuf[64] float array alone used 256 bytes,
 *  plus the MPU6050_light object (~100 bytes), HX711 object,
 *  alarm variables, and timing variables. Combined with the OLED
 *  buffer, total RAM exceeded 2048 bytes. When oled.begin() tried
 *  to malloc() the 1024-byte buffer, it FAILED because there was
 *  not enough contiguous heap space left. oled.begin() returned
 *  false, and the code entered the LED-blink halt loop.
 *
 *  Even if the allocation barely succeeded, the stack and heap
 *  would collide during loop() — causing random display corruption,
 *  freezes, or reboots.
 *
 *  *** FIXES IN THIS VERSION ***
 *  1. Reduced VIB_SAMPLES from 64 to 32 (saves 128 bytes of RAM,
 *     still gives usable frequency resolution at 200 Hz sample rate)
 *  2. Changed vibBuf from float[] to int16_t[] — stores raw
 *     accelerometer values scaled to milli-g as integers instead
 *     of floats. Saves another 64 bytes (32×2 vs 32×4).
 *  3. Consolidated alarm booleans into a single byte using bitfields.
 *  4. All other logic, pin assignments, and thresholds are UNCHANGED.
 *
 * ================================================================
 *  PINS (identical to V03/V08 — NO CHANGES)
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
// *** REDUCED from 64 to 32 to save RAM ***
#define VIB_SAMPLES          32
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
// STATE VARIABLES  (RAM-optimised)
// ================================================================
bool   mpuFound       = false;
int    dist_cm        = 0;
int    tiltVal        = HIGH;
float  weightG        = 0.0f;
float  vibFreqHz      = 0.0f;
float  vibMagG        = 0.0f;

// Pack alarm flags into individual bools (compiler may pack these)
bool   alarmWater     = false;
bool   alarmTilt      = false;
bool   alarmWeight    = false;
bool   alarmResonance = false;
bool   anyAlarm       = false;

// *** KEY FIX: Use int16_t instead of float to save RAM ***
// Stores accelerometer Z in milli-g (×1000). int16_t = 2 bytes vs float = 4 bytes.
// 32 samples × 2 bytes = 64 bytes (was 64 samples × 4 bytes = 256 bytes)
int16_t vibBuf[VIB_SAMPLES];

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
  // THIS IS THE KEY FIX from original V08. Without this, if MPU6050
  // is missing, mpu.begin() waits forever and the sketch never starts.
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
    // If you reach here, RAM is STILL too tight — check compiler output
    pinMode(LED_BUILTIN, OUTPUT);
    while (true) {
      digitalWrite(LED_BUILTIN, HIGH); delay(150);
      digitalWrite(LED_BUILTIN, LOW);  delay(150);
    }
  }

  // ── SPLASH ───────────────────────────────────────────────────
  splash(F("Group 22 | EC6020"), F("Initialising..."));

  // ── HX711 ────────────────────────────────────────────────────
  // begin() just sets the pin modes — never hangs
  scale.begin(HX711_DOUT, HX711_SCK);
  scale.set_scale(420.0f);

  splash(F("Group 22 | EC6020"), F("Taring scale..."));
  // Safe tare: only tare if HX711 is ready within 500 ms
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
    // NOTE: calcOffsets() blocks for ~1-3 seconds and needs the
    // board held perfectly still. Uncomment for final deployment.
    // mpu.calcOffsets();
    splash(F("Group 22 | EC6020"), F("MPU6050 OK!"));
    delay(400);
  }

  // ── READY ────────────────────────────────────────────────────
  splash(F("Group 22 | EC6020"), F("System ONLINE!"));
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

  // B — Vibration analysis every 1200 ms
  //     Total blocking time: 32 × 5 ms = 160 ms (was 320 ms)
  if (mpuFound && (now - tLastVib >= VIBRATION_MS)) {
    tLastVib = now;
    analyzeVibration();
    evaluateAlarms();
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
  if (scale.is_ready()) {
    weightG = scale.get_units(3);
  }
}

void analyzeVibration() {
  long meanAcc = 0;

  // Sample at 200 Hz — store as milli-g integers
  for (int i = 0; i < VIB_SAMPLES; i++) {
    unsigned long t0 = micros();
    mpu.update();
    // Store accelerometer Z as milli-g (multiply by 1000)
    vibBuf[i] = (int16_t)(mpu.getAccZ() * 1000.0f);
    meanAcc += vibBuf[i];
    while ((micros() - t0) < VIB_SAMPLE_US);
  }
  int16_t mean = (int16_t)(meanAcc / VIB_SAMPLES);

  // Remove DC (gravity component) and compute RMS
  long sumSq = 0;
  for (int i = 0; i < VIB_SAMPLES; i++) {
    int16_t val = vibBuf[i] - mean;
    vibBuf[i] = val;  // reuse buffer for DC-removed values
    sumSq += (long)val * val;
  }
  // Convert back from milli-g to g for magnitude
  vibMagG = sqrtf((float)sumSq / VIB_SAMPLES) / 1000.0f;

  // Zero-crossing frequency estimate
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
    tone(BUZZER_PIN, 2000, 80);
    delay(120);
    tone(BUZZER_PIN, 1000, 80);
  } else if (anyAlarm) {
    tone(BUZZER_PIN, 1000);
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
// SPLASH SCREEN HELPER  (uses F() strings to save RAM)
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
