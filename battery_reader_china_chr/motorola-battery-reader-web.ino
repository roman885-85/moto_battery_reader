#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <SPIFFS.h>
#include <esp_system.h>
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

// ROM-ID чипа DS2433. Потрібен НЕ лише для показу: з нього беруться ключі
// шифрування лічильників і дат у прошивці АКБ (key1 = ROM[1], key2 = ROM[6]) —
// див. impres_bms.h. Без нього ключ доводиться підбирати перебором.
uint8_t chipSN2433[8] = {0};
bool hasSN2433 = false;

// Людський опис причини останнього скидання ESP32 (esp_reset_reason()) —
// друкується найпершим повідомленням у setup(), щоб «перезавантажується під
// час заряду/розряду» одразу було видно як BROWNOUT/WDT/PANIC, а не
// доводилось здогадуватись за непрямими ознаками.
const char *resetReasonText(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "подача живлення";
        case ESP_RST_EXT:       return "зовнішній сигнал RESET";
        case ESP_RST_SW:        return "програмний esp_restart()";
        case ESP_RST_PANIC:     return "ПАНІКА/виключення — крах прошивки";
        case ESP_RST_INT_WDT:   return "апаратний WDT переривань (ISR завис)";
        case ESP_RST_TASK_WDT:  return "TASK WDT — головний цикл (loop) завис";
        case ESP_RST_WDT:       return "інший сторожовий таймер";
        case ESP_RST_DEEPSLEEP: return "вихід з глибокого сну";
        case ESP_RST_BROWNOUT:  return "BROWNOUT — просадка живлення нижче порогу";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "невідома причина";
    }
}

