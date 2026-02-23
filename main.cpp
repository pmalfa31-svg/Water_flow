#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <httpUpdate.h>

// WiFi configuration
const char* ssid = "Pietro";
const char* password = "ciao1234";

// Supabase credentials
const char* supabase_url = "https://yvqknhhptchiknvvtmfo.supabase.co/rest/v1/water_measurements";
const char* supabase_key = "sb_publishable_tLecHgcJpg6mrIzkQtr4MA_ylz5WnHq";

// URL Firmware GitHub (OTA)
const char* firmware_url = "https://raw.githubusercontent.com/pmalfa31-svg/Water_flow/main/firmware.bin";

// Monitoring Hardware
const int SENSOR_PIN = 27;  // water flow sensor
volatile unsigned long pulseCount = 0;
const float PULSES_PER_LITER = 396.0;

// Buffers e Timing
float hourlyIntervals[12] = {0}; // liters amount every 5min
int intervalCounter = 0;
unsigned long lastFiveMinCheck = 0;
const unsigned long FIVE_MINUTES = 300000;

// Weekly counter (168 hours)
int hourlyUploadCount = 0;
const int UPLOADS_PER_WEEK = 168; 

void IRAM_ATTR pulseCounter() { pulseCount++; }

// Functions prototypes
void sendHourlyData();
void checkGitHubUpdate();

void setup() {
    Serial.begin(115200);
    pinMode(SENSOR_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), pulseCounter, FALLING);
    
    Serial.println("System Rebooted.");

    // Firmware update check after reset
    Serial.println("Initialization: update checking.");
    WiFi.begin(ssid, password);
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) { 
        delay(500); 
        Serial.print(".");
        retry++; 
    }

    if (WiFi.status() == WL_CONNECTED) {
        checkGitHubUpdate(); // check new firmware on GitHub
    }
    
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("\nSetup completed. Impulses monitoring activated.");
}

void loop() {
    unsigned long currentMillis = millis();

    if (currentMillis - lastFiveMinCheck >= FIVE_MINUTES) {
        float liters = (float)pulseCount / PULSES_PER_LITER;
        if (intervalCounter < 12) hourlyIntervals[intervalCounter] = liters;
        pulseCount = 0;
        intervalCounter++;
        lastFiveMinCheck = currentMillis;
    }

    if (intervalCounter >= 12) {
        sendHourlyData();
        intervalCounter = 0;
        for(int i = 0; i < 12; i++) hourlyIntervals[i] = 0;
    }
}

void sendHourlyData() {
    WiFi.begin(ssid, password);
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) { delay(500); retry++; }

    if (WiFi.status() == WL_CONNECTED) {
        // Sending values to Supabase
        HTTPClient http;
        http.begin(supabase_url);
        http.addHeader("apikey", supabase_key);
        http.addHeader("Authorization", ("Bearer " + String(supabase_key)).c_str());
        http.addHeader("Content-Type", "application/json");

        float totalFlow = 0;
        for(int i = 0; i < 12; i++) totalFlow += hourlyIntervals[i];

        JsonDocument doc; 
        doc["flow_rate"] = totalFlow;
        doc["5min"] = hourlyIntervals[0]; doc["10min"] = hourlyIntervals[1];
        doc["15min"] = hourlyIntervals[2]; doc["20min"] = hourlyIntervals[3];
        doc["25min"] = hourlyIntervals[4]; doc["30min"] = hourlyIntervals[5];
        doc["35min"] = hourlyIntervals[6]; doc["40min"] = hourlyIntervals[7];
        doc["45min"] = hourlyIntervals[8]; doc["50min"] = hourlyIntervals[9];
        doc["55min"] = hourlyIntervals[10]; doc["60min"] = hourlyIntervals[11];

        String body;
        serializeJson(doc, body);
        http.POST(body);
        http.end();

        // Weekly firmware update check
        hourlyUploadCount++;
        if (hourlyUploadCount >= UPLOADS_PER_WEEK) {
            checkGitHubUpdate();
            hourlyUploadCount = 0;
        }
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

void checkGitHubUpdate() {
    Serial.println("Checking GitHub for new firmware...");
    WiFiClientSecure client;
    // GitHub richiede TLS, ma possiamo saltare la verifica del certificato per semplicità di test
    // Nota: in produzione sarebbe meglio usare il fingerprint
    client.setInsecure(); 

    t_httpUpdate_return ret = httpUpdate.update(client, firmware_url);

    switch (ret) {
        case HTTP_UPDATE_FAILED:
            Serial.printf("OTA Failed: %s\n", httpUpdate.getLastErrorString().c_str());
            break;
        case HTTP_UPDATE_NO_UPDATES:
            Serial.println("No updates on GitHub.");
            break;
        case HTTP_UPDATE_OK:
            Serial.println("OTA Success! Rebooting...");
            break;
    }
}