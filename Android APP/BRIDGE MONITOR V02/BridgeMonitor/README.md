# 🌉 Bridge Monitor — Android App
## Smart Bridge Safety Monitoring System | EC6020

---

## 📁 File Structure
```
BridgeMonitor/
├── app/
│   ├── build.gradle
│   └── src/main/
│       ├── AndroidManifest.xml          ← Bluetooth permissions
│       ├── java/com/bridgemonitor/
│       │   └── MainActivity.java        ← ALL app logic here
│       └── res/
│           ├── layout/
│           │   └── activity_main.xml    ← UI design
│           ├── drawable/
│           │   └── rounded_card.xml     ← Card shape
│           └── values/
│               └── styles.xml           ← Dark theme
├── build.gradle
└── settings.gradle
```

---

## 🚀 Step-by-Step Setup in Android Studio

### STEP 1 — Install Android Studio
1. Download from: https://developer.android.com/studio
2. Install it (default settings are fine)
3. Open Android Studio → wait for it to finish loading

### STEP 2 — Create a New Project
1. Click **"New Project"**
2. Select **"Empty Views Activity"**
3. Fill in:
   - **Name:** BridgeMonitor
   - **Package name:** com.bridgemonitor
   - **Language:** Java
   - **Minimum SDK:** API 21 (Android 5.0)
4. Click **Finish** — wait for Gradle to sync

### STEP 3 — Replace Files
Copy each file from this project into the matching location:

| This file | Goes into Android Studio at |
|-----------|----------------------------|
| `MainActivity.java` | `app/src/main/java/com/bridgemonitor/` |
| `activity_main.xml` | `app/src/main/res/layout/` |
| `AndroidManifest.xml` | `app/src/main/` |
| `styles.xml` | `app/src/main/res/values/` |
| `rounded_card.xml` | `app/src/main/res/drawable/` (create folder if missing) |
| `app/build.gradle` | `app/build.gradle` (replace existing) |

### STEP 4 — Sync Gradle
After copying files:
1. Android Studio will show a yellow bar: **"Gradle files have changed"**
2. Click **"Sync Now"**
3. Wait for it to finish (may take 1-2 minutes first time)

### STEP 5 — Connect Your Android Phone
1. On your phone: **Settings → Developer Options → USB Debugging → ON**
   (To enable Developer Options: Settings → About Phone → tap "Build Number" 7 times)
2. Connect phone to PC via USB
3. Allow USB debugging when prompted on phone
4. In Android Studio, your phone should appear in the top bar

### STEP 6 — Build & Run
1. Click the green ▶ **Run** button in Android Studio
2. Select your device
3. App will install and open on your phone

---

## 📱 How to Use the App

### Before launching app:
1. Go to your phone **Settings → Bluetooth**
2. Turn ON Bluetooth
3. Tap **"Pair new device"** or **"Scan"**
4. Find **"HC-05"** in the list
5. Tap it → Enter PIN: **1234** (or 0000)
6. It should now show as "Paired"

### In the app:
1. Tap **"📡 CONNECT HC-05"** button
2. A list of paired devices will appear
3. Tap **"HC-05"**
4. Wait 2-3 seconds — status will show **"Connected to HC-05"** in green
5. Sensor data will start updating every 500ms!

---

## 📊 What Each Display Shows

| Display | Sensor | Green | Orange | Red |
|---------|--------|-------|--------|-----|
| 💧 Water Level | HC-SR04 Ultrasonic | > 100cm | 75-100cm | < 75cm |
| ⚖️ Strain Load | HX711 + Load Cell | < 1000g | 1000-1500g | > 1500g |
| 📐 Tilt/Vibration | Tilt Sensor (PD2) | Stable | — | TILTED |
| 🚦 Status Banner | Combined | SAFE | WARNING | DANGER |

---

## 📡 Data Format from HC-05
The ATmega328P sends this format every 500ms:
```
$BRIDGE,45,0,320.5,SAFE#
         |  |  |     |
         |  |  |     └── Status: SAFE / WARNING / DANGER
         |  |  └──────── Weight in grams (e.g. 320.5g)
         |  └─────────── Tilt: 0=Stable, 1=Tilted
         └────────────── Water distance in cm (e.g. 45cm)
```

---

## ⚠️ Troubleshooting

| Problem | Solution |
|---------|----------|
| HC-05 not in paired list | Pair it first in phone Settings → Bluetooth |
| Connection fails | Make sure HC-05 LED is blinking (not solid) |
| App crashes on open | Make sure you granted Bluetooth permission |
| No data showing | Check `DEBUG_MODE 0` in firmware, check baud rate is 9600 |
| "Bluetooth not supported" | Your phone doesn't have Bluetooth (very rare) |
| Build error in Android Studio | Click Build → Clean Project, then Rebuild |

---

## 🔌 HC-05 Wiring Reminder
```
HC-05 VCC  →  5V
HC-05 GND  →  GND  
HC-05 TXD  →  ATmega PD0 (Pin 0) — direct connection
HC-05 RXD  →  ATmega PD1 (Pin 1) — via voltage divider!
                    PD1 → [1kΩ] → HC-05 RXD
                               → [2kΩ] → GND
```
