/*
 * ============================================================
 *   SMART BRIDGE SAFETY MONITORING SYSTEM  —  V06
 *   Group 22 | EC6020 Embedded Systems Design
 *   University of Jaffna
 * ============================================================
 *
 *  CHANGES FROM V05  (all bug fixes — no pin changes)
 *  ─────────────────────────────────────────────────
 *  FIX 1:  Wire.begin() moved to FIRST line of setup()
 *          → was called AFTER display.begin(), which corrupted
 *            the I2C bus and caused the OLED to stay blank.
 *
 *  FIX 2:  analyzeVibration() no longer called inside the
 *          SENSOR_INTERVAL timer block.
 *          → the 128-sample busy-wait took ~640 ms and blocked
 *            the entire loop, stalling OLED updates.
 *          It now runs on its own slower timer (VIB_INTERVAL).
 *
 *  FIX 3:  zSamples[] moved from stack to global memory.
 *          → 512 bytes on the stack + 1 KB SSD1306 buffer was
 *            exhausting the Uno's 2 KB RAM → random crashes.
 *
 *  FIX 4:  tiltPin changed to INPUT_PULLUP.
 *          → plain INPUT left the pin floating → false tilt
 *            alarms triggered on every boot.
 *          TILT_DANGER_STATE changed to LOW accordingly
 *          (SW-420 pulls LOW when tilted with pull-up enabled).
 *
 * ============================================================
 *
 *  PIN CONFIGURATION  (identical to V03 / V05 — unchanged)
 *  ────────────────────────────────────────────────────────
 *  D2  → Tilt Sensor        INPUT_PULLUP
 *  D3  → HX711 DOUT
 *  D4  → HX711 SCK
 *  D5  → Buzzer
 *  D6  → HC-05 RX  (SoftwareSerial — HC-05 TX pin)
 *  D7  → HC-05 TX  (SoftwareSerial — HC-05 RX pin)
 *  D9  → HC-SR04 TRIG
 *  D10 → HC-SR04 ECHO
 *  A4  → I2C SDA  (OLED + MPU6050 share bus)
 *  A5  → I2C SCL  (OLED + MPU6050 share bus)
 *
 *  I2C addresses:  OLED 0x3C  |  MPU6050 0x68  (no conflict)
 *
 *  LIBRARIES REQUIRED
 *  ──────────────────
 *  • Adafruit SSD1306   (OLED)
 *  • Adafruit GFX       (graphics core)
 *  • HX711              (by Bogdan Necula / bogde)
 *  • MPU6050_light      (by rfetick)
 *  • SoftwareSerial     (built-in)
 * ============================================================
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "HX711.h"
#include <MPU6050_light.h>
#include <SoftwareSerial.h>

// ============================================================
// 1. PIN DEFINITIONS  (unchanged from V03)
// ============================================================
const int trigPin       = 9;
const int echoPin       = 10;
const int tiltPin       = 2;
const int LOADCELL_DOUT = 3;
const int LOADCELL_SCK  = 4;
const int buzzerPin     = 5;
const int BT_RX_PIN     = 6;   // Arduino D6 → HC-05 TX
const int BT_TX_PIN     = 7;   // Arduino D7 → HC-05 RX

// ============================================================
// 2. THRESHOLDS & SETTINGS
// ============================================================
const int   CRITICAL_WATER_CM        = 75;
const float CRITICAL_WEIGHT_G        = 1500.0;

// FIX 4: With INPUT_PULLUP, the SW-420 / ball-tilt pulls LOW
//         when tilted.  Change back to HIGH if your specific
//         sensor module has an on-board inverter.
const int   TILT_DANGER_STATE        = LOW;

// Resonance detection
const float BRIDGE_RESONANT_FREQ_HZ  = 10.0;  // Hz — calibrate for your bridge
const float RESONANCE_TOLERANCE_HZ   = 2.0;   // ±Hz band

// Sampling  (FIX 3: reduced to 64 samples to halve RAM usage)
const int   FFT_SAMPLES              = 64;    // was 128 — saves 256 bytes of RAM
const float SAMPLE_RATE_HZ           = 200.0;
const long  SAMPLE_INTERVAL_US       = (long)(1000000.0 / SAMPLE_RATE_HZ); // 5000 µs

// Timers
const unsigned long SENSOR_INTERVAL  = 250;   // ms  — ultrasonic, tilt, weight, OLED
const unsigned long BT_INTERVAL      = 500;   // ms  — bluetooth packet
// FIX 2: vibration analysis runs on its own slower timer
//         64 samples × 5 ms = 320 ms blocking — acceptable on its own timer
const unsigned long VIB_INTERVAL     = 800;   // ms  — vibration / resonance analysis

// ============================================================
// 3. OBJECTS
// ============================================================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

HX711 scale;
MPU6050 mpu(Wire);
SoftwareSerial btSerial(BT_RX_PIN, BT_TX_PIN);

// ============================================================
// 4. GLOBAL VARIABLES
// ============================================================
float calibration_factor = 420.0;

// Sensor readings
int   distance       = 0;
int   tiltState      = LOW;   // safe default
float weight         = 0.0;
float dominantFreq   = 0.0;
float accelMag       = 0.0;

// FIX 3: moved off the stack into global memory (static allocation)
float zSamples[FFT_SAMPLES];

// Alarm flags
bool waterCritical     = false;
bool tiltCritical      = false;
bool weightCritical    = false;
bool resonanceCritical = false;
bool systemCritical    = false;

// Flags
bool mpuOK = false;

// Timing
unsigned long lastSensorRead = 0;
unsigned long lastVibRead    = 0;
unsigned long lastBTSend     = 0;

// ============================================================
// 5. SETUP
// ============================================================
void setup() {
  Serial.begin(9600);

  // ─── FIX 1: Wire.begin() MUST be first — before any I2C device ───
  Wire.begin();

  // Pin modes
  pinMode(trigPin,   OUTPUT);
  pinMode(echoPin,   INPUT);
  // FIX 4: INPUT_PULLUP prevents floating pin / false tilt alarm
  pinMode(tiltPin,   INPUT_PULLUP);
  pinMode(buzzerPin, OUTPUT);

  // ── OLED (now safe — Wire already started) ──
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 failed"));
    for (;;);  // halt — no point continuing without display
  }
  showBootScreen("Initializing...");

  // ── HX711 ──
  scale.begin(LOADCELL_DOUT, LOADCELL_SCK);
  scale.set_scale(calibration_factor);
  showBootScreen("Taring scale...");
  scale.tare();

  // ── MPU6050 (Wire already started above — no duplicate Wire.begin()) ──
  byte mpuStatus = mpu.begin();
  if (mpuStatus != 0) {
    Serial.print(F("MPU6050 error: "));
    Serial.println(mpuStatus);
    showBootScreen("MPU6050 FAILED!");
    delay(1500);
    mpuOK = false;
    // Continue anyway — OLED/HX711/ultrasonic still work
  } else {
    showBootScreen("MPU6050 OK...");
    mpu.calcOffsets();  // keep board still during this call (~1-2 s)
    mpuOK = true;
  }

  // ── HC-05 Bluetooth ──
  btSerial.begin(9600);
  showBootScreen("Bluetooth Ready");
  delay(400);

  showBootScreen("System ONLINE!");
  delay(800);
}

// ============================================================
// 6. MAIN LOOP
// ============================================================
void loop() {
  unsigned long now = millis();

  // ── A. Fast sensor reads + OLED (every 250 ms) ──
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    readUltrasonic();
    readTilt();
    readWeight();
    evaluateAlarms();   // uses last known dominantFreq
    updateOLED();
    controlBuzzer();
  }

  // ── B. Vibration analysis (every 800 ms, FIX 2: own timer) ──
  if (now - lastVibRead >= VIB_INTERVAL) {
    lastVibRead = now;
    if (mpuOK) {
      analyzeVibration();  // ~320 ms blocking — isolated here
    }
  }

  // ── C. Bluetooth packet (every 500 ms) ──
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
  long dur = pulseIn(echoPin, HIGH, 30000UL); // 30 ms timeout
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
 * Collects FFT_SAMPLES (64) Z-axis samples at 200 Hz.
 * Total blocking time = 64 × 5 ms = 320 ms.
 * Called on its own 800 ms timer (FIX 2).
 * Array is global, not stack-allocated (FIX 3).
 */
