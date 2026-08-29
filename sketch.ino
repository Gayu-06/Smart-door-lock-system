#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Keypad.h>

// OLED Display Settings (I2C Mode - 4 Pin)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// I2C Pins for OLED
#define I2C_SDA 21
#define I2C_SCL 22

// RFID MFRC522 Pins
#define RFID_SS 5
#define RFID_RST 2
MFRC522 mfrc522(RFID_SS, RFID_RST);

// Relay and Lock Control - CRITICAL FIX
#define RELAY_PIN 26
#define UNLOCK_DURATION 5000  // 5 seconds

// Touch Sensor Pin - CRITICAL FIX FOR FALSE TRIGGERS
#define TOUCH_PIN T0  // GPIO 4
int touchBaseline = 0;
int touchThreshold = 0;
bool touchWasTriggered = false;
unsigned long lastTouchTime = 0;
#define TOUCH_DEBOUNCE 1000  // 1 second debounce

// Keypad Setup
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {13, 12, 14, 27};
byte colPins[COLS] = {25, 33, 32, 15};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// Password Settings
String inputPassword = "";
const String correctPassword = "4711";
const int MAX_PASSWORD_LENGTH = 6;

// System State
bool doorUnlocked = false;
unsigned long unlockTime = 0;

void setup() {
  Serial.begin(115200);
  delay(500);
  
  Serial.println("=== SMART LOCK SYSTEM INITIALIZING ===");
  
  // Initialize Relay FIRST - CRITICAL FIX
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);  // Ensure LOCKED at startup
  Serial.println("Relay initialized: LOCKED");
  delay(100);
  
  // Initialize I2C for OLED
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  
  // Initialize OLED Display
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 failed at 0x3C, trying 0x3D"));
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
      Serial.println(F("OLED not found!"));
      for(;;);
    }
  }
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Smart Lock System");
  display.println("Initializing...");
  display.display();
  
  // CRITICAL: Calibrate Touch Sensor Baseline
  Serial.println("\n=== CALIBRATING TOUCH SENSOR ===");
  Serial.println("DO NOT TOUCH THE SENSOR NOW!");
  delay(2000);
  
  // Take multiple baseline readings
  long sum = 0;
  for(int i = 0; i < 10; i++) {
    int reading = touchRead(TOUCH_PIN);
    sum += reading;
    Serial.print("Reading ");
    Serial.print(i+1);
    Serial.print(": ");
    Serial.println(reading);
    delay(100);
  }
  
  touchBaseline = sum / 10;
  // Set threshold to 70% of baseline (30% reduction triggers touch)
  touchThreshold = touchBaseline - (touchBaseline * 0.3);
  
  Serial.print("Touch Baseline: ");
  Serial.println(touchBaseline);
  Serial.print("Touch Threshold: ");
  Serial.println(touchThreshold);
  Serial.println("Calibration complete!");
  
  // Initialize SPI for RFID
  SPI.begin();
  delay(100);
  
  // Initialize RFID
  mfrc522.PCD_Init();
  delay(100);
  
  // Check RFID Module
  byte version = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
  if (version == 0x00 || version == 0xFF) {
    Serial.println("RFID module ERROR!");
    displayMessage("RFID Error!", "Check Wiring");
    delay(2000);
  } else {
    Serial.print("RFID OK - Version: 0x");
    Serial.println(version, HEX);
  }
  
  // Display Ready Message
  displayMessage("System Ready", "RFID/Touch/Key");
  Serial.println("\n=== SYSTEM READY ===\n");
}

void loop() {
  // Check if door should be locked again
  if (doorUnlocked && (millis() - unlockTime > UNLOCK_DURATION)) {
    lockDoor();
  }
  
  // Check RFID (only if not already unlocked)
  if (!doorUnlocked) {
    checkRFID();
  }
  
  // Check Touch Sensor (only if not already unlocked and debounce passed)
  if (!doorUnlocked && (millis() - lastTouchTime > TOUCH_DEBOUNCE)) {
    checkTouch();
  }
  
  // Check Keypad (always check for password entry)
  checkKeypad();
  
  delay(100);
}

void checkRFID() {
  // Soft reset to prevent timeout
  mfrc522.PCD_SoftPowerUp();
  
  // Look for new cards
  if (!mfrc522.PICC_IsNewCardPresent()) {
    return;
  }
  
  // Select card
  if (!mfrc522.PICC_ReadCardSerial()) {
    mfrc522.PCD_Init();
    return;
  }
  
  // Card detected - any card unlocks
  Serial.println(">>> RFID Card Detected <<<");
  unlockDoor("RFID");
  
  // Proper shutdown
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  
  delay(500);
}

