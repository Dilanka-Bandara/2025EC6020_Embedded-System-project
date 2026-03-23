/*
 * ============================================================
 *   SMART BRIDGE SAFETY MONITORING SYSTEM  —  V07
 *   Group 22 | EC6020 Embedded Systems Design
 *   University of Jaffna
 * ============================================================
 *
 *  ROOT CAUSE OF BLANK DISPLAY (discovered in V05/V06)
 *  ────────────────────────────────────────────────────
 *  SoftwareSerial uses pin-change interrupts to receive bytes.
 *  The HX711 library disables ALL global interrupts (cli/sei)
 *  during its 24-bit bit-bang clock sequence.
 *  When both are active simultaneously, HX711's cli() blocks
 *  SoftwareSerial's ISR → SoftwareSerial corrupts its state →
 *  the sketch hangs inside scale.tare() / scale.get_units()
 *  waiting for HX711 DOUT to go LOW, which never happens.
 *  The display never gets past setup(), so it stays blank.
 *
 *  FIX  —  Use hardware UART (pins 0 & 1) for HC-05
 *  ────────────────────────────────────────────────────
 *  Hardware UART is interrupt-driven but handles cli/sei
 *  correctly — it does NOT conflict with HX711.
 *  • HC-05 TX  →  Arduino D0 (RX)
 *  • HC-05 RX  →  Arduino D1 (TX)  via 1kΩ+2kΩ divider
 *  SoftwareSerial is completely removed.
 *  Debug prints are sent only during setup before HC-05 is
 *  active; in loop(), Serial is the BT channel.
 *
 *  ALL OTHER BUGS FROM V06 ARE KEPT FIXED:
 *  • Wire.begin() first  (blank OLED fix)
 *  • analyzeVibration on its own timer  (stall fix)
 *  • zSamples[] global  (RAM fix)
 *  • INPUT_PULLUP on tiltPin  (false alarm fix)
 *
 * ============================================================
 *  PIN CONFIGURATION  (unchanged from V03 except HC-05)
 * ============================================================
 *  D0  → HC-05 TX  (hardware RX — was free in V03)
 *  D1  → HC-05 RX  (hardware TX via divider — was free in V03)
 *  D2  → Tilt Sensor  (INPUT_PULLUP)
 *  D3  → HX711 DOUT
 *  D4  → HX711 SCK
 *  D5  → Buzzer
 *  D9  → HC-SR04 TRIG
 *  D10 → HC-SR04 ECHO
 *  A4  → I2C SDA  (OLED + MPU6050)
 *  A5  → I2C SCL  (OLED + MPU6050)
 *
 *  D6, D7 — now FREE (SoftwareSerial removed)
 *
 *  I2C addresses:  OLED 0x3C  |  MPU6050 0x68
 *
 *  ⚠ IMPORTANT: Disconnect HC-05 from D0/D1 while uploading!
 *    The bootloader uses the same UART. Reconnect after upload.
 *
 * ============================================================
 *  LIBRARIES REQUIRED
 * ============================================================
 *  • Adafruit SSD1306   (OLED)
 *  • Adafruit GFX
 *  • HX711              (by Bogdan Necula / bogde)
 *  • MPU6050_light      (by rfetick)
 *  NO SoftwareSerial needed
 * ============================================================
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "HX711.h"
#include <MPU6050_light.h>

// ============================================================
// 1. PIN DEFINITIONS
// ============================================================
const int trigPin       = 9;
const int echoPin       = 10;
const int tiltPin       = 2;    // INPUT_PULLUP
const int LOADCELL_DOUT = 3;
const int LOADCELL_SCK  = 4;
const int buzzerPin     = 5;
// HC-05 uses D0 (RX) and D1 (TX) — hardware UART = Serial
// No pin constants needed — just use Serial.print()

// ============================================================
// 2. THRESHOLDS
// ============================================================
const int   CRITICAL_WATER_CM       = 75;
const float CRITICAL_WEIGHT_G       = 1500.0;
const int   TILT_DANGER_STATE       = LOW;   // LOW with INPUT_PULLUP

const float BRIDGE_RESONANT_FREQ_HZ = 10.0;
const float RESONANCE_TOLERANCE_HZ  = 2.0;

// Vibration sampling (64 samples × 5 ms = 320 ms blocking)
const int   FFT_SAMPLES             = 64;
const float SAMPLE_RATE_HZ          = 200.0;
const long  SAMPLE_INTERVAL_US      = 5000L; // 1 000 000 / 200

// Loop timers
const unsigned long SENSOR_INTERVAL = 300UL;  // ms
const unsigned long BT_INTERVAL     = 600UL;  // ms
const unsigned long VIB_INTERVAL    = 1000UL; // ms — vibration on own timer

// ============================================================
// 3. OBJECTS
// ============================================================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

HX711    scale;
MPU6050  mpu(Wire);

// ============================================================
// 4. GLOBALS
// ============================================================
float calibration_factor = 420.0;

int   distance       = 0;
int   tiltState      = HIGH;  // HIGH = safe with INPUT_PULLUP
float weight         = 0.0;
float dominantFreq   = 0.0;
float accelMag       = 0.0;

// Global sample buffer (not on stack — avoids RAM overflow)
float zSamples[FFT_SAMPLES];

bool waterCritical     = false;
bool tiltCritical      = false;
bool weightCritical    = false;
bool resonanceCritical = false;
bool systemCritical    = false;
bool mpuOK             = false;

unsigned long lastSensorRead = 0;
unsigned long lastVibRead    = 0;
unsigned long lastBTSend     = 0;

// ============================================================
// 5. SETUP
// ============================================================
void setup() {
  // ── Wire FIRST — before any I2C device ──
  Wire.begin();

  // Hardware UART for HC-05 (and debug before BT connected)
  Serial.begin(9600);

  // Pin modes
  pinMode(trigPin,   OUTPUT);
  pinMode(echoPin,   INPUT);
  pinMode(tiltPin,   INPUT_PULLUP); // pull-up → resting HIGH, tilted LOW
  pinMode(buzzerPin, OUTPUT);

  // ── OLED ──
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    // Can't show anything — blink LED 13 to signal failure
    pinMode(LED_BUILTIN, OUTPUT);
    while (1) { digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN)); delay(200); }
  }
  showBootScreen("HX711 init...");

  // ── HX711 ──
  // At this point HC-05 is NOT yet sending data over Serial,
  // so no interrupt conflict exists during tare.
  scale.begin(LOADCELL_DOUT, LOADCELL_SCK);
  scale.set_scale(calibration_factor);
  showBootScreen("Taring scale...");
  scale.tare();  // Safe — no SoftwareSerial ISR competing

  // ── MPU6050 ──
  showBootScreen("MPU6050 init...");
  byte mpuStatus = mpu.begin();
  if (mpuStatus != 0) {
    showBootScreen("MPU6050 FAIL");
    delay(1200);
    mpuOK = false;
  } else {
    showBootScreen("Calibrating IMU");
    mpu.calcOffsets(); // keep board still ~1 s
    mpuOK = true;
    showBootScreen("MPU6050 OK");
    delay(400);
  }

  showBootScreen("System ONLINE!");
  delay(600);
  display.clearDisplay();
  display.display();
}

// ============================================================
// 6. MAIN LOOP
// ============================================================
void loop() {
  unsigned long now = millis();

  // A — Fast sensors + OLED every 300 ms
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    readUltrasonic();
    readTilt();
    readWeight();
    evaluateAlarms();
    updateOLED();
    controlBuzzer();
  }

  // B — Vibration analysis every 1000 ms (320 ms blocking, own timer)
  if (now - lastVibRead >= VIB_INTERVAL) {
    lastVibRead = now;
    if (mpuOK) analyzeVibration();
  }

  // C — Bluetooth packet every 600 ms
  //     Serial.print() here goes to HC-05 via hardware UART
  //     No interrupt conflict with HX711
  if (now - lastBTSend >= BT_INTERVAL) {
    lastBTSend = now;
    sendBluetoothPacket();
  }
}

// ============================================================
// 7. SENSOR READERS
// ============================================================
void readUltrasonic() {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  long dur = pulseIn(echoPin, HIGH, 30000UL);
  distance = (dur > 0) ? (int)(dur * 0.034 / 2.0) : 0;
}

void readTilt() {
  tiltState = digitalRead(tiltPin);
}

void readWeight() {
  if (scale.is_ready()) {
    weight = scale.get_units(3);
  }
}

void analyzeVibration() {
  float zMean = 0.0;

  for (int i = 0; i < FFT_SAMPLES; i++) {
    unsigned long t0 = micros();
    mpu.update();
    zSamples[i] = mpu.getAccZ();
    zMean += zSamples[i];
    while ((micros() - t0) < (unsigned long)SAMPLE_INTERVAL_US);
  }

  zMean /= FFT_SAMPLES;

  float sumSq = 0.0;
  for (int i = 0; i < FFT_SAMPLES; i++) {
    zSamples[i] -= zMean;
    sumSq += zSamples[i] * zSamples[i];
  }
  accelMag = sqrt(sumSq / FFT_SAMPLES);

  int crossings = 0;
  for (int i = 1; i < FFT_SAMPLES; i++) {
    if ((zSamples[i-1] < 0.0f && zSamples[i] >= 0.0f) ||
        (zSamples[i-1] >= 0.0f && zSamples[i] < 0.0f)) {
      crossings++;
    }
  }

  float windowSec = (float)FFT_SAMPLES / SAMPLE_RATE_HZ;
  dominantFreq    = (float)crossings / 2.0f / windowSec;

  bool inBand = (dominantFreq >= (BRIDGE_RESONANT_FREQ_HZ - RESONANCE_TOLERANCE_HZ)) &&
                (dominantFreq <= (BRIDGE_RESONANT_FREQ_HZ + RESONANCE_TOLERANCE_HZ));
  resonanceCritical = inBand && (accelMag > 0.1f);
}

// ============================================================
// 8. ALARM EVALUATION
// ============================================================
void evaluateAlarms() {
  waterCritical  = (distance > 0 && distance < CRITICAL_WATER_CM);
  tiltCritical   = (tiltState == TILT_DANGER_STATE);
  weightCritical = (weight > CRITICAL_WEIGHT_G);
  systemCritical = waterCritical || tiltCritical || weightCritical || resonanceCritical;
}

// ============================================================
// 9. OLED DISPLAY
// ============================================================
void updateOLED() {
  display.clearDisplay();
  display.setTextColor(WHITE);

  if (systemCritical) {
    display.setTextSize(2);
    display.setCursor(10, 0);
    display.print(F("WARNING!"));
    display.drawLine(0, 17, 128, 17, WHITE);
    display.setTextSize(1);
    int y = 22;
    if (waterCritical    && y < 56) { display.setCursor(0,y); display.print(F("WATER: ")); display.print(distance); display.print(F("cm")); y+=10; }
    if (tiltCritical     && y < 56) { display.setCursor(0,y); display.print(F("TILT DETECTED!")); y+=10; }
    if (weightCritical   && y < 56) { display.setCursor(0,y); display.print(F("LOAD: ")); display.print(weight,0); display.print(F("g")); y+=10; }
    if (resonanceCritical&& y < 56) { display.setCursor(0,y); display.print(F("RESONANCE:")); display.print(dominantFreq,1); display.print(F("Hz")); }
  } else {
    display.setTextSize(1);
    display.drawLine(0, 32, 128, 32, WHITE);
    display.setCursor(0,  1); display.print(F("Water: ")); display.print(distance); display.print(F(" cm"));
    display.setCursor(0, 12); display.print(F("Tilt:  ")); display.print(tiltState == LOW ? F("CHECK!") : F("Stable"));
    display.setCursor(0, 22); display.print(F("Freq:  ")); display.print(dominantFreq, 1); display.print(F(" Hz"));
    display.setCursor(0, 36); display.print(F("Load (g):"));
    display.setTextSize(2);
    display.setCursor(0, 48); display.print(weight, 1);
  }

  display.display();
}

// ============================================================
// 10. BUZZER
// ============================================================
void controlBuzzer() {
  if (resonanceCritical) {
    tone(buzzerPin, 2000, 80); delay(130); tone(buzzerPin, 1000, 80);
  } else if (systemCritical) {
    tone(buzzerPin, 1000);
  } else {
    noTone(buzzerPin);
  }
}

// ============================================================
// 11. BLUETOOTH via hardware Serial
//  Packet: $BRIDGE,<dist>,<tilt>,<weight>,<freq>,<status>#
// ============================================================
void sendBluetoothPacket() {
  String status;
  if      (tiltCritical || resonanceCritical) status = F("DANGER");
  else if (waterCritical || weightCritical)   status = F("WARNING");
  else                                         status = F("SAFE");

  Serial.print(F("$BRIDGE,"));
  Serial.print(distance);
  Serial.print(',');
  Serial.print(tiltState == TILT_DANGER_STATE ? 1 : 0);
  Serial.print(',');
  Serial.print(weight, 1);
  Serial.print(',');
  Serial.print(dominantFreq, 2);
  Serial.print(',');
  Serial.print(status);
  Serial.println('#');
}

// ============================================================
// 12. BOOT SCREEN
// ============================================================
void showBootScreen(const char* msg) {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 16);
  display.println(F("Smart Bridge System"));
  display.println(F("Group 22 | EC6020"));
  display.drawLine(0, 36, 128, 36, WHITE);
  display.setCursor(8, 44);
  display.println(msg);
  display.display();
  delay(500);
}
