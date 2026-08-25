#pragma once
// ===========================================================================
//  bt_link.h — ПІДКЛЮЧЕННЯ КЛІЄНТСЬКИХ ПРОГРАМ ПО BLUETOOTH
// ===========================================================================
//  Той самий командний протокол, що й по USB (serial_api.h), тільки поверх
//  Bluetooth SPP — послідовного порту по радіо. Вибір саме SPP, а не BLE, тут
//  вирішує майже все:
//
//   • SPP на комп'ютері виглядає ЗВИЧАЙНИМ COM-портом. Отже `moto_gui.py`
//     (pyserial), `moto_bridge.py` і `client_usb.html` (Web Serial) працюють
//     БЕЗ ЖОДНИХ ЗМІН — треба лише обрати інший порт зі списку.
//   • BLE довелося б обгортати власним GATT-сервісом і переписувати транспорт
//     у кожному клієнті. Web Bluetooth уміє тільки BLE, але Web Serial уміє
//     COM-порт — тобто браузерний клієнт і так дістає BT через SPP.
//
//  ⚠️ ЦІНА: класичний Bluetooth — це великий стек. Прошивка з ним не влазить
//  у типову схему розділів Arduino IDE («Default 4MB with spiffs», 1.2 МБ під
//  програму). Потрібно обрати **Huge APP (3MB No OTA/1MB SPIFFS)**; SPIFFS
//  зменшиться до 1 МБ, чого вистачає з запасом (index.html ~226 КБ + заставка).
//  Перевірити це на етапі компіляції не можна — розділи задаються в IDE, а не
//  в коді, — тому пристрій друкує вільне місце при старті.
//
//  ⚠️ І ДРУГА ЦІНА: радіо одне на Wi-Fi і Bluetooth. Працюють вони разом, але
//  діляться ефіром і пам'яттю. Якщо Wi-Fi не потрібен, вимкнути точку доступу
//  помітно оздоровить обидва.
//
// ── БЕЗПЕКА: ЧОМУ ПО РАДІО ПРАВИЛА ІНШІ ────────────────────────────────────
//  По USB командний протокол відкритий повністю, і це виправдано: щоб щось
//  надіслати, треба фізично встромити кабель. «AUTH» там лише звіряє пароль і
//  підсвічує статус у клієнті — команди запису працюють і без нього.
//
//  По Bluetooth такого захисту немає. У радіусі дії опиняється будь-хто, а в
//  переліку команд є `WIPE33`, `WRITE33`, `CLEAN` — тобто повне стирання
//  пам'яті акумулятора. Мовчки винести цей самий перелік в ефір означало б
//  зробити з несправності «хтось поруч» справу однієї команди.
//
//  Тому по BT діє правило: ЧИТАТИ можна вільно (щоб клієнт міг знайти пристрій
//  і показати стан), а ЗМІНЮВАТИ щось — лише після `AUTH <пароль>`. По USB
//  поведінка НЕ змінюється: ламати наявні робочі процеси заради симетрії немає
//  сенсу, ризики там різні.
//
//  Класифікація команд — чиста функція serCmdIsWrite() нижче, щоб її міг
//  перевірити хостовий тест. Перелік «безпечних» узято ЗАКРИТИМ (усе, чого
//  немає в ньому, вважається записом): забути дописати нову команду в перелік
//  дозволених — це відмова в бік «попросить пароль», а забути в переліку
//  заборонених — у бік «пустить без пароля».
// ===========================================================================

#include <stdint.h>
#include <string.h>
#include <stdio.h>     // snprintf у btName(); під Arduino приходить транзитом,
                        // але тримати чужий include як умову власної збірки не варто

