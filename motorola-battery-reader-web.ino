#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include "settings.h"
#include "battery_reader.h"
#include "display.h"
#include "web_server.h"

// Глобальные объекты
WebServer server(HTTP_PORT);
BatteryReader battery(DS_PIN, PULLUP_PIN);

uint8_t batteryDump[DUMP_SIZE];
bool hasDump = false;

uint8_t batteryDump2438[DS2438_MEM_SIZE];
bool hasDump2438 = false;

// Серийный номер (лазерный 1-Wire ROM-ID) чипа DS2438 из последнего чтения
uint8_t chipSN2438[8] = {0};
bool hasSN2438 = false;

void setup() {
    Serial.begin(115200);
    Serial.println("\n\nMotorola Battery Reader Web Server (AP Mode)");
    Serial.println("==============================================");

    // Инициализация дисплея и кнопок меню + стартовая заставка
    displayInit();
    displayButtonSetup();
    displaySplash();
    delay(2500);
    displaySetStatus("ЗАПУСК...");

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
    
    // Загружаем сохраненные дампы из SPIFFS
    if (SPIFFS.begin(true)) {
        File file = SPIFFS.open("/dump.bin", "r");
        if (file) {
            size_t size = file.read(batteryDump, DUMP_SIZE);
            if (size == DUMP_SIZE) {
                hasDump = true;
                Serial.println("Loaded saved DS2433 dump from SPIFFS");
            }
            file.close();
        }

        File file2438 = SPIFFS.open("/dump2438.bin", "r");
        if (file2438) {
            size_t size = file2438.read(batteryDump2438, DS2438_MEM_SIZE);
            if (size == DS2438_MEM_SIZE) {
                hasDump2438 = true;
                Serial.println("Loaded saved DS2438 dump from SPIFFS");
            }
            file2438.close();
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

    // Готовність на дисплеї
    displayShow("ГОТОВО");
}

void loop() {
    // Обработка всех клиентских запросов
    // WebServer автоматически обрабатывает multipart upload в handleClient()
    server.handleClient();

    // Опрос кнопки перелистывания меню
    displayHandleButton();

    // После полного цикла перелистывания (возврат на 1-ю страницу) —
    // перечитываем аккумулятор, чтобы обновить данные.
    if (displayConsumeReadRequest()) {
        bool ok2433, ok2438;
        readAllChips(ok2433, ok2438);
    }

    // Периодическое обновление дисплея (свежие статус/дампы) раз в секунду
    static unsigned long lastDisplay = 0;
    if (millis() - lastDisplay > 1000) {
        lastDisplay = millis();
        displayRender();
    }

    // Индикация работы: быстрый зеленый миг каждые 3 секунды
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink > 3000) {
        lastBlink = millis();
        digitalWrite(LED_GREEN_PIN, HIGH);
        delay(30);
        digitalWrite(LED_GREEN_PIN, LOW);
    }
}