void checkTouch() {
  int touchValue = touchRead(TOUCH_PIN);
  
  // CRITICAL FIX: Only trigger if value drops BELOW threshold
  if (touchValue < touchThreshold) {
    // Verify with second reading to avoid false positive
    delay(50);
    int touchValue2 = touchRead(TOUCH_PIN);
    
    if (touchValue2 < touchThreshold) {
      Serial.print(">>> TOUCH DETECTED <<< Value: ");
      Serial.print(touchValue);
      Serial.print(" / Threshold: ");
      Serial.println(touchThreshold);
      
      unlockDoor("Touch");
      lastTouchTime = millis();  // Set debounce timer
      touchWasTriggered = true;
    }
  } else {
    // Reset trigger flag when not touching
    touchWasTriggered = false;
  }
}

void checkKeypad() {
  char key = keypad.getKey();
  
  if (key) {
    Serial.print("Key pressed: ");
    Serial.println(key);
    
    if (key == 'A') {
      // Submit password
      if (inputPassword == correctPassword) {
        Serial.println(">>> CORRECT PASSWORD <<<");
        unlockDoor("Keypad");
        inputPassword = "";
      } else {
        Serial.println(">>> WRONG PASSWORD <<<");
        displayMessage("Wrong Password!", "");
        inputPassword = "";
        delay(2000);
        displayMessage("System Ready", "RFID/Touch/Key");
      }
    } else if (key == '*') {
      // Clear password
      inputPassword = "";
      Serial.println("Password cleared");
      displayMessage("Cleared", "Enter Password:");
      delay(1000);
      displayMessage("System Ready", "RFID/Touch/Key");
    } else {
      // Add to password
      if (inputPassword.length() < MAX_PASSWORD_LENGTH) {
        inputPassword += key;
        displayKeypadInput(inputPassword);
      }
    }
  }
}

void unlockDoor(String method) {
  Serial.println("\n==================================");
  Serial.print(">>> UNLOCKING DOOR VIA: ");
  Serial.print(method);
  Serial.println(" <<<");
  Serial.println("==================================");
  
  // CRITICAL FIX: Force relay HIGH with verification
  digitalWrite(RELAY_PIN, HIGH);
  delay(100);  // Give relay time to actuate
  
  // Verify relay state
  int relayState = digitalRead(RELAY_PIN);
  Serial.print("Relay Pin State: ");
  Serial.println(relayState == HIGH ? "HIGH (UNLOCKED)" : "LOW (ERROR!)");
  
  doorUnlocked = true;
  unlockTime = millis();
  
  String message = "UNLOCKED: " + method;
  displayMessage(message, "Lock in 5s");
  
  Serial.println("Solenoid should be UNLOCKED now!");
  Serial.print("Auto-lock in ");
  Serial.print(UNLOCK_DURATION / 1000);
  Serial.println(" seconds\n");
}

void lockDoor() {
  Serial.println("\n==================================");
  Serial.println(">>> LOCKING DOOR <<<");
  Serial.println("==================================");
  
  // CRITICAL FIX: Force relay LOW with verification
  digitalWrite(RELAY_PIN, LOW);
  delay(100);
  
  // Verify relay state
  int relayState = digitalRead(RELAY_PIN);
  Serial.print("Relay Pin State: ");
  Serial.println(relayState == LOW ? "LOW (LOCKED)" : "HIGH (ERROR!)");
  
  doorUnlocked = false;
  
  displayMessage("Door LOCKED", "System Ready");
  Serial.println("Solenoid should be LOCKED now!\n");
  
  delay(2000);
  displayMessage("System Ready", "RFID/Touch/Key");
}

void displayMessage(String line1, String line2) {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(0, 10);
  display.println(line1);
  display.setTextSize(1);
  display.setCursor(0, 40);
  display.println(line2);
  display.display();
}

void displayKeypadInput(String input) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Enter Password:");
  display.println("# OK  * Clear");
  display.setTextSize(2);
  display.setCursor(0, 25);
  
  // Display asterisks
  for (int i = 0; i < input.length(); i++) {
    display.print("*");
  }
  display.display();
}