void setup() {
    // ПРИМІТКА: setRxBufferSize() тут НЕ викликаємо. Великі команди запису по USB
    // (WRITE33 ~1 КБ) надсилаються КЛІЄНТОМ частинами по ~200 Б із мікропаузами
    // (client_usb.html / moto_gui.py / moto_bridge.py), тож типового 256-байтного
    // UART-буфера достатньо і жодного втручання в ініціалізацію не потрібно.
    //
    // TX-буфер, навпаки, ЗБІЛЬШУЄМО. Заряд/розряд логують у Serial щосекунди
    // ГОДИНАМИ (chargeTask()/dischargeTask()) — при заповненому TX-буфері
    // (закритий монітор порту, повільний/відсутній читач на іншому кінці)
    // Serial.print() на ESP32 БЛОКУЄ виклик, доки не звільниться місце; якщо
    // ніхто й ніколи не читає — блокує головний цикл НАЗАВЖДИ, доки не
    // втрутиться сторожовий таймер і не перезавантажить пристрій. Саме це,
    // найімовірніше, стоїть за скаргою «зависання й самовільні
    // перезавантаження». Викликати ДО begin() — інакше не подіє.
    Serial.setTxBufferSize(2048);
    Serial.begin(115200);

    // ⚑ ПЕРШЕ, ЩО РОБИМО — ГАСИМО СИЛОВИЙ КЛЮЧ І ENABLE.
    // Якщо пристрій перезавантажився сторожем посеред розряду (див.
    // dischargeWatchdog у discharge.h), пакет усе ще підключений до резистора.
    // Поки не виконано жодного рядка ініціалізації, піни — входи, і затвор
    // тримає лише підтяжка. Опускаємо обидва явно й одразу, до дисплея, Wi-Fi
    // і всього іншого, що може зайняти секунди або впасти.
#ifdef LOAD_PIN
    pinMode(LOAD_PIN, OUTPUT);   digitalWrite(LOAD_PIN, LOW);
#endif
#ifdef CHARGE_PWM_PIN
    // CHARGE_PWM_PIN — ШІМ на базу NPN, що прочиняє P-MOSFET. LOW = NPN
    // закритий, затвор підтягнуто до +, ключ ЗАКРИТИЙ (див. схему в
    // settings.h). Ставимо це найпершим, ще до дисплея/Wi-Fi: якщо пристрій
    // перезавантажився сторожем посеред заряду, ключ усе ще може бути
    // прочинений наведенням, поки піни — входи.
    pinMode(CHARGE_PWM_PIN, OUTPUT);
    digitalWrite(CHARGE_PWM_PIN, LOW);
#endif
    pinMode(PULLUP_PIN, OUTPUT); digitalWrite(PULLUP_PIN, LOW);

    Serial.println("\n\nMotorola Battery Reader Web Server (AP Mode)");
    Serial.println("==============================================");
    // Причина ЦЬОГО старту — щоб відрізнити подачу живлення від несподіваного
    // скидання (BROWNOUT — просадка живлення під час вмикання силового
    // каскаду заряду/розряду, TASK_WDT — завис головний цикл, PANIC — крах
    // прошивки). Особливо важливо для скарг «перезавантажується під час
    // заряду/розряду» — сама ця причина одразу відсікає половину гіпотез.
    Serial.printf("RESET REASON: %s (код %d)\n",
                  resetReasonText(esp_reset_reason()), (int)esp_reset_reason());

    // Ініціалізація дисплея і кнопок меню + стартова заставка
    displayInit();
    displayButtonSetup();
    displayIntro();                 // чорний екран -> плавна поява/зникнення заставки
    displaySetStatus("ЗАПУСК...");

    // Налаштування світлодіодів (неблокуюча індикація)
    ledInit();
    // Налаштування звуку читаємо ДО стартового «привіту»: інакше пристрій із
    // вимкненим звуком однаково писнув би при кожному вмиканні. SPIFFS нижче
    // монтується ще раз — повторний begin() на вже змонтованій ФС нешкідливий.
    // Разом зі звуком піднімаємо й системну дату: вона потрібна вже в меню
    // пристрою (наробіток рахується від дати першого запуску), а клієнт може
    // не прийти взагалі.
    if (SPIFFS.begin(true)) { soundCfgLoad(); deviceClockLoad(); }
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
    // Вимикаємо WiFi modem-sleep: періодичні цикли «сон/пробудження» радіо
    // додають переривання саме там, де 1-Wire (бітбенг у battery_reader.cpp)
    // критично залежить від точності delayMicroseconds() — це задокументована
    // причина нестабільних/«сміттєвих» читань DS2433/DS2438 САМЕ під час
    // активного WiFi-трафіку (arduino-esp32#755 та інші). Скарга власника
    // «помилка лише у веб-версії» це й підтверджує: веб-інтерфейс тримає
    // радіо значно активнішим (постійні HTTP-запити/опитування), ніж USB
    // або меню пристрою, де WiFi лише простоює в фоні. Пристрій живиться не
    // від батарейки, тож постійно активне радіо — прийнятна ціна.
    WiFi.setSleep(false);

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
    // Зарядний DC/DC — так само, у безпечний стан ДО всього іншого.
    chargeInit();

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

    // Керований заряд: та сама логіка опитування й запобіжників, окремий стан
    // (charge.h). Заряд і розряд не можуть іти одночасно — обидва старти
    // взаємно перевіряють стан одне одного (chargeStart()/dischargeStart()).
    chargeTask();

    // Зняти утримання сигналу enable після зупинки розряду/заряду. Робиться
    // тут, а не в dischargeStop()/chargeStop(), щоб ці файли не залежали від
    // драйвера 1-Wire: зупинку викликають і з веба, і з USB, і з кнопки на
    // пристрої.
    if (dischargeConsumeReleaseEnable()) battery.holdEnable(false);
    if (chargeConsumeReleaseEnable())    battery.holdEnable(false);

    // Дисплей у цьому проєкті перемальовується ПО ПОДІЯХ, а розряд — процес
    // фоновий: без цього рядка показання лишалися б статичними до наступного
    // натискання кнопки. Рівень 2 (вхід у режим / вихід із нього) вимагає ПОВНОЇ
    // перемальовки, інакше поверх моніторингу видно попередню сторінку;
    // рівень 1 (нові показання раз на 5 с) оновлює лише рядки, без блимання.
    { uint8_t dirty = dischargeConsumeDirty();
      if (dirty) displayDischargeRefresh(dirty >= 2); }

    // ⚑ ТЕ САМЕ ДЛЯ ЗАРЯДУ. Цього рядка тут не було взагалі: charge.h справно
    // виставляв прапорець (chargeMarkDirty) на вхід у режим і на кожне нове
    // показання, drawPageCharge() був намальований — але consume нікому було
    // викликати, тож заряд НІКОЛИ не оновлював екран сам. На вигляд це рівно
    // те, на що скарга: запустив заряд — сторінка заряду не з'явилась, а коли
    // з'являлась (після сторонньої перемальовки), цифри стояли мертві до
    // натискання кнопки. Розряд працював лише тому, що рядок вище є.
    { uint8_t dirty = chargeConsumeDirty();
      if (dirty) displayChargeRefresh(dirty >= 2); }

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
        // Розряд із меню пристрою — до цілі, обраної сусіднім пунктом.
        else if (act == OP_DISCHARGE)     { const char *e = dischargeStart(dischargeTargetMv());
                                            if (e) { Serial.println(e); displayShow("РОЗРЯД: ЗБІЙ");
                                                     ledSet(LED_ERROR); } }
        // «Ціль розряду» нічого не пише — лише перемикає напругу по колу.
        else if (act == OP_DISCHARGE_TGT) { char m[24];
                                            snprintf(m, sizeof(m), "ЦІЛЬ %.2f В",
                                                     dischargeCycleTarget() / 1000.0);
                                            displayShow(m); ledSet(LED_IDLE); }
        // Заряд через DC/DC — до цілі, обраної сусіднім пунктом (у %).
        else if (act == OP_CHARGE_DCDC)   { const char *e = chargeStart(chargeTargetPct());
                                            if (e) { Serial.println(e); displayShow("ЗАРЯД: ЗБІЙ");
                                                     ledSet(LED_ERROR); } }
        // «Ціль заряду» нічого не пише — лише перемикає відсоток по колу.
        else if (act == OP_CHARGE_TGT)    { char m[24];
                                            snprintf(m, sizeof(m), "ЦІЛЬ %u%%", chargeCycleTarget());
                                            displayShow(m); ledSet(LED_IDLE); }
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

    // Анімація батареї на головній сторінці ТА на сторінці розряду (~9 к/с).
    // Оновлює лише область шкали батареї, не чіпаючи цифри %. Під час розряду
    // це ще й ознака «процес живий»: статична шкала на довгій операції читалась
    // би як зависання. УВІМКНЕНА типово;
    // за потреби вимикається через #define DISABLE_BATTERY_ANIM у settings.h.
#ifndef DISABLE_BATTERY_ANIM
    static unsigned long lastAnim = 0;
    if (millis() - lastAnim > ANIM_GRADIENT_MS) { lastAnim = millis(); displayAnimTick(); }
#endif
}