// Чи змінює команда стан пакета або пристрою. Рядок — велике латинське ім'я
// команди, як його розбирає serialExec().
//
//  ⚑ ПЕРЕЛІК ЗАКРИТИЙ І САМЕ «БІЛИЙ». Тут перелічено те, що НІЧОГО не міняє;
//  усе інше — запис. Якщо завтра з'явиться нова команда й про неї забудуть,
//  вона автоматично потрапить у «потрібен пароль». Зворотний варіант (чорний
//  перелік) при тій самій забудькуватості пустив би її в ефір без пароля.
inline bool serCmdIsWrite(const char *cmd) {
    static const char *kReadOnly[] = {
        "AUTH",        // сам пароль
        "PING", "INFO", "READ", "GET33", "GET38",
        "TEMPLATES", "SAMPLES", "OPS", "FIXES",
        "RESTOREPLAN", // лише РАХУЄ план, нічого не пише
        "WIZLIST",     // перелік кроків майстра
        "SOUND",       // налаштування звуку самого пристрою: у пакет не пише
        "CLOCK",       // дата пристрою: у пакет не пише
    };
    for (unsigned i = 0; i < sizeof(kReadOnly) / sizeof(kReadOnly[0]); i++)
        if (strcmp(cmd, kReadOnly[i]) == 0) return false;
    return true;
}

// Чи виконувати команду, що прийшла ЦИМ транспортом.
//  viaBt  — прийшла по Bluetooth (а не по USB);
//  authed — клієнт уже надіслав правильний AUTH.
inline bool serCmdAllowed(const char *cmd, bool viaBt, bool authed) {
#if defined(BT_REQUIRE_AUTH) && (BT_REQUIRE_AUTH == 0)
    (void)cmd; (void)viaBt; (void)authed;
    return true;                       // явно вимкнено власником
#else
    if (!viaBt) return true;           // USB — фізичний доступ і є перепусткою
    if (!serCmdIsWrite(cmd)) return true;
    return authed;
#endif
}

// ── ДОВЖИНИ Й ФОРМАТ: static_assert, а не #if ─────────────────────────────
//  Препроцесор не вміє sizeof — `#if (sizeof(BT_NAME) - 1) > 26` дає
//  «missing binary operator before token "("». static_assert бачить справжню
//  довжину рядка, спрацьовує на компіляції так само жорстко і, на відміну від
//  #if, працює ще й у хостових тестах.
#if defined(BT_ENABLED)
  // 26 = стеля імені SPP (31) мінус п'ять символів суфікса «-XXXX».
  #if defined(BT_NAME) && !defined(BT_NAME_EXACT)
    static_assert(sizeof(BT_NAME) - 1 <= 26,
        "BT_NAME задовге: разом із суфіксом «-XXXX» ім'я не влізе в межу Bluetooth. "
        "Скоротіть або задайте BT_NAME_EXACT.");
    static_assert(sizeof(BT_NAME) - 1 >= 1, "BT_NAME порожнє.");
  #endif
  #if defined(BT_NAME_EXACT)
    static_assert(sizeof(BT_NAME_EXACT) - 1 >= 1 && sizeof(BT_NAME_EXACT) - 1 <= 31,
        "BT_NAME_EXACT мусить бути від 1 до 31 символу.");
  #endif
  #ifdef BT_PIN
    // Legacy pairing приймає ЛИШЕ цифри. Літера тут не дає помилки збірки сама
    // по собі — вона просто ламає спарювання, і шукати причину довелося б у
    // налаштуваннях системи, а не в цьому рядку.
    constexpr bool btPinAllDigits(const char *s) {
        return *s == '\0' ? true : ((*s >= '0' && *s <= '9') && btPinAllDigits(s + 1));
    }
    static_assert(btPinAllDigits(BT_PIN),
        "BT_PIN мусить складатися ЛИШЕ з цифр: Bluetooth legacy pairing інших символів не приймає.");
    static_assert(sizeof(BT_PIN) - 1 >= 4 && sizeof(BT_PIN) - 1 <= 16,
        "BT_PIN мусить бути від 4 до 16 цифр.");
  #endif
#endif

