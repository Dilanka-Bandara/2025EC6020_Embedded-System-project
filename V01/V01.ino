#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "HX711.h"

// --- 1. SCREEN SETTINGS ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- 2. PIN DEFINITIONS (PRESERVED) ---
// Ultrasonic
const int trigPin = 9;
const int echoPin = 10;
// Tilt Sensor
const int tiltPin = 2;
// HX711 Weight Sensor (NEW)
const int LOADCELL_DOUT_PIN = 3;
const int LOADCELL_SCK_PIN = 4;

// --- 3. VARIABLES ---
long duration;
int distance;
int tiltState;
float weight;

// --- 4. INITIALIZE HX711 ---
HX711 scale;

// ! CALIBRATION FACTOR !
// You MUST change this value to make your specific load cell accurate.
// Start with 420.0. If the weight is too high, increase this number.
// If the weight is too low, decrease this number.
float calibration_factor = 420.0; 

void setup() {
  Serial.begin(9600);

  // --- Setup Pins ---
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
  pinMode(tiltPin, INPUT);

  // --- Setup OLED ---
  // Try address 0x3C first. If screen stays black, change to 0x3D
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
    Serial.println(F("SSD1306 allocation failed"));
    for(;;); // Stop here if screen fails
  }
  
  // Show startup message
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(10, 20);
  display.println("System Loading...");
  display.println("Don't touch scale");
  display.display();

  // --- Setup HX711 ---
  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  scale.set_scale(calibration_factor);
  scale.tare(); // Reset scale to 0 (This assumes scale is empty at startup)

  delay(2000); // Wait for sensors to settle
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
  // get_units(3) averages 3 readings to stop the numbers from jumping around
  weight = scale.get_units(3); 

  // --- D. UPDATE OLED DISPLAY ---
  display.clearDisplay();
  
  // Draw a layout line to separate sections
  display.drawLine(0, 32, 128, 32, WHITE); // Horizontal line in middle

  // 1. TOP SECTION: Distance & Tilt
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("Dist: ");
  display.print(distance);
  display.print(" cm");

  display.setCursor(0, 16);
  display.print("Tilt: ");
  if(tiltState == HIGH) { // Depending on your sensor, HIGH might be Tilted or Flat
    display.print("TILTED!");
  } else {
    display.print("Stable");
  }

  // 2. BOTTOM SECTION: Weight
  display.setTextSize(1);
  display.setCursor(0, 38);
  display.print("Weight/Load:");
  
  display.setTextSize(2); // Make weight number bigger
  display.setCursor(0, 50);
  display.print(weight, 1); // 1 decimal place
  display.print(" g");

  display.display(); // Push everything to screen

  // --- E. SERIAL MONITOR (For debugging/Calibration) ---
  Serial.print("D:"); Serial.print(distance);
  Serial.print(" | T:"); Serial.print(tiltState);
  Serial.print(" | W:"); Serial.println(weight);

  delay(250); // Small delay for readability
}