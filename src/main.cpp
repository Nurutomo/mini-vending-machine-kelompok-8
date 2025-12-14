#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <ESP32Servo.h>
#include <Preferences.h>

// --- OBJEK & PIN ---
LiquidCrystal_I2C lcd(0x27, 16, 2);
Preferences preferences;

// Keypad Setup
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {32, 33, 25, 26}; // {13, 12, 14, 27}; 
byte colPins[COLS] = {27, 14, 12, 13}; // {26, 25, 33, 32};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// Actuators
Servo servo1, servo2, servo3, servo4;
const int pinServo[4] = {15, 2, 4, 17};
// Individual servo speed calibration (microseconds for forward rotation)
int servoSpeed[4] = {1250, 1250, 1250, 1250}; // Adjust these values for each servo

// Tilting servo (Jungkat Jungkit) - 180 degree servo
Servo tiltingServo;
const int TILTING_SERVO_PIN = 5;
const int TILT_NEUTRAL = 87;  // Facing up (neutral position)
const int TILT = 35;
const int TILT_ACCEPT = TILT_NEUTRAL - TILT;   // Tilt to accept side
const int TILT_REJECT = TILT_NEUTRAL + TILT;  // Tilt to reject side 

// Sensors
#define LDR_TOP 34
#define LDR_BOTTOM 35
int threshold = 2000;
int thresholdTop = 2000;
int thresholdBottom = 2000;
bool calibrationDone = false; 

// --- KALIBRASI KOIN (MICROS) ---
// Range Koin 1 (Misal Rp 500)
#define COIN1_MINX 1.15
#define COIN1_MAXX 1.35
#define COIN1_MINY 20500
#define COIN1_MAXY 30200
// #define COIN1_MINZ 1.9
// #define COIN1_MAXZ 2.21
#define COIN1_VAL 500
// Range Koin 2 (Misal Rp 1000)
#define COIN2_MINX 0.76
#define COIN2_MAXX 0.92
#define COIN2_MINY 29000  
#define COIN2_MAXY 33300
// #define COIN2_MINZ 29000  
// #define COIN2_MAXZ 33300
#define COIN2_VAL 1000

#define THRESHOLD 0.3

// Variables
int saldo = 0;
int hargaBarang[4] = {500, 500, 1000, 1000}; 
unsigned long startTime[2] = {0, 0};
unsigned long endTime[2] = {0, 0};
int readingCoin = 0;
char CheatCode[4] = {'1', '5', '2', '9'};
char ResetCode[4] = {'0', '8', '0', '8'};
char keyCombination[4] = {' ', ' ', ' ', ' '}; // To store last 4 keys pressed

int a = 0;
int b = 0;
int c = 0;

// Non-blocking state machines
enum ServoState { SERVO_IDLE, SERVO_DISPENSING };
ServoState servoState = SERVO_IDLE;
unsigned long servoStartTime = 0;
int activeServoIndex = -1;

int beepPin = 18;
int beepEnd = 0;

enum DisplayState { DISPLAY_MENU, DISPLAY_PRICES, DISPLAY_SELECTED, DISPLAY_MESSAGE };
DisplayState displayState = DISPLAY_MENU;
DisplayState lastDisplayState = DISPLAY_MENU;
unsigned long displayEndTime = 0;
bool displayDisplayed = true;
String displayMessage = "";

// Price display cycling
unsigned long lastDisplaySwitch = 0;
const unsigned long DISPLAY_SWITCH_INTERVAL = 12500; // Switch every 3 seconds

// Item selection system
int selectedItem = -1; // -1 = no selection, 0-3 = selected item
unsigned long selectionTime = 0;
const unsigned long SELECTION_TIMEOUT = 10000; // 10 seconds to confirm with #

// Scrolling text system
String scrollText = "Selamat Datang di Mesin Vending - Masukkan Koin - Pilih Barang A, B, C, atau D - Tekan # untuk Membeli   ";
int scrollPosition = 0;
unsigned long lastScrollTime = 0;
const unsigned long SCROLL_INTERVAL = DISPLAY_SWITCH_INTERVAL / (scrollText.length() * 24); // 100ms between scroll steps

// Time optimization - single millis() call per loop
unsigned long currentTime = 0;

// FreeRTOS LCD task
TaskHandle_t lcdTaskHandle = NULL;
SemaphoreHandle_t lcdMutex = NULL;
bool lcdNeedsUpdate = false;

// Tilting servo state management
enum TiltState { TILT_IDLE, TILT_MOVING };
TiltState tiltState = TILT_IDLE;
unsigned long tiltStartTime = 0;
const unsigned long TILT_DURATION = 750; // 1 second tilt duration