void analyzeVibration() {
  float zMean = 0.0;

  for (int i = 0; i < FFT_SAMPLES; i++) {
    unsigned long t0 = micros();
    mpu.update();
    zSamples[i] = mpu.getAccZ();
    zMean += zSamples[i];
    // Busy-wait for the rest of the 5 ms slot
    while ((micros() - t0) < (unsigned long)SAMPLE_INTERVAL_US);
  }

  zMean /= FFT_SAMPLES;

  // Remove DC offset
  float sumSq = 0.0;
  for (int i = 0; i < FFT_SAMPLES; i++) {
    zSamples[i] -= zMean;
    sumSq += zSamples[i] * zSamples[i];
  }
  accelMag = sqrt(sumSq / FFT_SAMPLES);

  // Zero-crossing frequency estimate
  int crossings = 0;
  for (int i = 1; i < FFT_SAMPLES; i++) {
    if ((zSamples[i - 1] < 0.0f && zSamples[i] >= 0.0f) ||
        (zSamples[i - 1] >= 0.0f && zSamples[i] < 0.0f)) {
      crossings++;
    }
  }

  float windowSec = (float)FFT_SAMPLES / SAMPLE_RATE_HZ;
  dominantFreq    = (float)crossings / 2.0f / windowSec;

  bool inBand = (dominantFreq >= (BRIDGE_RESONANT_FREQ_HZ - RESONANCE_TOLERANCE_HZ)) &&
                (dominantFreq <= (BRIDGE_RESONANT_FREQ_HZ + RESONANCE_TOLERANCE_HZ));
  resonanceCritical = inBand && (accelMag > 0.1f);

  Serial.print(F("Freq:"));  Serial.print(dominantFreq, 2);
  Serial.print(F("Hz Mag:")); Serial.print(accelMag, 3);
  Serial.print(F("g Res:"));  Serial.println(resonanceCritical ? F("YES") : F("no"));
}