#if defined(BT_ENABLED) && defined(ARDUINO)
  //  ⚑ ЛИШЕ ДЛЯ ЗБІРКИ ПІД ARDUINO. Хостові тести включають цей заголовок
  //   заради чистого правила доступу (serCmdAllowed), і стека Bluetooth у них
  //   немає й бути не може. Без цієї умови перевірка валила б увесь набір —
  //   рівно так, як щойно сталося з перевіркою TJpg_Decoder.
  #if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
    #error "BT_ENABLED увімкнено, але в конфігурації плати немає класичного Bluetooth. На ESP32-S3/C3/C6 його НЕМАЄ ФІЗИЧНО (там лише BLE) — оберіть плату на звичайному ESP32 або закоментуйте BT_ENABLED."
  #endif
  #if defined(__has_include)
    #if !__has_include(<BluetoothSerial.h>)
      #error "BT_ENABLED увімкнено, але заголовка BluetoothSerial.h немає: ядро зібране без класичного Bluetooth."
    #endif
  #endif
  #include <BluetoothSerial.h>
  #include <esp_bt_device.h>
  // ⚑ Версія ядра потрібна нижче для вибору сигнатури setPin(). Якщо макрос
  //  невідомий, препроцесор мовчки вважає його нулем — і збірка на ядрі 3.x
  //  пішла б гілкою для 2.x, тобто впала б на тій самій помилці, яку ця гілка
  //  й покликана обійти. Та сама пастка, що вже описана у wdt.h.
  #if defined(__has_include)
    #if __has_include(<esp_arduino_version.h>)
      #include <esp_arduino_version.h>
    #endif
  #endif
  #if !defined(ESP_ARDUINO_VERSION_MAJOR)
    #error "ESP_ARDUINO_VERSION_MAJOR невідомий: bt_link.h не може обрати правильну сигнатуру BluetoothSerial::setPin(). Підключіть <Arduino.h> раніше за bt_link.h."
  #endif

BluetoothSerial SerialBT;
static bool g_btUp = false;

// Ім'я, під яким пристрій видно в ефірі. За замовчуванням до нього додаються
// чотири шістнадцяткові цифри з MAC — інакше два прилади поруч виглядали б
// однаково, і сплутати їх було б справою випадку.
inline String btName() {
#ifdef BT_NAME_EXACT
    return String(BT_NAME_EXACT);
#else
    const uint8_t *m = esp_bt_dev_get_address();
    char suf[8] = "";
    if (m) snprintf(suf, sizeof(suf), "-%02X%02X", m[4], m[5]);
    return String(BT_NAME) + suf;
#endif
}

inline bool btUp()        { return g_btUp; }
inline bool btConnected() { return g_btUp && SerialBT.hasClient(); }

inline void btBegin() {
#ifdef BT_PIN
    // Простий числовий PIN (legacy pairing). Це не криптографія, а перший
    // бар'єр: без нього спарується будь-хто в радіусі дії. Другий бар'єр —
    // AUTH на командах запису (див. serCmdAllowed вище).
    //
    // ⚑ СИГНАТУРА setPin() РІЗНА В РІЗНИХ ЯДРАХ, і це не дрібниця — збірка
    //  просто не проходить:
    //    arduino-esp32 2.x:  bool setPin(const char *pin);
    //    arduino-esp32 3.x:  bool setPin(const char *pin, uint8_t len);
    //  Довжину беремо з самого літерала (sizeof - 1), а не пишемо числом:
    //  інакше зміна BT_PIN тихо розійшлася б із переданою довжиною, і PIN
    //  або обрізався б, або читався за межами рядка.
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    SerialBT.setPin(BT_PIN, (uint8_t)(sizeof(BT_PIN) - 1));
  #else
    SerialBT.setPin(BT_PIN);
  #endif
#endif
    g_btUp = SerialBT.begin(btName());
    if (!g_btUp) {
        Serial.println("BT: НЕ ЗАПУСТИВСЯ. Найімовірніша причина — прошивка "
                       "зібрана зі схемою розділів без місця під Bluetooth-стек. "
                       "Оберіть у IDE «Huge APP (3MB No OTA/1MB SPIFFS)».");
        return;
    }
    Serial.printf("BT: увімкнено, ім'я «%s»%s. Клієнти під'єднуються як до "
                  "ЗВИЧАЙНОГО COM-порту — після пари в системі з'явиться "
                  "послідовний порт.\n",
                  btName().c_str(),
#ifdef BT_PIN
                  ", PIN заданий"
#else
                  ", БЕЗ PIN"
#endif
                  );
#if !defined(BT_REQUIRE_AUTH) || (BT_REQUIRE_AUTH != 0)
    Serial.println("BT: команди ЗАПИСУ потребують «AUTH <пароль>»; читання — вільне.");
#else
    Serial.println("BT: ⚠ перевірку пароля на записі ВИМКНЕНО (BT_REQUIRE_AUTH 0).");
#endif
}
#else
inline bool btUp()        { return false; }
inline bool btConnected() { return false; }
inline void btBegin()     {}
#endif  // BT_ENABLED
