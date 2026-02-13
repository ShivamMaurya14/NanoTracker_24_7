#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
#include <EEPROM.h>

// ==========================================
// CONFIGURATION
// ==========================================
const String PHONE_NUMBER = "+919999999999";    // Owner/User Number
const String LOGISTICS_NUMBER = "+918888888888"; // Logistics Company API/SMS Gateway
unsigned long sleepSeconds = 3600;           // Default: 1 Hour Deep Sleep

// SIM800L Pins (SoftwareSerial)
// ESP8266 Pin D1 (GPIO 5) -> SIM800L TX
// ESP8266 Pin D2 (GPIO 4) -> SIM800L RX
const int RX_PIN = 5; 
const int TX_PIN = 4;

// SIM800L Commands
const String CMD_SLEEP = "AT+CSCLK=2"; // Auto-sleep when no data
const String CMD_WAKE  = "AT";         // Sending characters wakes it up

SoftwareSerial sim800(RX_PIN, TX_PIN);

void setup() {
  // 0. Disable WiFi immediately to save power (~70mA saved)
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();
  delay(1);

  // 1. Initialize Debug Serial (Hardware) and GSM Serial (Software)
  Serial.begin(115200);
  sim800.begin(9600);
  delay(1000);
  
  // Initialize EEPROM for Settings
  EEPROM.begin(512);
  unsigned long storedSleep;
  EEPROM.get(0, storedSleep);
  
  // Basic validation (10s to 24h)
  if (storedSleep >= 10 && storedSleep <= 86400) {
    sleepSeconds = storedSleep;
    Serial.println("[EEPROM] Loaded Sleep Time: " + String(sleepSeconds));
  } else {
    Serial.println("[EEPROM] Using Default Sleep Time: " + String(sleepSeconds));
  }

  Serial.println("\n[SYSTEM] Wake Up...");

  // 2. Handshake with SIM800L
  // Send a dummy to wake it from sleep if needed
  sim800.println("AT"); 
  delay(100);
  
  if (waitForATResponse("AT", 2000)) {
    Serial.println("[GSM] SIM800L Ready");
  } else {
    Serial.println("[GSM] SIM800L Not Responding! (Check Power)");
    // Even if it fails, we go to sleep to save battery and try again later
    goToDeepSleep(); 
  }

  // 3. Configure Text Mode for SMS
  sendATCommand("AT+CMGF=1"); // Text Mode
  delay(1000);

  // 4. Wait for Network Registration
  // CREG? returns +CREG: <n>,<stat>
  // <stat> 1 = Registered Home, 5 = Registered Roaming
  if (waitForNetworkRegistration(20000)) {
    Serial.println("[GSM] Network Registered");
    
    // 5. Get Cell ID and LAC
    String locationData = getCellTowerData();
    
    if (locationData.length() > 0) {
      String battInfo = getBatteryStatus();
      String finalMessage = locationData + "\n" + battInfo;
      
      Serial.println("[GSM] Sending SMS to User: " + finalMessage);
      sendSMS(PHONE_NUMBER, finalMessage);
      
      delay(2000); // Small delay between messages
      
      Serial.println("[GSM] Sending SMS to Logistics: " + finalMessage);
      sendSMS(LOGISTICS_NUMBER, finalMessage);
    } else {
      Serial.println("[ERROR] Failed to retrieve Cell ID/LAC");
      sendSMS(PHONE_NUMBER, "Tracker Alive: GPS/Cell update failed.");
    }

  } else {
    Serial.println("[ERROR] Network Registration Timeout");
  }

  // 6. Check for Remote Commands (SMS)
  checkIncomingCommands();

  // 7. Put SIM800L to Sleep (Power Saving)
  Serial.println("[GSM] Putting SIM800L to Sleep...");
  sendATCommand(CMD_SLEEP);
  delay(200);

  // 8. Enter Deep Sleep
  goToDeepSleep();
}

void loop() {
  // Loop is never reached in Deep Sleep logic
}

// ==========================================
// HELPER FUNCTIONS
// ==========================================

void goToDeepSleep() {
  Serial.println("[SYSTEM] Entering Deep Sleep for " + String(sleepSeconds) + " seconds.");
  Serial.flush(); // Wait for serial data to complete
  
  // WAKE_RF_DEFAULT: Wake up with radio (WiFi) calibration. 
  // Can use WAKE_RF_DISABLED if WiFi isn't used to save more power, 
  // but standard usage often keeps it default.
  ESP.deepSleep(sleepSeconds * 1000000ULL); 
}

bool waitForATResponse(String cmd, unsigned long timeout) {
  sim800.println(cmd);
  unsigned long start = millis();
  while (millis() - start < timeout) {
    if (sim800.available()) {
      String response = sim800.readString();
      if (response.indexOf("OK") != -1) {
        return true;
      }
    }
  }
  return false;
}

