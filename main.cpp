#include <Arduino.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "esp32/ulp.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <httpUpdate.h>

// 396 impulses per liter
// frequency 200hz

// RTC memory
#define ULP_COUNTER_INDEX    0   // ULP counter impulses
#define SAMPLE_COUNT_INDEX   1   // 5 minutes samples taken
#define HISTORY_START_INDEX  2   // 12 samples array index

// RTC variables
RTC_DATA_ATTR uint32_t samples[12]; // 12 samples every 5 minutes
RTC_DATA_ATTR int samples_index = 0;
static RTC_NOINIT_ATTR uint32_t ulp_impulse_counter;

// Network configuration
IPAddress local_IP(192, 168, 1, 150);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(8, 8, 8, 8);

// WiFi configuration
const char* ssid = "Tabletg";
const char* password = "123Stella!";

// Supabase credentials (database)
const char* supabase_url = "https://yvqknhhptchiknvvtmfo.supabase.co/rest/v1/water_measurements";
const char* supabase_key = "sb_publishable_tLecHgcJpg6mrIzkQtr4MA_ylz5WnHq";



void setup_ulp() {
    // 1. Pin inizialization
    rtc_gpio_init(GPIO_NUM_27); // water sensor input
    rtc_gpio_set_direction(GPIO_NUM_27, RTC_GPIO_MODE_INPUT_ONLY);

    // 2. Memory address ULP program -- Assembly!
    const ulp_insn_t program[] = {
        I_MOVI(R3, ULP_COUNTER_INDEX), // R3 pointing to counter memory cell
        
        M_LABEL(1), // low level waiting loop
            // bit 17 in  RTC input register
            I_RD_REG(RTC_GPIO_IN_REG, RTC_GPIO_IN_NEXT_S + 17, RTC_GPIO_IN_NEXT_S + 17), 
            M_BGE(1, 1),                    // if bit>=1 (HIGH), back to LABEL 1

        M_LABEL(2),                         // high level waiting loop
            I_RD_REG(RTC_GPIO_IN_REG, RTC_GPIO_IN_NEXT_S + 17, RTC_GPIO_IN_NEXT_S + 17),
            M_BL(2, 1),                     // if bit<1 (LOW), back to LABEL 2

        // relative impulse (rising edge) ---
        I_LD(R2, R3, 0),                    // Load current counter value in R2
        I_ADDI(R2, R2, 1),                  // Increment R2 (impulse +1)
        I_ST(R2, R3, 0),                    // Save new value in RTC memory
        
        M_BX(1),                            // Back to first loop (wait to LOW)
    };

    // 3. Programme loading in RTC memory
    size_t size = sizeof(program) / sizeof(ulp_insn_t);
    ulp_process_macros_and_load(0, program, &size);

    // 4. Configuration ULP timer
    ulp_set_wakeup_period(0, 100); 
    
    // Esecution start
    ulp_run(0);  
}

void server_data_sending(uint32_t* data) {
    Serial.println("Starting Wi-Fi connection...");
    
    // Prevention reset 
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA); 
    
    // Connection begin
    WiFi.begin(ssid, password);


    // Security Timeout: try to connect for max 15 seconds
    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000) {
        delay(250);
        Serial.print(".");
    }

    // Data upload to Supabase
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(supabase_url);
        http.addHeader("apikey", supabase_key);
        http.addHeader("Authorization", ("Bearer " + String(supabase_key)).c_str());
        http.addHeader("Content-Type", "application/json");

        float totalFlow = 0;
        for(int i = 0; i < 12; i++) totalFlow += data[i];

        JsonDocument doc; 
        doc["flow_rate"] = totalFlow;
        
        // Mapping 5min intervals to the existing JSON structure
        doc["5min"]  = data[0];  doc["10min"] = data[1];
        doc["15min"] = data[2];  doc["20min"] = data[3];
        doc["25min"] = data[4];  doc["30min"] = data[5];
        doc["35min"] = data[6];  doc["40min"] = data[7];
        doc["45min"] = data[8];  doc["50min"] = data[9];
        doc["55min"] = data[10]; doc["60min"] = data[11];
        

        long long int potValue = 0;
        potValue = analogRead(GPIO_NUM_33);
        Serial.printf("Input read voltage: %d\n", potValue);
        doc["Voltage"] = (potValue*805*62/15000); //resistor: 15k - 47k

        String body;
        serializeJson(doc, body);
        int httpResponseCode = http.POST(body);
        
        if (httpResponseCode > 0) {
            Serial.printf("Data sent! Response: %d\n", httpResponseCode);
        } else {
            Serial.printf("Error sending data: %s\n", http.errorToString(httpResponseCode).c_str());
        }
        
        http.end();

    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}




void setup() {
    Serial.begin(115200);

    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    uint32_t total_impulse = 0;

    // Pointer to ULP slow memory
    uint32_t* rtc_mem_ptr = (uint32_t*)RTC_SLOW_MEM;

    if (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED) {
        // --- FIRST START ---
        Serial.println("System inizialisation and ULP...");
        
        // Reset ULP memory before start
        rtc_mem_ptr[ULP_COUNTER_INDEX] = 0;
        samples_index = 0;
        for(int i=0; i<12; i++) samples[i] = 0;

        setup_ulp(); 
    } else {
        // --- RISVEGLIO OGNI 5 MINUTI ---
        
        // 1. Leggi gli impulsi accumulati dall'ULP (solo i 16 bit bassi)
        total_impulse = rtc_mem_ptr[ULP_COUNTER_INDEX] & 0xFFFF;

        // 2. Reset immediato del contatore ULP per non perdere i nuovi impulsi
        rtc_mem_ptr[ULP_COUNTER_INDEX] = 0;

        // 3. Salva nel buffer orario
        samples[samples_index] = total_impulse;
        Serial.printf("Campione [%d/12] salvato: %u impulsi\n", samples_index + 1, total_impulse);
        
        samples_index++;

        // 4. Se è passata un'ora (12 campioni da 5 min), invia a Supabase
        // per provare metto 
        if (samples_index >= 12) {
            server_data_sending(samples);
            // Reset indice dopo l'invio
            samples_index = 0; 
        }
    }

    // 5. Configura il prossimo risveglio (300 secondi)
    // esp_sleep_enable_timer_wakeup(300ULL * 1000000ULL);
    esp_sleep_enable_timer_wakeup(10ULL * 1000000ULL); // così sono ogni 10 secondi
    // aggiornamento totale circa 2 minuti

    
    Serial.println("Deep Sleep mode. ULP keep couting...");
    Serial.flush(); // Assicura che il messaggio venga inviato prima del sonno
    esp_deep_sleep_start();
}

void loop(){

}
