#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "HX711.h"

// --- 1. SCREEN SETTINGS ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- 2. PIN DEFINITIONS ---
// Ultrasonic
const int trigPin = 9;
const int echoPin = 10;
// Tilt Sensor
const int tiltPin = 2;
// HX711 Weight Sensor (BF350 Strain Gauge)
const int LOADCELL_DOUT_PIN = 3;
const int LOADCELL_SCK_PIN = 4;
// Buzzer (NEW)
const int buzzerPin = 5; // Connect your buzzer to digital pin 5

// --- 3. VARIABLES & THRESHOLDS ---
long duration;
int distance;
int tiltState;
float weight;

// ! CRITICAL THRESHOLDS !
const int CRITICAL_WATER_LEVEL_CM = 75; // 0.75m threshold
const float CRITICAL_WEIGHT_G = 1500.0; //  strain gauge's critical stress limit
const int TILT_DANGER_STATE = HIGH;     // Change to LOW if  sensor is active-low when tilted

// --- 4. INITIALIZE HX711 ---
HX711 scale;
float calibration_factor = 420.0; 

void setup() {
  Serial.begin(9600);

  // --- Setup Pins ---
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
  pinMode(tiltPin, INPUT);
  pinMode(buzzerPin, OUTPUT); // Initialize buzzer

  // --- Setup OLED ---
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
    Serial.println(F("SSD1306 allocation failed"));
    for(;;); 
  }
  
  // Show startup message
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(10, 20);
  display.println("System Loading...");
  display.println("Calibrating Sensors");
  display.display();

  // --- Setup HX711 ---
  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  scale.set_scale(calibration_factor);
  scale.tare(); 

  delay(2000); 
}

void loop() {
  // --- A. READ ULTRASONIC SENSOR ---
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  duration = pulseIn(echoPin, HIGH);
  distance = duration * 0.034 / 2;

  // --- B. READ TILT SENSOR ---
  tiltState = digitalRead(tiltPin);

  // --- C. READ WEIGHT (HX711) ---
  weight = scale.get_units(3); 

  // --- D. EVALUATE CRITICAL SITUATIONS ---
  // Note: We check distance > 0 to prevent false alarms if the sensor misreads and returns 0.
  bool waterCritical = (distance > 0 && distance < CRITICAL_WATER_LEVEL_CM);
  bool tiltCritical = (tiltState == TILT_DANGER_STATE);
  bool weightCritical = (weight > CRITICAL_WEIGHT_G);

  bool systemCritical = waterCritical || tiltCritical || weightCritical;

  // --- E. UPDATE OLED & BUZZER ---
  display.clearDisplay();

  if (systemCritical) {
    // --- WARNING MODE ---
    tone(buzzerPin, 1000); // Sound the buzzer at 1000Hz

    // Draw a prominent Warning header
    display.setTextSize(2);
    display.setCursor(15, 0);
    display.print("WARNING!");
    
    display.drawLine(0, 18, 128, 18, WHITE); // Divider line

    // List the specific reasons
    display.setTextSize(1);
    int cursorY = 25;

    if (waterCritical) {
      display.setCursor(0, cursorY);
      display.print("- WATER LEVEL HIGH: ");
      display.print(distance);
      display.print("cm");
      cursorY += 12;
    }
    
    if (tiltCritical) {
      display.setCursor(0, cursorY);
      display.print("- STRUCTURAL TILT!");
      cursorY += 12;
    }
    
    if (weightCritical) {
      display.setCursor(0, cursorY);
      display.print("- OVERLOAD: ");
      display.print(weight, 1);
      display.print("g");
    }
    
  } else {
    // --- NORMAL MODE ---
    noTone(buzzerPin); // Turn off the buzzer

    // Draw a layout line to separate sections
    display.drawLine(0, 32, 128, 32, WHITE);

    // 1. TOP SECTION: Distance & Tilt
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("Water Lvl: ");
    display.print(distance);
    display.print(" cm");

    display.setCursor(0, 16);
    display.print("Bridge: ");
    display.print("Stable");

    // 2. BOTTOM SECTION: Weight
    display.setTextSize(1);
    display.setCursor(0, 38);
    display.print("Stress Load:");
    
    display.setTextSize(2); 
    display.setCursor(0, 50);
    display.print(weight, 1); 
    display.print(" g");
  }

  display.display(); // Push everything to screen

  // --- F. SERIAL MONITOR ---
  Serial.print("D:"); Serial.print(distance);
  Serial.print(" | T:"); Serial.print(tiltState);
  Serial.print(" | W:"); Serial.println(weight);

  delay(250); 
}