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
    // ПРИМІТКА: setRxBufferSize() тут НЕ викликаємо. Великі команди запису по USB
    // (WRITE33 ~1 КБ) надсилаються КЛІЄНТОМ частинами по ~200 Б із мікропаузами
    // (client_usb.html / moto_gui.py / moto_bridge.py), тож типового 256-байтного
    // UART-буфера достатньо і жодного втручання в ініціалізацію не потрібно.
    Serial.begin(115200);
    Serial.println("\n\nMotorola Battery Reader Web Server (AP Mode)");
    Serial.println("==============================================");

    // Ініціалізація дисплея і кнопок меню + стартова заставка
    displayInit();
    displayButtonSetup();
    displayIntro();                 // чорний екран -> плавна поява/зникнення заставки
    displaySetStatus("ЗАПУСК...");

    // Налаштування світлодіодів (неблокуюча індикація)
    ledInit();
    buzzSelfTest();     // стартовий «чирп» самоперевірки динаміка (+ діагностика в Serial)

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
    
    // SPIFFS монтуємо (потрібен для журналів Майстра відновлення й збереження
    // дампів під час сесії). Але збережений останній дамп при старті НЕ
    // завантажуємо: дисплей/веб/USB лишаються ЧИСТИМИ (без даних), поки не
    // зчитають акумулятор. Щоб повернути показ останнього дампа після
    // перезавантаження — додайте у settings.h: #define LOAD_SAVED_DUMPS_ON_BOOT
    if (SPIFFS.begin(true)) {
#ifdef LOAD_SAVED_DUMPS_ON_BOOT
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
#else
        Serial.println("Clean start: saved battery dump NOT loaded (read a battery to populate)");
#endif
    } else {
        Serial.println("SPIFFS mount failed!");
    }
    
    // Розрядне навантаження — у безпечний стан ДО всього іншого.
    dischargeInit();

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

    // Готовність на дисплеї — плавний вхід у головне меню.
    displaySetStatus("ГОТОВО");
    displayFadeInMain();
}

void loop() {
    // Captive-portal: обробляємо DNS-запити (усі домени -> 192.168.4.1).
    dnsServer.processNextRequest();

    // Обробка усіх клієнтських запитів
    // WebServer автоматично обробляє multipart upload в handleClient()
    server.handleClient();

    // Командний протокол по USB-Serial (Windows-клієнт). Працює паралельно з Wi-Fi.
    serialTask();

    // Керований розряд: опитування монітора й запобіжники (реальна робота раз
    // на DISCHARGE_POLL_MS). Викликаємо ДО обробки кнопок, щоб аварійні
    // відсічки спрацьовували з мінімальною затримкою.
    dischargeTask();

    // Дисплей у цьому проєкті перемальовується ПО ПОДІЯХ, а розряд — процес
    // фоновий: без цього рядка показання лишалися б статичними до наступного
    // натискання кнопки. Рівень 2 (вхід у режим / вихід із нього) вимагає ПОВНОЇ
    // перемальовки, інакше поверх моніторингу видно попередню сторінку;
    // рівень 1 (нові показання раз на 10 с) оновлює лише рядки, без блимання.
    { uint8_t dirty = dischargeConsumeDirty();
      if (dirty) displayDischargeRefresh(dirty >= 2); }

    // Опитування кнопки перегортання меню
    displayHandleButton();

    // Після повного циклу перегортання (повернення на 1-ю сторінку) —
    // перечитуємо акумулятор, щоб оновити дані.
    if (displayConsumeReadRequest()) {
        bool ok2433, ok2438;
        readAllChips(ok2433, ok2438);      // наприкінці сам робить 1 повний перемальовок
    }

    // Підтверджена в меню дисплея дія. Порядок і склад операцій задає
    // operations.h — той самий каталог, що малюють екран, веб і USB-клієнт.
    // Раніше номери дій були «зашиті» тут числами 0..6 і розходилися з тим,
    // що показував екран; тепер розбір іде через opTemplateOf*/opExpert().
    int act = displayConsumeActionRequest();
    if (act >= 0) {
        int tm = opTemplateOfModel(act);
        int tn = opTemplateOfNew(act);
        int ex = opExpert(act);
        if      (act == OP_CELLSWAP)      performRecalPrepare(false);
        else if (act == OP_CELLSWAP_DEEP) performRecalPrepare(true);
        else if (act == OP_REPAIR)        performRepair();
        else if (act == OP_SETCHARGE)     { int p = chargePctFromVoltage();
                                            if (p >= 0) performSetChargePct(p); }
        else if (act == OP_RESET)         performReset();
        else if (act == OP_CLEAN)         performFactoryClean();
        else if (act == OP_DISCHARGE)     { const char *e = dischargeStart(0);
                                            if (e) { Serial.println(e); displayShow("РОЗРЯД: ЗБІЙ");
                                                     ledSet(LED_ERROR); } }
        // «Модель <X>» — модельна частина еталона, БЕЗ навченого хвоста донора.
        else if (tm >= 0)                 performRestoreTemplate(BATTERY_TEMPLATES[tm].name);
        // «Новий <X>» — порожній чіп -> робочий АКБ; заряд 50 % ємності моделі.
        else if (tn >= 0)                 { const char *nm = BATTERY_TEMPLATES[tn].name;
                                            performInitBattery(nm, impresRatedMah(nm) / 2); }
        else if (ex == OP_WIPE33_REL)     performWipe2433();
        else if (ex == OP_WIPE38_REL)     performWipe2438();
        else if (ex == OP_REBOOT_REL)     { displayShow("ПЕРЕЗАВАНТАЖ."); Serial.flush();
                                            delay(300); ESP.restart(); }
    }

    // Екранний Майстер відновлення: 1 = аналіз (зчитати + оновити діагноз),
    // 2 = виконати наступний крок плану. Логіку тримає recovery.h; кнопки —
    // display.h/display_color.h; тут лише зв'язуємо (двигун доступний після
    // include web_server.h -> recovery.h).
    int wizReq = displayConsumeWizRequest();
    if (wizReq == 1) {
        bool a2433, a2438; readAllChips(a2433, a2438);
        wizDeviceRefresh();
        displayRender();
    } else if (wizReq == 2) {
        wizDeviceRunNext();
        displayRender();
    }

    // Дисплей перемальовується по подіям (натискання кнопки, читання/запис),
    // тому цикл не блокується повільним рендером і кнопки чутливі.

    // Неблокуюча індикація світлодіодами (пульс очікування / читання / запис
    // / успіх / помилка — режим задають обробники через ledSet()).
    ledTask();

    // Червоний «світлофільтр» на екрані на час оповіщення про помилку: увесь
    // вміст стає червоним відтінком (без блимання), поки активний LED_ERROR,
    // і повертається до норми, коли індикатор виходить із режиму помилки.
    {
        static LedMode prevLed = LED_BOOT;
        if (g_ledMode != prevLed) {
            if (g_ledMode == LED_ERROR)      displaySetErrorTint(true);
            else if (prevLed == LED_ERROR)   displaySetErrorTint(false);
            prevLed = g_ledMode;
        }
    }

    // Анімація батареї на головній сторінці (~9 к/с) — пульсація заповнення.
    // Оновлює лише область шкали батареї, не чіпаючи цифри %. УВІМКНЕНА типово;
    // за потреби вимикається через #define DISABLE_BATTERY_ANIM у settings.h.
#ifndef DISABLE_BATTERY_ANIM
    static unsigned long lastAnim = 0;
    if (millis() - lastAnim > ANIM_GRADIENT_MS) { lastAnim = millis(); displayAnimTick(); }
#endif
}