bool waitForNetworkRegistration(unsigned long timeout) {
  unsigned long start = millis();
  while (millis() - start < timeout) {
    sim800.println("AT+CREG?");
    delay(500);
    if (sim800.available()) {
      String response = sim800.readString();
      // Check for Registered (1) or Roaming (5)
      // Responses like: +CREG: 0,1 or +CREG: 0,5
      if (response.indexOf(",1") != -1 || response.indexOf(",5") != -1) {
        return true;
      }
    }
    delay(1000);
  }
  return false;
}

String getCellTowerData() {
  // Request detailed network info: AT+CREG=2 enables returning Hex LAC and CID
  sendATCommand("AT+CREG=2");
  delay(500);
  
  sim800.println("AT+CREG?");
  delay(500);
  
  // Buffer for reading
  String response = "";
  unsigned long start = millis();
  while (millis() - start < 2000) {
    if (sim800.available()) {
      response += (char)sim800.read();
    }
  }

  // Expected Response format: +CREG: 2,<stat>,"<lac>","<ci>"
  // Example: +CREG: 2,1,"1A2B","3C4D"
  
  Serial.println("[DEBUG] CREG Response: " + response);
  
  int statIndex = response.indexOf("+CREG: 2,");
  if (statIndex != -1) {
    // Basic parsing logic
    // Find first quote for LAC
    int firstQuote = response.indexOf('"', statIndex);
    int secondQuote = response.indexOf('"', firstQuote + 1);
    
    // Find third quote for CI
    int thirdQuote = response.indexOf('"', secondQuote + 1);
    int fourthQuote = response.indexOf('"', thirdQuote + 1);
    
    if (firstQuote != -1 && fourthQuote != -1) {
      String lacHex = response.substring(firstQuote + 1, secondQuote);
      String ciHex = response.substring(thirdQuote + 1, fourthQuote);
      
      // Convert Hex Strings to Long (integers)
      long lac = strtol(lacHex.c_str(), NULL, 16);
      long ci = strtol(ciHex.c_str(), NULL, 16);
      
      // Format 1: Raw Data for OpenCelliD
      String message = "Tracker Info:\n";
      message += "LAC: " + String(lac) + "\n";
      message += "CID: " + String(ci) + "\n";
      
      // Format 2: Attempt to construct a helpful URL (Requires MCC/MNC usually 404/45 for India etc)
      // Since we can't easily auto-detect MCC/MNC without AT+COPS?, we provide the Raw codes.
      // User can input these into http://www.opencellid.org/
      
      message += "Link: http://www.opencellid.org/ (Use LAC/CID)";
      
      return message;
    }
  }
  
  return "";
}

String getBatteryStatus() {
  // Read Analog Pin A0
  // NOTE: ESP8266 Analog Pin range is 0-1.0V.
  // To measure a Li-Ion (4.2V), you MUST use a voltage divider (e.g. R1=330k, R2=100k).
  // Formula: Vout = Vin * (R2 / (R1 + R2)) -> Vin = Vout * ((R1+R2)/R2)
  int raw = analogRead(A0);
  
  // Example calibration for a 4.3V max input range (Adjust based on your resistors)
  // Voltage = (raw / 1023.0) * 4.3; 
  float voltage = (raw / 1023.0) * 4.3; 
  
  return "Batt: " + String(voltage, 2) + "V";
}

void sendATCommand(String cmd) {
  sim800.println(cmd);
}

void sendSMS(String number, String message) {
  sim800.println("AT+CMGS=\"" + number + "\"");
  delay(1000);
  sim800.print(message);
  delay(100);
  sim800.write(26); // ASCII Code for CTRL+Z to send
  delay(5000); // Wait for network to send
  Serial.println("[GSM] SMS Sent request completed");
}

void checkIncomingCommands() {
  Serial.println("[GSM] Checking for SMS commands...");
  
  // List unread messages
  sim800.println("AT+CMGL=\"REC UNREAD\"");
  delay(500);
  
  String content = "";
  unsigned long start = millis();
  // Wait up to 3 seconds for full response
  while (millis() - start < 3000) {
    while (sim800.available()) {
      content += (char)sim800.read();
    }
    delay(10);
  }

  // Check content for Keywords
  // "MODE:TRACK" -> 60s sleep
  // "MODE:SAVE"  -> 3600s sleep
  
  if (content.indexOf("MODE:TRACK") != -1) {
    sleepSeconds = 60; // 1 minute
    Serial.println("[CMD] TRACK MODE ACTIVATED! Sleep = 60s");
    EEPROM.put(0, sleepSeconds);
    EEPROM.commit();
  } 
  else if (content.indexOf("MODE:SAVE") != -1) {
    sleepSeconds = 3600; // 1 Hour
    Serial.println("[CMD] SAVE MODE ACTIVATED. Sleep = 3600s");
    EEPROM.put(0, sleepSeconds);
    EEPROM.commit();
  }
  
  // If we processed messages, clean up to prevent buffer overflow
  if (content.indexOf("+CMGL:") != -1) {
     Serial.println("[GSM] Deleting processed messages...");
     sim800.println("AT+CMGD=1,4"); // Delete All Messages
     delay(2000);
  }
}