// ============================================================
// 8. ALARM EVALUATION
// ============================================================
void evaluateAlarms() {
  waterCritical  = (distance > 0 && distance < CRITICAL_WATER_CM);
  tiltCritical   = (tiltState == TILT_DANGER_STATE);
  weightCritical = (weight > CRITICAL_WEIGHT_G);
  // resonanceCritical updated inside analyzeVibration()

  systemCritical = waterCritical || tiltCritical || weightCritical || resonanceCritical;
}

// ============================================================
// 9. OLED DISPLAY
// ============================================================
void updateOLED() {
  display.clearDisplay();

  if (systemCritical) {
    // ── ALARM SCREEN ──
    display.setTextSize(2);
    display.setTextColor(WHITE);
    display.setCursor(10, 0);
    display.print(F("WARNING!"));
    display.drawLine(0, 17, 128, 17, WHITE);

    display.setTextSize(1);
    int y = 22;

    if (waterCritical && y < 55) {
      display.setCursor(0, y);
      display.print(F("WATER: ")); display.print(distance); display.print(F("cm"));
      y += 10;
    }
    if (tiltCritical && y < 55) {
      display.setCursor(0, y);
      display.print(F("TILT DETECTED!"));
      y += 10;
    }
    if (weightCritical && y < 55) {
      display.setCursor(0, y);
      display.print(F("OVERLOAD: ")); display.print(weight, 0); display.print(F("g"));
      y += 10;
    }
    if (resonanceCritical && y < 55) {
      display.setCursor(0, y);
      display.print(F("RESONANCE:")); display.print(dominantFreq, 1); display.print(F("Hz"));
    }

  } else {
    // ── NORMAL SCREEN ──
    display.setTextColor(WHITE);
    display.drawLine(0, 32, 128, 32, WHITE);

    display.setTextSize(1);

    // Top half
    display.setCursor(0, 1);
    display.print(F("Water: "));
    display.print(distance);
    display.print(F(" cm"));

    display.setCursor(0, 12);
    display.print(F("Tilt:  "));
    // FIX 4: logic flipped to match INPUT_PULLUP (LOW = tilted)
    display.print(tiltState == LOW ? F("CHECK!") : F("Stable"));

    display.setCursor(0, 22);
    display.print(F("Freq:  "));
    display.print(dominantFreq, 1);
    display.print(F(" Hz"));

    // Bottom half
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
    tone(buzzerPin, 2000, 80);
    delay(120);
    tone(buzzerPin, 1000, 80);
  } else if (systemCritical) {
    tone(buzzerPin, 1000);
  } else {
    noTone(buzzerPin);
  }
}

// ============================================================
// 11. BLUETOOTH
//  Packet: $BRIDGE,<dist>,<tilt>,<weight>,<freq>,<status>#
// ============================================================
void sendBluetoothPacket() {
  String status;
  if (tiltCritical || resonanceCritical) {
    status = F("DANGER");
  } else if (waterCritical || weightCritical) {
    status = F("WARNING");
  } else {
    status = F("SAFE");
  }

  // Send to HC-05
  btSerial.print('$');
  btSerial.print(F("BRIDGE,"));
  btSerial.print(distance);
  btSerial.print(',');
  // FIX 4: tilt logic flipped — LOW = tilted (INPUT_PULLUP)
  btSerial.print(tiltState == TILT_DANGER_STATE ? 1 : 0);
  btSerial.print(',');
  btSerial.print(weight, 1);
  btSerial.print(',');
  btSerial.print(dominantFreq, 2);
  btSerial.print(',');
  btSerial.print(status);
  btSerial.print('#');
  btSerial.println();

  // Mirror to USB Serial for debugging
  Serial.print(F("BT> D:")); Serial.print(distance);
  Serial.print(F(" T:"));    Serial.print(tiltState == TILT_DANGER_STATE ? 1 : 0);
  Serial.print(F(" W:"));    Serial.print(weight, 1);
  Serial.print(F(" F:"));    Serial.print(dominantFreq, 2);
  Serial.print(F(" S:"));    Serial.println(status);
}

// ============================================================
// 12. BOOT SCREEN HELPER
// ============================================================
void showBootScreen(const char* msg) {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 18);
  display.println(F("Smart Bridge System"));
  display.println(F("Group 22 | EC6020"));
  display.drawLine(0, 37, 128, 37, WHITE);
  display.setCursor(8, 44);
  display.println(msg);
  display.display();
  delay(500);
}
