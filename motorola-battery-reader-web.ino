#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include "settings.h"
#include "battery_reader.h"
#include "web_server.h"

// Глобальные объекты
WebServer server(HTTP_PORT);
BatteryReader battery(DS_PIN, PULLUP_PIN);

uint8_t batteryDump[DUMP_SIZE];
bool hasDump = false;

void setup() {
    Serial.begin(115200);
    Serial.println("\n\nMotorola Battery Reader Web Server (AP Mode)");
    Serial.println("==============================================");
    
    // Настройка светодиодов
    pinMode(LED_GREEN_PIN, OUTPUT);
    pinMode(LED_RED_PIN, OUTPUT);
    digitalWrite(LED_GREEN_PIN, LOW);
    digitalWrite(LED_RED_PIN, LOW);
    
    // Инициализация батареи
    if (!battery.begin()) {
        Serial.println("ERROR: Failed to initialize battery reader");
        digitalWrite(LED_RED_PIN, HIGH);
        while(1) delay(1000);
    }
    Serial.println("Battery reader initialized");
    
    // Создаем точку доступа
    Serial.printf("Creating Access Point: %s\n", AP_SSID);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    
    IPAddress IP = WiFi.softAPIP();
    Serial.print("AP IP Address: ");
    Serial.println(IP);
    
    // Мигаем зеленым при успешном создании AP
    for (int i = 0; i < 5; i++) {
        digitalWrite(LED_GREEN_PIN, HIGH);
        delay(100);
        digitalWrite(LED_GREEN_PIN, LOW);
        delay(100);
    }
    
    // Загружаем сохраненный дамп из SPIFFS
    if (SPIFFS.begin(true)) {
        File file = SPIFFS.open("/dump.bin", "r");
        if (file) {
            size_t size = file.read(batteryDump, DUMP_SIZE);
            if (size == DUMP_SIZE) {
                hasDump = true;
                Serial.println("Loaded saved dump from SPIFFS");
            }
            file.close();
        }
    } else {
        Serial.println("SPIFFS mount failed!");
    }
    
    // Запускаем веб-сервер
    setupWebServer();
    
    Serial.println("\n==============================================");
    Serial.println("READY!");
    Serial.printf("Connect to Wi-Fi: %s\n", AP_SSID);
    Serial.printf("Password: %s\n", AP_PASSWORD);
    Serial.printf("Open browser: http://%s\n", ESP_IP);
    Serial.println("==============================================");
    
    // Долгий зеленый сигнал готовности
    digitalWrite(LED_GREEN_PIN, HIGH);
    delay(1000);
    digitalWrite(LED_GREEN_PIN, LOW);
}

void loop() {
    // Обработка всех клиентских запросов
    // WebServer автоматически обрабатывает multipart upload в handleClient()
    server.handleClient();
    
    // Индикация работы: быстрый зеленый миг каждые 3 секунды
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink > 3000) {
        lastBlink = millis();
        digitalWrite(LED_GREEN_PIN, HIGH);
        delay(30);
        digitalWrite(LED_GREEN_PIN, LOW);
    }
}