// --- FUNGSI ---
void gerakJungkatJungkit(bool valid) {
  if(tiltState == TILT_IDLE) {
    tiltState = TILT_MOVING;
    tiltStartTime = currentTime;
    
    if(valid) {
      tiltingServo.write(TILT_ACCEPT); // Tilt to accept side (45 degrees)
      // Serial.println("Miringkan ke sisi TERIMA");
    } else {
      tiltingServo.write(TILT_REJECT); // Tilt to reject side (135 degrees)
      // Serial.println("Miringkan ke sisi TOLAK");
    }
  }
}

String getScrollingText(String text, int startPos) {
  String result = "";
  int textLen = text.length();
  
  for(int i = 0; i < 16; i++) { // LCD width is 16 characters
    int charIndex = (startPos + i) % textLen;
    result += text.charAt(charIndex);
  }
  return result;
}

void updateDisplayCore() {
  // Handle message timeout
  if (displayState == DISPLAY_MESSAGE && displayEndTime < currentTime) {
    displayDisplayed = false;
    displayState = DISPLAY_MENU;
    lastDisplaySwitch = currentTime;
  }
  
  // Handle selection timeout
  if (selectedItem != -1 && currentTime - selectionTime >= SELECTION_TIMEOUT) {
    selectedItem = -1;
    displayState = DISPLAY_MENU;
    displayDisplayed = false;
    lastDisplaySwitch = currentTime;
  }
  
  // Auto-switch between menu and prices (only when not showing message and no selection)
  if (displayState != DISPLAY_MESSAGE && displayState != DISPLAY_SELECTED && 
      selectedItem == -1 && currentTime - lastDisplaySwitch >= DISPLAY_SWITCH_INTERVAL) {
    displayDisplayed = false;
    if (displayState == DISPLAY_MENU) {
      displayState = DISPLAY_PRICES;
    } else {
      displayState = DISPLAY_MENU;
    }
    lastDisplaySwitch = currentTime;
  }
  
  if (!displayDisplayed) {
    // lcd.clear();
    switch (displayState) {
      case DISPLAY_MENU: {
        // Update scroll position
        if(currentTime - lastScrollTime >= SCROLL_INTERVAL) {
          scrollPosition++;
          if(scrollPosition >= scrollText.length()) {
            scrollPosition = 0;
          }
          lastScrollTime = currentTime;
          displayDisplayed = false; // Force redraw for smooth scrolling
        }
        
        String scrollLine = getScrollingText(scrollText, scrollPosition);
        lcd.setCursor(0,0); lcd.print(scrollLine);
        lcd.setCursor(0,1); lcd.print("Saldo: Rp"); lcd.print(saldo); lcd.print("       ");
        
        // Return early to enable continuous scrolling
        return;
      } break;
      case DISPLAY_PRICES: {
        lcd.clear();
        lcd.setCursor(0,0); lcd.print("A:"); lcd.print(hargaBarang[0]); 
        lcd.setCursor(8,0); lcd.print("B:"); lcd.print(hargaBarang[1]);
        lcd.setCursor(0,1); lcd.print("C:"); lcd.print(hargaBarang[2]); 
        lcd.setCursor(8,1); lcd.print("D:"); lcd.print(hargaBarang[3]);
      } break;
      case DISPLAY_SELECTED: {
        lcd.clear();
        char itemName = 'A' + selectedItem;
        lcd.setCursor(0,0); lcd.print("Barang "); lcd.print(itemName); 
        lcd.print(": Rp"); lcd.print(hargaBarang[selectedItem]);
        lcd.setCursor(0,1); lcd.print("Tekan # utk beli");
      } break;
      case DISPLAY_MESSAGE: {
        lcd.clear();
        lcd.setCursor(0,0); lcd.print(displayMessage);
      } break;
    }
    displayDisplayed = true;
  }
}

// Thread-safe display update function
void updateDisplay() {
  lcdNeedsUpdate = true;
}

void showMessage(String message, unsigned long duration) {
  displayMessage = message;
  lastDisplayState = displayState;
  displayState = DISPLAY_MESSAGE;
  displayEndTime = currentTime + duration; // Keep millis() here as it's called from various contexts
  displayDisplayed = false;
}

// LCD Task - runs on separate core
void lcdTask(void* parameter) {
  for(;;) {
    if(xSemaphoreTake(lcdMutex, portMAX_DELAY)) {
      updateDisplayCore();
      xSemaphoreGive(lcdMutex);
    }
    vTaskDelay(50 / portTICK_PERIOD_MS); // 20 FPS refresh rate
  }
}

