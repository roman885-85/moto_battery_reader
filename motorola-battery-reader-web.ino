#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <SPIFFS.h>
#include "settings.h"
#include "leds.h"
#include "battery_reader.h"
#include "display.h"
#include "web_server.h"
#include "serial_api.h"

// Глобальні об'єкти
WebServer server(HTTP_PORT);
DNSServer dnsServer;                 // captive-portal: авто-відкриття сторінки
BatteryReader battery(DS_PIN, PULLUP_PIN);

uint8_t batteryDump[DUMP_SIZE];
bool hasDump = false;

uint8_t batteryDump2438[DS2438_MEM_SIZE];
bool hasDump2438 = false;

// Серійний номер (лазерний 1-Wire ROM-ID) чипа DS2438 з останнього читання
uint8_t chipSN2438[8] = {0};
bool hasSN2438 = false;

void setup() {
    // Збільшуємо буфер прийому UART ДО Serial.begin(): типовий 256-байтний буфер
    // переповнюється великими командами запису по USB (WRITE33 = 512 Б -> ~1 КБ
    // hex), через що вони приходили побитими і запис не відбувався. 4 КБ вистачає.
    Serial.setRxBufferSize(4096);
    Serial.begin(115200);
    Serial.println("\n\nMotorola Battery Reader Web Server (AP Mode)");
    Serial.println("==============================================");

    // Ініціалізація дисплея і кнопок меню + стартова заставка
    displayInit();
    displayButtonSetup();
    displaySplash();
    delay(2500);
    displaySetStatus("ЗАПУСК...");

    // Налаштування світлодіодів (неблокуюча індикація)
    ledInit();

    // Ініціалізація батареї
    if (!battery.begin()) {
        Serial.println("ERROR: Failed to initialize battery reader");
        ledWrite(false, true);   // постійний червоний — фатальна помилка
        while(1) delay(1000);
    }
    Serial.println("Battery reader initialized");
    
    // Створюємо точку доступу
    Serial.printf("Creating Access Point: %s\n", AP_SSID);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    
    IPAddress IP = WiFi.softAPIP();
    Serial.print("AP IP Address: ");
    Serial.println(IP);

    // Captive-portal DNS: відповідаємо адресою ESP на будь-який домен, щоб телефон/ПК
    // при підключенні до Wi-Fi одразу запропонував відкрити нашу сторінку.
    dnsServer.start(53, "*", IP);
    Serial.println("Captive-portal DNS started");

    // Короткий зелений сигнал успішного старта AP
    ledSet(LED_OK);
    
    // Завантажуємо збережені дампи з SPIFFS
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
    
    // Запускаємо веб-сервер
    setupWebServer();
    
    Serial.println("\n==============================================");
    Serial.println("READY!");
    Serial.printf("Connect to Wi-Fi: %s\n", AP_SSID);
    Serial.printf("Password: %s\n", AP_PASSWORD);
    Serial.printf("Open browser: http://%s\n", ESP_IP);
    Serial.println("==============================================");
    
    // Переходимо в режим очікування (зелений «пульс» раз на 3 с)
    ledSet(LED_IDLE);

    // Готовність на дисплеї
    displayShow("ГОТОВО");
}

void loop() {
    // Captive-portal: обробляємо DNS-запити (усі домени -> 192.168.4.1).
    dnsServer.processNextRequest();

    // Обробка усіх клієнтських запитів
    // WebServer автоматично обробляє multipart upload в handleClient()
    server.handleClient();

    // Командний протокол по USB-Serial (Windows-клієнт). Працює паралельно з Wi-Fi.
    serialTask();

    // Опитування кнопки перегортання меню
    displayHandleButton();

    // Після повного циклу перегортання (повернення на 1-ю сторінку) —
    // перечитуємо акумулятор, щоб оновити дані.
    if (displayConsumeReadRequest()) {
        bool ok2433, ok2438;
        readAllChips(ok2433, ok2438);
    }

    // Підтверджена в меню дисплея дія: 0=Скидання 1=Ремонт 2=Очистка 3=Стерти2433
    // 4=Перезавантаження, 5..=«Новий АКБ <модель>» (ініціалізація порожнього чипа
    // еталоном обраної моделі з паспортною ємністю BATTERY_RATED_MAH).
    int act = displayConsumeActionRequest();
    if      (act == 0) performReset();
    else if (act == 1) performRepair();
    else if (act == 2) performFactoryClean();
    else if (act == 3) performWipe2433();
    else if (act == 4) { displayShow("ПЕРЕЗАВАНТАЖ."); Serial.flush(); delay(300); ESP.restart(); }
    else if (act >= NUM_BASE_ACTIONS && act < NUM_BASE_ACTIONS + BATTERY_TEMPLATE_COUNT)
        performInitBattery(BATTERY_TEMPLATES[act - NUM_BASE_ACTIONS].name, BATTERY_RATED_MAH);

    // Дисплей перемальовується по подіям (натискання кнопки, читання/запис),
    // тому цикл не блокується повільним рендером і кнопки чутливі.

    // Неблокуюча індикація світлодіодами (пульс очікування / читання / запис
    // / успіх / помилка — режим задають обробники через ledSet()).
    ledTask();
}
