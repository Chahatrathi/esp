#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <nvs_flash.h> 
#include <Update.h> 

// =========================================================================
//  FIRMWARE CONFIGURATION (Increment CURRENT_VERSION every time you update!)
// =========================================================================
const String DEVICE_ID = "ARTI-GROUP-S3-01"; 
const String CURRENT_VERSION = "1.0.0"; 

// --- CLOUD BACKEND ---
const char* GOOGLE_SCRIPT_URL = "https://script.google.com/a/macros/aracharatventures.com/s/AKfycbwsCd8A5YusMVxA7KU5PBW-G5RcmMD-_KCcr7triRLtY_N_ygTYcTTJA02hM2Jc5WeM/exec";
// --- WIFI CONFIGURATION ---
const char* WIFI_SSID = "Note";       
const char* WIFI_PASSWORD = "12345678"; 

// --- PIN DEFINITIONS (SMARTELEX S3 PICO HAT) ---
#define ONE_WIRE_BUS 4  
#define MOISTURE_PIN 5  
#define WIFI_LED_PIN 6       
#define PROD_STOP_LED_PIN 7  

// --- CALIBRATION & THRESHOLDS ---
const int DRY_VALUE = 2569;  
const int WET_VALUE = 745;  
const float MAX_SAFE_TEMP = 65.0;     
const int MIN_SAFE_MOISTURE_PCT = 20; 

// --- OBJECTS ---
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensors(&oneWire);

float lastTemp = 0;
int lastMoist = 0;

// --- FUNCTION PROTOTYPES ---
void connectWiFi();
void handleCloudOperations();
void checkForUpdates();
void performOTA(String downloadUrl);
void sendDataToGoogle(float t, int m, String status);

void setup() {
  Serial.begin(115200);
  delay(2000); 
  
  Serial.println("\n--- SYSTEM BOOT ---");
  Serial.println("Current Firmware Version: " + CURRENT_VERSION);

  // NVS Memory Initialization
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND || err != ESP_OK) {
    nvs_flash_erase();
    err = nvs_flash_init();
  }

  pinMode(WIFI_LED_PIN, OUTPUT);
  pinMode(PROD_STOP_LED_PIN, OUTPUT);
  digitalWrite(WIFI_LED_PIN, LOW);
  digitalWrite(PROD_STOP_LED_PIN, LOW);

  tempSensors.begin();
  analogReadResolution(12); 

  connectWiFi();
}

void loop() {
  // 1. Read local sensors
  tempSensors.requestTemperatures();
  lastTemp = tempSensors.getTempCByIndex(0);
  
  int rawMoisture = analogRead(MOISTURE_PIN);
  lastMoist = constrain(map(rawMoisture, DRY_VALUE, WET_VALUE, 0, 100), 0, 100);
  
  Serial.print("\nRaw Moisture: "); Serial.println(rawMoisture);
  Serial.print("Temperature: "); Serial.print(lastTemp); Serial.println(" C");

  // 2. Wake up Wi-Fi
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  // 3. Process Cloud & OTA Cycle
  if (WiFi.status() == WL_CONNECTED) {
    handleCloudOperations();
  } 

  // 4. Shut down Wi-Fi Radio to save energy/bandwidth
  Serial.println("Cycle complete. Powering down Wi-Fi radio...");
  WiFi.disconnect(true, true); 
  WiFi.mode(WIFI_OFF);         
  digitalWrite(WIFI_LED_PIN, LOW); 

  // 5. Deep cycle delay (5 Minutes)
  Serial.println("Sleeping for 5 minutes...");
  delay(300000); 
}

void connectWiFi() {
  WiFi.mode(WIFI_STA); 
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  Serial.print("Connecting to Wi-Fi");
  int attempts = 0;
  
  while (WiFi.status() != WL_CONNECTED && attempts < 20) { 
    digitalWrite(WIFI_LED_PIN, HIGH);
    delay(250);
    digitalWrite(WIFI_LED_PIN, LOW);
    delay(250);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(WIFI_LED_PIN, HIGH); 
    Serial.println("\nWiFi Connected!");
  } else {
    digitalWrite(WIFI_LED_PIN, LOW); 
    Serial.println("\nWiFi Connection failed.");
  }
}