void dispenseItem(int index) {
  if(servoState == SERVO_IDLE) {
    showMessage("Mengeluarkan...", 1000);
    activeServoIndex = index;
    servoState = SERVO_DISPENSING;
    servoStartTime = currentTime; // Keep millis() here as it's called from keypad context
    
    Servo* target;
    switch(index) {
      case 0: target = &servo1; break;
      case 1: target = &servo2; break;
      case 2: target = &servo3; break;
      case 3: target = &servo4; break;
      default: return;
    }
    target->writeMicroseconds(servoSpeed[index]); // Use calibrated speed for this servo
  }
}

void calibrateThreshold() {
  showMessage("Kalibrasi...", 4800);
  updateDisplay();
  // Sample ambient light levels
  long sumTop = 0, sumBottom = 0;
  const int samples = 4096;
  
  for(int i = 0; i < samples; i++) {
    sumTop += 4095 - analogRead(LDR_TOP);
    sumBottom += 4095 - analogRead(LDR_BOTTOM);
  }
  
  int avgTop = sumTop / samples;
  int avgBottom = sumBottom / samples;
  
  // Set thresholds at 90% of ambient light (when object blocks light)
  thresholdTop = avgTop * THRESHOLD;
  thresholdBottom = avgBottom * THRESHOLD;
  
  // Save calibrated thresholds to preferences
  
  Serial.print("Calibrated - Top: "); Serial.print(avgTop);
  Serial.print(" -> "); Serial.print(thresholdTop);
  Serial.print(", Bottom: "); Serial.print(avgBottom);
  Serial.print(" -> "); Serial.println(thresholdBottom);
  
  showMessage("Kalibrasi OK", 1000);
  calibrationDone = true;
}

void beep(int durationMs) {
  beepEnd = currentTime + durationMs; // Keep millis() here as it's called from various contexts
}

void cekValidasiKoin(float rate1, int rate2) {
  int nilai = 0;
  bool valid = false;

  if (rate1 >= COIN1_MINX && rate1 <= COIN1_MAXX && rate2 >= COIN1_MINY && rate2 <= COIN1_MAXY) {
    nilai = COIN1_VAL; valid = true;
  } else if (rate1 >= COIN2_MINX && rate1 <= COIN2_MAXX && rate2 >= COIN2_MINY && rate2 <= COIN2_MAXY) {
    nilai = COIN2_VAL; valid = true;
  }

  if (valid) {
    saldo += nilai;
    preferences.putInt("saldo", saldo); // Simpan permanen
    showMessage("+Rp" + String(nilai), 1500);
    gerakJungkatJungkit(true);
    beep(100);
  } else {
    showMessage("Invalid!", 1500);
    gerakJungkatJungkit(false);
    beep(1000);
  }
}

void setup() {
  Serial.begin(115200);
  lcd.init(); lcd.backlight();

  pinMode(beepPin, OUTPUT);
  
  servo1.attach(pinServo[0]); servo2.attach(pinServo[1]);
  servo3.attach(pinServo[2]); servo4.attach(pinServo[3]);
  servo1.writeMicroseconds(1500); servo2.writeMicroseconds(1500); 
  servo3.writeMicroseconds(1500); servo4.writeMicroseconds(1500);

  // Initialize tilting servo (jungkat jungkit)
  tiltingServo.attach(TILTING_SERVO_PIN);
  tiltingServo.write(TILT_NEUTRAL); // Start in neutral position (facing up)
  
  preferences.begin("vending", false);
  saldo = preferences.getInt("saldo", 0);
  
  // Create LCD mutex and task
  lcdMutex = xSemaphoreCreateMutex();
  
  // Create LCD task on Core 0 (Core 1 is used by Arduino loop)
  xTaskCreatePinnedToCore(
    lcdTask,          // Task function
    "LCD Task",       // Task name
     4096,            // Stack size
    NULL,            // Parameters
    1,               // Priority
    &lcdTaskHandle,  // Task handle
    0                // Core 0
  );
  
  calibrateThreshold();
}