void handleCloudOperations() {
  // Step A: Look into the control tower to check for new firmware updates
  checkForUpdates();

  // Step B: Continue standard operations if no update triggered a restart
  WiFiClientSecure client;
  client.setInsecure(); 
  HTTPClient http;
  http.setTimeout(15000);
  
  String fetchUrl = String(GOOGLE_SCRIPT_URL) + "?deviceID=" + DEVICE_ID;
  http.begin(client, fetchUrl);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  
  int httpCode = http.GET();
  String cloudCommand = "RUNNING";
  
  if (httpCode == 200 || httpCode == 302) {
    cloudCommand = http.getString();
    cloudCommand.trim(); 
    Serial.println("Received System Command: " + cloudCommand);
  }
  http.end();

  // Strict Hardware LED Rules
  if (cloudCommand == "STOPPED") {
    digitalWrite(PROD_STOP_LED_PIN, HIGH);
  } else {
    digitalWrite(PROD_STOP_LED_PIN, LOW); 
  }

  // Calculate Data Reporting State
  String systemStatus = "RUNNING";
  if (cloudCommand == "STOPPED") {
    systemStatus = "STOPPED_BY_CLOUD";
  } else if (lastTemp >= MAX_SAFE_TEMP || lastMoist <= MIN_SAFE_MOISTURE_PCT) {
    systemStatus = "STOPPED_BY_SENSOR";
  }

  // Post back sensor telemetry details
  sendDataToGoogle(lastTemp, lastMoist, systemStatus);
}

void checkForUpdates() {
  Serial.println("Checking Control Tower for firmware updates...");
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  
  String checkUrl = String(GOOGLE_SCRIPT_URL) + "?action=checkUpdate&version=" + CURRENT_VERSION + "&deviceID=" + DEVICE_ID;
  
  http.begin(client, checkUrl);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  
  int httpCode = http.GET();
  if (httpCode == 200) {
    String response = http.getString();
    StaticJsonDocument<300> doc;
    DeserializationError error = deserializeJson(doc, response);
    
    if (!error) {
      bool updateAvailable = doc["updateAvailable"] | false;
      if (updateAvailable) {
        String downloadUrl = doc["url"].as<String>();
        Serial.println("New software version found! Initiating OTA download...");
        performOTA(downloadUrl);
      } else {
        Serial.println("Firmware is up to date.");
      }
    }
  } else {
    Serial.printf("Failed to contact update server. Code: %d\n", httpCode);
  }
  http.end();
}

void performOTA(String downloadUrl) {
  WiFiClientSecure client;
  client.setInsecure(); 
  HTTPClient http;
  http.setTimeout(30000); 
  
  Serial.println("Connecting to Binary Server: " + downloadUrl);
  http.begin(client, downloadUrl);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  
  int httpCode = http.GET();
  if (httpCode == 200) {
    int contentLength = http.getSize();
    Serial.println("File size to download: " + String(contentLength) + " bytes.");
    
    bool canBegin = Update.begin(contentLength);
    if (canBegin) {
      Serial.println("Flashing secondary memory block...");
      WiFiClient* stream = http.getStreamPtr();
      size_t written = Update.writeStream(*stream);
      
      if (written == contentLength) {
        Serial.println("Written structural integrity verified: " + String(written));
      }
      
      if (Update.end()) {
        if (Update.isFinished()) {
          Serial.println("[SUCCESS] OTA Complete! Rebooting hardware engine...");
          delay(1000);
          ESP.restart(); 
        } else {
          Serial.println("[ERROR] Update failed to finalize cleanly.");
        }
      } else {
        Serial.printf("[ERROR] Flash error occurred: %s\n", Update.errorString());
      }
    } else {
      Serial.println("[ERROR] Not enough space allocated on flash partition layout for update.");
    }
  } else {
    Serial.printf("[ERROR] Could not download binary file. HTTP Code: %d\n", httpCode);
  }
  http.end();
}

void sendDataToGoogle(float t, int m, String status) {
  WiFiClientSecure client;
  client.setInsecure(); 
  HTTPClient http;
  http.setTimeout(15000);
  
  http.begin(client, GOOGLE_SCRIPT_URL);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<200> doc;
  doc["deviceID"] = DEVICE_ID;
  doc["temp"] = t;
  doc["moisture"] = m;
  doc["status"] = status; 
  
  String jsonOutput;
  serializeJson(doc, jsonOutput);
  
  int httpCode = http.POST(jsonOutput);
  if (httpCode > 0) {
    Serial.printf("Telemetry logged successfully. Server response: %d\n", httpCode);
  }
  http.end();
}