void loop() {
  currentTime = millis();
  // Non-blocking tilting servo control
  if(tiltState == TILT_MOVING && currentTime - tiltStartTime >= TILT_DURATION) {
    tiltingServo.write(TILT_NEUTRAL); // Return to neutral position (facing up)
    tiltState = TILT_IDLE;
    // Serial.println("Kembali ke posisi NETRAL");
  }
  
  // Non-blocking servo control
  if(servoState == SERVO_DISPENSING && currentTime - servoStartTime >= 2000) {
    Servo* target;
    switch(activeServoIndex) {
      case 0: target = &servo1; break;
      case 1: target = &servo2; break;
      case 2: target = &servo3; break;
      case 3: target = &servo4; break;
      default: target = nullptr;
    }
    if(target) target->writeMicroseconds(1500); // Stop (neutral position for continuous rotation servo)
    servoState = SERVO_IDLE;
    activeServoIndex = -1;
  }
  
  
  // Sensor Logic (only if calibration is done)
  if(calibrationDone) {
    int valTop = 4095 - analogRead(LDR_TOP);
    int valBot = 4095 - analogRead(LDR_BOTTOM);
    bool isTopDark = valTop < thresholdTop;
    bool isBotDark = valBot < thresholdBottom;
    
    // Serial.println("LDR Top: " + String(valTop) + ", Bottom: " + String(valBot));
    unsigned long t = micros();
    switch (readingCoin) {
      case 0: {
        if (isTopDark && !isBotDark) {
          startTime[0] = t;
          readingCoin = 1;
        }
      } break;
      case 1: {
        if (!isTopDark && !isBotDark) {
          endTime[0] = t;
          readingCoin = 2;
          a = endTime[0] - startTime[0];
        }
      } break;
      case 2: {
        if (isBotDark && !isTopDark) {
          startTime[1] = t;
          readingCoin = 3;
          b = startTime[1] - endTime[0];
        }
      } break;
      case 3: {
        if (!isBotDark && !isTopDark) {
          endTime[1] = t;
          readingCoin = 0;
          c = endTime[1] - startTime[1];
          Serial.print(a);
          Serial.print(",");
          Serial.print(b);
          Serial.print(",");
          Serial.print(c);
          Serial.print(",");
          float rate = (float(a + c) * .5) / float(b);
          float rate2 = float(a) / float(c);
          Serial.print(rate);
          Serial.print(",");
          Serial.print(rate2);
          Serial.print(",");
          Serial.println(rate / rate2);
          cekValidasiKoin(rate, b);
        }
      } break;
    }
  }

  
  // Keypad Logic
  char key = keypad.getKey();
  if (key && servoState == SERVO_IDLE) {
    beep(50);
    Serial.println(key);
    // Shift existing keys left and add new key at the end
    for(int i = 0; i < 3; i++) {
      keyCombination[i] = keyCombination[i + 1];
    }
    keyCombination[3] = key;
    // Check for cheat code
    if (strncmp(keyCombination, CheatCode, 4) == 0) {
      saldo += 5000;
      preferences.putInt("saldo", saldo);
      showMessage("Cheat activated!", 1500);
      // Clear the combination to prevent repeated activation
      memset(keyCombination, ' ', 4);
    }
    if (strncmp(keyCombination, ResetCode, 4) == 0) {
      saldo = 0;
      preferences.putInt("saldo", saldo);
      showMessage("Saldo reset!", 1500);
      // Clear the combination to prevent repeated activation
      memset(keyCombination, ' ', 4);
    }
    // Item selection (A, B, C, D)
    int idx = -1;
    if(key=='A') idx=0; else if(key=='B') idx=1; 
    else if(key=='C') idx=2; else if(key=='D') idx=3;
    
    if(idx != -1) {
      // Select item and show its price
      selectedItem = idx;
      selectionTime = currentTime;
      displayState = DISPLAY_SELECTED;
      displayDisplayed = false;
      Serial.print("Barang dipilih: "); Serial.println(char('A' + idx));
    }
    
    // Confirm purchase with #
    else if(key == '#' && selectedItem != -1) {
      if(saldo >= hargaBarang[selectedItem]) {
        dispenseItem(selectedItem);
        beep(500);
        saldo -= hargaBarang[selectedItem];
        preferences.putInt("saldo", saldo);
        selectedItem = -1; // Clear selection
        displayState = DISPLAY_MESSAGE; // Will show "Processing..." from dispenseItem
      } else {
        beep(1000);
        showMessage("Saldo Tidak Cukup!", 1500);
        selectedItem = -1; // Clear selection on insufficient balance
      }
    }
    
    // Cancel selection with *
    else if(key == '*') {
      selectedItem = -1;
      displayState = DISPLAY_MENU;
      displayDisplayed = false;
      lastDisplaySwitch = currentTime;
      Serial.println("Pilihan dibatalkan");
    }
  }
  
  // Display updates are handled by LCD task
  digitalWrite(beepPin, beepEnd >= currentTime);
}